/*
 * gstore_fdw.c
 *
 * On GPU column based data store as FDW provider.
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"
#include "cuda_plcuda.h"

/*
 * GpuStoreChunk
 */
struct GpuStoreChunk
{
	dlist_node	chain;
	pg_crc32	hash;			/* hash value by (database_oid + table_oid) */
	Oid			database_oid;
	Oid			table_oid;
	TransactionId xmax;
	TransactionId xmin;
	CommandId	cid;
	bool		xmax_commited;
	bool		xmin_commited;
	cl_uint		kds_nitems;			/* copy of kds->nitems */
	cl_uint		kds_length;			/* copy of kds->length */
	cl_int		cuda_dindex;		/* set by 'pinning' option */
	CUipcMemHandle ipc_mhandle;
	dsm_handle	dsm_handle;
};
typedef struct GpuStoreChunk	GpuStoreChunk;

/*
 * GpuStoreMap - status of local mapping
 */
struct GpuStoreMap
{
	dsm_segment	   *dsm_seg;
};
typedef struct GpuStoreMap		GpuStoreMap;

#define GPUSTOREMAP_FOR_CHUNK(gs_chunk)			\
	(&gstore_maps[(gs_chunk) - (gstore_head->gs_chunks)])

/*
 * GpuStoreHead
 */
#define GSTORE_CHUNK_HASH_NSLOTS	97
typedef struct
{
	pg_atomic_uint32 has_warm_chunks;
	slock_t			lock;
	dlist_head		free_chunks;
	dlist_head		active_chunks[GSTORE_CHUNK_HASH_NSLOTS];
	GpuStoreChunk	gs_chunks[FLEXIBLE_ARRAY_MEMBER];
} GpuStoreHead;

/* ---- static functions ---- */
static void	gstore_fdw_table_options(Oid gstore_oid,
									int *p_pinning, int *p_format);
static void gstore_fdw_column_options(Oid gstore_oid, AttrNumber attnum,
									  int *p_compression);

/* ---- static variables ---- */
static int				gstore_max_relations;		/* GUC */
static shmem_startup_hook_type shmem_startup_next;
static object_access_hook_type object_access_next;
static GpuStoreHead	   *gstore_head = NULL;
static GpuStoreMap	   *gstore_maps = NULL;
static Oid				reggstore_type_oid = InvalidOid;

/* relation 'format' options */
#define GSTORE_FORMAT__PGSTROM			1
//#define GSTORE_FORMAT__CUPY			2

/* column 'compression' option */
#define GSTORE_COMPRESSION__NONE		1
#define GSTORE_COMPRESSION__PGLZ		2

/*
 * gstore_fdw_satisfies_visibility - equivalent to HeapTupleSatisfiesMVCC,
 * but simplified for GpuStoreChunk.
 */
