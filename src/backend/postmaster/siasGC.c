/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "access/hio.h"
#include "access/htup.h"
#include "access/sias.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/siasGC.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


static bool GcRegion(Relation rel, DirtyRegionData *drd);
NON_EXEC_STATIC void SiasGcWorkerMain();
static void PrepareTerminateSiasGcWorker();
static void TerminateSiasGcWorker(bool replaceProcess);



static SiasGcShmemStruct *SiasGcShmem;
static char gcRunDurationFileDir[128];

/* Flag to tell if we are in an SiasGc process */
static bool am_SiasGc_worker = false;

RelFileNode rfn;
Relation rel = NULL;
DirtyRegionData *drd;
HASH_SEQ_STATUS searchStatus;
bool servingRequest = false;
bool interruptable = true;
bool terminate = false;



int
StartSiasGcWorker(void)
{
	pid_t		worker_pid;

	switch ((worker_pid = fork_process()))
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork Sias garbage collection worker process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			SiasGcWorkerMain();
			break;
#endif
		default:
			return (int) worker_pid;
	}

	/* shouldn't get here */
	return 0;
}

NON_EXEC_STATIC void
SiasGcWorkerMain()
{
	sigjmp_buf	local_sigjmp_buf;
	Oid			dbid;


	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_SiasGc_worker = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* record Start Time for logging */
	MyStartTime = time(NULL);

	/* Identify myself via ps */
	init_ps_display("Sias garbage collection worker process", "", "", "");

	SetProcessingMode(InitProcessing);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.	(SiasGc probably never has any
	 * child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes that
	 * happen during a single daemon iteration, so we can ignore SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);

	/*
	 * WARNING: The behavior in response to signals has only been adjusted
	 * 			as far as it was necessary to shut down correctly.
	 */

	/*
	 * SIGTERM means abort and exit cleanly, and SIGQUIT means abandon ship.
	 */
//	pqsignal(SIGINT, TerminateSiasGcWorker);
	pqsignal(SIGTERM, PrepareTerminateSiasGcWorker);
	pqsignal(SIGQUIT, PrepareTerminateSiasGcWorker);

	pqsignal(SIGFPE, FloatExceptionHandler);

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.	Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	SetProcessingMode(NormalProcessing);





	bool found;
	uint32 workerId;
	bool workToDo;
	RequestQueue *requestQueuePtr;
	int consecutiveIdlePasses;
	TransactionId cutOffTxId;
	FILE *file;
	GarbageCollectionInfo *gci;
	int i, n;
	RegionNumber regionNum;
	int numGcRuns;


	SpinLockAcquire(&(SiasGcShmem->numWorkersMutex));
	workerId = SiasGcShmem->numWorkers++;
	SpinLockRelease(&(SiasGcShmem->numWorkersMutex));

	requestQueuePtr = &SiasGcShmem->requestQueue;
	consecutiveIdlePasses = 0;
	numGcRuns = 0;

	printf("new SiasGC process running - PID %i\n", MyProcPid);


	while (consecutiveIdlePasses < SIAS_GC_MAX_IDLE_PASSES)
	{
		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		workToDo = true;

		while (workToDo)
		{
			workToDo = false;

			/*
			 * See if there is any relation, that needs to be
			 * GC'ed, but isn't assigned to a SiasGc worker yet.
			 */
			interruptable = false;
			SpinLockAcquire(&(requestQueuePtr->mutex));
			if (requestQueuePtr->numElements > 0)
			{
				/*
				 * This worker process can only work on relations,
				 * that belong to the same database it's connected to.
				 */
				if (requestQueuePtr->elements[requestQueuePtr->head].dbNode == MyDatabaseId
						|| MyDatabaseId == InvalidOid)
				{
					rfn = requestQueuePtr->elements[requestQueuePtr->head];
					MemSet(&requestQueuePtr->elements[requestQueuePtr->head], 0, sizeof(RelFileNode));

					if (++requestQueuePtr->head == SIAS_MAX_RELATIONS)
					{
						requestQueuePtr->head = 0;
					}
					if (--requestQueuePtr->numElements == 0)
					{
						requestQueuePtr->head = -1;
						requestQueuePtr->tail = -1;
					}

					servingRequest = true;
					workToDo = true;
				}
			}
			SpinLockRelease(&(requestQueuePtr->mutex));

			interruptable = true;
			if (terminate)
			{
				TerminateSiasGcWorker(false);
			}

			if (workToDo)
			{
				if (MyDatabaseId == InvalidOid)
				{
					char		dbname[NAMEDATALEN];

					SetProcessingMode(InitProcessing);
					InitPostgres(NULL, rfn.dbNode, NULL, dbname);
					SetProcessingMode(NormalProcessing);
					set_ps_display(dbname, false);

					char resOwnerName[(8 * sizeof(workerId) + 2) / 3 + 15];
					sprintf(resOwnerName, "SiasGcResOwner %u", workerId);
					CurrentResourceOwner = ResourceOwnerCreate(NULL, resOwnerName);
				}

				drd = NULL;
				rel = relation_open(rfn.relNode, AccessShareLock);

				rel->rd_gci->hasSiasGcWorker = true;
				gci = rel->rd_gci;

				while (workToDo)
				{
					workToDo = false;

					if (!drd)
					{
						hash_seq_init(&searchStatus, gci->dirtyRegions);
					}

					drd = (DirtyRegionData*) hash_seq_search(&searchStatus);

					cutOffTxId = GetOldestXmin(true, true);

					while (!workToDo && drd)
					{
						if (drd->prevMaxUpdaterXmin < cutOffTxId)
						{
							regionNum = GetBlocksRegionNumber(drd->firstBlock);
							SpinLockAcquire(&(gci->regionGcCountersMutexes[regionNum]));
							if (gci->newlyUpdatedData[regionNum] <= SIAS_MAX_NEWLY_UPDATED_DATA
									|| gci->maxUpdaterXmin[regionNum] < cutOffTxId)
							{
								workToDo = true;

#ifdef SIAS_GC_TEST
								file = fopen("/home/richard/Documents/file1.txt", "a");
								fprintf(file, "GcRegion %u %u allowed %u %u\n", rel->rd_id, drd->firstBlock, drd->prevMaxUpdaterXmin, cutOffTxId);
								fclose(file);
#endif
							}
							else
							{
								/*
								 * There could be too many updated tuples,
								 * which would cause hot writes during GC
								 * if they are still visible.
								 *
								 * Wait at least until all of these tuples
								 * will be invisible.
								 */
								drd->prevMaxUpdaterXmin = gci->maxUpdaterXmin[regionNum];
								gci->newlyUpdatedData[regionNum] = 0;

#ifdef SIAS_GC_TEST
								file = fopen("/home/richard/Documents/file1.txt", "a");
								fprintf(file, "GcRegion %u %u delay 2\n", rel->rd_id, drd->firstBlock);
								fclose(file);
#endif
							}
							SpinLockRelease(&(gci->regionGcCountersMutexes[regionNum]));
						}
						else
						{
#ifdef SIAS_GC_TEST
							file = fopen("/home/richard/Documents/file1.txt", "a");
							fprintf(file, "GcRegion %u %u disallowed %u %u\n", rel->rd_id, drd->firstBlock, drd->prevMaxUpdaterXmin, cutOffTxId);
							fclose(file);
#endif
						}

						if (!workToDo)
						{
							drd = (DirtyRegionData*) hash_seq_search(&searchStatus);
						}
					}

					if (workToDo)
					{
						consecutiveIdlePasses = 0;
						numGcRuns++;

						if (GcRegion(rel, drd))
						{
							interruptable = false;

							SpinLockAcquire(&(gci->dirtyRegionsMutex));
							hash_search(gci->dirtyRegions, &drd->firstBlock, HASH_REMOVE, NULL);
							if (--gci->numDirtyRegions == 0)
							{
								break;
							}
							SpinLockRelease(&(gci->dirtyRegionsMutex));

							interruptable = true;
							if (terminate)
							{
								TerminateSiasGcWorker(false);
							}
						}

						/*
						 * Don't let a GC worker process do too
						 * much work. Replace it with a new one.
						 *
						 * This is a workaround to limit the main
						 * memory usage of individual GC worker
						 * processes, which seems to increase
						 * with each GcRegion(..) call.
						 */
						if (numGcRuns >= 50)
						{
							TerminateSiasGcWorker(true);
						}
					}
					else
					{
						/*
						 * There are dirty regions, but none of
						 * them is ready to be GC'ed.
						 *
						 * Wait a moment and try again.
						 */
						pg_usleep(SIAS_GC_SLEEP_INTERVAL*3);

						workToDo = true;
						if (++consecutiveIdlePasses == SIAS_GC_MAX_IDLE_PASSES)
						{
							elog(WARNING, "There are dirty regions in relation %u, but they haven't become GC'able yet.", rel->rd_id);
							consecutiveIdlePasses = 0;
						}
					}
				}

				/*
				 * There are no more regions to GC in this relation.
				 */

				if (drd)
				{
					hash_seq_term(&searchStatus);
				}

				/*
				 * It's important to switch hasSiasGcWorker
				 * while holding dirtyRegionsMutex in order
				 * to comply with the GC request protocol,
				 * which guarantees that there can be at most
				 * one worker per relation and a requester
				 * registers a GC request iff it's necessary.
				 */
				rel->rd_gci->hasSiasGcWorker = false;
				servingRequest = false;
				SpinLockRelease(&(gci->dirtyRegionsMutex));

				relation_close(rel, AccessShareLock);
				rel = NULL;

				interruptable = true;
				if (terminate)
				{
					TerminateSiasGcWorker(false);
				}

				/*
				 * Don't let a GC worker process do too
				 * much work. Replace it with a new one.
				 *
				 * This is a workaround to limit the main
				 * memory usage of individual GC worker
				 * processes, which seems to increase
				 * with each GcRegion(..) call.
				 */
				if (numGcRuns >= 50)
				{
					TerminateSiasGcWorker(true);
				}

				workToDo = true;
			}
			else
			{
				if (++consecutiveIdlePasses == SIAS_GC_MAX_IDLE_PASSES)
				{
					SpinLockAcquire(&(SiasGcShmem->numWorkersMutex));

					/*
					 * This is the only SiasGc worker process.
					 *
					 * There should always be at least one running,
					 * even if there's nothing to do in the moment.
					 * (The current implementation doesn't prevent a
					 * situation where one worker has already decided
					 * to quit, but not yet decreased numWorkers,
					 * and another worker just checks numWorkers to
					 * see if it's okay to quit and does so, because
					 * numWorkers > 1. But this is quite unlikely.)
					 *
					 * This avoids forking and shutting down processes
					 * over and over if GC requests occur infrequently.
					 */
					if (SiasGcShmem->numWorkers == 1)
					{
						consecutiveIdlePasses = 0;
					}

					SpinLockRelease(&(SiasGcShmem->numWorkersMutex));
				}
			}
		}

		/* Nap for the configured time. */
		pg_usleep(SIAS_GC_SLEEP_INTERVAL);
	}

	TerminateSiasGcWorker(false);
}

