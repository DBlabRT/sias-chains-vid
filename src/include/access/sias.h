/*
 * sias.h
 *
 */

#ifndef SIAS_H
#define SIAS_H

#define SIAS

// general parameter
#define SIAS_MAX_BLKNUM RELSEG_SIZE*2
#define SIAS_MAX_LINP_OFFNUM 250
#define SIAS_MAX_RELATIONS 40
#define SIAS_FIRST_VID ((uint32) 1)
#define SIAS_MAX_REGIONS 100000
#define SIAS_MAX_VISIBLE_VERSIONS 20	/* upper bound on the number of concurrently visible tuple versions of a data item */

// target block management parameter
#define SIAS_APPEND_SIZE 256 /* should equal erase unit size of flash drive */
#define SIAS_CONCURRENT_TARGET_BLOCKS 10 /* > 0 */
#define SIAS_OVERFLOW_TARGET_BLOCKS Min(3 * SIAS_CONCURRENT_TARGET_BLOCKS, SIAS_APPEND_SIZE - 1) /* 0 < SIAS_OVERFLOW_TARGET_BLOCKS < SIAS_APPEND_SIZE*/
#define SIAS_TOTAL_ACTIVE_TARGET_BLOCKS (SIAS_APPEND_SIZE + SIAS_OVERFLOW_TARGET_BLOCKS)
#define SIAS_InvalidSlotNumber ((int) -1)
#define SIAS_MAX_REPLACE_TRIES 1 /* > 0 */
#define SIAS_FIRST_INITIAL_TARGET_BLOCK ((SIAS_MIN_CLEANED_REGIONS + 1) * SIAS_APPEND_SIZE) /* all blocks in front of this are reserved for garbage collection initially */

// bucket buffer management parameter
#define SIAS_BUFFER_POOL_SIZE 1024
#define SIAS_BUFFER_POOL_PARTITIONS 128
#define SIAS_BUCKET_SIZE 1024
#define SIAS_BUCKET_ENTRY_SIZE 6

// garbage collection parameter
#define SIAS_GC
//#define SIAS_GC_TEST
#define SIAS_GARBAGE_THRESHOLD_HOT 60	/* percentage of updated tuples within a region of hot data at which garbage collection is initiated for that region */
#define SIAS_GARBAGE_THRESHOLD_COLD 40	/* percentage of updated tuples within a region of cold data at which garbage collection is initiated for that region */
#define SIAS_MIN_CLEANED_REGIONS 10 	/* > 0, minimum number of cleaned regions, which must always be in gci->cleanedRegionsQueue */
#define SIAS_MAX_QUEUED_REGIONS 1024
#define SIAS_UPDATE_XMIN_INTERVAL 10	/* every SIAS_UPDATE_XMIN_INTERVAL many GC'ed blocks, the cut off tx-id is updated; the smaller this parameter, the more tuple versions are classified invisible */
#define SIAS_GC_SLEEP_INTERVAL 1000000L	/* How many microseconds should the GC worker process sleep after it has done all work? */
#define SIAS_GC_MAX_IDLE_PASSES 100		/* How many consecutive passes may a SiasGc worker stay without work before it shuts down? */
#define SIAS_INITIAL_COLD_REGION 0		/* first block of the region, which is the first region to write cold data to */
#define SIAS_MAX_NEWLY_UPDATED_DATA BLCKSZ	/* Maximum amount of updated tuples within a region that is
 	 	 	 	 	 	 	 	 	 		acceptable to still start/continue GC (This is only used heuristically.) */
#define SIAS_GC_RUN_DURATION_FILE "SiasGcRunDuration.txt"

// shutdown/restart parameter
#define SIAS_PERSISTENCY_CONFIG_FILE "SiasPersConfig.txt"
#define SIAS_PERS_DB_NAME "dbt2"
#ifdef SIAS_GC
	#define SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS 9	/* number of relation specific structs in shared memory, that have to be persisted at shutdown and restored during startup */
#else
	#define SIAS_NUM_REL_SPECIFIC_PERS_STRUCTS 3	/* number of relation specific structs in shared memory, that have to be persisted at shutdown and restored during startup */
#endif
#ifdef SIAS_GC
	#define SIAS_NUM_GENERAL_PERS_STRUCTS 4			/* number of general SIAS structs in shared memory, that have to be persisted at shutdown and restored during startup */