static bool
gstore_fdw_satisfies_visibility(GpuStoreChunk *gs_chunk, Snapshot snapshot)
{
	if (!gs_chunk->xmin_commited)
	{
		if (!TransactionIdIsValid(gs_chunk->xmin))
			return false;		/* aborted or crashed */
		if (TransactionIdIsCurrentTransactionId(gs_chunk->xmin))
		{
			if (gs_chunk->cid >= snapshot->curcid)
				return false;	/* inserted after scan started */
			
			if (gs_chunk->xmax == InvalidTransactionId)
				return true;	/* nobody delete it yet */

			if (!TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
			{
				/* deleting subtransaction must have aborted */
				gs_chunk->xmax = InvalidTransactionId;
				return true;
			}
			if (gs_chunk->cid >= snapshot->curcid)
				return true;    /* deleted after scan started */
			else
				return false;   /* deleted before scan started */
		}
		else if (XidInMVCCSnapshot(gs_chunk->xmin, snapshot))
			return false;
		else if (TransactionIdDidCommit(gs_chunk->xmin))
			gs_chunk->xmin_commited = true;
		else
		{
			/* it must have aborted or crashed */
			gs_chunk->xmin = InvalidTransactionId;
			return false;
		}
	}
	else
	{
		/* xmin is committed, but maybe not according to our snapshot */
		if (gs_chunk->xmin != FrozenTransactionId &&
			XidInMVCCSnapshot(gs_chunk->xmin, snapshot))
			return false;	/* treat as still in progress */
	}
	/* by here, the inserting transaction has committed */
	if (!TransactionIdIsValid(gs_chunk->xmax))
		return true;	/* nobody deleted yet */

	if (!gs_chunk->xmax_commited)
	{
		if (TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
		{
			if (gs_chunk->cid >= snapshot->curcid)
				return true;    /* deleted after scan started */
			else
				return false;   /* deleted before scan started */
        }

		if (XidInMVCCSnapshot(gs_chunk->xmax, snapshot))
			return true;

        if (!TransactionIdDidCommit(gs_chunk->xmax))
        {
            /* it must have aborted or crashed */
			gs_chunk->xmax = InvalidTransactionId;
            return true;
        }
		/* xmax transaction committed */
		gs_chunk->xmax_commited = true;
	}
    else
	{
		/* xmax is committed, but maybe not according to our snapshot */
		if (XidInMVCCSnapshot(gs_chunk->xmax, snapshot))
			return true;        /* treat as still in progress */
    }
	/* xmax transaction committed */
	return false;
}

/*
 * gstore_fdw_mapped_chunk
 */
static inline kern_data_store *
gstore_fdw_mapped_chunk(GpuStoreChunk *gs_chunk)
{
	GpuStoreMap	   *gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);

	if (!gs_map->dsm_seg)
	{
		gs_map->dsm_seg = dsm_attach(gs_chunk->dsm_handle);
		dsm_pin_mapping(gs_map->dsm_seg);
	}
	else if (dsm_segment_handle(gs_map->dsm_seg) != gs_chunk->dsm_handle)
	{
		dsm_detach(gs_map->dsm_seg);

		gs_map->dsm_seg = dsm_attach(gs_chunk->dsm_handle);
		dsm_pin_mapping(gs_map->dsm_seg);
	}
	return (kern_data_store *)dsm_segment_address(gs_map->dsm_seg);
}

/*
 * gstore_fdw_lookup_chunk
 */
static GpuStoreChunk *
gstore_fdw_lookup_chunk_nolock(Relation frel, Snapshot snapshot)
{
	Oid				gstore_oid = RelationGetRelid(frel);
	GpuStoreChunk  *gs_chunk = NULL;
	dlist_iter		iter;
	pg_crc32		hash;
	int				index;

	INIT_LEGACY_CRC32(hash);
	COMP_LEGACY_CRC32(hash, &MyDatabaseId, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &gstore_oid, sizeof(Oid));
	FIN_LEGACY_CRC32(hash);
	index = hash % GSTORE_CHUNK_HASH_NSLOTS;

	dlist_foreach(iter, &gstore_head->active_chunks[index])
	{
		GpuStoreChunk  *gs_temp = dlist_container(GpuStoreChunk,
												  chain, iter.cur);
		if (gs_temp->hash == hash &&
			gs_temp->database_oid == MyDatabaseId &&
			gs_temp->table_oid == gstore_oid &&
			gstore_fdw_satisfies_visibility(gs_temp, snapshot))
		{
			if (!gs_chunk)
				gs_chunk = gs_temp;
			else
				elog(ERROR, "Bug? multiple GpuStoreChunks are visible");
		}
	}
	return gs_chunk;
}

static GpuStoreChunk *
gstore_fdw_lookup_chunk(Relation frel, Snapshot snapshot)
{
	GpuStoreChunk  *gs_chunk;

	PG_TRY();
	{
		gs_chunk = gstore_fdw_lookup_chunk_nolock(frel, snapshot);
	}
	PG_CATCH();
	{
		SpinLockRelease(&gstore_head->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&gstore_head->lock);

	return gs_chunk;
}


#if 0
static GpuStoreChunk *
gstore_fdw_next_chunk(GpuStoreChunk *gs_chunk, Snapshot snapshot)
{
	Oid			database_oid = gs_chunk->database_oid;
	Oid			table_oid = gs_chunk->table_oid;
	pg_crc32	hash = gs_chunk->hash;
	int			index = hash % GSTORE_CHUNK_HASH_NSLOTS;
	dlist_head *active_chunks = &gstore_head->active_chunks[index];
	dlist_node *dnode;

	while (dlist_has_next(active_chunks, &gs_chunk->chain))
	{
		dnode = dlist_next_node(active_chunks, &gs_chunk->chain);
		gs_chunk = dlist_container(GpuStoreChunk, chain, dnode);

		if (gs_chunk->hash == hash &&
			gs_chunk->database_oid == database_oid &&
			gs_chunk->table_oid == table_oid)
		{
			if (gstore_fdw_satisfies_visibility(gs_chunk, snapshot))
				return gs_chunk;
		}
	}
	return NULL;
}
#endif

/*
 * gstoreGetForeignRelSize
 */
static void
gstoreGetForeignRelSize(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid ftable_oid)
{
	Relation		frel;
	Snapshot		snapshot;
	GpuStoreChunk  *gs_chunk;

	frel = heap_open(ftable_oid, AccessShareLock);
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	gs_chunk = gstore_fdw_lookup_chunk(frel, snapshot);
	UnregisterSnapshot(snapshot);

	baserel->rows	= (gs_chunk ? gs_chunk->kds_nitems : 0);
	baserel->pages	= (gs_chunk ? gs_chunk->kds_length / BLCKSZ : 0);
	heap_close(frel, NoLock);
}

/*
 * gstoreGetForeignPaths
 */
static void
gstoreGetForeignPaths(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	ParamPathInfo *param_info;
	ForeignPath *fpath;
	Cost		startup_cost = baserel->baserestrictcost.startup;
	Cost		per_tuple = baserel->baserestrictcost.per_tuple;
	Cost		run_cost;
	QualCost	qcost;

	param_info = get_baserel_parampathinfo(root, baserel, NULL);
	if (param_info)
	{
		cost_qual_eval(&qcost, param_info->ppi_clauses, root);
		startup_cost += qcost.startup;
		per_tuple += qcost.per_tuple;
	}
	run_cost = per_tuple * baserel->rows;

	fpath = create_foreignscan_path(root,
									baserel,
									NULL,	/* default pathtarget */
									baserel->rows,
									startup_cost,
									startup_cost + run_cost,
									NIL,	/* no pathkeys */
									NULL,	/* no outer rel either */
									NULL,	/* no extra plan */
									NIL);	/* no fdw_private */
	add_path(baserel, (Path *) fpath);
}

/*
 * gstoreGetForeignPlan
 */
static ForeignScan *
gstoreGetForeignPlan(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid,
					 ForeignPath *best_path,
					 List *tlist,
					 List *scan_clauses,
					 Plan *outer_plan)
{
	List	   *scan_quals = NIL;
	ListCell   *lc;

	foreach (lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;
		scan_quals = lappend(scan_quals, rinfo->clause);
	}

	return make_foreignscan(tlist,
							scan_quals,
							baserel->relid,
							NIL,		/* fdw_exprs */
							NIL,		/* fdw_private */
							NIL,		/* fdw_scan_tlist */
							NIL,		/* fdw_recheck_quals */
							NULL);		/* outer_plan */
}

/*
 * gstoreScanState - state object for scan
 */
typedef struct
{
	GpuStoreChunk  *gs_chunk;
	cl_ulong		gs_index;
	Relation		gs_rel;
	bool			pinning;
	cl_uint			nattrs;
	AttrNumber		attnos[FLEXIBLE_ARRAY_MEMBER];
} gstoreScanState;

/*
 * gstoreBeginForeignScan
 */
static void
gstoreBeginForeignScan(ForeignScanState *node, int eflags)
{
	EState	   *estate = node->ss.ps.state;
	gstoreScanState *gss_state;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "cannot scan gstore_fdw table without MVCC snapshot");

	gss_state = palloc0(sizeof(gstoreScanState));
	gss_state->gs_chunk = NULL;
	gss_state->gs_index = 0;

	node->fdw_state = (void *)gss_state;
}

/*
 * gstoreIterateForeignScan
 */
static TupleTableSlot *
gstoreIterateForeignScan(ForeignScanState *node)
{
	gstoreScanState	*gss_state = (gstoreScanState *) node->fdw_state;
	Relation		frel = node->ss.ss_currentRelation;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	EState		   *estate = node->ss.ps.state;
	Snapshot		snapshot = estate->es_snapshot;
	kern_data_store *kds;
	cl_long			i, j;

	ExecClearTuple(slot);
	if (!gss_state->gs_chunk)
	{
		gss_state->gs_chunk = gstore_fdw_lookup_chunk(frel, snapshot);
		if (!gss_state->gs_chunk)
			return NULL;
	}
	kds = gstore_fdw_mapped_chunk(gss_state->gs_chunk);

	if (gss_state->gs_index >= kds->nitems)
		return NULL;

	i = gss_state->gs_index++;
	ExecStoreAllNullTuple(slot);

	for (j=0; j < kds->ncols; j++)
	{
		void   *addr = kern_get_datum_column(kds, j, i);
		int		attlen = kds->colmeta[j].attlen;

		if (!addr)
			slot->tts_isnull[j] = true;
		else
		{
			slot->tts_isnull[j] = false;
			if (!kds->colmeta[j].attbyval)
				slot->tts_values[j] = PointerGetDatum(addr);
			else if (attlen == sizeof(cl_char))
				slot->tts_values[j] = CharGetDatum(*((cl_char *)addr));
			else if (attlen == sizeof(cl_short))
				slot->tts_values[j] = Int16GetDatum(*((cl_short *)addr));
			else if (attlen == sizeof(cl_int))
				slot->tts_values[j] = Int32GetDatum(*((cl_int *)addr));
			else if (attlen == sizeof(cl_long))
				slot->tts_values[j] = Int64GetDatum(*((cl_long *)addr));
			else
				elog(ERROR, "unexpected attlen: %d", attlen);
		}
	}
	ExecMaterializeSlot(slot);
	return slot;
}

/*
 * gstoreReScanForeignScan
 */
static void
gstoreReScanForeignScan(ForeignScanState *node)
{
	gstoreScanState *gss_state = (gstoreScanState *) node->fdw_state;

	gss_state->gs_chunk = NULL;
	gss_state->gs_index = 0;
}

/*
 * gstoreEndForeignScan
 */
static void
gstoreEndForeignScan(ForeignScanState *node)
{
}

/*
 * gstoreIsForeignRelUpdatable
 */
static int
gstoreIsForeignRelUpdatable(Relation rel)
{
	return (1 << CMD_INSERT) | (1 << CMD_DELETE);
}

/*
 * gstorePlanDirectModify - allows only DELETE with no WHERE-clause
 */
static bool
gstorePlanDirectModify(PlannerInfo *root,
					   ModifyTable *plan,
					   Index resultRelation,
					   int subplan_index)
{
	CmdType		operation = plan->operation;
	Plan	   *subplan = (Plan *) list_nth(plan->plans, subplan_index);

	/* only DELETE command */
	if (operation != CMD_DELETE)
		return false;
	/* no WHERE-clause */
	if (subplan->qual != NIL)
		return false;
	/* no RETURNING-clause */
	if (plan->returningLists != NIL)
		return false;
	/* subplan should be GpuStore FDW */
	if (!IsA(subplan, ForeignScan))
		return false;

	/* OK, Update the operation */
	((ForeignScan *) subplan)->operation = CMD_DELETE;

	return true;
}

/*
 * gstorePlanForeignModify
 */
static List *
gstorePlanForeignModify(PlannerInfo *root,
						ModifyTable *plan,
						Index resultRelation,
						int subplan_index)
{
	CmdType		operation = plan->operation;

	if (operation != CMD_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gstore_fdw: not a supported operation"),
				 errdetail("gstore_fdw supports either INSERT into an empty GpuStore or DELETE without WHERE-clause only")));

	return NIL;
}

