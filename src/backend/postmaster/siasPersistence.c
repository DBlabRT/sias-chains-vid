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


NON_EXEC_STATIC void SiasPersWorkerMain(bool startup);

static void ShutdownSias();
static void PrepareSiasShutdown();
static void PersistSiasStructs();

static void StartupSias();
static bool LoadSiasStructs();
static void RelinkSiasStructs();

static void TerminateSiasPersWorker(void);

/* Flag to tell if we are in an SiasPers process */
static bool am_SiasPers_worker = false;

static char persistencyConfigFileDir[128];



int
StartSiasPersWorker(bool startup)
{
	pid_t		worker_pid;

	switch ((worker_pid = fork_process()))
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork Sias persistence worker process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			SiasPersWorkerMain(startup);
			break;
#endif
		default:
			return (int) worker_pid;
	}

	/* shouldn't get here */
	return 0;
}

NON_EXEC_STATIC void
SiasPersWorkerMain(bool startup)
{
	sigjmp_buf	local_sigjmp_buf;
	Oid			dbid;


	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_SiasPers_worker = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* record Start Time for logging */
	MyStartTime = time(NULL);

	/* Identify myself via ps */
	init_ps_display("Sias persistence worker process", "", "", "");

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
	pqsignal(SIGTERM, TerminateSiasPersWorker);
	pqsignal(SIGQUIT, TerminateSiasPersWorker);

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

	/*
	 * connect to database SIAS_PERS_DB_NAME
	 */
	InitPostgres(SIAS_PERS_DB_NAME, InvalidOid, NULL, NULL);

	SetProcessingMode(NormalProcessing);

	printf("new SiasPers process running - PID %i\n", MyProcPid);

	if (startup)
	{
		CurrentResourceOwner = ResourceOwnerCreate(NULL, "SiasPersStartupResOwner");
		StartupSias();
	}
	else
	{
		CurrentResourceOwner = ResourceOwnerCreate(NULL, "SiasPersShutdownResOwner");
		ShutdownSias();
	}

	TerminateSiasPersWorker();
}

static void
ShutdownSias()
{
	printf("## ShutdownSias begin ##\n");
	struct timespec t1, t2;
	clock_gettime(0, &t1);

	PrepareSiasShutdown();
	PersistSiasStructs();

	clock_gettime(0, &t2);
	unsigned long long t = 1000 * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1000000;
	printf("SIAS shutdown took %llu ms\n", t);
	printf("## ShutdownSias end ##\n");
}