#else
	#define SIAS_NUM_GENERAL_PERS_STRUCTS 3			/* number of general SIAS structs in shared memory, that have to be persisted at shutdown and restored during startup */
#endif


#include "postgres.h"
#include "utils/relcache.h"
#include "storage/buf_internals.h"
#include "storage/bufpage.h"
#include "storage/block.h"
#include "storage/off.h"
#include "storage/s_lock.h"
#include "storage/itemptr.h"

/*
 * A region comprises SIAS_APPEND_SIZE many
 * consecutively BlockNumber'ed blocks.
 *
 * The first region of a fork begins at block 0,
 * the second region of a fork begins at block SIAS_APPEND_SIZE,
 * and so on.
 * -> region are distinct, they don't overlap
 */
typedef uint32 RegionNumber;
#define InvalidRegionNumber		((RegionNumber) 0xFFFFFFFF)
#define MaxRegionNumber			((RegionNumber) 0xFFFFFFFE)

typedef enum
{
	CACHE_NONE,			/* not cached */
	CACHE_HOT,			/* cached by append in hot region */
	CACHE_COLD			/* cached by garbage collection in cold region */
} CacheLocation;

typedef struct ItemVid
{
	uint32 vid;
	uint32 versionId;
} ItemVid;

typedef struct UserRelationIdCounter
{
	// 0-based
	uint32 	nextUserRelationId;
	slock_t	mutex;
} UserRelationIdCounter;

typedef struct RegionsQueue
{
	BlockNumber *elements;
	int head;
	int tail;
	int numElements;
	slock_t mutex;
} RegionsQueue;

typedef struct GcStatistics
{
	uint32 numTuplesTotal;
	uint32 numDistinctVids;	/* only correct, if GC isn't interrupted or seenVids is always restored after each break */
	uint32 numGcRuns;
	uint32 numHotWrites;
	uint32 numInplace;
	uint32 numColdWrites;
} GcStatistics;

typedef struct DirtyRegionData
{
	BlockNumber firstBlock;		/* first block of the to-be-GC'ed region */
	TransactionId prevMaxUpdaterXmin;	/* xmin cut off value has to be greater than this to allow GC to continue with this region */
	BlockNumber nextBlock;		/* next block in the region to be GC'ed */
	float tuplesPerBlock;			/* only an approximation */
	GcStatistics stats;
	unsigned long long elapsedMillisecs;
} DirtyRegionData;

typedef struct DirtyRegionsQueue
{
	DirtyRegionData *elements;
	int head;
	int tail;
	int numElements;
	slock_t mutex;
} DirtyRegionsQueue;


/*
 * Encapsulates all objects used to
 * work on VID-TID-mapping.
 */
typedef struct VidMappingInfo
{
	bool needRebuild;

	Relation bucketRelation;	/* allows to access bucket blocks of the VID-TID-mapping */
	slock_t vidIncrementMutex;	/* ensures, that only one tx tries to acquire a new VID at a time*/
	ItemVid curVid;	/* highest VID assigned so far*/


	BufferDesc *bufferDescriptors;
	char	   *bufferBlocks;
	BufferStrategyControl *strategyControl;
	LWLockId *bufferPartitionLocks;
	LWLockId *bufferVidLocks;
	slock_t  *bufferVidExclusiveLocks;
	LWLockId bufFreelistLock;
	uint32	userRelationId;		/* 0-based consecutive numbering of all user relations */

	BlockNumber highestUsedBlock;
	slock_t highestUsedBlockMutex;

	BlockNumber highestWrittenBlock;
	slock_t highestWrittenBlockMutex;
} VidMappingInfo;

/*
 * Encapsulates all objects used to
 * manage the target blocks of a
 * user relation in order to
 * realize the append approach.
 */