/*
 * gstoreLoadState - state object for INSERT
 */
typedef struct
{
	GpuContext *gcontext;	/* GpuContext, if pinned gstore */
	size_t		length;		/* available size except for KDS header */
	size_t		nrooms;		/* available max number of items */
	size_t		nitems;		/* current number of items */
	MemoryContext memcxt;	/* memcxt for construction per chunk */
	HTAB	  **cs_vl_dict;	/* dictionary of varlena datum, if any */
	size_t	   *cs_extra_sz;/* usage by varlena datum */
	bool	   *cs_hasnull;	/* true, if any NULL */
	bits8	  **cs_nullmap;	/* NULL-bitmap */
	void	  **cs_values;	/* array of values */
} gstoreLoadState;

/*
 * gstore_fdw_load_gpu_preserved
 */
static void
gstore_fdw_load_gpu_preserved(GpuContext *gcontext,
							  CUipcMemHandle *ptr_mhandle,
							  dsm_segment *dsm_seg)
{
	CUipcMemHandle ipc_mhandle;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;
	size_t		length = dsm_segment_map_length(dsm_seg);

	rc = gpuMemAllocPreserved(gcontext->cuda_dindex,
							  &ipc_mhandle,
							  length);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuMemAllocPreserved: %s", errorText(rc));
	PG_TRY();
	{
		rc = gpuIpcOpenMemHandle(gcontext,
								 &m_deviceptr,
								 ipc_mhandle,
								 CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuIpcOpenMemHandle: %s", errorText(rc));

		rc = cuCtxPushCurrent(gcontext->cuda_context);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuCtxPushCurrent: %s", errorText(rc));

		rc = cuMemcpyHtoD(m_deviceptr,
						  dsm_segment_address(dsm_seg),
						  length);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemcpyHtoD: %s", errorText(rc));

		rc = gpuIpcCloseMemHandle(gcontext, m_deviceptr);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuIpcCloseMemHandle: %s", errorText(rc));

		rc = cuCtxPopCurrent(NULL);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuCtxPopCurrent: %s", errorText(rc));
	}
	PG_CATCH();
	{
		gpuMemFreePreserved(gcontext->cuda_dindex, ipc_mhandle);
		PG_RE_THROW();
	}
	PG_END_TRY();
	memcpy(ptr_mhandle, &ipc_mhandle, sizeof(CUipcMemHandle));
}

/*
 * gstore_fdw_writeout_pgstrom
 */
static void
gstore_fdw_writeout_pgstrom(Relation relation, gstoreLoadState *gs_lstate)
{
	GpuContext	   *gcontext = gs_lstate->gcontext;
	Oid				table_oid = RelationGetRelid(relation);
	TupleDesc		tupdesc = RelationGetDescr(relation);
	size_t			nitems = gs_lstate->nitems;
	size_t			length;
	size_t			usage;
	size_t			ncols, i, j;
	pg_crc32		hash;
	cl_int			cuda_dindex = (gcontext ? gcontext->cuda_dindex : -1);
	dsm_segment	   *dsm_seg;
	kern_data_store *kds;
	CUipcMemHandle	ipc_mhandle;
	dlist_node	   *dnode;
	GpuStoreChunk  *gs_chunk = NULL;
	GpuStoreMap	   *gs_map = NULL;

	ncols = tupdesc->natts - (FirstLowInvalidHeapAttributeNumber + 1);
	length = usage = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[j];

		if (attr->attlen < 0)
		{
			length += (MAXALIGN(sizeof(cl_uint) * nitems) +
					   MAXALIGN(gs_lstate->cs_extra_sz[j]));
		}
		else
		{
			length += MAXALIGN(att_align_nominal(attr->attlen,
												 attr->attalign) * nitems);
			if (gs_lstate->cs_hasnull[j])
				length += MAXALIGN(BITMAPLEN(nitems));
		}
	}
	dsm_seg = dsm_create(length, 0);
	kds = dsm_segment_address(dsm_seg);
	init_kernel_data_store(kds,
						   tupdesc,
						   length,
						   KDS_FORMAT_COLUMN,
						   nitems);
	kds->nitems = nitems;
	kds->table_oid = RelationGetRelid(relation);
	pgstrom_ccache_writeout_chunk(kds,
								  gs_lstate->cs_nullmap,
								  gs_lstate->cs_hasnull,
								  gs_lstate->cs_values,
								  gs_lstate->cs_vl_dict,
								  gs_lstate->cs_extra_sz);

	/* allocation of device memory if 'pinning' mode */
	if (gcontext)
		gstore_fdw_load_gpu_preserved(gcontext, &ipc_mhandle, dsm_seg);
	else
		memset(&ipc_mhandle, 0, sizeof(CUipcMemHandle));

	/* pin the DSM segment to servive over the transaction */
	dsm_pin_mapping(dsm_seg);
	dsm_pin_segment(dsm_seg);

	/* hash value */
	INIT_LEGACY_CRC32(hash);
    COMP_LEGACY_CRC32(hash, &MyDatabaseId, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &table_oid, sizeof(Oid));
	FIN_LEGACY_CRC32(hash);

	SpinLockAcquire(&gstore_head->lock);
	if (dlist_is_empty(&gstore_head->free_chunks))
	{
		SpinLockRelease(&gstore_head->lock);
		if (gcontext)
			gpuMemFreePreserved(cuda_dindex, ipc_mhandle);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("too many gstore_fdw chunks required")));
	}
	dnode = dlist_pop_head_node(&gstore_head->free_chunks);
	gs_chunk = dlist_container(GpuStoreChunk, chain, dnode);
	gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);
	memset(gs_chunk, 0, sizeof(GpuStoreChunk));
	gs_chunk->hash = hash;
	gs_chunk->database_oid = MyDatabaseId;
	gs_chunk->table_oid = table_oid;
	gs_chunk->xmax = InvalidTransactionId;
	gs_chunk->xmin = GetCurrentTransactionId();
	gs_chunk->cid = GetCurrentCommandId(true);
	gs_chunk->xmax_commited = false;
	gs_chunk->xmin_commited = false;
	gs_chunk->kds_length = kds->length;
	gs_chunk->kds_nitems = kds->nitems;
	memcpy(&gs_chunk->ipc_mhandle, &ipc_mhandle, sizeof(CUipcMemHandle));
	gs_chunk->cuda_dindex = cuda_dindex;
	gs_chunk->dsm_handle = dsm_segment_handle(dsm_seg);
	gs_map->dsm_seg = dsm_seg;

	i = hash % GSTORE_CHUNK_HASH_NSLOTS;
	dlist_push_tail(&gstore_head->active_chunks[i], &gs_chunk->chain);
	pg_atomic_add_fetch_u32(&gstore_head->has_warm_chunks, 1);
	SpinLockRelease(&gstore_head->lock);
}

