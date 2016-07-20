/*-------------------------------------------------------------------------
 *
 * memstat.c
 *	  Explore memry stats
 *
 * Copyright (c) 2015-2016, Teodor Sigaev
 *
 *-------------------------------------------------------------------------
 */
#include <sys/time.h>

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <access/htup_details.h>
#include <access/twophase.h>
#include <optimizer/plancat.h>
#include <postmaster/autovacuum.h>
#include <storage/ipc.h>
#include <storage/proc.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/memutils.h>

PG_MODULE_MAGIC;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static double	prevTic = 0.0;
static int		ticPeriod = 10;

static bool
checkTick()
{
	struct	timeval	t;
	double			t_secs;

	gettimeofday(&t, NULL);

	t_secs = (double)t.tv_sec + t.tv_usec * 1e-6;

	if (t_secs - prevTic >= ticPeriod)
	{
		prevTic = t_secs;
		return true;
	}
	else
	{
		return false;
	}
}

typedef struct MemoryContextIteratorState {
	MemoryContext			context;
	int						level;
} MemoryContextIteratorState;

#define N_MC_STAT		1024

/*
 * see InitializeMaxBackends(), this module should be last in
 * shared_preload_libraries list !
 */
#define PROCARRAY_MAXPROCS  (MaxConnections + autovacuum_max_workers + 1 + \
							 max_worker_processes + max_prepared_xacts)

typedef struct MemoryContextStat {
	NameData				name;
	int32					level;
	MemoryContextCounters	stat;
} MemoryContextStat;


typedef struct BackendMemoryStat {
	LWLock				*lock;
	int32				pid;
	int32				nContext;
	MemoryContextStat	stats[FLEXIBLE_ARRAY_MEMBER];
} BackendMemoryStat;

static BackendMemoryStat *memstats = NULL;
static int32 MyBackendProcNo = -1;

#define BMSSIZE		(MAXALIGN(offsetof(BackendMemoryStat, stats)) + \
					 MAXALIGN(N_MC_STAT * sizeof(MemoryContextStat)))
#define NthBMS(n)	((BackendMemoryStat*)(((char*)memstats) + (n) * BMSSIZE))

static void
iterateMemoryContext(MemoryContextIteratorState *state)
{
	MemoryContext	context = state->context;

	AssertArg(MemoryContextIsValid(context));

	if (context->firstchild)
	{
		/* perfor first-depth search */
		state->context = context->firstchild;
		state->level++;
	}
	else if (context->nextchild)
	{
		/* goto next child if current context doesn't have a child */
		state->context = context->nextchild;
	}
	else if (context->parent)
	{
		/*
		 * walk up on tree to first parent which has a next child,
		 * that parent context was already visited
		 */
		while(context)
		{
			context = context->parent;
			state->level--;

			if (context == NULL)
			{
				/* we visited the whole context's tree */
				state->context = NULL;
				break;
			}
			else if (context->nextchild)
			{
				state->context = context->nextchild;
				break;
			}
		}
	}
}

static void
getMemoryContextStat(MemoryContext context, MemoryContextCounters *stat)
{
	AssertArg(MemoryContextIsValid(context));

	/* Examine the context itself */
	memset(stat, 0, sizeof(*stat));
	(*context->methods->stats) (context, 0, false, stat);
}

/*
 * Get list of used memory in bytes.
 */