bool
RequestGC(Relation rel, BlockNumber firstBlock)
{
	bool putFirstRequest;
	RequestQueue *requestQueuePtr;
	GarbageCollectionInfo *gci;
	RegionNumber regionNum;
	bool found;
	DirtyRegionData *drd;


#ifndef SIAS_GC
	return false;
#endif

	//TODO testing
//	if (rel->rd_id == 16394)
//		return false;

	putFirstRequest = false;
	requestQueuePtr = &SiasGcShmem->requestQueue;
	gci = rel->rd_gci;
	regionNum = GetBlocksRegionNumber(firstBlock);

//	printf("RequestGC %u %u %u\n", rel->rd_id, rel->rd_node.dbNode, firstBlock);
#ifdef SIAS_GC_TEST
	FILE *file;
	file = fopen("/home/richard/Documents/file1.txt", "a");
	fprintf(file, "RequestGC %u %u %u\n", rel->rd_id, rel->rd_node.dbNode, firstBlock);
	fclose(file);
#endif

	SpinLockAcquire(&(gci->dirtyRegionsMutex));

	if (gci->numDirtyRegions >= SIAS_MAX_QUEUED_REGIONS)
	{
		elog(WARNING, "Already at least SIAS_MAX_QUEUED_REGIONS many regions of relation %u queued for garbage collection. Consider increasing parameter SIAS_MAX_QUEUED_REGIONS.", rel->rd_id);
	}

	drd = (DirtyRegionData*) hash_search(gci->dirtyRegions, &firstBlock, HASH_ENTER, &found);

	if (found)
	{
		elog(ERROR, "Tried to overwrite entry in gci->dirtyRegions.");
	}

	if (!drd)
	{
		elog(ERROR, "Didn't get valid entry of gci->dirtyRegions.");
	}

	drd->firstBlock = firstBlock;
	drd->nextBlock = firstBlock;
	drd->prevMaxUpdaterXmin = gci->maxUpdaterXmin[regionNum];
	drd->tuplesPerBlock = 0.0f;

	putFirstRequest = (++gci->numDirtyRegions == 1);

	SpinLockRelease(&(gci->dirtyRegionsMutex));

	gci->newlyUpdatedData[regionNum] = 0;

	if (!putFirstRequest)
	{
		return true;
	}

	if (gci->hasSiasGcWorker)
	{
		return true;
	}
	else
	{
		/*
		 * We need to request GC for this relation.
		 */

		SpinLockAcquire(&(requestQueuePtr->mutex));

		if (requestQueuePtr->numElements == SIAS_MAX_RELATIONS)
		{
			elog(WARNING, "Can't queue garbage collection request. requestQueue full. Consider increasing parameter SIAS_MAX_RELATIONS");
			SpinLockRelease(&(requestQueuePtr->mutex));
			return false;
		}

		if (++requestQueuePtr->tail == SIAS_MAX_RELATIONS)
		{
			requestQueuePtr->tail = 0;
		}

		requestQueuePtr->elements[requestQueuePtr->tail] = rel->rd_node;

		if (++requestQueuePtr->numElements == 1)
		{
			requestQueuePtr->head = requestQueuePtr->tail;
		}

		SpinLockAcquire(&(SiasGcShmem->numWorkersMutex));
		if (requestQueuePtr->numElements > SiasGcShmem->numWorkers * 2)
		{
			SendPostmasterSignal(PMSIGNAL_START_SIASGC_WORKER);
		}
		SpinLockRelease(&(SiasGcShmem->numWorkersMutex));

		SpinLockRelease(&(requestQueuePtr->mutex));

		return true;
	}
}