/*
 * gstore_fdw_release_chunk
 */
static void
gstore_fdw_release_chunk(GpuStoreChunk *gs_chunk)
{
	GpuStoreMap    *gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);

	dlist_delete(&gs_chunk->chain);
	if (gs_chunk->cuda_dindex >= 0)
		gpuMemFreePreserved(gs_chunk->cuda_dindex, gs_chunk->ipc_mhandle);
	if (gs_map->dsm_seg)
		dsm_detach(gs_map->dsm_seg);
	gs_map->dsm_seg = NULL;
#if PG_VERSION_NUM >= 100000
	/*
	 * NOTE: PG9.6 has no way to release DSM segment once pinned.
	 * dsm_unpin_segment() was newly supported at PG10.
	 */
	dsm_unpin_segment(gs_chunk->dsm_handle);
#endif
	memset(gs_chunk, 0, sizeof(GpuStoreMap));
	gs_chunk->dsm_handle = UINT_MAX;
	dlist_push_head(&gstore_head->free_chunks,
					&gs_chunk->chain);
}

/*
 * gstoreBeginForeignModify
 */
static void
gstoreBeginForeignModify(ModifyTableState *mtstate,
						 ResultRelInfo *rrinfo,
						 List *fdw_private,
						 int subplan_index,
						 int eflags)
{
	EState		   *estate = mtstate->ps.state;
	Relation		relation = rrinfo->ri_RelationDesc;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	GpuContext	   *gcontext = NULL;
	GpuStoreChunk  *gs_chunk;
	gstoreLoadState *gs_lstate;
	MemoryContext	oldcxt;
	int				pinning;
	int				format;
	cl_int			i, ncols;

	gstore_fdw_table_options(RelationGetRelid(relation), &pinning, &format);
	if (pinning >= 0)
	{
		gcontext = AllocGpuContext(pinning, false);
		if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
			ActivateGpuContext(gcontext);
	}
	LockRelationOid(RelationGetRelid(relation), ShareUpdateExclusiveLock);
	gs_chunk = gstore_fdw_lookup_chunk(relation, estate->es_snapshot);
	if (gs_chunk)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gstore_fdw: foreign table \"%s\" is not empty",
						RelationGetRelationName(relation))));
	/* state object */
	ncols = tupdesc->natts - FirstLowInvalidHeapAttributeNumber;
	gs_lstate = palloc0(sizeof(gstoreLoadState));
	gs_lstate->cs_vl_dict = palloc0(sizeof(HTAB *) * ncols);
	gs_lstate->cs_extra_sz = palloc0(sizeof(size_t) * ncols);
	gs_lstate->cs_hasnull = palloc0(sizeof(bool) * ncols);
	gs_lstate->cs_nullmap = palloc0(sizeof(bits8 *) * ncols);
	gs_lstate->cs_values = palloc0(sizeof(void *) * ncols);

	gs_lstate->gcontext = gcontext;
	gs_lstate->memcxt = AllocSetContextCreate(estate->es_query_cxt,
											  "gstore_fdw temporary context",
											  ALLOCSET_DEFAULT_SIZES);
	gs_lstate->nrooms = 10000;	/* tentative */

	oldcxt = MemoryContextSwitchTo(gs_lstate->memcxt);
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];

		if (attr->attlen < 0)
		{
			gs_lstate->cs_vl_dict[i] =
				create_varlena_dictionary(gs_lstate->nrooms);
			gs_lstate->cs_values[i] = palloc0(sizeof(vl_dict_key *) *
											  gs_lstate->nrooms);
		}
		else
		{
			gs_lstate->cs_values[i] =
				palloc0(att_align_nominal(attr->attlen,
										  attr->attalign) * gs_lstate->nrooms);
			gs_lstate->cs_nullmap[i] = palloc0(BITMAPLEN(gs_lstate->nrooms));
		}
	}
	MemoryContextSwitchTo(oldcxt);
	rrinfo->ri_FdwState = gs_lstate;
}

/*
 * gstoreExecForeignInsert
 */
static TupleTableSlot *
gstoreExecForeignInsert(EState *estate,
						ResultRelInfo *rrinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	gstoreLoadState *gs_lstate = rrinfo->ri_FdwState;
	size_t		j;

	slot_getallattrs(slot);
	/*
	 * expand local buffer on demand
	 */
	if (gs_lstate->nitems == gs_lstate->nrooms)
	{
		MemoryContext	oldcxt = MemoryContextSwitchTo(gs_lstate->memcxt);

		gs_lstate->nrooms += gs_lstate->nrooms + 5000;
		for (j=0; j < tupdesc->natts; j++)
		{
			Form_pg_attribute attr = tupdesc->attrs[j];

			if (attr->attlen < 0)
			{
				Assert(!gs_lstate->cs_nullmap[j]);
				gs_lstate->cs_values[j]
					= repalloc(gs_lstate->cs_values[j],
							   sizeof(vl_dict_key *) * gs_lstate->nrooms);
			}
			else
			{
				gs_lstate->cs_values[j]
					= repalloc(gs_lstate->cs_values[j],
							   att_align_nominal(attr->attlen,
												 attr->attalign) *
							   gs_lstate->nrooms);
				gs_lstate->cs_nullmap[j]
					= repalloc(gs_lstate->cs_nullmap[j],
							   BITMAPLEN(gs_lstate->nrooms));
			}
		}
		MemoryContextSwitchTo(oldcxt);
	}
	pgstrom_ccache_extract_row(tupdesc,
							   gs_lstate->nitems++,
							   gs_lstate->nrooms,
							   slot->tts_isnull,
							   slot->tts_values,
							   gs_lstate->cs_nullmap,
							   gs_lstate->cs_hasnull,
							   gs_lstate->cs_values,
							   gs_lstate->cs_vl_dict,
							   gs_lstate->cs_extra_sz);
	return slot;
}

/*
 * gstoreExecForeignDelete
 */
static TupleTableSlot *
gstoreExecForeignDelete(EState *estate,
						ResultRelInfo *rinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	elog(ERROR, "Only Direct DELETE is supported");
}

/*
 * gstoreEndForeignModify
 */
static void
gstoreEndForeignModify(EState *estate,
					   ResultRelInfo *rrinfo)
{
	gstoreLoadState *gs_lstate = rrinfo->ri_FdwState;

	if (gs_lstate->nitems > 0)
	{
		/* writeout by 'pgstrom' format */
		gstore_fdw_writeout_pgstrom(rrinfo->ri_RelationDesc, gs_lstate);
	}
	if (gs_lstate->gcontext)
		PutGpuContext(gs_lstate->gcontext);
	MemoryContextDelete(gs_lstate->memcxt);
}

/*
 * gstoreBeginDirectModify
 */