typedef struct TargetBlockInfo
{
	bool needRebuild;

	BlockNumber firstPrevAppendBlock;	/* first block of the region, which has been issued to be written last */
	BlockNumber firstNextAppendBlock;	/* first block of the region, which will be issued to be written next */
	BlockNumber firstOverflowBlock;		/* first block of the region, whose first SIAS_OVERFLOW_TARGET_BLOCKS many blocks */

	LWLockId blockNumberMappingLock;	/* prevents inconsistency during mapping of a BlockNumber to an offset (and vice versa)
	 	 	 	 	 	 	 	 	 	or a cache position due to concurrently running manipulations of firstActiveTargetBlock
	 	 	 	 	 	 	 	 	 	or manipulations of the caches */

	BlockNumber *targetBlockSlots;		/* all blocks currently available for (potentially concurrent) writes */
	LWLockId *slotLocks;				/* protects the respective target block slot; only holder of exclusive lock
										is allowed to write to the target block which is currently assigned to the target
										block slot it has locked; other processes might queue up to get this lock */

	int *slotsInUse;					/* list of all the indexes of target block slots, which are used currently */
	int *unusedSlots;					/* list of all the indexes of target block slots, which aren't used currently */
	int numUnusedSlots;					/* # target block slots, which aren't assigned to any process */
	BlockNumber *previouslyUsedBlocks;	/* list of all target blocks, which are assigned to an unused target block slot */
	int numPreviouslyUsedBlocks;		/* # target blocks in previouslyUsedBlocks */
	int *slotUsers;						/* one entry for each target block slot; To how many processes (#active users <=1,
	 	 	 	 	 	 	 	 	 	#waiting users >=0) is the respective target block slot currently assigned? */
	slock_t slotManagementMutex;		/* protects the 6 members above */


	int nextSlotPointer;				/* index into slotsInUse pointing to the slot,
										which will be considered next to be offered to a writing process,
										in case this process neither requests a specific target block
										nor are there any unused slots */
	LWLockId assignmentLock;			/* realizes mutual exclusion of processes during the assignment of target block slots */

	int fullNonOverflowTargetBlocks;	/* # full blocks among the first SIAS_APPEND_SIZE many active target blocks */
	int fullOverflowTargetBlocks;		/* # full blocks among the last SIAS_OVERFLOW_TARGET_BLOCKS many active target blocks */

	bool *targetBlockInitialized;		/* one entry for each active target block; true, if PageInit() has been called already */

	int *targetBlockSlotIndex;			/* one entry for each active target block, which is the target block's position
										in targetBlockSlots or SIAS_InvalidSlotNumber otherwise */
	slock_t *tbsiMutexes;				/* each of these mutexes protects the respective entry in targetBlockSlotIndex */
	LWLockId appendLock;				/* operations, which disallow an append to run concurrently, acquire
	 	 	 	 	 	 	 	 	 	this lock in shared mode; an append itself needs to lock it exclusively;
	 	 	 	 	 	 	 	 	 	This way the append has to wait for other operations to finish. */
	LWLockId runningAppendLock;			/* held until end of physical append, so the next append can wait for the
										previous append to finish */
	BlockNumber highestAppendedBlock;	/* highest BlockNumber'ed block, which has been appended */
	slock_t highestAppendedBlockMutex;	/* protects highestAppendedBlock (not necessary in the moment) */
	LWLockId replacementLock;			/* required to mutually exclude different processes from the
	 	 	 	 	 	 	 	 	 	replacement of full target blocks by new ones */

	char *prevAppendCache;				/* points to the cache, that stores the target blocks, which are/were
										appended by the last append, while they aren't in a buffer */
	char *nextAppendCache;				/* points to the cache, that stores the target blocks, which will be
										appended by the next append, while they aren't in a buffer */
	char *overflowCache;				/* points to the cache, that stores the overflow target blocks,
										while they aren't in a buffer */
	/*
	 * static pointers to physical cache locations; these should never be used directly,
	 * instead use the three pointers above
	 */
	char *cache1;
	char *cache2;
	char *cache3;

	BlockNumber highestUsedBlock;		/* highest BlockNumber'ed block, that has been at least initialized;
	 	 	 	 	 	 	 	 	 	it is the highest known block of a relation that might contain useful data */
	slock_t highestUsedBlockMutex;		/* protects highestUsedBlock */

	int highestUsedTargetBlockIndex;	/* highest active target block index of any used target block
	 	 	 	 	 	 	 	 	 	during the current append pass */

	BlockNumber highestAssignedRegion;	/* first block of the region, which is the highest BlockNumber'ed region,
	 	 	 	 	 	 	 	 	 	that has ever been considered for any use */

	BlockNumber highestWrittenBlockPrevAppend;	/* highest BlockNumber'ed block of the region starting at firstPrevAppendBlock,
												that has been written during the previous (perhaps still running) physical append pass */
	slock_t highestWrittenBlockPrevAppendMutex;	/* protects highestWrittenBlockPrevAppend */

	int numAppends;						/* # initiated appends */
	bool prevAppendComplete;
} TargetBlockInfo;