static bool
GcRegion(Relation rel, DirtyRegionData *drd)
{
//	printf("GcRegion %u %u begin\n", rel->rd_id, drd->firstBlock);
	//TODO testing
//	if (rel->rd_id == 16394)
//		return true;
//	FILE *file;
#ifdef SIAS_GC_TEST
	file = fopen("/home/richard/Documents/file1.txt", "a");
	fprintf(file, "GcRegion %u %u begin\n", rel->rd_id, drd->firstBlock);
	fclose(file);
#endif

	GarbageCollectionInfo *gci;
	TargetBlockInfo *tbi;
	bool resumedGc;
	bool regionCompletelyGced;
	float fractionGced;

	BlockNumber gcBlock;
	Buffer	gcBuffer;
	Page gcPage;
	OffsetNumber gcOffset;
	int lines;
	ItemId gcLpp;
	HeapTupleHeader gcTuple;
	uint32 vid;
	bool vidSeen;

	Buffer buf;
	Page page;
	ItemId lpp;
	HeapTupleHeader tuple;
	ItemPointerData tupleItemPointer;

	Buffer coldBuffer;
	Page coldPage;
	RegionNumber regionNum;

	Buffer hotBuffer;
	Page hotPage;
	ItemId hotLpp;
	HeapTupleHeader hotTuple;

	bool predRelocated;
	bool inGcRegion;
	ItemPointerData predTid;
	int numHotTuplesToWrite;
	int i;

	TransactionId cutOffTxId;
	ItemPointerData entryPoint;
	ItemPointerData prevEP;
	bool allVisibleVersionsReached;
	bool prevEpSuccessorReached;
	int oldestVisibleVersionInRegion;	/* counted from (initial) entry point (=1) */
	bool coldData;
	OffsetNumber newOffset;

	char *visibleVersions;
	uint32 visibleVersionsSize;
	uint32 *visibleVersionsLengths;
	BlockNumber *visibleVersionsBlocks;
	OffsetNumber *visibleVersionsOffsets;
	uint32 visibleVersionsWriteOffset;
	uint32 visibleVersionsReadOffset;
	int numVisibleVersions;
	int maxVisibleVersions;
	//TODO testing
//	uint32 *indexesOrdered;
//	ItemPointerData oldestTid;
//	ItemPointerData latestEP;
	struct timespec t1, t2;

	HTAB *seenVids;
	HASHCTL hashTableInfo;

	uint32 seenNewlyUpdatedData;



	// get start time
	clock_gettime(0, &t1);

	gci = rel->rd_gci;
	tbi = rel->rd_tbi;
	resumedGc = (drd->firstBlock != drd->nextBlock);


	//TODO testing
//	if (rel->rd_id == 16422 && !resumedGc)
//	{
//		printf("delay GC\n");
//		pg_usleep(SIAS_GC_SLEEP_INTERVAL * 20);
//		printf("continue GC\n");
//	}

	hashTableInfo.keysize = sizeof(uint32);
	hashTableInfo.entrysize = sizeof(uint32);
	hashTableInfo.hash = oid_hash;
	seenVids = hash_create("seenVids", SIAS_MAX_LINP_OFFNUM * SIAS_APPEND_SIZE, &hashTableInfo, HASH_ELEM | HASH_FUNCTION);

	/*
	 * If GC of this region has been paused and is going to be
	 * continued now, we could restore the state of seenVids
	 * from before the break.
	 *
	 * This is done by scanning all pages that have already been
	 * GC'ed and inserting the VIDs of all their tuples into seenVids.
	 *
	 * But first we have to check if it's even worth the effort.
	 */
	if (resumedGc)
	{
		fractionGced = ((float) (drd->nextBlock - drd->firstBlock)) / SIAS_APPEND_SIZE;
		if ((((1 - fractionGced) * drd->tuplesPerBlock) / (GetBlocksRegionNumber(rel->rd_tbi->highestAppendedBlock) + 1 - gci->cleanedRegionsQueue.numElements)) >= 0.5)
		{
			for (gcBlock = drd->firstBlock; gcBlock < drd->nextBlock; gcBlock++)
			{
				gcBuffer = ReadBuffer(rel, gcBlock);
				LockBuffer(gcBuffer, BUFFER_LOCK_SHARE);
				gcPage = BufferGetPage(gcBuffer);

				lines = PageGetMaxOffsetNumber(gcPage);

				for (gcOffset = FirstOffsetNumber, gcLpp = PageGetItemId(gcPage, gcOffset);
					 gcOffset <= lines;
					 gcOffset++, gcLpp++)
				{
					if (ItemIdIsNormal(gcLpp))
					{
						gcTuple = (HeapTupleHeader) PageGetItem(gcPage, gcLpp);
						vid = HeapTupleHeaderGetVid(gcTuple);
						hash_search(seenVids, &vid, HASH_ENTER, NULL);
					}
				}

				UnlockReleaseBuffer(gcBuffer);
			}
		}
	}
	else
	{
		drd->stats.numTuplesTotal = 0;
		drd->stats.numDistinctVids = 0;
		drd->stats.numGcRuns = 0;
		drd->stats.numHotWrites = 0;
		drd->stats.numInplace = 0;
		drd->stats.numColdWrites = 0;
		drd->elapsedMillisecs = 0;
	}

	visibleVersions = NULL;
	maxVisibleVersions = SIAS_MAX_VISIBLE_VERSIONS;
	visibleVersionsLengths = palloc0(sizeof(uint32) * maxVisibleVersions);
	visibleVersionsBlocks = palloc0(sizeof(BlockNumber) * maxVisibleVersions);
	visibleVersionsOffsets = palloc0(sizeof(OffsetNumber) * maxVisibleVersions);
//	indexesOrdered = palloc0(sizeof(uint32) * maxVisibleVersions);
	gcPage = palloc0(BLCKSZ);

	coldBuffer = ReadBuffer(rel, gci->coldWriteBlock);
	LockBuffer(coldBuffer, BUFFER_LOCK_EXCLUSIVE);
	coldPage = BufferGetPage(coldBuffer);
	/*
	 * We need to ensure that the page belonging
	 * to coldWriteBlock is initialized, because
	 * we assume coldPage to be ready for writes
	 * in the following code.
	 */
	if (!gci->coldWriteBlockInited)
	{
		InitSiasBuffer(coldBuffer);
		gci->coldWriteBlockInited = true;
		UpdateHighestUsedBlock(tbi, gci->coldWriteBlock);
	}
	LockBuffer(coldBuffer, BUFFER_LOCK_UNLOCK);

	hotBuffer = InvalidBuffer;

	seenNewlyUpdatedData = 0;

	for (gcBlock = drd->nextBlock; gcBlock < drd->firstBlock + SIAS_APPEND_SIZE; gcBlock++)
	{
//		printf("gc %u %u\n", rel->rd_id, gcBlock);

		/*
		 * update cutOffTxId now and then in order
		 * to classify more versions as invisible
		 */
		if ((gcBlock - drd->nextBlock) % SIAS_UPDATE_XMIN_INTERVAL == 0)
		{
			cutOffTxId = GetOldestXmin(true, true);
#ifdef SIAS_GC_TEST
			file = fopen("/home/richard/Documents/file1.txt", "a");
			fprintf(file, "cutOffTxId %u\n", cutOffTxId);
			fclose(file);
#endif

			regionNum = GetBlocksRegionNumber(drd->firstBlock);
			SpinLockAcquire(&(gci->regionGcCountersMutexes[regionNum]));
			if (gci->maxUpdaterXmin[regionNum] < cutOffTxId)
			{
				gci->newlyUpdatedData[regionNum] = 0;
				drd->prevMaxUpdaterXmin = gci->maxUpdaterXmin[regionNum];
				seenNewlyUpdatedData = 0;
			}
			SpinLockRelease(&(gci->regionGcCountersMutexes[regionNum]));
		}

		gcBuffer = ReadBuffer(rel, gcBlock);
		LockBuffer(gcBuffer, BUFFER_LOCK_SHARE);
		memcpy(gcPage, BufferGetPage(gcBuffer), BLCKSZ);
		UnlockReleaseBuffer(gcBuffer);

		lines = PageGetMaxOffsetNumber(gcPage);

		drd->tuplesPerBlock = ((drd->tuplesPerBlock * (gcBlock - drd->firstBlock)) + lines) / (gcBlock - drd->firstBlock + 1);

#ifdef SIAS_GC_TEST
		file = fopen("/home/richard/Documents/file1.txt", "a");
		fprintf(file, "process block %u with %i tuples\n", gcBlock, lines);
		fclose(file);
#endif

		for (gcOffset = FirstOffsetNumber, gcLpp = PageGetItemId(gcPage, gcOffset);
			 gcOffset <= lines;
			 gcOffset++, gcLpp++)
		{
			if (ItemIdIsNormal(gcLpp))
			{
				drd->stats.numTuplesTotal++;
				gcTuple = (HeapTupleHeader) PageGetItem(gcPage, gcLpp);
				vid = HeapTupleHeaderGetVid(gcTuple);
				hash_search(seenVids, &vid, HASH_ENTER, &vidSeen);

				/*
				 * If this vid hasn't been seen before in the region
				 * during this garbage collection pass, we have to
				 * process the tuple together with all other versions
				 * of the same data item.
				 *
				 * Otherwise it has already been processed earlier,
				 * when another tuple with the same VID occurred.
				 */
				if (!vidSeen)
				{
#ifdef SIAS_GC_TEST
					file = fopen("/home/richard/Documents/file1.txt", "a");
					fprintf(file, "process vid %u\n", vid);
					fclose(file);
#endif

					drd->stats.numDistinctVids++;

					getItemPointerFromVidArray(rel, vid, &entryPoint);

					//TODO testing
//					if (rel->rd_id == 16422 && !resumedGc && (vid == 1 || vid == 80))
//					{
//						printf("delay GC\n");
//						pg_usleep(SIAS_GC_SLEEP_INTERVAL * 20);
//						printf("continue GC\n");
//					}

					/*
					 * traverse backwards starting at the item's entry point
					 *
					 * Store a local copy of each visited tuple version in
					 * order to avoid accessing them again later.
					 */

					memset(visibleVersionsLengths, 0, sizeof(uint32) * maxVisibleVersions);
					memset(visibleVersionsBlocks, 0, sizeof(BlockNumber) * maxVisibleVersions);
					memset(visibleVersionsOffsets, 0, sizeof(OffsetNumber) * maxVisibleVersions);
					visibleVersionsWriteOffset = 0;
					numVisibleVersions = 0;
//					memset(indexesOrdered, 0, sizeof(uint32) * maxVisibleVersions);

					allVisibleVersionsReached = false;
					oldestVisibleVersionInRegion = 0;

					visibleVersionsBlocks[0] = ItemPointerGetBlockNumber(&entryPoint);
					visibleVersionsOffsets[0] = ItemPointerGetOffsetNumber(&entryPoint);

					buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&entryPoint));
					LockBuffer(buf, BUFFER_LOCK_SHARE);
					page = BufferGetPage(buf);
					lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&entryPoint));
					tuple = (HeapTupleHeader) PageGetItem(page, lpp);

					if (!visibleVersions)
					{
						visibleVersionsSize = ItemIdGetLength(gcLpp) * maxVisibleVersions;
						visibleVersions = palloc0(visibleVersionsSize);
					}
					else
					{
						memset(visibleVersions, 0, visibleVersionsSize);
					}

					while (!allVisibleVersionsReached)
					{
						if (visibleVersionsWriteOffset + ItemIdGetLength(lpp)
								> visibleVersionsSize)
						{
							visibleVersionsSize = visibleVersionsSize * 2;
							repalloc(visibleVersions, visibleVersionsSize);
						}

						ItemPointerSet(&tupleItemPointer, visibleVersionsBlocks[numVisibleVersions], visibleVersionsOffsets[numVisibleVersions]);

						/*
						 * store local copy of tuple version
						 */
						memcpy(visibleVersions + visibleVersionsWriteOffset, tuple, ItemIdGetLength(lpp));
						tuple = (HeapTupleHeader) (visibleVersions + visibleVersionsWriteOffset);
						visibleVersionsLengths[numVisibleVersions] = ItemIdGetLength(lpp);
						/*
						 * If there's too much newly updated data, which
						 * could result in hot writes, we want to pause GC
						 * of this region.
						 *
						 * Since we only count the total size of all recently
						 * updated tuples within this region, we don't know
						 * how much of it is actually ahead of this GC pass.
						 *
						 * To roughly estimate this, we keep track of
						 * the newly updated tuples, that we have already seen,
						 * but this is always <= the true amount.
						 *
						 * That's what makes the estimated amount of recently
						 * updated tuples ahead of this GC pass a conservative
						 * guess.
						 */
						if (numVisibleVersions > 0 && BelongsToRegion(drd->firstBlock, visibleVersionsBlocks[numVisibleVersions])
								&& HeapTupleHeaderGetXmin((HeapTupleHeader) (visibleVersions + visibleVersionsWriteOffset - visibleVersionsLengths[numVisibleVersions - 1])) > drd->prevMaxUpdaterXmin)
						{
							seenNewlyUpdatedData += visibleVersionsLengths[numVisibleVersions];
						}
						numVisibleVersions++;
						if (BelongsToRegion(drd->firstBlock, BufferGetBlockNumber(buf)))
						{
							oldestVisibleVersionInRegion = numVisibleVersions;
							visibleVersionsReadOffset = visibleVersionsWriteOffset;
						}
						visibleVersionsWriteOffset += ItemIdGetLength(lpp);

						/*
						 * We can stop traversing the version chain backwards if we reach ..
						 *
						 * .. a committed version, which is visible to all txs.
						 *    (We don't test this properly, which would be costly,
						 *    but rather accept copying potentially too many versions
						 *    by using a too conservative threshold (cutOffTxId).)
						 *
						 *    OR
						 *
						 * .. the first version, whose t_ctid points to itself.
						 */
						if (((HeapTupleHeaderGetXmin(tuple) < cutOffTxId)
								&& TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
								|| ItemPointerEquals(&tuple->t_ctid, &tupleItemPointer))
						{
							LockBuffer(buf, BUFFER_LOCK_UNLOCK);
							if (!(oldestVisibleVersionInRegion > 0
									&& numVisibleVersions == 1))
							{
								ReleaseBuffer(buf);
							}
							allVisibleVersionsReached = true;
						}
						else
						{
							if (numVisibleVersions == maxVisibleVersions)
							{
								elog(WARNING, "There are more than %u visible versions for VID %u of relation %u.",
										numVisibleVersions, vid, rel->rd_id);

								maxVisibleVersions += 10;

								repalloc(visibleVersionsLengths, sizeof(uint32) * maxVisibleVersions);
								repalloc(visibleVersionsBlocks, sizeof(BlockNumber) * maxVisibleVersions);
								repalloc(visibleVersionsOffsets, sizeof(OffsetNumber) * maxVisibleVersions);
//								repalloc(indexesOrdered, sizeof(uint32) * maxVisibleVersions);
							}

							if (ItemPointerGetBlockNumber(&tuple->t_ctid) != BufferGetBlockNumber(buf))
							{
								UnlockReleaseBuffer(buf);
								buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&tuple->t_ctid));
								LockBuffer(buf, BUFFER_LOCK_SHARE);
								page = BufferGetPage(buf);
							}
							visibleVersionsBlocks[numVisibleVersions] = ItemPointerGetBlockNumber(&tuple->t_ctid);
							visibleVersionsOffsets[numVisibleVersions] = ItemPointerGetOffsetNumber(&tuple->t_ctid);
							lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&tuple->t_ctid));
							tuple = (HeapTupleHeader) PageGetItem(page, lpp);
						}
					}	// end of: while (!allVisibleVersionsReached)

					//TODO testing