static void
gstoreBeginDirectModify(ForeignScanState *node, int eflags)
{
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *rrinfo = estate->es_result_relation_info;
	Relation	frel = rrinfo->ri_RelationDesc;

	LockRelationOid(RelationGetRelid(frel), ShareUpdateExclusiveLock);
}

/*
 * gstoreIterateDirectModify
 */
static TupleTableSlot *
gstoreIterateDirectModify(ForeignScanState *node)
{
	EState		   *estate = node->ss.ps.state;
	ResultRelInfo  *rrinfo = estate->es_result_relation_info;
	Relation		frel = rrinfo->ri_RelationDesc;
	Snapshot		snapshot = estate->es_snapshot;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	Instrumentation *instr = node->ss.ps.instrument;
	GpuStoreChunk  *gs_chunk;
	GpuStoreMap	   *gs_map;

	SpinLockAcquire(&gstore_head->lock);
	gs_chunk = gstore_fdw_lookup_chunk(frel, snapshot);
	Assert(!TransactionIdIsValid(gs_chunk->xmax));
	gs_chunk->xmax = GetCurrentTransactionId();
	gs_chunk->cid = GetCurrentCommandId(true);
	estate->es_processed += gs_chunk->kds_nitems;
	if (instr)
		instr->tuplecount += gs_chunk->kds_nitems;
	gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);
	if (gs_map->dsm_seg)
	{
		dsm_detach(gs_map->dsm_seg);
		gs_map->dsm_seg = NULL;
	}
	pg_atomic_add_fetch_u32(&gstore_head->has_warm_chunks, 1);
	SpinLockRelease(&gstore_head->lock);

	return ExecClearTuple(slot);
}

/*
 * gstoreEndDirectModify
 */
static void
gstoreEndDirectModify(ForeignScanState *node)
{}

/*
 * gstoreXactCallbackPerChunk
 */
static bool
gstoreOnXactCallbackPerChunk(bool is_commit, GpuStoreChunk *gs_chunk,
							 TransactionId oldestXmin)
{
	if (TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
	{
		if (is_commit)
			gs_chunk->xmax_commited = true;
		else
			gs_chunk->xmax = InvalidTransactionId;
	}
	if (TransactionIdIsCurrentTransactionId(gs_chunk->xmin))
	{
		if (is_commit)
			gs_chunk->xmin_commited = true;
		else
		{
			gstore_fdw_release_chunk(gs_chunk);
			return false;
		}
	}

	if (TransactionIdIsValid(gs_chunk->xmax))
	{
		/* someone tried to delete chunk, but not commited yet */
		if (!gs_chunk->xmax_commited)
			return true;
		/*
		 * chunk deletion is commited, but some open transactions may
		 * still reference the chunk
		 */
		if (!TransactionIdPrecedes(gs_chunk->xmax, oldestXmin))
			return true;

		/* Otherwise, GpuStoreChunk can be released immediately */
		gstore_fdw_release_chunk(gs_chunk);
	}
	else if (TransactionIdIsNormal(gs_chunk->xmin))
	{
		/* someone tried to insert chunk, but not commited yet */
		if (!gs_chunk->xmin_commited)
			return true;
		/*
		 * chunk insertion is commited, but some open transaction may
		 * need MVCC style visibility control
		 */
		if (!TransactionIdPrecedes(gs_chunk->xmin, oldestXmin))
			return true;

		/* Otherwise, GpuStoreChunk can be visible to everybody */
		gs_chunk->xmin = FrozenTransactionId;
	}
	else if (!TransactionIdIsValid(gs_chunk->xmin))
	{
		/* GpuChunk insertion aborted */
		gstore_fdw_release_chunk(gs_chunk);
	}
	return false;
}

/*
 * gstoreXactCallback
 */
static void
gstoreXactCallback(XactEvent event, void *arg)
{
	TransactionId oldestXmin;
	bool		is_commit;
	bool		meet_warm_chunks = false;
	cl_int		i;

	if (event == XACT_EVENT_COMMIT)
		is_commit = true;
	else if (event == XACT_EVENT_ABORT)
		is_commit = false;
	else
		return;		/* do nothing */

//	elog(INFO, "gstoreXactCallback xid=%u", GetCurrentTransactionIdIfAny());

	if (pg_atomic_read_u32(&gstore_head->has_warm_chunks) == 0)
		return;

	oldestXmin = GetOldestXmin(NULL, true);
	SpinLockAcquire(&gstore_head->lock);
	for (i=0; i < GSTORE_CHUNK_HASH_NSLOTS; i++)
	{
		dlist_mutable_iter	iter;

		dlist_foreach_modify(iter, &gstore_head->active_chunks[i])
		{
			GpuStoreChunk  *gs_chunk
				= dlist_container(GpuStoreChunk, chain, iter.cur);

			if (gstoreOnXactCallbackPerChunk(is_commit, gs_chunk, oldestXmin))
				meet_warm_chunks = true;
		}
	}
	if (!meet_warm_chunks)
		pg_atomic_write_u32(&gstore_head->has_warm_chunks, 0);
	SpinLockRelease(&gstore_head->lock);
}

#if 0
/*
 * gstoreSubXactCallback - just for debug
 */
static void
gstoreSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	elog(INFO, "gstoreSubXactCallback event=%s my_xid=%u pr_xid=%u",
		 (event == SUBXACT_EVENT_START_SUB ? "StartSub" :
		  event == SUBXACT_EVENT_COMMIT_SUB ? "CommitSub" :
		  event == SUBXACT_EVENT_ABORT_SUB ? "AbortSub" :
		  event == SUBXACT_EVENT_PRE_COMMIT_SUB ? "PreCommitSub" : "???"),
		 mySubid, parentSubid);
}
#endif

/*
 * relation_is_gstore_fdw
 */
static bool
relation_is_gstore_fdw(Oid table_oid)
{
	HeapTuple	tup;
	Oid			fserv_oid;
	Oid			fdw_oid;
	Oid			handler_oid;
	PGFunction	handler_fn;
	Datum		datum;
	char	   *prosrc;
	char	   *probin;
	bool		isnull;
	/* it should be foreign table, of course */
	if (get_rel_relkind(table_oid) != RELKIND_FOREIGN_TABLE)
		return false;
	/* pull OID of foreign-server */
	tup = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign table %u", table_oid);
	fserv_oid = ((Form_pg_foreign_table) GETSTRUCT(tup))->ftserver;
	ReleaseSysCache(tup);

	/* pull OID of foreign-data-wrapper */
	tup = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(fserv_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "foreign server with OID %u does not exist", fserv_oid);
	fdw_oid = ((Form_pg_foreign_server) GETSTRUCT(tup))->srvfdw;
	ReleaseSysCache(tup);

	/* pull OID of FDW handler function */
	tup = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdw_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u",fdw_oid);
	handler_oid = ((Form_pg_foreign_data_wrapper) GETSTRUCT(tup))->fdwhandler;
	ReleaseSysCache(tup);
	/* pull library path & function name */
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", handler_oid);
	if (((Form_pg_proc) GETSTRUCT(tup))->prolang != ClanguageId)
		elog(ERROR, "FDW handler function is not written with C-language");

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", handler_oid);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", handler_oid);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);
	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_gstore_fdw_handler)
		return false;
	/* OK, it is GpuStore foreign table */
	return true;
}

/*
 * gstore_fdw_table_options
 */