/*
 * Encapsulates all objects used to
 * do garbage collection and manage
 * reusable regions.
 */
typedef struct GarbageCollectionInfo
{
	bool needRebuild;

	BlockNumber coldWriteRegion;		/* first block of the region, which is currently used by the garbage
	 	 	 	 	 	 	 	 	 	collection to relocate cold tuples to */
	BlockNumber coldWriteBlock;			/* block inside coldWriteRegion, that is currently used to write cold data */
	bool coldWriteBlockInited;			/* Has InitSiasBuffer(..) been called for coldWriteBlock yet? */

	LWLockId coldWriteRegionLock;		/* Has to be acquired in shared mode by anybody, who has to prevent the
	 	 	 	 	 	 	 	 	 	coldWriteRegion from being changed. The GC needs to lock it exclusively
	 	 	 	 	 	 	 	 	 	in order to switch coldWriteRegion to another region. */

	RegionsQueue cleanedRegionsQueue;

	HTAB *dirtyRegions;
	int numDirtyRegions;
	slock_t dirtyRegionsMutex;

	int *updateDownCounters;		/* one entry for each region, counts down the remaining number of updates/deletes
	 	 	 	 	 	 	 	 	of tuples within that region until it's going to be issued for garbage collection */
	TransactionId *maxUpdaterXmin;	/* one entry for each region r, tracks the maximum of all direct successor
	 	 	 	 	 	 	 	 	(of a version inside r) versions' t_xmin values */
	uint32 *newlyUpdatedData;		/* one entry for each region r, tracks the cumulative length of all tuples in r,
	 	 	 	 	 	 	 	 	which have already been updated, but could still be visible when GC happens */
	slock_t *regionGcCountersMutexes;	/* one entry for each region, protecting its updateDownCounters and maxUpdaterXmin entry */

	char *coldCache;					/* stores the blocks, which belong to coldWriteRegion, while they aren't buffered */

	BlockNumber highestWrittenColdBlock;	/* highest BlockNumber'ed block of the region starting at coldWriteRegion,
											that has been written since we selected this region as coldWriteRegion */
	slock_t highestWrittenColdBlockMutex;	/* protects highestWrittenColdBlock */

	bool hasSiasGcWorker;

	uint32 numGcHot;				/* number of hot regions, that have been GC'ed */
	uint32 numGcCold;				/* number of cold regions, that have been GC'ed */
	unsigned long long totalGcDurationMillisecs;

	bool prevAppendComplete;

} GarbageCollectionInfo;

/*
 * makes bucket buffers, used for different user relations,
 * distinguishable by coupling them with a pointer to their
 * user relation's VidMappingInfo object.
 * Currently only used as a container for bucketbuffers array in resowner.c
 */
typedef struct BucketBufferId
{
	VidMappingInfo 	*vmi;
	Buffer			buffer;
} BucketBufferId;

typedef struct rfnHashLookupEntry
{
	RelFileNode rfn;
	uint32 userRelationId;
} rfnHashLookupEntry;

HTAB *rfnUserRelationIdHashTable;

typedef struct oldestVersionEntry
{
	uint32 vid;
	ItemPointerData tid;
} oldestVersionEntry;

typedef struct persHeader
{
	Size size;
	char name[SHMEM_INDEX_KEYSIZE];
} persHeader;

#define setTupleValidMacro(invalidationVector, blockNumber, offsetNumber) \
	do { \
		Assert((blockNumber) < SIAS_MAX_BLKNUM); \
		Assert((offsetNumber) <= SIAS_MAX_LINP_OFFNUM); \
		int recordNum = ((blockNumber) * SIAS_MAX_LINP_OFFNUM) + (offsetNumber) - 1; \
		Assert(!isTupleValidMacro((invalidationVector), recordNum)); \
		int byteNum = (recordNum) / 8; \
		(invalidationVector)->bits[byteNum] |= (0x1 << (7 - ((recordNum) % 8))); \
		if (byteNum > (invalidationVector)->endOffset) \
			(invalidationVector)->endOffset = byteNum; \
	} while (0)

#define setTupleInvalidMacro(invalidationVector, blockNumber, offsetNumber) \
	do { \
		Assert((blockNumber) < SIAS_MAX_BLKNUM); \
		Assert((offsetNumber) <= SIAS_MAX_LINP_OFFNUM); \
		int recordNum = ((blockNumber) * SIAS_MAX_LINP_OFFNUM) + (offsetNumber) - 1; \
		Assert(isTupleValidMacro((invalidationVector), recordNum)); \
		int byteNum = (recordNum) / 8; \
		(invalidationVector)->bits[byteNum] &= (0xFF ^ (0x1 << (7 - ((recordNum) % 8)))); \
	} while (0)