//					ItemPointerSet(&oldestTid,
//								   visibleVersionsBlocks[numVisibleVersions - 1],
//								   visibleVersionsOffsets[numVisibleVersions - 1]);

					/*
					 * Garbage collection concerning this vid is only
					 * necessary, if any of the visible tuple versions
					 * of this vid is located inside the region,
					 * that is GC'ed right now.
					 */
					if (oldestVisibleVersionInRegion > 0)
					{
#ifdef SIAS_GC_TEST
						file = fopen("/home/richard/Documents/file1.txt", "a");
						fprintf(file, "clean %u of vid %u\n", numVisibleVersions, vid);
						fclose(file);
#endif

						drd->stats.numGcRuns++;
						coldData = false;

						if (numVisibleVersions == 1)
						{
							LockBuffer(coldBuffer, BUFFER_LOCK_EXCLUSIVE);

							/*
							 * The currently used page is full, so
							 * get a new page to write cold data to.
							 */
							if (MAXALIGN(visibleVersionsLengths[0]) > PageGetHeapFreeSpace(coldPage))
							{
								UnlockReleaseBuffer(coldBuffer);
								gci->coldWriteBlock++;
								gci->coldWriteBlockInited = false;

								/*
								 * We've filled all blocks of gci->coldWriteRegion.
								 *
								 * Write it to storage and then get a new region
								 * to write cold data.
								 */
								if (gci->coldWriteBlock == gci->coldWriteRegion + SIAS_APPEND_SIZE)
								{
#ifdef SIAS_GC_TEST
									file = fopen("/home/richard/Documents/file1.txt", "a");
									fprintf(file, "cold region full %u\n", gci->coldWriteRegion);
									fclose(file);
#endif

									if (!gci->prevAppendComplete)
									{
										elog(WARNING, "prevAppend (cold) incomplete %u", rel->rd_id);
									}
									gci->prevAppendComplete = false;
									WriteRegion(rel, gci->coldWriteRegion, false, false);
									gci->prevAppendComplete = true;

									/*
									 * switch to a new gci->coldWriteRegion
									 */
									LWLockAcquire(gci->coldWriteRegionLock, LW_EXCLUSIVE);
									gci->coldWriteRegion = GetWriteableRegion(rel, false);

#ifdef SIAS_GC_TEST
									file = fopen("/home/richard/Documents/file1.txt", "a");
									fprintf(file, "new cold region %u\n", gci->coldWriteRegion);
									fclose(file);
#endif

									gci->coldWriteBlock = gci->coldWriteRegion;
									MemSet((char *) gci->coldCache, 0, BLCKSZ * SIAS_APPEND_SIZE);
									SpinLockAcquire(&(gci->highestWrittenColdBlockMutex));
									gci->highestWrittenColdBlock = InvalidBlockNumber;
									SpinLockRelease(&(gci->highestWrittenColdBlockMutex));
									LWLockRelease(gci->coldWriteRegionLock);

									ResetRegionsGcCounters(rel, gci->coldWriteRegion);
								}

								coldBuffer = ReadBuffer(rel, gci->coldWriteBlock);
								LockBuffer(coldBuffer, BUFFER_LOCK_EXCLUSIVE);
								InitSiasBuffer(coldBuffer);
								gci->coldWriteBlockInited = true;
								UpdateHighestUsedBlock(tbi, gci->coldWriteBlock);
								coldPage = BufferGetPage(coldBuffer);
							}

							newOffset = PageAddItem(coldPage,
													(Item) visibleVersions,
													visibleVersionsLengths[0],
													InvalidOffsetNumber,
													false,
													true);

							//TODO testing
//							if (true)
//							{
//								PageHeader pHdr = coldPage;
//								if (pHdr->pd_special == 8192 && (pHdr->pd_lower == 0 || pHdr->pd_upper == 0))
//								{
//									char s2[128];
//									sprintf(s2, "page header after cold insert: %u %u %u %u %u %u",
//											PageGetPageSize(pHdr),
//											PageGetPageLayoutVersion(pHdr),
//											pHdr->pd_flags,
//											pHdr->pd_lower,
//											pHdr->pd_upper,
//											pHdr->pd_special);
//									file = fopen("/home/richard/Documents/file1.txt", "a");
//									fprintf(file, "%s (%u %u)\n", s2, rel->rd_id, gci->coldWriteBlock);
//									fclose(file);
//								}
//							}

							if (newOffset == InvalidOffsetNumber)
							{
								elog(PANIC, "failed to add cold tuple to page");
							}

							/*
							 * Prevent the version, we expect to still be
							 * the entry point, from being updated.
							 *
							 * This is deadlock-safe, because there can't be
							 * an updater with a share lock on buf waiting
							 * to lock coldBuffer exclusively. That's because
							 * no other process than this GC worker is allowed
							 * to write to the region starting with block
							 * gci->coldWriteRegion.
							 */
							LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

							coldData = isTupleValid(rel, vid, &entryPoint);

							if (coldData)
							{
								MarkBufferDirty(coldBuffer);
								LockBuffer(coldBuffer, BUFFER_LOCK_UNLOCK);

								/*
								 * update VID-TID-mapping
								 */
								insertVidTidPair(rel, vid, gci->coldWriteBlock, newOffset);

								/*
								 * buf is still holding the page, where the vid's
								 * entry point tuple was found earlier.
								 *
								 * Now it's save to unlock it and allow potentially
								 * waiting processes to update it, which will fail,
								 * because the tuple they're trying to update isn't
								 * the entry point of this vid anymore since we just
								 * changed the VID-TID-mapping.
								 */
								UnlockReleaseBuffer(buf);

								drd->stats.numColdWrites++;

								//TODO testing
//								ItemPointerSet(&oldestTid,
//										gci->coldWriteBlock,
//										newOffset);
							}
							else
							{
								UnlockReleaseBuffer(buf);
								//TODO testing
//								char s1[128];
//								bool deletionError = false;
//								PageHeader pHdr = coldPage;
//								sprintf(s1, "page header before: %u %u %u %u %u %u\n",
//										PageGetPageSize(pHdr),
//										PageGetPageLayoutVersion(pHdr),
//										pHdr->pd_flags,
//										pHdr->pd_lower,
//										pHdr->pd_upper,
//										pHdr->pd_special);
//								char s2[128];
								if (!PageDeleteLastItem(coldPage))
								{
//									deletionError = true;
									elog(WARNING, "failed to delete last item of coldPage");
								}
								//TODO testing
//								if (true)
//								{
//									PageHeader pHdr = coldPage;
//									if (pHdr->pd_special == 8192 && (pHdr->pd_lower == 0 || pHdr->pd_upper == 0))
//									{
//										char s2[128];
//										sprintf(s2, "page header after cold deletion: %u %u %u %u %u %u",
//												PageGetPageSize(pHdr),
//												PageGetPageLayoutVersion(pHdr),
//												pHdr->pd_flags,
//												pHdr->pd_lower,
//												pHdr->pd_upper,
//												pHdr->pd_special);
//										file = fopen("/home/richard/Documents/file1.txt", "a");
//										fprintf(file, "%s\n%s (%u %u)\n", s2, s1, rel->rd_id, gci->coldWriteBlock);
//										fclose(file);
//									}
//								}
								if (!PageHeaderIsValid(coldPage))
								{
//									sprintf(s2, "page header after: %u %u %u %u %u %u\n",
//											PageGetPageSize(pHdr),
//											PageGetPageLayoutVersion(pHdr),
//											pHdr->pd_flags,
//											pHdr->pd_lower,
//											pHdr->pd_upper,
//											pHdr->pd_special);
//									file = fopen("/home/richard/Documents/file1.txt", "a");
//									fprintf(file, "PageDeleteLastItem error:\n%s\n%s\n (%u, %u, %u, %u, %u)\n", s1, s2,
//											rel->rd_id, gci->coldWriteBlock, deletionError, visibleVersionsLengths[0],
//											PageGetMaxOffsetNumber(coldPage));
//									fclose(file);
									elog(ERROR, "invalid page header after deletion of last item in coldPage");
								}
								LockBuffer(coldBuffer, BUFFER_LOCK_UNLOCK);
							}
						}
						/*
						 * We have to process >1 hot data tuples.
						 */
						if (!coldData)
						{
							predRelocated = false;
							numHotTuplesToWrite = oldestVisibleVersionInRegion;
							i = numHotTuplesToWrite - 1;

							//TODO testing
//							int j;
//							for (j = 0; j < numVisibleVersions - oldestVisibleVersionInRegion; j++)
//							{
//								indexesOrdered[j] = numVisibleVersions - j - 1;
//							}
//							j--;


							while (numHotTuplesToWrite > 0)
							{
								inGcRegion = BelongsToRegion(drd->firstBlock, visibleVersionsBlocks[i]);
//								indexesOrdered[++j] = i;

								if (predRelocated || inGcRegion)
								{
									/*
									 * Try to get the page holding the to-be-updated tuple.
									 * If it's still cached when we are holding the exclusive
									 * lock on its buffer, then it's save to in-place update it.
									 */
									//TODO testing
									if (!inGcRegion && (IsCached(rel, visibleVersionsBlocks[i], false) == CACHE_HOT))
//									if (false)
									{
										/*
										 * It's valid to access the desired page without use
										 * of RelationGetBufferForSiasTuple(..), because we
										 * just intend to perform an in-place update.
										 * Thus we don't have to coordinate our operation
										 * with other regular writers, which usually add
										 * new tuples to heap pages.
										 */
										hotBuffer = ReadBuffer(rel, visibleVersionsBlocks[i]);
										LockBuffer(hotBuffer, BUFFER_LOCK_EXCLUSIVE);

										if (!(IsCached(rel, visibleVersionsBlocks[i], false) == CACHE_HOT))
										{
											/*
											 * We failed to buffer and exclusive lock the desired
											 * page before it was written to disk.
											 *
											 * Now we have to get a regular target block to
											 * insert a new copy of the to-be-updated tuple.
											 */
											UnlockReleaseBuffer(hotBuffer);
											hotBuffer = RelationGetBufferForSiasTuple(rel, visibleVersionsLengths[i], InvalidBuffer,
																					  0, NULL, InvalidBlockNumber);
										}
									}
									else
									{
										hotBuffer = RelationGetBufferForSiasTuple(rel, visibleVersionsLengths[i], InvalidBuffer,
																				  0, NULL, InvalidBlockNumber);
									}

									//TODO testing
									if (BufferGetBlockNumber(hotBuffer) == visibleVersionsBlocks[i])
//									if (false)
									{
										/*
										 * in-place update t_ctid
										 */

										hotPage = BufferGetPage(hotBuffer);
										hotLpp = PageGetItemId(hotPage, visibleVersionsOffsets[i]);
										hotTuple = (HeapTupleHeader) PageGetItem(hotPage, hotLpp);
										hotTuple->t_ctid = predTid;

										ItemPointerSetBlockNumber(&predTid, visibleVersionsBlocks[i]);
										ItemPointerSetOffsetNumber(&predTid, visibleVersionsOffsets[i]);
										predRelocated = false;

										//TODO testing
										PageSetSiasInplace(hotPage);
										drd->stats.numInplace++;

#ifdef SIAS_GC_TEST
										file = fopen("/home/richard/Documents/file1.txt", "a");
										fprintf(file, "in-place hot update %u\n", BufferGetBlockNumber(hotBuffer));
										fclose(file);
#endif

									}
									else
									{
										/*
										 * relocate tuple version and update t_ctid
										 */

										hotPage = BufferGetPage(hotBuffer);
										hotTuple = (HeapTupleHeader) (visibleVersions + visibleVersionsReadOffset);

										/*
										 * The t_ctid value of the oldest tuple to
										 * be relocated doesn't change.
										 */
										if (i != oldestVisibleVersionInRegion - 1)
										{
											hotTuple->t_ctid = predTid;
										}

										HeapTupleHeaderSetSiasRelocated(hotTuple);

										newOffset = PageAddItem(hotPage,
																(Item) hotTuple,
																visibleVersionsLengths[i],
																InvalidOffsetNumber,
																false,
																true);

#ifdef SIAS_GC_TEST
										file = fopen("/home/richard/Documents/file1.txt", "a");
										fprintf(file, "out-of-place hot write %u\n", BufferGetBlockNumber(hotBuffer));
										fclose(file);
#endif

										if (newOffset == InvalidOffsetNumber)
										{
											elog(PANIC, "failed to add hot tuple to page");
										}

										/*
										 * The version of which we just created a new copy
										 * isn't the current entry point, thus it has a
										 * successor.
										 * That's why we need to count the new copy as
										 * garbage.
										 */
										if (numHotTuplesToWrite > 1)
										{
											UpdateRegionGcCounters(rel,
																   BufferGetBlockNumber(hotBuffer),
																   HeapTupleHeaderGetXmin((HeapTupleHeader) (visibleVersions + visibleVersionsReadOffset - visibleVersionsLengths[i-1])),
																   visibleVersionsLengths[i]);
										}

										ItemPointerSetBlockNumber(&predTid, BufferGetBlockNumber(hotBuffer));
										ItemPointerSetOffsetNumber(&predTid, newOffset);
										predRelocated = true;

										//TODO testing
//										if (i == oldestVisibleVersionInRegion - 1)
//										{
//											if (numVisibleVersions == oldestVisibleVersionInRegion)
//											{
//												oldestTid = predTid;
//											}
//										}

									}

									MarkBufferDirty(hotBuffer);
									UnlockReleaseBuffer(hotBuffer);

									drd->stats.numHotWrites++;
								}

								if (--i >= 0)
								{
									visibleVersionsReadOffset -= visibleVersionsLengths[i];
								}

								if (--numHotTuplesToWrite == 0)
								{
#ifdef SIAS_GC_TEST
									file = fopen("/home/richard/Documents/file1.txt", "a");
									fprintf(file, "work done?\n");
									fclose(file);
#endif



									/*
									 * There might be newly inserted versions, which haven't
									 * been regarded so far, because they didn't exist when
									 * we started to traverse the version chain from the
									 * at that time valid entry point.
									 *
									 * If so, gather them and continue the updating process.
									 *
									 * note: These versions can't possibly be inside this GC region!
									 */

									buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&entryPoint));

									/*
									 * Prevent the version, we expect to still be
									 * the entry point, from being updated.
									 */
									LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

									if (predRelocated && !isTupleValid(rel, vid, &entryPoint))
									{
#ifdef SIAS_GC_TEST
										file = fopen("/home/richard/Documents/file1.txt", "a");
										fprintf(file, "no, new tuples\n");
										fclose(file);
#endif

										/*
										 * The version, that was the entry point before,
										 * isn't the latest version anymore.
										 *
										 * Thus there have been updates in the meantime
										 * and we have to process further versions.
										 */

										UnlockReleaseBuffer(buf);
										prevEP = entryPoint;
										getItemPointerFromVidArray(rel, vid, &entryPoint);

										buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&entryPoint));
										LockBuffer(buf, BUFFER_LOCK_SHARE);
										page = BufferGetPage(buf);
										lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&entryPoint));
										tuple = (HeapTupleHeader) PageGetItem(page, lpp);

										if (numVisibleVersions == maxVisibleVersions)
										{
											elog(WARNING, "There are more than %u visible versions for VID %u of relation %u.",
													numVisibleVersions, vid, rel->rd_id);

											maxVisibleVersions += 10;

											repalloc(visibleVersionsLengths, sizeof(uint32) * maxVisibleVersions);
											repalloc(visibleVersionsBlocks, sizeof(BlockNumber) * maxVisibleVersions);
											repalloc(visibleVersionsOffsets, sizeof(OffsetNumber) * maxVisibleVersions);
//											repalloc(indexesOrdered, sizeof(uint32) * maxVisibleVersions);
										}

										visibleVersionsBlocks[numVisibleVersions] = ItemPointerGetBlockNumber(&entryPoint);
										visibleVersionsOffsets[numVisibleVersions] = ItemPointerGetOffsetNumber(&entryPoint);

										prevEpSuccessorReached = false;

										while (!prevEpSuccessorReached)
										{
											if (visibleVersionsWriteOffset + ItemIdGetLength(lpp)
													> visibleVersionsSize)
											{
												visibleVersionsSize = visibleVersionsSize * 2;
												repalloc(visibleVersions, visibleVersionsSize);
											}

											/*
											 * store local copy of tuple version
											 */
											memcpy(visibleVersions + visibleVersionsWriteOffset, tuple, ItemIdGetLength(lpp));
											tuple = (HeapTupleHeader) (visibleVersions + visibleVersionsWriteOffset);
											visibleVersionsLengths[numVisibleVersions] = ItemIdGetLength(lpp);
											visibleVersionsReadOffset = visibleVersionsWriteOffset;
											visibleVersionsWriteOffset += ItemIdGetLength(lpp);
											numVisibleVersions++;
											numHotTuplesToWrite++;

											if (!ItemPointerEquals(&prevEP, &tuple->t_ctid))
											{
												if (numVisibleVersions == maxVisibleVersions)
												{
													elog(WARNING, "There are more than %u visible versions for VID %u of relation %u.",
															numVisibleVersions, vid, rel->rd_id);

													maxVisibleVersions += 10;

													repalloc(visibleVersionsLengths, sizeof(uint32) * maxVisibleVersions);
													repalloc(visibleVersionsBlocks, sizeof(BlockNumber) * maxVisibleVersions);
													repalloc(visibleVersionsOffsets, sizeof(OffsetNumber) * maxVisibleVersions);
//													repalloc(indexesOrdered, sizeof(uint32) * maxVisibleVersions);
												}

												if (ItemPointerGetBlockNumber(&tuple->t_ctid) != BufferGetBlockNumber(buf))
												{
													UnlockReleaseBuffer(buf);
													buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&tuple->t_ctid));
													LockBuffer(buf, BUFFER_LOCK_SHARE);
													page = BufferGetPage(buf);
												}

												visibleVersionsBlocks[numVisibleVersions] = ItemPointerGetBlockNumber(&tuple->t_ctid);
												visibleVersionsOffsets[numVisibleVersions] = ItemPointerGetOffsetNumber(&tuple->t_ctid);
												lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&tuple->t_ctid));
												tuple = (HeapTupleHeader) PageGetItem(page, lpp);
											}
											else
											{
												prevEpSuccessorReached = true;
											}
										}

										UnlockReleaseBuffer(buf);

										/*
										 * The new copy of the version, which we falsely assumed
										 * to still be the entry point, has to be counted as garbage,
										 * because it has a successor by now.
										 */
										UpdateRegionGcCounters(rel,
															   ItemPointerGetBlockNumber(&predTid),
															   HeapTupleHeaderGetXmin((HeapTupleHeader) (visibleVersions + visibleVersionsReadOffset)),
															   visibleVersionsLengths[i + 1]);

										i = numVisibleVersions - 1;
									}
									else if (predRelocated)
									{
#ifdef SIAS_GC_TEST
										file = fopen("/home/richard/Documents/file1.txt", "a");
										fprintf(file, "yes, update VID-TID-map\n");
										fclose(file);
#endif

										/*
										 * update VID-TID-mapping
										 */
										insertVidTidPair(rel, vid, ItemPointerGetBlockNumber(&predTid), ItemPointerGetOffsetNumber(&predTid));

										/*
										 * buf is still holding the page, where the vid's
										 * entry point tuple was found earlier.
										 *
										 * Now it's save to unlock it and allow potentially
										 * waiting processes to update it, which will fail,
										 * because the tuple they're trying to update isn't
										 * the entry point of this vid anymore since we just
										 * changed the VID-TID-mapping.
										 */
//										getItemPointerFromVidArray(rel, vid, &latestEP);
										UnlockReleaseBuffer(buf);

										/*
										 * The old copy of the version, which is the entry point,
										 * has become garbage by us creating a new copy of it.
										 *
										 * In case the old copy is located inside the GC region,
										 * there is no need to count this as garbage.
										 */
										if (!inGcRegion)
										{
											UpdateRegionGcCounters(rel,
																   ItemPointerGetBlockNumber(&entryPoint),
																   0,
																   visibleVersionsLengths[i + 1]);
										}
									}
									else
									{
//										getItemPointerFromVidArray(rel, vid, &latestEP);
										UnlockReleaseBuffer(buf);
									}
								}	// end of: if (--numHotTuplesToWrite == 0)
							}	// end of: while (numHotTuplesToWrite > 0)

							//TODO testing