static void
__gstore_fdw_table_options(List *options,
						  int *p_pinning,
						  int *p_format)
{
	ListCell   *lc;
	int			pinning = -1;
	int			format = -1;

	foreach (lc, options)
	{
		DefElem	   *defel = lfirst(lc);

		if (strcmp(defel->defname, "pinning") == 0)
		{
			if (pinning >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"pinning\" option appears twice")));
			pinning = atoi(defGetString(defel));
			if (pinning < 0 || pinning >= numDevAttrs)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"pinning\" on unavailable GPU device")));
		}
		else if (strcmp(defel->defname, "format") == 0)
		{
			char   *format_name;

			if (format >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"format\" option appears twice")));
			format_name = defGetString(defel);
			if (strcmp(format_name, "pgstrom") == 0 ||
				strcmp(format_name, "default") == 0)
				format = GSTORE_FDW_FORMAT__PGSTROM;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("gstore_fdw: format \"%s\" is unknown",
								format_name)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("gstore_fdw: unknown option \"%s\"",
							defel->defname)));
		}
	}
	if (pinning < 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("gstore_fdw: No pinning GPU device"),
				 errhint("use 'pinning' option to specify GPU device")));

	/* put default if not specified */
	if (format < 0)
		format = GSTORE_FDW_FORMAT__PGSTROM;

	/* result the results */
	if (p_pinning)
		*p_pinning = pinning;
	if (p_format)
		*p_format = format;
}

static void
gstore_fdw_table_options(Oid gstore_oid, int *p_pinning, int *p_format)
{
	HeapTuple	tup;
	Datum		datum;
	bool		isnull;
	List	   *options = NIL;

	tup = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(gstore_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign table %u", gstore_oid);
	datum = SysCacheGetAttr(FOREIGNTABLEREL, tup,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (!isnull)
		options = untransformRelOptions(datum);
	__gstore_fdw_table_options(options, p_pinning, p_format);
	ReleaseSysCache(tup);
}

/*
 * gstore_fdw_column_options
 */
static void
__gstore_fdw_column_options(List *options, int *p_compression)
{
	ListCell   *lc;
	char	   *temp;
	int			compression = -1;

	foreach (lc, options)
	{
		DefElem	   *defel = lfirst(lc);

		if (strcmp(defel->defname, "compression") == 0)
		{
			if (compression >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"compression\" option appears twice")));
			temp = defGetString(defel);
			if (pg_strcasecmp(temp, "none") == 0)
				compression = GSTORE_COMPRESSION__NONE;
			else if (pg_strcasecmp(temp, "pglz") == 0)
				compression = GSTORE_COMPRESSION__PGLZ;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unknown compression logic: %s", temp)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("gstore_fdw: unknown option \"%s\"",
							defel->defname)));
		}
	}
	/* set default, if no valid options were supplied */
	if (compression < 0)
		compression = GSTORE_COMPRESSION__NONE;

	/* set results */
	if (p_compression)
		*p_compression = compression;
}

static void
gstore_fdw_column_options(Oid gstore_oid, AttrNumber attnum,
						  int *p_compression)
{
	List	   *options = GetForeignColumnOptions(gstore_oid, attnum);

	__gstore_fdw_column_options(options, p_compression);
}

/*
 * gstore_fdw_post_drop
 */
static void
gstore_fdw_post_drop(Oid relid, AttrNumber attnum,
					 ObjectAccessDrop *arg)
{
	GpuStoreChunk *gs_chunk;
	pg_crc32	hash;
	int			index;
	dlist_iter	iter;

	INIT_LEGACY_CRC32(hash);
	COMP_LEGACY_CRC32(hash, &MyDatabaseId, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &relid, sizeof(Oid));
	FIN_LEGACY_CRC32(hash);
	index = hash % GSTORE_CHUNK_HASH_NSLOTS;

	SpinLockAcquire(&gstore_head->lock);
	dlist_foreach(iter, &gstore_head->active_chunks[index])
	{
		gs_chunk = dlist_container(GpuStoreChunk, chain, iter.cur);

		if (gs_chunk->hash == hash &&
			gs_chunk->database_oid == MyDatabaseId &&
			gs_chunk->table_oid == relid &&
			gs_chunk->xmax == InvalidTransactionId)
		{
			gs_chunk->xmax = GetCurrentTransactionId();
		}
	}
	pg_atomic_add_fetch_u32(&gstore_head->has_warm_chunks, 1);
	SpinLockRelease(&gstore_head->lock);
}

/*
 * gstore_fdw_object_access
 */
static void
gstore_fdw_object_access(ObjectAccessType access,
						 Oid classId,
						 Oid objectId,
						 int subId,
						 void *arg)
{
	if (object_access_next)
		(*object_access_next)(access, classId, objectId, subId, arg);

	switch (access)
	{
		case OAT_DROP:
			if (classId == RelationRelationId &&
				relation_is_gstore_fdw(objectId))
				gstore_fdw_post_drop(objectId, subId, arg);
			break;

		default:
			/* do nothing */
			break;
	}
}

/*
 * pgstrom_gstore_fdw_validator
 */
Datum
pgstrom_gstore_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	switch (catalog)
	{
		case ForeignTableRelationId:
			__gstore_fdw_table_options(options, NULL, NULL);
			break;

		case AttributeRelationId:
			__gstore_fdw_column_options(options, NULL);
			break;

		case ForeignServerRelationId:
			if (options)
				elog(ERROR, "gstore_fdw: no options are supported on SERVER");
			break;

		case ForeignDataWrapperRelationId:
			if (options)
				elog(ERROR, "gstore_fdw: no options are supported on FOREIGN DATA WRAPPER");
			break;

		default:
			elog(ERROR, "gstore_fdw: no options are supported on catalog %s",
				 get_rel_name(catalog));
			break;
	}
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_validator);

/*
 * pgstrom_gstore_fdw_handler
 */
Datum
pgstrom_gstore_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* functions for scanning foreign tables */
	routine->GetForeignRelSize	= gstoreGetForeignRelSize;
	routine->GetForeignPaths	= gstoreGetForeignPaths;
	routine->GetForeignPlan		= gstoreGetForeignPlan;
	routine->BeginForeignScan	= gstoreBeginForeignScan;
	routine->IterateForeignScan	= gstoreIterateForeignScan;
	routine->ReScanForeignScan	= gstoreReScanForeignScan;
	routine->EndForeignScan		= gstoreEndForeignScan;

	/* functions for INSERT/DELETE foreign tables */
	routine->IsForeignRelUpdatable = gstoreIsForeignRelUpdatable;

	routine->PlanForeignModify	= gstorePlanForeignModify;
	routine->BeginForeignModify	= gstoreBeginForeignModify;
	routine->ExecForeignInsert	= gstoreExecForeignInsert;
	routine->ExecForeignDelete	= gstoreExecForeignDelete;
	routine->EndForeignModify	= gstoreEndForeignModify;

	routine->PlanDirectModify	= gstorePlanDirectModify;
    routine->BeginDirectModify	= gstoreBeginDirectModify;
    routine->IterateDirectModify = gstoreIterateDirectModify;
    routine->EndDirectModify	= gstoreEndDirectModify;

	PG_RETURN_POINTER(routine);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_handler);

/*
 * pgstrom_reggstore_in
 */