#define getTidFromVidMacro(relation, vid, blockNumberPointer, offsetNumberPointer) \
	do { \
		ItemPointer itemPointer = palloc(sizeof(ItemPointerData)); \
		getItemPointerFromVidArray(relation, vid, itemPointer); \
		*blockNumberPointer = ItemPointerGetBlockNumber(itemPointer); \
		*offsetNumberPointer = ItemPointerGetOffsetNumber(itemPointer); \
	} while (0)

// bucket number = its BlockNumber
// BlockNumber is 0-based
#define getBucketNumberFromVidMacro(vid) \
	((vid - 1) / SIAS_BUCKET_SIZE)

// 1-based offset within bucket
#define getBucketOffsetFromVidMacro(vid) \
	(((vid - 1) % SIAS_BUCKET_SIZE) + 1)

#define lockVidMacro(rel, vid, lockmode) \
	do { \
		ItemPointer tid = malloc(sizeof(ItemPointerData)); \
		ItemPointerSetBlockNumber(tid, (BlockNumber) getBucketNumberFromVidMacro(vid)); \
		ItemPointerSetOffsetNumber(tid, (OffsetNumber) getBucketOffsetFromVidMacro(vid)); \
		LockTuple(rel, tid, lockmode); \
	} while (0)

#define unlockVidMacro(rel, vid, lockmode) \
	do { \
		ItemPointer tid = malloc(sizeof(ItemPointerData)); \
		ItemPointerSetBlockNumber(tid, (BlockNumber) getBucketNumberFromVidMacro(vid)); \
		ItemPointerSetOffsetNumber(tid, (OffsetNumber) getBucketOffsetFromVidMacro(vid)); \
		UnlockTuple(rel, tid, lockmode); \
	} while (0)

// maps a given bucket buffer number of a given user
// relation to the corresponding position in BucketBufferPrivateRefCount
#define getOverallBucketBufferNumberMacro(userRelationId, buf) \
	((SIAS_BUFFER_POOL_SIZE * (userRelationId)) + (buf) - 1)

/*
 * returns the LWLockId of a Vid lock for a
 * VID-TID entry, which currently resides in bucket
 * buffer "buf" at position "offsetNumber"
 */
#define getVidLockIdMacro(bufferVidLocks, buf, offsetNumber) \
	(bufferVidLocks[(SIAS_BUCKET_SIZE * ((buf) - 1)) + ((offsetNumber) - 1)])

#define advanceIndexPointer(indexPointer, numberIndexedElements) \
	do { \
		if (++(*(indexPointer)) >= numberIndexedElements) \
				((*(indexPointer)) = 0); \
	} while(0)

#define GetTargetBlockNumber(tbi, offset) \
(\
		AssertMacro(0 <= (offset)), \
		AssertMacro((offset) < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS), \
		((offset) < SIAS_APPEND_SIZE) ? \
			((tbi)->firstNextAppendBlock + (offset)) \
		: \
		  	((tbi)->firstOverflowBlock + (offset) - SIAS_APPEND_SIZE) \
)

#define GetTargetBlockOffset(tbi, blockNumber) \
(\
	((tbi)->firstNextAppendBlock <= (blockNumber) \
			&& (blockNumber) < (tbi)->firstNextAppendBlock + SIAS_APPEND_SIZE) ? \
					((blockNumber) - (tbi)->firstNextAppendBlock) \
	: \
	  	  ((tbi)->firstOverflowBlock <= (blockNumber) \
	  			&& (blockNumber) < (tbi)->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS) ? \
	  					((blockNumber) - (tbi)->firstOverflowBlock + SIAS_APPEND_SIZE) \
		  : \
						(-1) \
)

#define IsActiveTargetBlock(tbi, blockNumber) \
(\
	(((tbi)->firstNextAppendBlock <= (blockNumber) \
		&& (blockNumber) < (tbi)->firstNextAppendBlock + SIAS_APPEND_SIZE) \
	|| ((tbi)->firstOverflowBlock <= (blockNumber) \
			&& (blockNumber) < (tbi)->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS)) \
)