static void
PrepareSiasShutdown()
{
	Relation rel;
	TargetBlockInfo *tbi;
	int r, i;
	Oid *userRelationOids;
	bool found;
	BlockNumber swap;
	BlockNumber newWriteRegions[3];
	int numWriteRegions;
	HASH_SEQ_STATUS searchStatus;
	DirtyRegionData *drdList;
	DirtyRegionData *drd;
	rfnHashLookupEntry *rfnUrIdList;
	rfnHashLookupEntry *rfnEntry;
	SiasGcShmemStruct *SiasGcShmem;
	char name[SHMEM_INDEX_KEYSIZE];


	userRelationOids = ShmemInitStruct("userRelationOids",
			(sizeof(Oid) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		elog(ERROR, "Didn't find userRelationOids array to prepare shutdown.");
	}

	for (r = 0; r < SIAS_MAX_RELATIONS; r++)
	{
		if (!OidIsValid(userRelationOids[r]))
		{
			break;
		}

//		printf("prepare relation %u\n", userRelationOids[r]);
		rel = relation_open(userRelationOids[r], AccessShareLock);
		tbi = rel->rd_tbi;

		// output GC statistics
		printf("relation %u GC runs: %u hot, %u cold\n",
				rel->rd_id,
				rel->rd_gci->numGcHot,
				rel->rd_gci->numGcCold);
		printf("average GC run duration: %llu ms\n", (rel->rd_gci->totalGcDurationMillisecs / Max(1, (rel->rd_gci->numGcHot + rel->rd_gci->numGcCold))));

//		printf("before writing %u-%u (%u), %u-%u, %u-%u, %u, %u-%u\n",
//				tbi->firstPrevAppendBlock, tbi->firstPrevAppendBlock+SIAS_APPEND_SIZE-1,
//				tbi->highestWrittenBlockPrevAppend,
//				tbi->firstNextAppendBlock, tbi->firstNextAppendBlock+SIAS_APPEND_SIZE-1,
//				tbi->firstOverflowBlock, tbi->firstOverflowBlock+SIAS_APPEND_SIZE-1,
//				tbi->highestUsedBlock,
//				rel->rd_gci->coldWriteRegion, rel->rd_gci->coldWriteRegion+SIAS_APPEND_SIZE-1);

		/*
		 * latest append is supposed to be completed
		 */
		if (tbi->firstPrevAppendBlock != InvalidBlockNumber
				&& ((tbi->firstPrevAppendBlock + SIAS_APPEND_SIZE - 1) != tbi->highestWrittenBlockPrevAppend))
		{
			elog(ERROR, "previous append incomplete (rel %u, %u-%u (%u))",
					rel->rd_id,
					tbi->firstPrevAppendBlock,
					tbi->firstPrevAppendBlock + SIAS_APPEND_SIZE - 1,
					tbi->highestWrittenBlockPrevAppend);
		}

		newWriteRegions[0] = InvalidBlockNumber;
		newWriteRegions[1] = InvalidBlockNumber;
		newWriteRegions[2] = InvalidBlockNumber;
		numWriteRegions = 2;
		/*
		 * write all cached regions to storage,
		 * if there have been writes to them
		 *
		 * If there are GC requests due to this,
		 * they won't go through to GC workers,
		 * because these have already been terminated
		 * and new ones can't be forked.
		 */
		if (tbi->highestUsedTargetBlockIndex >= 0)
		{
			WriteRegion(rel, tbi->firstNextAppendBlock, true, false);
		}
		else
		{
			newWriteRegions[0] = tbi->firstNextAppendBlock;
		}
		if (tbi->highestUsedTargetBlockIndex >= SIAS_APPEND_SIZE)
		{
			WriteRegion(rel, tbi->firstOverflowBlock, true, true);
		}
		else
		{
			newWriteRegions[1] = tbi->firstOverflowBlock;
		}
#ifdef SIAS_GC
		if (!(rel->rd_gci->coldWriteBlock == rel->rd_gci->coldWriteRegion
				&& !rel->rd_gci->coldWriteBlockInited))
		{
			WriteRegion(rel, rel->rd_gci->coldWriteRegion, false, false);
		}
		else
		{
			newWriteRegions[2] = rel->rd_gci->coldWriteRegion;
		}
		numWriteRegions = 3;
#endif

		for (i = 0; i < numWriteRegions; i++)
		{
			if (newWriteRegions[i] == InvalidBlockNumber)
			{
				newWriteRegions[i] = GetWriteableRegion(rel, true);
			}
		}
		if (newWriteRegions[0] > newWriteRegions[1])
		{
			swap = newWriteRegions[0];
			newWriteRegions[0] = newWriteRegions[1];
			newWriteRegions[1] = swap;
		}
		if (newWriteRegions[1] > newWriteRegions[2])
		{
			swap = newWriteRegions[1];
			newWriteRegions[1] = newWriteRegions[2];
			newWriteRegions[2] = swap;
		}
		if (newWriteRegions[0] > newWriteRegions[1])
		{
			swap = newWriteRegions[0];
			newWriteRegions[0] = newWriteRegions[1];
			newWriteRegions[1] = swap;
		}
		/*
		 * assign new write regions
		 */
		i = 0;
#ifdef SIAS_GC
		rel->rd_gci->coldWriteRegion = newWriteRegions[i++];
		ResetRegionsGcCounters(rel, newWriteRegions[0]);
		ResetRegionsGcCounters(rel, newWriteRegions[1]);
		ResetRegionsGcCounters(rel, newWriteRegions[2]);
#endif
		tbi->firstNextAppendBlock = newWriteRegions[i++];
		tbi->firstOverflowBlock = newWriteRegions[i];


#ifdef SIAS_GC
		/*
		 * store gci->dirtyRegions as a list
		 * of DirtyRegionData entries (drdList [relOid]).
		 */
		sprintf(name, "drdList %u", rel->rd_id);
		drdList = (DirtyRegionData *) ShmemInitStruct(name, (sizeof(DirtyRegionData) * SIAS_MAX_QUEUED_REGIONS), &found);

		i = 0;
		memset(drdList, 0, sizeof(DirtyRegionData) * SIAS_MAX_QUEUED_REGIONS);
		hash_seq_init(&searchStatus, rel->rd_gci->dirtyRegions);

		drd = (DirtyRegionData *) hash_seq_search(&searchStatus);
		while (drd && i < SIAS_MAX_QUEUED_REGIONS)
		{
			drdList[i] = *drd;
			i++;
			drd = (DirtyRegionData *) hash_seq_search(&searchStatus);
		}

		if (drd)
		{
			hash_seq_term(&searchStatus);
			rel->rd_gci->numDirtyRegions = SIAS_MAX_QUEUED_REGIONS;
			elog(ERROR, "There are more than SIAS_MAX_QUEUED_REGIONS entries in gci->dirtyRegions of relation %u. Can't persist all of them.", rel->rd_id);
		}
#endif


		/*
		 * remember that *Info structs need
		 * to be rebuild during next startup
		 *
		 * note: It's crucial that nobody calls
		 * 		 any RelationAllocate*Info method
		 * 		 after the needRebuild fields are
		 * 		 set true. Otherwise these would
		 * 		 be set false again.
		 */
		rel->rd_vmi->needRebuild = true;
		tbi->needRebuild = true;
#ifdef SIAS_GC
		rel->rd_gci->needRebuild = true;
#endif

		/*
		 * flush dirty bucket buffers
		 */
		FlushRelationBucketBuffers(rel);

		relation_close(rel, AccessShareLock);
	}


	/*
	 * store rfnUserRelationIdHashTable as a list
	 * of rfnHashLookupEntry entries (rfnUrIdList)
	 */
	rfnUrIdList = (rfnHashLookupEntry *) ShmemInitStruct("rfnUrIdList", (sizeof(rfnHashLookupEntry) * SIAS_MAX_RELATIONS), &found);
	memset(rfnUrIdList, 0, sizeof(rfnHashLookupEntry) * SIAS_MAX_RELATIONS);
	i = 0;
	hash_seq_init(&searchStatus, rfnUserRelationIdHashTable);
	rfnEntry = (rfnHashLookupEntry *) hash_seq_search(&searchStatus);

	while (rfnEntry && i < SIAS_MAX_RELATIONS)
	{
		rfnUrIdList[i] = *rfnEntry;
		i++;
		rfnEntry = (rfnHashLookupEntry *) hash_seq_search(&searchStatus);
	}

	if (rfnEntry)
	{
		hash_seq_term(&searchStatus);
		elog(ERROR, "There are more than SIAS_MAX_RELATIONS entries in rfnUserRelationIdHashTable.");
	}


#ifdef SIAS_GC
	SiasGcShmem = (SiasGcShmemStruct *) ShmemInitStruct("siasGcShmem",
								  	  	  	  sizeof(SiasGcShmemStruct),
											  &found);

	/*
	 * remember that SiasGcShmemStruct needs
	 * to be rebuild during next startup
	 */
	SiasGcShmem->needRebuild = true;
#endif

}

static void
PersistSiasStructs()
{
	FILE 	   *persConfigFile;
	RelFileNode rfn;
	SMgrRelation	persRel;
	uint32 		userRelId;
	Oid 		*userRelationOids;
	Oid 		relOid;
	char		name[SHMEM_INDEX_KEYSIZE];
	char		structNames[SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS + SIAS_NUM_GENERAL_PERS_STRUCTS][SHMEM_INDEX_KEYSIZE];
	ShmemIndexEnt *shmemIndexEntry;
	uint32		s;
	persHeader	hdr;
	bool		found;

	Page		page;
	BlockNumber	blockNum;
	Size		remainingSpace;
	Size		requiredSpace;
	Size		writtenSize;
	char		*readPtr;


	/*
	 * There hasn't been a relation to hold
	 * SIAS shmem structures yet.
	 *
	 * Create a new relation for this purpose.
	 */
	if (access(persistencyConfigFileDir, F_OK) == -1)
	{
		rfn.spcNode = MyDatabaseTableSpace;
		rfn.dbNode = MyDatabaseId;

		Relation pg_class_desc = heap_open(RelationRelationId, RowExclusiveLock);
		rfn.relNode = GetNewRelFileNode(rfn.spcNode, pg_class_desc);
		heap_close(pg_class_desc, RowExclusiveLock);

		/*
		 * remember RelFileNode via .txt file
		 */
		persConfigFile = fopen(persistencyConfigFileDir, "w");
		fprintf(persConfigFile, "spcNode=%u dbNode=%u relNode=%u", rfn.spcNode, rfn.dbNode, rfn.relNode);
		fclose(persConfigFile);

		/*
		 * create storage
		 */
		persRel = smgropen(rfn);
		smgrcreate(persRel, MAIN_FORKNUM, false);
	}
	else
	{
		persConfigFile = fopen(persistencyConfigFileDir, "r");
		if (fscanf(persConfigFile, "spcNode=%u dbNode=%u relNode=%u", &rfn.spcNode, &rfn.dbNode, &rfn.relNode) == 0)
		{
			fclose(persConfigFile);
			elog(WARNING, "The SiasPersConfig file doesn't contain the required information.");
		}
		fclose(persConfigFile);

		persRel = smgropen(rfn);
	}

	userRelationOids = ShmemInitStruct("userRelationOids",
						(sizeof(Oid) * SIAS_MAX_RELATIONS), &found);

	s = 0;
	/*
	 * all names of shmem structs
	 * concerning VidMappingInfo,
	 * which have to be persisted
	 */
	strcpy(structNames[s++], "VidMappingInfo");
	strcpy(structNames[s++], "BucketRelation");
	/*
	 * all names of shmem structs
	 * concerning TargetBlockInfo,
	 * which have to be persisted
	 */
	strcpy(structNames[s++], "TargetBlockInfo");
#ifdef SIAS_GC
	/*
	 * all names of shmem structs
	 * concerning GarbageCollectionInfo,
	 * which have to be persisted
	 *
	 * place GC structs after all other relation
	 * specific structs, so they can be skipped
	 * easily if GC is disabled
	 */
	strcpy(structNames[s++], "GarbageCollectionInfo");
	strcpy(structNames[s++], "cleanedRegionsQueue");
	strcpy(structNames[s++], "drdList");
	strcpy(structNames[s++], "updateDownCounters");
	strcpy(structNames[s++], "maxUpdaterXmin");
	strcpy(structNames[s++], "newlyUpdatedData");
#endif

	/*
	 * all names of shmem structs
	 * holding general SIAS data,
	 * which have to be persisted
	 */
	strcpy(structNames[s++], "UserRelationIdCounter");
	strcpy(structNames[s++], "userRelationOids");
	strcpy(structNames[s++], "rfnUrIdList");
#ifdef SIAS_GC
	/*
	 * place GC structs after all other
	 * general structs, so they can be skipped
	 * easily if GC is disabled
	 */
	strcpy(structNames[s++], "siasGcShmem");
#endif


	/*
	 * use block 0 for meta data
	 */
	blockNum = 1;
	page = palloc0(BLCKSZ);
	remainingSpace = BLCKSZ;
	printf("-- begin storing --\n");

	/*
	 * relation specific structs
	 */
	for (userRelId = 0; userRelId < numberUserRelations(); userRelId++)
	{
		relOid = userRelationOids[userRelId];

		for (s = 0; s < SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS; s++)
		{
			sprintf(name, "%s %u", structNames[s], relOid);
//			printf("persist %s\n", name);
			shmemIndexEntry = GetShmemIndexEntry(name);

			if (!shmemIndexEntry)
			{
				elog(ERROR, "Couldn't find shmem struct '%s'.", name);
			}

			hdr.size = shmemIndexEntry->size;
			strcpy(hdr.name, name);

			/*
			 * header should always be contained
			 * entirely by one page
			 */
			if (sizeof(persHeader) > remainingSpace)
			{
				smgrextend(persRel, MAIN_FORKNUM, blockNum, page, false);
				blockNum++;
				memset(page, 0, BLCKSZ);
				remainingSpace = BLCKSZ;
			}

			/*
			 * store header
			 */
			memcpy((page + BLCKSZ - remainingSpace), &hdr, sizeof(persHeader));
			remainingSpace -= sizeof(persHeader);

			/*
			 * store struct
			 */
			requiredSpace = shmemIndexEntry->size;
			readPtr = shmemIndexEntry->location;
			while (requiredSpace > 0)
			{
				if (remainingSpace == 0)
				{
					smgrextend(persRel, MAIN_FORKNUM, blockNum, page, false);
					blockNum++;
					memset(page, 0, BLCKSZ);
					remainingSpace = BLCKSZ;
				}

				writtenSize = Min(remainingSpace, requiredSpace);
				memcpy((page + BLCKSZ - remainingSpace), readPtr, writtenSize);
				remainingSpace -= writtenSize;
				requiredSpace -= writtenSize;
				readPtr += writtenSize;
			}
		}
	}

	/*
	 * general structs
	 */
	for (s = SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS; s < (SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS + SIAS_NUM_GENERAL_PERS_STRUCTS); s++)
	{
		sprintf(name, "%s", structNames[s]);
//		printf("persist %s\n", name);
		shmemIndexEntry = GetShmemIndexEntry(name);

		if (!shmemIndexEntry)
		{
			elog(ERROR, "Couldn't find shmem struct '%s'.", name);
		}

		hdr.size = shmemIndexEntry->size;
		strcpy(hdr.name, name);

		/*
		 * header should always be contained
		 * entirely by one page
		 */
		if (sizeof(persHeader) > remainingSpace)
		{
			smgrextend(persRel, MAIN_FORKNUM, blockNum, page, false);
			blockNum++;
			memset(page, 0, BLCKSZ);
			remainingSpace = BLCKSZ;
		}

		/*
		 * store header
		 */
		memcpy((page + BLCKSZ - remainingSpace), &hdr, sizeof(persHeader));
		remainingSpace -= sizeof(persHeader);

		/*
		 * store struct
		 */
		requiredSpace = shmemIndexEntry->size;
		readPtr = shmemIndexEntry->location;
		while(requiredSpace > 0)
		{
			if (remainingSpace == 0)
			{
				smgrextend(persRel, MAIN_FORKNUM, blockNum, page, false);
				blockNum++;
				memset(page, 0, BLCKSZ);
				remainingSpace = BLCKSZ;
			}

			writtenSize = Min(remainingSpace, requiredSpace);
			memcpy((page + BLCKSZ - remainingSpace), readPtr, writtenSize);
			remainingSpace -= writtenSize;
			requiredSpace -= writtenSize;
			readPtr += writtenSize;
		}
	}

	printf("-- end storing --\n");

	if (remainingSpace < BLCKSZ)
	{
		smgrextend(persRel, MAIN_FORKNUM, blockNum, page, false);
	}

	memset(page, 0, BLCKSZ);
	remainingSpace = BLCKSZ;

	/*
	 * write meta data to block 0
	 */
	*((BlockNumber *) page) = blockNum;
	*((uint32 *) (page + sizeof(BlockNumber))) = (SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS * numberUserRelations())
														+ SIAS_NUM_GENERAL_PERS_STRUCTS;
	smgrwrite(persRel, MAIN_FORKNUM, 0, page, false);

	pfree(page);
}

static void
StartupSias()
{
	printf("## StartupSias begin ##\n");
	struct timespec t1, t2;
	clock_gettime(0, &t1);

	if (LoadSiasStructs())
	{
		InitRfnUserRelationIdHashTable(true);
		RelinkSiasStructs();
#ifdef SIAS_GC
		SiasGcShmemInit();
#endif
		clock_gettime(0, &t2);
		unsigned long long t = 1000 * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1000000;
		printf("SIAS startup took %llu ms\n", t);
	}
	printf("## StartupSias end ##\n");
}

/*
 * LoadSiasStructs
 *
 * Reads all Sias structs, which got written to
 * storage during the last shutdown and
 * copies them back to shmem.
 */
static bool
LoadSiasStructs()
{
	FILE 	   *persConfigFile;
	RelFileNode rfn;
	SMgrRelation	persRel;

	BlockNumber	maxBlock;
	uint32		numStructs;

	uint32		s;
	persHeader	hdr;
	Page		page;
	BlockNumber	blockNum;
	Size		remainingSpace;
	Size		readSize;
	Size 		seenSize;
	char		*readPtr;
	char		*writePtr;
	bool		readingDone;
	char		*siasStruct;
	Size		maxStructSize;
	bool		found;
	char		*shmemLocation;


	/*
	 * See if there is a config file
	 * containing the RelFileNode of the
	 * relation holding SIAS shmem
	 * structures, which got stored during
	 * the last shutdown.
	 */
	if (access(persistencyConfigFileDir, F_OK) == -1)
	{
		elog(WARNING, "There is no SiasPersConfig file.");
		return false;
	}
	else
	{
		persConfigFile = fopen(persistencyConfigFileDir, "r");
		if (fscanf(persConfigFile, "spcNode=%u dbNode=%u relNode=%u", &rfn.spcNode, &rfn.dbNode, &rfn.relNode) == 0)
		{
			fclose(persConfigFile);
			elog(ERROR, "The SiasPersConfig file doesn't contain the required information.");
		}
		fclose(persConfigFile);

		persRel = smgropen(rfn);
	}

	s = 0;
	blockNum = 0;
	page = palloc0(BLCKSZ);
	maxStructSize = 10485760;
	siasStruct = palloc0(maxStructSize);
	remainingSpace = BLCKSZ;
	readSize = 0;
	seenSize = 0;
	hdr.size = 0;
	readingDone = false;

	/*
	 * Read the meta data on the first page.
	 */
	smgrread(persRel, MAIN_FORKNUM, 0, page);
	maxBlock = *((BlockNumber *) page);
	numStructs = *((uint32 *) (page + sizeof(BlockNumber)));

	if (smgrnblocks(persRel, MAIN_FORKNUM) <= maxBlock)
	{
		elog(ERROR, "Sias persRel doesn't contain as many blocks as it should. (%u <= %u)", smgrnblocks(persRel, MAIN_FORKNUM), maxBlock);
	}

	printf("-- begin loading --\n");
	/*
	 * Read the persisted SIAS structs and
	 * copy each to shmem.
	 */
	for (blockNum = 1; blockNum <= maxBlock; blockNum++)
	{
		smgrread(persRel, MAIN_FORKNUM, blockNum, page);
		remainingSpace = BLCKSZ;
		readPtr = page;

		while (0 < remainingSpace
				&& !readingDone)
		{
			if (seenSize < hdr.size)
			{
				readSize = Min(remainingSpace, (hdr.size - seenSize));
				memcpy(writePtr, readPtr, readSize);
				writePtr += readSize;
				seenSize += readSize;
				readPtr += readSize;
				remainingSpace -= readSize;

				/*
				 * completely read a Sias struct
				 */
				if (seenSize == hdr.size)
				{
					/*
					 * copy the Sias struct
					 * to a new shmem location
					 */
					shmemLocation = ShmemInitStruct(hdr.name, hdr.size, &found);
//					printf("load %s\n", hdr.name);
					memcpy(shmemLocation, siasStruct, hdr.size);

					if (++s == numStructs)
					{
						readingDone = true;
					}
				}
				else if (seenSize > hdr.size)
				{
					elog(ERROR, "Read beyond struct end while loading Sias struct");
				}
			}
			/*
			 * headers should always be contained
			 * entirely by one page
			 */
			else if (sizeof(persHeader) <= remainingSpace)
			{
				/*
				 * read next header
				 */
				hdr = *((persHeader *) readPtr);

				if (hdr.size > 0)
				{
					readPtr += sizeof(persHeader);
					remainingSpace -= sizeof(persHeader);

					if (maxStructSize < hdr.size)
					{
						maxStructSize = maxStructSize * 2;
						repalloc(siasStruct, maxStructSize);
					}
					memset(siasStruct, 0, maxStructSize);
					writePtr = siasStruct;
					seenSize = 0;
				}
				else
				{
					readingDone = true;
				}
			}
			else
			{
				break;
			}
		}

		if (readingDone)
		{
			break;
		}
	}

	printf("-- end loading --\n");

	if (blockNum < maxBlock)
	{
		elog(ERROR, "Restoring of Sias shmem structs terminated before reaching maxBlock.");
	}

	if (s != numStructs)
	{
		elog(ERROR, "Didn't restore as many Sias shmem structs as demanded.");
	}

	pfree(page);
	pfree(siasStruct);

	return true;
}

/*
 * RelinkSiasStructs
 *
 * Initiates re-linking of the various
 * Sias shmem structs.
 *
 * Assumption: required Sias shmem structs
 * 			   have already been loaded
 */
static void
RelinkSiasStructs()
{
	Relation rel;
	int r;
	Oid *userRelationOids;
	bool found;


	userRelationOids = ShmemInitStruct("userRelationOids",
			(sizeof(Oid) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		elog(ERROR, "Didn't find userRelationOids array to relink Sias structs.");
	}

	for (r = 0; r < SIAS_MAX_RELATIONS; r++)
	{
		if (!OidIsValid(userRelationOids[r]))
		{
			break;
		}
		/*
		 * Just open each Sias relation.
		 *
		 * The RelationAllocate*Info methods will
		 * do the actual re-linking of the various
		 * shmem structs.
		 */
		rel = relation_open(userRelationOids[r], AccessExclusiveLock);

//		printf("after relink %u-%u (%u), %u-%u, %u-%u, %u, %u-%u\n",
//				rel->rd_tbi->firstPrevAppendBlock, rel->rd_tbi->firstPrevAppendBlock+SIAS_APPEND_SIZE-1,
//				rel->rd_tbi->highestWrittenBlockPrevAppend,
//				rel->rd_tbi->firstNextAppendBlock, rel->rd_tbi->firstNextAppendBlock+SIAS_APPEND_SIZE-1,
//				rel->rd_tbi->firstOverflowBlock, rel->rd_tbi->firstOverflowBlock+SIAS_APPEND_SIZE-1,
//				rel->rd_tbi->highestUsedBlock,
//				rel->rd_gci->coldWriteRegion, rel->rd_gci->coldWriteRegion+SIAS_APPEND_SIZE-1);

		relation_close(rel, AccessExclusiveLock);
	}
}

void
SetPersistencyConfigFileDir(char *dataDir)
{
	sprintf(persistencyConfigFileDir, "%s/%s", dataDir, SIAS_PERSISTENCY_CONFIG_FILE);
}



/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */


static void
TerminateSiasPersWorker(void)
{
	ResourceOwner resOwner;


	if (CurrentResourceOwner != NULL)
	{
		resOwner = CurrentResourceOwner;
		CurrentResourceOwner = NULL;
		ResourceOwnerDelete(resOwner);
	}

	printf("SiasPers worker dies - PID %i\n", MyProcPid);
	kill(PostmasterPid, SIGCHLD);
	proc_exit(0);
}

bool
IsSiasPersWorkerProcess(void)
{
	return am_SiasPers_worker;
}