PG_FUNCTION_INFO_V1(get_local_memory_stats);
Datum
get_local_memory_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext		*funcctx;
	MemoryContextIteratorState	*state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc				tupdesc;
		MemoryContext			oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		state = palloc0(sizeof(*state));
		state->context = TopMemoryContext;
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (MemoryContextIteratorState*) funcctx->user_fctx;
	if (state && state->context)
	{
		Datum					values[6];
		bool					nulls[6];
		HeapTuple				tuple;
		MemoryContextCounters	stat;

		getMemoryContextStat(state->context, &stat);
		memset(nulls, 0, sizeof(nulls));

		/* Fill data */
		values[0] = PointerGetDatum(cstring_to_text(state->context->name));
		values[1] = Int32GetDatum(state->level);
		values[2] = Int64GetDatum(stat.nblocks);
		values[3] = Int64GetDatum(stat.freechunks);
		values[4] = Int64GetDatum(stat.totalspace);
		values[5] = Int64GetDatum(stat.freespace);

		/* Data are ready */
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* go next context */
		iterateMemoryContext(state);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

static Size
getMemstatSize()
{
	return  PROCARRAY_MAXPROCS * BMSSIZE;
}

static void
allocShmem(void)
{
	bool	found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	memstats = ShmemInitStruct("contrib/memstat",
							   getMemstatSize(),
							   &found);

	if (!found)
	{
		int32	i;
		LWLockPadded	*LockPadded = GetNamedLWLockTranche("memstat");

		for(i=0; i<PROCARRAY_MAXPROCS; i++)
		{
			NthBMS(i)->pid = -1;
			NthBMS(i)->lock = &(LockPadded[i].lock);
		}
	}

	LWLockRelease(AddinShmemInitLock);
}

static void
cleanupMyStat(int code, Datum arg)
{
	if (MyBackendProcNo >= 0)
	{
		BackendMemoryStat			*myStat;

		Assert(MyBackendProcNo < PROCARRAY_MAXPROCS);

		myStat = NthBMS(MyBackendProcNo);

		/*
		 * Do not acquire lock here because it could be called somewhere
		 * at_exit call and undefined amount of internal structures is already
		 * in inconsistent state, the possible risk is that reader could 
		 * get inconsistent data. C'est la vie.
		 */
		myStat->pid = -1;
		MyBackendProcNo = -1;
	}
}

static void
collectLocalMemoryStats(QueryDesc *queryDesc, int eflags)
{
	BackendMemoryStat			*myStat;
	MemoryContextIteratorState	state;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (MyBackendProcNo < 0)
	{
		on_proc_exit(cleanupMyStat, 0);
		MyBackendProcNo = MyProc->pgprocno;
		Assert(MyBackendProcNo >= 0);
	}

	if (checkTick() == false)
		return;

	Assert(MyBackendProcNo < PROCARRAY_MAXPROCS);
	myStat = NthBMS(MyBackendProcNo);

	/*
	 * do not wait if reader currently locks our slot
	 */
	if (LWLockConditionalAcquire(myStat->lock, LW_EXCLUSIVE) == false)
		return;

	myStat->pid = MyProc->pid;
	myStat->nContext = 0;
	state.context = TopMemoryContext;
	state.level = 0;

	/*
	 * walk through all memory context and fill stat table in shared memory
	 */
	do {
		MemoryContextStat	*mcs = myStat->stats + myStat->nContext;
		int					namelen = strlen(state.context->name);

		if (namelen > NAMEDATALEN - 1)
			namelen = NAMEDATALEN - 1;
		memcpy(mcs->name.data, state.context->name, namelen);
		mcs->name.data[namelen] = '\0';

		mcs->level = state.level;

		getMemoryContextStat(state.context, &mcs->stat);
		myStat->nContext++;

		iterateMemoryContext(&state);
	} while (state.context && myStat->nContext < N_MC_STAT);

	LWLockRelease(myStat->lock);
}

typedef struct InstanceState {
	int32				iBackend;
	int32				iContext;
	BackendMemoryStat	*stat;
} InstanceState;

static bool
copyBackendMemoryStat(InstanceState *state, int32 iBackend)
{
	for(; iBackend < PROCARRAY_MAXPROCS; iBackend++)
	{
		BackendMemoryStat	*BackendStat = NthBMS(iBackend);

		LWLockAcquire(BackendStat->lock, LW_SHARED);
		if (BackendStat->pid < 0)
		{
			/* this slot isn't used */
			LWLockRelease(BackendStat->lock);
			continue;
		}

		memcpy(state->stat, BackendStat, BMSSIZE);
		state->stat->lock = NULL; /* just to be sure */
		state->iBackend = iBackend;
		state->iContext = 0;
		LWLockRelease(BackendStat->lock);

		return true;
	}

	return false;
}
/*
 * Get list of used memory in whole instance in bytes.
 */
PG_FUNCTION_INFO_V1(get_instance_memory_stats);
Datum
get_instance_memory_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext			*funcctx;
	InstanceState			*state;
	Datum					values[7];
	bool					nulls[7];
	HeapTuple				tuple;
	MemoryContextStat		*ContextStat;

	if (MyBackendProcNo < 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("memory stat collection isn't worked"),
				 errhint("add memstat to shared_preload_libraries")));

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc				tupdesc;
		MemoryContext			oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		state = palloc0(sizeof(*state));
		state->iBackend = 0;
		/*
		 * we make a copy of backend stat struct to prevent lossing stat
		 * on the fly if that backend will exit while we are printing it
		 */
		state->stat = palloc(BMSSIZE);
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);

		/* at least our backend will be in list */
		copyBackendMemoryStat(state, 0);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (InstanceState*) funcctx->user_fctx;

	if (state->iContext >= state->stat->nContext)
	{
		/* got to the text slot */
		if (copyBackendMemoryStat(state, state->iBackend + 1) == false)
			SRF_RETURN_DONE(funcctx);
	}

	ContextStat = state->stat->stats + state->iContext;

	memset(nulls, 0, sizeof(nulls));

	/* Fill data */
	values[0] = Int32GetDatum(state->stat->pid);
	values[1] = PointerGetDatum(cstring_to_text(ContextStat->name.data));
	values[2] = Int32GetDatum(ContextStat->level);
	values[3] = Int64GetDatum(ContextStat->stat.nblocks);
	values[4] = Int64GetDatum(ContextStat->stat.freechunks);
	values[5] = Int64GetDatum(ContextStat->stat.totalspace);
	values[6] = Int64GetDatum(ContextStat->stat.freespace);

	/* Data are ready */
	tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

	/* go next context */
	state->iContext++;

	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}

extern void _PG_init(void);
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	RequestAddinShmemSpace(getMemstatSize());
	RequestNamedLWLockTranche("memstat", PROCARRAY_MAXPROCS);

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = collectLocalMemoryStats;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = allocShmem;

	DefineCustomIntVariable("memstat.period",
							"Sets period to collect memory statistics",
							"zero means collecting after each query",
							&ticPeriod,
							10,
							0, 60*60*24*31,
							PGC_SUSET, GUC_UNIT_S,
							NULL, NULL, NULL);
}

extern void _PG_fini(void);
void
_PG_fini(void)
{
	ExecutorStart_hook = prev_ExecutorStart;
	shmem_startup_hook = prev_shmem_startup_hook;
}