#define IsOverflowBlock(tbi, blockNumber) \
(\
	((tbi)->firstOverflowBlock <= (blockNumber) \
			&& (blockNumber) < (tbi)->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS) \
)

#define PointerOfCachedTargetBlock(tbi, blockNumber) \
(\
	((tbi)->firstPrevAppendBlock <= (blockNumber) \
			&& (blockNumber) < (tbi)->firstPrevAppendBlock + SIAS_APPEND_SIZE) ? \
					((tbi)->prevAppendCache + (((blockNumber) - (tbi)->firstPrevAppendBlock) * BLCKSZ)) \
	: \
		((tbi)->firstNextAppendBlock <= (blockNumber) \
				&& (blockNumber) < (tbi)->firstNextAppendBlock + SIAS_APPEND_SIZE) ? \
						((tbi)->nextAppendCache + (((blockNumber) - (tbi)->firstNextAppendBlock) * BLCKSZ)) \
		: \
			  ((tbi)->firstOverflowBlock <= (blockNumber) \
					&& (blockNumber) < (tbi)->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS) ? \
							((tbi)->overflowCache + (((blockNumber) - (tbi)->firstOverflowBlock) * BLCKSZ)) \
			  : \
							(NULL) \
)

#define ReadCachedTargetBlockIntoBuffer(tbi, blockNumber, bufferPtr) \
		do { \
			LWLockAcquire((tbi)->blockNumberMappingLock, LW_SHARED); \
			char *cachePtr = PointerOfCachedTargetBlock(tbi, blockNumber); \
			LWLockRelease((tbi)->blockNumberMappingLock); \
			memcpy((bufferPtr), cachePtr, BLCKSZ); \
		} while(0)

#ifdef SIAS_GC
#define CacheSiasBlock(rfnPtr, blockNum, bufferPtr) \
		do { \
			char *cachePtr; \
			TargetBlockInfo *tbi = mapRelFileNodeToTbi(rfnPtr); \
			LWLockAcquire((tbi)->blockNumberMappingLock, LW_SHARED); \
			if ((tbi->firstPrevAppendBlock <= (blockNum) \
				&& (blockNum) < tbi->firstPrevAppendBlock + SIAS_APPEND_SIZE) \
					|| IsActiveTargetBlock(tbi, blockNum)) \
			{ \
				cachePtr = PointerOfCachedTargetBlock(tbi, blockNum); \
				if (cachePtr == NULL) \
				{ \
					elog(ERROR, "cachePtr invalid (SIAS hot cache)"); \
				} \
				LWLockRelease((tbi)->blockNumberMappingLock); \
			} \
			else \
			{ \
				LWLockRelease((tbi)->blockNumberMappingLock); \
				GarbageCollectionInfo *gci = mapRelFileNodeToGci(rfnPtr); \
				LWLockAcquire(gci->coldWriteRegionLock, LW_SHARED); \
				if (gci->coldWriteRegion <= (blockNum) \
						&& (blockNum) < gci->coldWriteRegion + SIAS_APPEND_SIZE) \
				{ \
					cachePtr = gci->coldCache + (((blockNum) - gci->coldWriteRegion) * BLCKSZ); \
				} \
				else \
				{ \
					elog(ERROR, "trying to cache SIAS page, which isn't used for writes right now %u %u %u %u %u %u", (blockNum), gci->coldWriteRegion, tbi->firstPrevAppendBlock, tbi->firstNextAppendBlock, tbi->firstOverflowBlock, PageIsSiasInplace(bufferPtr)); \
				} \
				LWLockRelease(gci->coldWriteRegionLock); \
			} \
			memcpy(cachePtr, (bufferPtr), BLCKSZ); \
		} while(0)
#else
#define CacheSiasBlock(rfnPtr, blockNum, bufferPtr) \
		do { \
			char *cachePtr; \
			TargetBlockInfo *tbi = mapRelFileNodeToTbi(rfnPtr); \
			LWLockAcquire((tbi)->blockNumberMappingLock, LW_SHARED); \
			if ((tbi->firstPrevAppendBlock <= (blockNum) \
				&& (blockNum) < tbi->firstPrevAppendBlock + SIAS_APPEND_SIZE) \
					|| IsActiveTargetBlock(tbi, blockNum)) \
			{ \
				cachePtr = PointerOfCachedTargetBlock(tbi, blockNum); \
			} \
			else \
			{ \
				elog(ERROR, "trying to cache SIAS page, which isn't used for writes right now"); \
			} \
			LWLockRelease((tbi)->blockNumberMappingLock); \
			memcpy(cachePtr, (bufferPtr), BLCKSZ); \
		} while(0)