//							buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&latestEP));
//							LockBuffer(buf, BUFFER_LOCK_SHARE);
//							page = BufferGetPage(buf);
//							lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&latestEP));
//							tuple = (HeapTupleHeader) PageGetItem(page, lpp);
//							allVisibleVersionsReached = false;
//							tupleItemPointer = latestEP;
//							while (!allVisibleVersionsReached)
//							{
//								if (HeapTupleHeaderGetVid(tuple) != vid)
//								{
//									elog(WARNING, "%u wrong VID %u %u", rel->rd_id, HeapTupleHeaderGetVid(tuple), vid);
//								}
//								if (BelongsToRegion(drd->firstBlock, BufferGetBlockNumber(buf)))
//								{
//									elog(WARNING, "%u still in GC region", rel->rd_id);
//								}
//
//
//								if (((HeapTupleHeaderGetXmin(tuple) < cutOffTxId)
//										&& TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
//										|| ItemPointerEquals(&tuple->t_ctid, &tupleItemPointer))
//								{
//									allVisibleVersionsReached = true;
//								}
//								else
//								{
//									ItemPointerSet(&tupleItemPointer, ItemPointerGetBlockNumber(&tuple->t_ctid),
//											ItemPointerGetOffsetNumber(&tuple->t_ctid));
//									if (ItemPointerGetBlockNumber(&tupleItemPointer) != BufferGetBlockNumber(buf))
//									{
//										UnlockReleaseBuffer(buf);
//										buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&tupleItemPointer));
//										LockBuffer(buf, BUFFER_LOCK_SHARE);
//										page = BufferGetPage(buf);
//									}
//									lpp = PageGetItemId(page, ItemPointerGetOffsetNumber(&tupleItemPointer));
//									tuple = (HeapTupleHeader) PageGetItem(page, lpp);
//								}
//							}
//							UnlockReleaseBuffer(buf);
//							if (!ItemPointerEquals(&tupleItemPointer, &oldestTid))
//							{
//								file = fopen("/home/richard/Documents/file1.txt", "a");
//								fprintf(file, "%u %u couldn't reach oldest visible version of VID %u\n", rel->rd_id, drd->firstBlock, vid);
//								fclose(file);
//								elog(WARNING, "%u %u couldn't reach oldest visible version of VID %u", rel->rd_id, drd->firstBlock, vid);
//							}
						}	// end of: We have to process >1 hot data tuples.
					}	// end of: if (oldestVisibleVersionInRegion > 0)
				}	// end of: if (!hash_search(seenVids, &vid, HASH_FIND, NULL))
				else
				{
#ifdef SIAS_GC_TEST
					file = fopen("/home/richard/Documents/file1.txt", "a");
					fprintf(file, "skip vid %u\n", vid);
					fclose(file);
#endif
				}
			}	// end of: if (ItemIdIsNormal(gcLpp))
		}	// end of: tuple loop

		/*
		 * In case this GC pass is interrupted,
		 * we will know which pages have already
		 * been GC'ed completely.
		 */
		drd->nextBlock = gcBlock + 1;

		if (gcBlock < drd->firstBlock + SIAS_APPEND_SIZE - 1
				&& ((SIAS_APPEND_SIZE - (gcBlock - drd->firstBlock + 1)) * BLCKSZ > SIAS_MAX_NEWLY_UPDATED_DATA))
		{
			regionNum = GetBlocksRegionNumber(drd->firstBlock);
			SpinLockAcquire(&(gci->regionGcCountersMutexes[regionNum]));
			if (gci->newlyUpdatedData[regionNum] < seenNewlyUpdatedData)
			{
				elog(ERROR, "Counting of seenNewlyUpdatedData went wrong.");
			}

			/*
			 * If there are too many updated,
			 * potentially still visible tuples
			 * in the remaining blocks, we better
			 * halt GC to reduce the number of
			 * necessary hot writes.
			 *
			 * seenNewlyUpdatedData is a conservative
			 * approximation, because there might
			 * be updated tuples, which won't be
			 * processed during the remaining GC of
			 * this region, but haven't been accounted
			 * for in seenNewlyUpdatedData either.
			 */
			if (gci->newlyUpdatedData[regionNum] - seenNewlyUpdatedData > SIAS_MAX_NEWLY_UPDATED_DATA
					&& gci->maxUpdaterXmin[regionNum] >= cutOffTxId)
			{
				drd->prevMaxUpdaterXmin = gci->maxUpdaterXmin[regionNum];
				gci->newlyUpdatedData[regionNum] = 0;
				SpinLockRelease(&(gci->regionGcCountersMutexes[regionNum]));

				break;
			}
			SpinLockRelease(&(gci->regionGcCountersMutexes[regionNum]));
		}
	}	// end of: gcBlock loop

	ReleaseBuffer(coldBuffer);

	regionCompletelyGced = (gcBlock == drd->firstBlock + SIAS_APPEND_SIZE);

	clock_gettime(0, &t2);
	unsigned long long t = 1000 * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1000000;
	drd->elapsedMillisecs += t;

	/*
	 * This region is completely GC'ed.
	 */
	if (regionCompletelyGced)
	{
		//TODO testing
		AddCleanedRegion(rel, drd->firstBlock);

		if (PageIsSiasHot(gcPage))
		{
			gci->numGcHot++;
		}
		else
		{
			gci->numGcCold++;
		}

		gci->totalGcDurationMillisecs += drd->elapsedMillisecs;
		FILE *gcRunDurationFile;
		gcRunDurationFile = fopen(gcRunDurationFileDir, "a");
		fprintf(gcRunDurationFile, "%u/%llu\n", rel->rd_id, drd->elapsedMillisecs);
		fclose(gcRunDurationFile);

//		printf("total tuples: %u\ndistinct Vids: %u\nGC runs: %u\ncold writes: %u\nhot writes: %u\ninplace: %u\n",
//				drd->stats.numTuplesTotal, drd->stats.numDistinctVids, drd->stats.numGcRuns,
//				drd->stats.numColdWrites, drd->stats.numHotWrites, drd->stats.numInplace);
//		printf("GcRegion %u %u end\n", rel->rd_id, drd->firstBlock);
#ifdef SIAS_GC_TEST
		file = fopen("/home/richard/Documents/file1.txt", "a");
		fprintf(file, "GcRegion %u %u end\n", rel->rd_id, drd->firstBlock);
		fclose(file);
#endif
//		printf("Gc %u total: %u %u\n", rel->rd_id, gci->numGcHot, gci->numGcCold);
	}
	/*
	 * We're pausing GC of this region.
	 */
	else
	{
#ifdef SIAS_GC_TEST
		file = fopen("/home/richard/Documents/file1.txt", "a");
		fprintf(file, "GcRegion %u %u pause\n", rel->rd_id, drd->firstBlock);
		fclose(file);
#endif
	}


	//TODO testing
	if (regionCompletelyGced)
	{
//		for (gcBlock = drd->firstBlock; gcBlock < drd->firstBlock + SIAS_APPEND_SIZE; gcBlock++)
//		{
//			gcBuffer = ReadBuffer(rel, gcBlock);
//			LockBuffer(gcBuffer, BUFFER_LOCK_EXCLUSIVE);
//			gcPage = BufferGetPage(gcBuffer);
//			PageInit(gcPage, BLCKSZ, 0);
//			MarkBufferDirty(gcBuffer);
//			UnlockReleaseBuffer(gcBuffer);
//		}
	}
	else
	{

	}


	if (visibleVersions)
	{
		pfree(visibleVersions);
	}
	pfree(visibleVersionsLengths);
	pfree(visibleVersionsBlocks);
	pfree(visibleVersionsOffsets);
	pfree(gcPage);
	hash_destroy(seenVids);
	pfree(seenVids);