Datum
pgstrom_reggstore_in(PG_FUNCTION_ARGS)
{
	Datum	datum = regclassin(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_in);

/*
 * pgstrom_reggstore_out
 */
Datum
pgstrom_reggstore_out(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	return regclassout(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_out);

/*
 * pgstrom_reggstore_recv
 */
Datum
pgstrom_reggstore_recv(PG_FUNCTION_ARGS)
{
	/* exactly the same as oidrecv, so share code */
	Datum	datum = oidrecv(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_recv);

/*
 * pgstrom_reggstore_send
 */
Datum
pgstrom_reggstore_send(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_send);

/*
 * get_reggstore_type_oid
 */
Oid
get_reggstore_type_oid(void)
{
	if (!OidIsValid(reggstore_type_oid))
	{
		Oid		temp_oid;

		temp_oid = GetSysCacheOid2(TYPENAMENSP,
								   CStringGetDatum("reggstore"),
								   ObjectIdGetDatum(PG_PUBLIC_NAMESPACE));
		if (!OidIsValid(temp_oid) ||
			!type_is_reggstore(temp_oid))
			elog(ERROR, "type \"reggstore\" is not defined");
		reggstore_type_oid = temp_oid;
	}
	return reggstore_type_oid;
}

/*
 * reset_reggstore_type_oid
 */
static void
reset_reggstore_type_oid(Datum arg, int cacheid, uint32 hashvalue)
{
	reggstore_type_oid = InvalidOid;
}

/*
 * pgstrom_gstore_export_ipchandle
 */
Datum
pgstrom_gstore_export_ipchandle(PG_FUNCTION_ARGS)
{
	Oid				gstore_oid = PG_GETARG_OID(0);
	Relation		frel;
	cl_int			pinning;
	GpuStoreChunk  *gs_chunk;
	char		   *result;

	if (!relation_is_gstore_fdw(gstore_oid))
		elog(ERROR, "relation %u is not gstore_fdw foreign table",
			 gstore_oid);

	frel = heap_open(gstore_oid, AccessShareLock);
	gstore_fdw_table_options(gstore_oid, &pinning, NULL);
	if (pinning < 0)
		elog(ERROR, "gstore_fdw: foreign table \"%s\" is not pinned on a particular GPU devices",
			 RelationGetRelationName(frel));
	if (pinning >= numDevAttrs)
		elog(ERROR, "gstore_fdw: foreign table \"%s\" is not pinned on a valid GPU device",
			 RelationGetRelationName(frel));

	gs_chunk = gstore_fdw_lookup_chunk(frel, GetActiveSnapshot());
	if (!gs_chunk)
		PG_RETURN_NULL();

	result = palloc(VARHDRSZ + sizeof(CUipcMemHandle));
	memcpy(result + VARHDRSZ, &gs_chunk->ipc_mhandle, sizeof(CUipcMemHandle));
	SET_VARSIZE(result, VARHDRSZ + sizeof(CUipcMemHandle));

	heap_close(frel, NoLock);

	PG_RETURN_POINTER(result);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_export_ipchandle);

/*
 * pgstrom_lo_export_ipchandle
 */
Datum
pgstrom_lo_export_ipchandle(PG_FUNCTION_ARGS)
{
	elog(ERROR, "not implemented yet");
	PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(pgstrom_lo_export_ipchandle);

/*
 * pgstrom_lo_import_ipchandle
 */
Datum
pgstrom_lo_import_ipchandle(PG_FUNCTION_ARGS)
{
	elog(ERROR, "not implemented yet");
	PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(pgstrom_lo_import_ipchandle);

/*
 * type_is_reggstore
 */
bool
type_is_reggstore(Oid type_oid)
{
	Oid			typinput;
	HeapTuple	tup;
	char	   *prosrc;
	char	   *probin;
	Datum		datum;
	bool		isnull;
	PGFunction	handler_fn;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type_oid);
	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	ReleaseSysCache(tup);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(typinput));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", typinput);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", typinput);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", typinput);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);

	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_reggstore_in)
		return false;
	/* ok, it is reggstore type */
	return true;
}

/*
 * load_normal_gstore_fdw
 */
static CUdeviceptr
load_normal_gstore_fdw(GpuContext *gcontext, Relation frel)
{
	GpuStoreChunk  *gs_chunk;
	kern_data_store *kds_src;
	kern_data_store *kds_dst;
	CUresult		rc;
	CUdeviceptr		m_gstore;

	gs_chunk = gstore_fdw_lookup_chunk(frel, GetActiveSnapshot());
	if (!gs_chunk)
		return 0UL;		/* empty GpuStore */

	/* allocation of managed memory */
	rc = gpuMemAllocManagedRaw(gcontext,
							   &m_gstore,
							   gs_chunk->kds_length,
							   CU_MEM_ATTACH_GLOBAL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuMemAllocManagedRaw: %s", errorText(rc));
	kds_dst = (kern_data_store *)m_gstore;
	kds_src = gstore_fdw_mapped_chunk(gs_chunk);
	Assert(kds_src->length == gs_chunk->kds_length);
	memcpy(kds_dst, kds_src, gs_chunk->kds_length);

	return m_gstore;
}

/*
 * load_pinned_gstore_fdw
 */
static CUdeviceptr
load_pinned_gstore_fdw(GpuContext *gcontext, Relation frel)
{
	GpuStoreChunk  *gs_chunk;
	CUdeviceptr		m_deviceptr;
	CUresult		rc;

	gs_chunk = gstore_fdw_lookup_chunk(frel, GetActiveSnapshot());
	if (!gs_chunk)
		return 0UL;		/* empty GpuStore */
	if (gs_chunk->cuda_dindex != gcontext->cuda_dindex)
		elog(ERROR, "GPU context works on the different device where '%s' foreign table is pinned", RelationGetRelationName(frel));

	rc = cuCtxPushCurrent(gcontext->cuda_context);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuCtxPushCurrent: %s", errorText(rc));

	rc = gpuIpcOpenMemHandle(gcontext,
							 &m_deviceptr,
							 gs_chunk->ipc_mhandle,
							 CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuIpcOpenMemHandle: %s", errorText(rc));

	rc = cuCtxPopCurrent(NULL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuCtxPopCurrent: %s", errorText(rc));

	return m_deviceptr;
}

/*
 * gstore_fdw_preferable_device
 */
int
gstore_fdw_preferable_device(FunctionCallInfo fcinfo)
{
	FmgrInfo   *flinfo = fcinfo->flinfo;
	HeapTuple	protup;
	oidvector  *proargtypes;
	cl_int		i, cuda_dindex = -1;

	protup = SearchSysCache1(PROCOID, ObjectIdGetDatum(flinfo->fn_oid));
	if (!HeapTupleIsValid(protup))
		elog(ERROR, "cache lookup failed function %u", flinfo->fn_oid);
	proargtypes = &((Form_pg_proc)GETSTRUCT(protup))->proargtypes;
	for (i=0; i < proargtypes->dim1; i++)
	{
		Oid		gstore_oid;
		int		pinning;

		if (proargtypes->values[i] != REGGSTOREOID)
			continue;
		gstore_oid = DatumGetObjectId(fcinfo->arg[i]);
		if (!relation_is_gstore_fdw(gstore_oid))
			elog(ERROR, "relation %u is not gstore_fdw foreign table",
				 gstore_oid);
		gstore_fdw_table_options(gstore_oid, &pinning, NULL);
		if (pinning >= 0)
		{
			Assert(pinning < numDevAttrs);
			if (cuda_dindex < 0)
				cuda_dindex = pinning;
			else if (cuda_dindex != pinning)
				elog(ERROR, "function %s: called with gstore_fdw foreign tables in different location",
					 format_procedure(flinfo->fn_oid));
		}
	}
	ReleaseSysCache(protup);

	return cuda_dindex;
}

/*
 * gstore_fdw_load_function_args
 */
void
gstore_fdw_load_function_args(GpuContext *gcontext,
							  FunctionCallInfo fcinfo,
							  List **p_gstore_oid_list,
							  List **p_gstore_devptr_list,
							  List **p_gstore_dindex_list)
{
	FmgrInfo   *flinfo = fcinfo->flinfo;
	HeapTuple	protup;
	oidvector  *proargtypes;
	List	   *gstore_oid_list = NIL;
	List	   *gstore_devptr_list = NIL;
	List	   *gstore_dindex_list = NIL;
	ListCell   *lc;
	int			i;

	protup = SearchSysCache1(PROCOID, ObjectIdGetDatum(flinfo->fn_oid));
	if (!HeapTupleIsValid(protup))
		elog(ERROR, "cache lookup failed function %u", flinfo->fn_oid);
	proargtypes = &((Form_pg_proc)GETSTRUCT(protup))->proargtypes;
	for (i=0; i < proargtypes->dim1; i++)
	{
		Relation	frel;
		Oid			gstore_oid;
		int			pinning;
		CUdeviceptr	m_deviceptr;

		if (proargtypes->values[i] != REGGSTOREOID)
			continue;
		gstore_oid = DatumGetObjectId(fcinfo->arg[i]);
		/* already loaded? */
		foreach (lc, gstore_oid_list)
		{
			if (gstore_oid == lfirst_oid(lc))
				break;
		}
		if (lc != NULL)
			continue;

		if (!relation_is_gstore_fdw(gstore_oid))
			elog(ERROR, "relation %u is not gstore_fdw foreign table",
				 gstore_oid);

		gstore_fdw_table_options(gstore_oid, &pinning, NULL);
		if (pinning >= 0 && gcontext->cuda_dindex != pinning)
			elog(ERROR, "unable to load gstore_fdw foreign table \"%s\" on the GPU device %d; GpuContext is assigned on the device %d",
				 get_rel_name(gstore_oid), pinning, gcontext->cuda_dindex);

		frel = heap_open(gstore_oid, AccessShareLock);
		if (pinning < 0)
			m_deviceptr = load_normal_gstore_fdw(gcontext, frel);
		else
			m_deviceptr = load_pinned_gstore_fdw(gcontext, frel);
		heap_close(frel, NoLock);

		gstore_oid_list = lappend_oid(gstore_oid_list, gstore_oid);
		gstore_devptr_list = lappend(gstore_devptr_list,
									 (void *)m_deviceptr);
		gstore_dindex_list = lappend_int(gstore_dindex_list, pinning);
	}
	ReleaseSysCache(protup);
	*p_gstore_oid_list = gstore_oid_list;
	*p_gstore_devptr_list = gstore_devptr_list;
	*p_gstore_dindex_list = gstore_dindex_list;
}

/*
 * pgstrom_gstore_fdw_format
 */
Datum
pgstrom_gstore_fdw_format(PG_FUNCTION_ARGS)
{
	Oid				gstore_oid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(gstore_oid))
		PG_RETURN_NULL();
	/* currently, only 'pgstrom' is the supported format */
	PG_RETURN_TEXT_P(cstring_to_text("pgstrom"));
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_format);

/*
 * pgstrom_gstore_fdw_height
 */
Datum
pgstrom_gstore_fdw_height(PG_FUNCTION_ARGS)
{
	Oid				gstore_oid = PG_GETARG_OID(0);
	Relation		frel;
	GpuStoreChunk  *gs_chunk;
	int64			retval = 0;

	if (!relation_is_gstore_fdw(gstore_oid))
		PG_RETURN_NULL();

	frel = heap_open(gstore_oid, AccessShareLock);
	gs_chunk = gstore_fdw_lookup_chunk(frel, GetActiveSnapshot());
	if (gs_chunk)
		retval = gs_chunk->kds_nitems;
	heap_close(frel, NoLock);

	PG_RETURN_INT64(retval);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_height);

/*
 * pgstrom_gstore_fdw_width
 */
Datum
pgstrom_gstore_fdw_width(PG_FUNCTION_ARGS)
{
	Oid				gstore_oid = PG_GETARG_OID(0);
	Relation		frel;
	int64			retval = 0;

	if (!relation_is_gstore_fdw(gstore_oid))
		PG_RETURN_NULL();

	frel = heap_open(gstore_oid, AccessShareLock);
	retval = RelationGetNumberOfAttributes(frel);
	heap_close(frel, NoLock);

	PG_RETURN_INT64(retval);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_width);

/*
 * pgstrom_gstore_fdw_rawsize
 */
Datum
pgstrom_gstore_fdw_rawsize(PG_FUNCTION_ARGS)
{
	Oid				gstore_oid = PG_GETARG_OID(0);
	Relation		frel;
	GpuStoreChunk  *gs_chunk;
	int64			retval = 0;

	if (!relation_is_gstore_fdw(gstore_oid))
		PG_RETURN_NULL();

	frel = heap_open(gstore_oid, AccessShareLock);
	gs_chunk = gstore_fdw_lookup_chunk(frel, GetActiveSnapshot());
	if (gs_chunk)
		retval = gs_chunk->kds_length;
	heap_close(frel, NoLock);

	PG_RETURN_INT64(retval);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_rawsize);

/*
 * pgstrom_startup_gstore_fdw
 */
static void
pgstrom_startup_gstore_fdw(void)
{
	bool		found;
	int			i;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	gstore_head = ShmemInitStruct("GPU Store Control Structure",
								  offsetof(GpuStoreHead,
										   gs_chunks[gstore_max_relations]),
								  &found);
	if (found)
		elog(ERROR, "Bug? shared memory for gstore_fdw already built");
	gstore_maps = calloc(gstore_max_relations, sizeof(GpuStoreMap));
	if (!gstore_maps)
		elog(ERROR, "out of memory");
	SpinLockInit(&gstore_head->lock);
	dlist_init(&gstore_head->free_chunks);
	for (i=0; i < GSTORE_CHUNK_HASH_NSLOTS; i++)
		dlist_init(&gstore_head->active_chunks[i]);
	for (i=0; i < gstore_max_relations; i++)
	{
		GpuStoreChunk  *gs_chunk = &gstore_head->gs_chunks[i];

		memset(gs_chunk, 0, sizeof(GpuStoreChunk));
		gs_chunk->dsm_handle = UINT_MAX;

		dlist_push_tail(&gstore_head->free_chunks, &gs_chunk->chain);
	}
}

/*
 * pgstrom_init_gstore_fdw
 */
void
pgstrom_init_gstore_fdw(void)
{
	DefineCustomIntVariable("pg_strom.gstore_max_relations",
							"maximum number of gstore_fdw relations",
							NULL,
							&gstore_max_relations,
							100,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	RequestAddinShmemSpace(MAXALIGN(offsetof(GpuStoreHead,
											gs_chunks[gstore_max_relations])));
	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_gstore_fdw;

	object_access_next = object_access_hook;
	object_access_hook = gstore_fdw_object_access;

	RegisterXactCallback(gstoreXactCallback, NULL);
	//RegisterSubXactCallback(gstoreSubXactCallback, NULL);

	/* invalidation of reggstore_oid variable */
	CacheRegisterSyscacheCallback(TYPEOID, reset_reggstore_type_oid, 0);
}