#endif

#define ValidSlotIndex(slot) \
	(0 <= (slot) && (slot) < SIAS_CONCURRENT_TARGET_BLOCKS)

#define GetTargetBlockSlotIndex(tbi, activeIndex, resultPtr) \
		do { \
			int activeIndexEvaluated = (activeIndex); \
			SpinLockAcquire(&((tbi)->tbsiMutexes[(activeIndexEvaluated)])); \
			*(resultPtr) = (tbi)->targetBlockSlotIndex[(activeIndexEvaluated)]; \
			SpinLockRelease(&((tbi)->tbsiMutexes[(activeIndexEvaluated)])); \
		} while(0)

#define SetTargetBlockSlotIndex(tbi, activeIndex, newValue) \
		do { \
			int activeIndexEvaluated = (activeIndex); \
			SpinLockAcquire(&((tbi)->tbsiMutexes[(activeIndexEvaluated)])); \
			(tbi)->targetBlockSlotIndex[(activeIndexEvaluated)] = (newValue); \
			SpinLockRelease(&((tbi)->tbsiMutexes[(activeIndexEvaluated)])); \
		} while(0)

#define RemoveArrayEntry(arrayPtr, elementSize, element, numElements, indexPtr, caller) \
		do { \
			int i; \
			typeof(arrayPtr) arrayPtrEval = (arrayPtr); \
			int elementSizeEval = (elementSize); \
			typeof(arrayPtr[0]) elementEval = (element); \
			int numElementsEval = (numElements); \
			int *indexPtrEval; \
			Assert(0 < numElementsEval); \
			\
			for (i = 0; i < numElementsEval; i++) \
			{ \
				if (arrayPtrEval[i] == elementEval) \
					break; \
			} \
			if (i == numElementsEval) \
			{ \
				for (i = 0; i < numElementsEval; i++) \
				{ \
					printf("%i ", arrayPtrEval[i]); \
				} \
				printf("\nsearching %i\n", elementEval); \
				elog(ERROR, "%u array doesn't contain the to-be-removed element", caller); \
			} \
			if ((indexPtr) != NULL) \
			{ \
				indexPtrEval = ((int*) (indexPtr)); \
				*(indexPtrEval) = i; \
			} \
			memmove(arrayPtrEval + i, \
					arrayPtrEval + i + 1, \
					(numElementsEval - i - 1) * elementSizeEval); \
		} while(0)

#define NumUsedSlots(tbi) \
	(SIAS_CONCURRENT_TARGET_BLOCKS - (tbi)->numUnusedSlots)

#define ValidNextSlotPointer(tbi) \
	(0 <= (tbi)->nextSlotPointer && (tbi)->nextSlotPointer < NumUsedSlots(tbi))

#define GetBlocksRegionNumber(block) \
	((block) / (SIAS_APPEND_SIZE))

#define BelongsToRegion(firstBlockOfRegion, block) \
	((firstBlockOfRegion) <= (block) && (block) < (firstBlockOfRegion) + SIAS_APPEND_SIZE)

#ifdef SIAS_GC
#define UpdateRegionGcCounters(rel, blockNum, updaterXmin, len) \
	do { \
		RegionNumber regionOfBlock; \
		regionOfBlock = GetBlocksRegionNumber(blockNum); \
		SpinLockAcquire(&((rel)->rd_gci->regionGcCountersMutexes[regionOfBlock])); \
		(rel)->rd_gci->maxUpdaterXmin[regionOfBlock] = Max((rel)->rd_gci->maxUpdaterXmin[regionOfBlock], (updaterXmin)); \
		(rel)->rd_gci->newlyUpdatedData[regionOfBlock] += (len); \
		if (--(rel)->rd_gci->updateDownCounters[regionOfBlock] == 0) \
		{ \
			RequestGC(rel, regionOfBlock * SIAS_APPEND_SIZE); \
		} \
		SpinLockRelease(&((rel)->rd_gci->regionGcCountersMutexes[regionOfBlock])); \
	} while(0)
#else
#define UpdateRegionGcCounters(rel, blockNum, updaterXmin, len)
#endif