//	pfree(indexesOrdered);

	return regionCompletelyGced;
}

/*
 * SiasGcShmemInit
 *		Allocate and initialize Sias garbage collection-related shared memory
 */
void
SiasGcShmemInit(void)
{
	bool found;
	bool rebuild;


	SiasGcShmem = ShmemInitStruct("siasGcShmem",
								  sizeof(SiasGcShmemStruct),
								  &found);

	if (!found || SiasGcShmem->needRebuild)
	{
		RequestQueue *requestQueuePtr;
		rebuild = found;

		requestQueuePtr = &SiasGcShmem->requestQueue;
		if (!rebuild)
		{
			MemSet(requestQueuePtr->elements, 0, sizeof(RelFileNode) * SIAS_MAX_RELATIONS);
			requestQueuePtr->head = -1;
			requestQueuePtr->tail = -1;
			requestQueuePtr->numElements = 0;
			SpinLockInit(&(requestQueuePtr->mutex));
		}

		SiasGcShmem->numWorkers = 0;
		SpinLockInit(&(SiasGcShmem->numWorkersMutex));

		SiasGcShmem->needRebuild = false;
	}
}

void
SetGcRunDurationFileDir(char *dataDir)
{
	sprintf(gcRunDurationFileDir, "%s/%s", dataDir, SIAS_GC_RUN_DURATION_FILE);
}