#define IsTargetBlockInitialized(rel, blockNum) \
	((rel)->rd_tbi->targetBlockInitialized[GetTargetBlockOffset((rel)->rd_tbi, blockNum)])

#define SetTargetBlockInitialized(rel, blockNum) \
	(rel)->rd_tbi->targetBlockInitialized[GetTargetBlockOffset((rel)->rd_tbi, blockNum)] = true;

#define UpdateHighestUsedBlock(tbi, blockNum) \
	do { \
		SpinLockAcquire(&((tbi)->highestUsedBlockMutex)); \
		if ((tbi)->highestUsedBlock < (blockNum) || (tbi)->highestUsedBlock == InvalidBlockNumber) \
		{ \
			(tbi)->highestUsedBlock = (blockNum); \
		} \
		SpinLockRelease(&((tbi)->highestUsedBlockMutex)); \
	} while(0)

#define UpdateHighestAppendedBlock(tbi, blockNum) \
	do { \
		SpinLockAcquire(&((tbi)->highestAppendedBlockMutex)); \
		if ((tbi)->highestAppendedBlock < (blockNum) || (tbi)->highestAppendedBlock == InvalidBlockNumber) \
		{ \
			(tbi)->highestAppendedBlock = (blockNum); \
		} \
		SpinLockRelease(&((tbi)->highestAppendedBlockMutex)); \
	} while(0)

#define ResetRegionsGcCounters(rel, blockNum) \
	do { \
		RegionNumber regionOfBlock; \
		regionOfBlock = GetBlocksRegionNumber(blockNum); \
		SpinLockAcquire(&((rel)->rd_gci->regionGcCountersMutexes[regionOfBlock])); \
		(rel)->rd_gci->updateDownCounters[regionOfBlock] = 0; \
		(rel)->rd_gci->maxUpdaterXmin[regionOfBlock] = 0; \
		(rel)->rd_gci->newlyUpdatedData[regionOfBlock] = 0; \
		SpinLockRelease(&((rel)->rd_gci->regionGcCountersMutexes[regionOfBlock])); \
	} while (0)


extern uint32 numberUserRelations(void);

extern uint32 nextUserRelationId(void);

extern VidMappingInfo** getUserRelationVmiPointerArray(void);

extern void RelationAllocateVidMappingInfo(Relation rel);

extern void RelationAllocateTargetBlockInfo(Relation rel);

extern void RelationAllocateGarbageCollectionInfo(Relation rel);

extern bool IsAssignedTargetBlock(Relation rel, BlockNumber blockNum);

extern int AccessTargetBlockSlot(Relation rel, BlockNumber otherBlock, bool forInsert,
		Buffer otherBuffer, bool *otherBufferUnlocked);

extern void ReleaseTargetBlockSlot(Relation rel, int slot);

extern void ReplaceTargetBlock(Relation rel, int slotOfTargetBlock, BlockNumber targetBlock, bool usedReplacementAllowed);

extern void WriteRegion(Relation rel, BlockNumber firstBlock, bool hotData, bool isOverflowRegion);

extern bool HasEverBeenAppended(TargetBlockInfo *tbi, BlockNumber blockNum);

extern CacheLocation IsCached(Relation rel, BlockNumber blockNum, bool checkCold);

extern Page GetInitBucket(void);

extern void getItemPointerFromVidArray(Relation rel, uint32 vid, ItemPointer iptr);

extern void insertVidTidPair(Relation rel, uint32 vid, BlockNumber blkNum, OffsetNumber offNum);

extern ItemPointer BucketReadItemPointer(Page bucket, OffsetNumber offsetNumber);

extern uint32 getNewVid(Relation rel);

extern bool isTupleValid(Relation rel, uint32 vid, ItemPointer itemPointer);

extern BlockNumber GetWriteableRegion(Relation rel, bool forHotData);

extern BlockNumber GetNeverUsedRegion(Relation rel);

extern BlockNumber GetCleanedRegion(Relation rel, bool forHotData);

extern void AddCleanedRegion(Relation rel, BlockNumber firstBlockOfRegion);

extern TargetBlockInfo* mapRelFileNodeToTbi(RelFileNode *rfn);

extern GarbageCollectionInfo* mapRelFileNodeToGci(RelFileNode *rfnPtr);

extern Size SiasShmemSize(void);

extern void InitRfnUserRelationIdHashTable(bool rebuild);

extern void InitSharedSIASobjects(void);

extern void PrintData(Pointer start, int length);


#endif /* SIAS_H */