bool
IsSiasGcWorkerProcess(void)
{
	return am_SiasGc_worker;
}





/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

static void
PrepareTerminateSiasGcWorker()
{
	if (interruptable)
	{
		TerminateSiasGcWorker(false);
	}

	terminate = true;
}

static void
TerminateSiasGcWorker(bool replaceProcess)
{
	ResourceOwner resOwner;
	bool requeueRequest;
	RequestQueue *requestQueuePtr;


	interruptable = false;

	if (servingRequest)
	{
		/*
		 * If this process is working on a GC request,
		 * the respective relation is guaranteed to have
		 * at least one dirty region left.
		 *
		 * That's why we must requeue the request,
		 * to ensure that the remaining dirty regions
		 * will be cleaned eventually.
		 */
		requeueRequest = true;
		requestQueuePtr = &SiasGcShmem->requestQueue;

		if (rel)
		{
			rel->rd_gci->hasSiasGcWorker = false;

			if (drd)
			{
				hash_seq_term(&searchStatus);
			}

			relation_close(rel, AccessShareLock);
		}

		/*
		 * Requeue the request this process is working
		 * on in the moment.
		 *
		 * This is necessary, because no other GC worker
		 * will continue cleaning this relation without
		 * seeing a GC request in the request queue.
		 * But there won't be such, because no backend
		 * would post a new request unless it's adding
		 * the first element to dirtyRegions, which
		 * won't happen either, because dirtyRegions isn't
		 * empty right now (and won't get empty unless
		 * a GC worker cleans its entries).
		 */
		if (requeueRequest)
		{
			SpinLockAcquire(&(requestQueuePtr->mutex));

			if (requestQueuePtr->numElements == SIAS_MAX_RELATIONS)
			{
				elog(WARNING, "Can't queue garbage collection request. requestQueue full. Consider increasing parameter SIAS_MAX_RELATIONS");
			}
			else
			{
				if (++requestQueuePtr->tail == SIAS_MAX_RELATIONS)
				{
					requestQueuePtr->tail = 0;
				}

				requestQueuePtr->elements[requestQueuePtr->tail] = rfn;

				if (++requestQueuePtr->numElements == 1)
				{
					requestQueuePtr->head = requestQueuePtr->tail;
				}
			}

			SpinLockRelease(&(requestQueuePtr->mutex));
		}
	}

	SpinLockAcquire(&(SiasGcShmem->numWorkersMutex));
	SiasGcShmem->numWorkers--;
	SpinLockRelease(&(SiasGcShmem->numWorkersMutex));

	if (CurrentResourceOwner != NULL)
	{
		resOwner = CurrentResourceOwner;
		CurrentResourceOwner = NULL;
		ResourceOwnerDelete(resOwner);
	}

	if (replaceProcess)
	{
		printf("replace SiasGC process - PID %i\n", MyProcPid);
		SendPostmasterSignal(PMSIGNAL_START_SIASGC_WORKER);
	}

	proc_exit(0);
}
