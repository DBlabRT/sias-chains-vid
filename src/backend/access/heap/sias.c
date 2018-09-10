#include "postgres.h"

#include "access/heapam.h"
#include "access/sias.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/block.h"
#include "storage/off.h"
#include "storage/spin.h"
#include "storage/smgr.h"
#include "storage/lock.h"
#include <time.h>



static void RememberVmiPointer(uint32 userRelationId, VidMappingInfo *vmi);
static void RememberTbiPointer(uint32 userRelationId, TargetBlockInfo *tbi);
static void RememberGciPointer(uint32 userRelationId, GarbageCollectionInfo *gci);
static void RememberRelOid(uint32 userRelationId, Oid oid);
static void RememberUserRelationIdOfRelFileNode(RelFileNode *rfnPtr, uint32 userRelationId);
static void InitUserRelationIdCounter(void);
static void InitUserRelationVmis(void);
static void InitUserRelationTbis(void);
static void InitUserRelationGcis(void);
static void InitUserRelationOids(void);
static Size VmiShmemSize(void);
static Size TbiShmemSize(void);
static Size GciShmemSize(void);


/*
 * both get allocated and initialized by the postmaster;
 * backends only attach to these structures,
 * which reside in shared memory
 */
static UserRelationIdCounter  *uric;
static VidMappingInfo **userRelationVmis;
static TargetBlockInfo **userRelationTbis;
static GarbageCollectionInfo **userRelationGcis;
static Oid *userRelationOids;
static Page initBucket = NULL;




uint32
numberUserRelations(void)
{
	return uric->nextUserRelationId;
}

uint32
nextUserRelationId(void)
{
	uint32 nextId;


	SpinLockAcquire(&(uric->mutex));

	// several arrays are limited
	// in size by factor SIAS_MAX_RELATIONS
	if (uric->nextUserRelationId >= SIAS_MAX_RELATIONS) {
		SpinLockRelease(&(uric->mutex));
		elog(ERROR, "There can't be any more SIAS relations. adjust parameter SIAS_MAX_RELATIONS");
	}

	nextId = uric->nextUserRelationId++;
	SpinLockRelease(&(uric->mutex));

	return nextId;
}

/*
 * returns array of VidMappingInfo pointers of all
 * the user relations sorted by userRelationId
 */
VidMappingInfo**
getUserRelationVmiPointerArray(void)
{
	return userRelationVmis;
}

static void
RememberVmiPointer(uint32 userRelationId, VidMappingInfo *vmi)
{
	userRelationVmis[userRelationId] = vmi;
}

static void
RememberTbiPointer(uint32 userRelationId, TargetBlockInfo *tbi)
{
	userRelationTbis[userRelationId] = tbi;
}

static void
RememberGciPointer(uint32 userRelationId, GarbageCollectionInfo *gci)
{
	userRelationGcis[userRelationId] = gci;
}

static void
RememberRelOid(uint32 userRelationId, Oid oid)
{
	userRelationOids[userRelationId] = oid;
}

static void
RememberUserRelationIdOfRelFileNode(RelFileNode *rfnPtr, uint32 userRelationId)
{
	rfnHashLookupEntry *result;
	bool	found;

	result = (rfnHashLookupEntry *)
		hash_search_with_hash_value(rfnUserRelationIdHashTable,
									(void *) rfnPtr,
									get_hash_value(rfnUserRelationIdHashTable, (void *) rfnPtr),
									HASH_ENTER,
									&found);

	if (found)
	{
		elog(ERROR, "There is already an entry in rfnUserRelationIdHashTable for this RelFileNode.");
	}
	else
	{
		// insert value referring to key
		result->userRelationId = userRelationId;
	}
}

void
RelationAllocateVidMappingInfo(Relation rel)
{
	VidMappingInfo *vmi;
	bool 			found;
	bool			rebuild;
	char			name[SHMEM_INDEX_KEYSIZE];
	int 			i;


	sprintf(name, "VidMappingInfo %u", rel->rd_id);
	rel->rd_vmi = ShmemInitStruct(name,
			sizeof(VidMappingInfo), &found);

	vmi = rel->rd_vmi;

	if (!found || vmi->needRebuild)
	{
		rebuild = found;


		// initialize all members of VidMappingInfo

		/*
		 * vmi->needRebuild
		 */
		vmi->needRebuild = false;

		/*
		 * vmi->bucketRelation
		 */
		sprintf(name, "BucketRelation %u", rel->rd_id);
		vmi->bucketRelation = ShmemInitStruct(name,
				sizeof(RelationData), &found);

		/*
		 * vmi->vidIncrementMutex
		 */
		SpinLockInit(&(vmi->vidIncrementMutex));

		/*
		 * vmi->curVid
		 */
		if (!rebuild)
		{
			vmi->curVid.vid = (SIAS_FIRST_VID - 1);
		}



		// new bucket buffer pool

		/*
		 * vmi->bufferDescriptors
		 * vmi->bufferBlocks
		 */
		InitBucketBufferPool(rel);

		/*
		 * vmi->strategyControl
		 */
		BucketStrategyInitialize(rel, true);

		/*
		 * vmi->bufferPartitionLocks
		 *
		 * There is no equivalent method to initialize
		 * the partition locks for postgre's shared buffers,
		 * because their LWLockIds are fixed.
		 * (see enum LWLockId in lwlock.h)
		 */
		sprintf(name, "bufferPartitionLocks %u", rel->rd_id);
		vmi->bufferPartitionLocks = ShmemInitStruct(name,
				(sizeof(LWLockId) * SIAS_BUFFER_POOL_PARTITIONS), &found);

		for (i = 0; i < SIAS_BUFFER_POOL_PARTITIONS; i++)
		{
			vmi->bufferPartitionLocks[i] = LWLockAssign();
		}

		// used for full VID-locking through LWLocks
		/*char name5[(8 * sizeof(rel->rd_id) + 2) / 3 + 15];
		sprintf(name5, "bufferVidLocks %u", rel->rd_id);
		vmi->bufferVidLocks = ShmemInitStruct(name5,
				(sizeof(LWLockId) * (SIAS_BUFFER_POOL_SIZE * SIAS_BUCKET_SIZE)), &found);

		for (i = 0; i < (SIAS_BUFFER_POOL_SIZE * SIAS_BUCKET_SIZE); i++)
		{
			vmi->bufferVidLocks[i] = LWLockAssign();
		}*/

		/*
		 * vmi->bufferVidExclusiveLocks
		 */
		sprintf(name, "bufferVidExclusiveLocks %u", rel->rd_id);
		vmi->bufferVidExclusiveLocks = ShmemInitStruct(name,
				(sizeof(slock_t) * (SIAS_BUFFER_POOL_SIZE * SIAS_BUCKET_SIZE)), &found);

		for (i = 0; i < (SIAS_BUFFER_POOL_SIZE * SIAS_BUCKET_SIZE); i++)
		{
			SpinLockInit(&(vmi->bufferVidExclusiveLocks[i]));
		}

		/*
		 * vmi->bufFreelistLock
		 */
		vmi->bufFreelistLock = LWLockAssign();

		/*
		 * vmi->userRelationId
		 */
		if (!rebuild)
		{
			vmi->userRelationId = nextUserRelationId();
		}

		/*
		 * vmi->highestUsedBlock
		 */
		if (!rebuild)
		{
			vmi->highestUsedBlock = InvalidBlockNumber;
		}

		/*
		 * vmi->highestUsedBlockMutex
		 */
		SpinLockInit(&(vmi->highestUsedBlockMutex));

		/*
		 * vmi->highestWrittenBlock
		 */
		if (!rebuild)
		{
			vmi->highestWrittenBlock = InvalidBlockNumber;
		}

		/*
		 * vmi->highestWrittenBlockMutex
		 */
		SpinLockInit(&(vmi->highestWrittenBlockMutex));


		RememberVmiPointer(vmi->userRelationId, vmi);

		if (!rebuild)
		{
			RememberRelOid(vmi->userRelationId, rel->rd_id);
		}
	}
	else {
		vmi->bucketRelation->rd_smgr = smgropen(vmi->bucketRelation->rd_node);
	}
}

void
RelationAllocateTargetBlockInfo(Relation rel)
{
	TargetBlockInfo *tbi;
	bool 			found;
	int				i;
	char*			page;
	bool			rebuild;
	char			name[SHMEM_INDEX_KEYSIZE];


	sprintf(name, "TargetBlockInfo %u", rel->rd_id);
	rel->rd_tbi = ShmemInitStruct(name,
								  sizeof(TargetBlockInfo),
								  &found);

	tbi = rel->rd_tbi;

	if (!found || tbi->needRebuild)
	{
		rebuild = found;

		if (!rebuild)
		{
			RememberUserRelationIdOfRelFileNode(&(rel->rd_node), rel->rd_vmi->userRelationId);
		}

		RememberTbiPointer(rel->rd_vmi->userRelationId, tbi);

		// initialize all members of TargetBlockInfo

		/*
		 * tbi->needRebuild
		 */
		tbi->needRebuild = false;

		/*
		 * tbi->blockNumberMappingLock
		 */
		tbi->blockNumberMappingLock = LWLockAssign();

		/*
		 * tbi->highestAppendedBlock
		 */
		if (!rebuild)
		{
			tbi->highestAppendedBlock = InvalidBlockNumber;
		}

		/*
		 * tbi->highestAppendedBlockMutex
		 */
		SpinLockInit(&(tbi->highestAppendedBlockMutex));

		/*
		 * tbi->cache1
		 */
		sprintf(name, "cache1 %u", rel->rd_id);
		tbi->cache1 = ShmemInitStruct(name,
									  BLCKSZ * SIAS_APPEND_SIZE,
									  &found);
		MemSet((char *) tbi->cache1, 0, BLCKSZ * SIAS_APPEND_SIZE);

		/*
		 * tbi->cache2
		 */
		sprintf(name, "cache2 %u", rel->rd_id);
		tbi->cache2 = ShmemInitStruct(name,
									  BLCKSZ * SIAS_APPEND_SIZE,
									  &found);
		MemSet((char *) tbi->cache2, 0, BLCKSZ * SIAS_APPEND_SIZE);

		/*
		 * tbi->cache3
		 */
		sprintf(name, "cache3 %u", rel->rd_id);
		tbi->cache3 = ShmemInitStruct(name,
									  BLCKSZ * SIAS_APPEND_SIZE,
									  &found);
		MemSet((char *) tbi->cache3, 0, BLCKSZ * SIAS_APPEND_SIZE);

		/*
		 * tbi->highestAssignedRegion
		 */
		if (!rebuild)
		{
			tbi->highestAssignedRegion = SIAS_FIRST_INITIAL_TARGET_BLOCK;
		}

		/*
		 * tbi->firstPrevAppendBlock
		 */
		tbi->firstPrevAppendBlock = InvalidBlockNumber;

		/*
		 * tbi->firstNextAppendBlock
		 */
		if (!rebuild)
		{
			tbi->firstNextAppendBlock = SIAS_FIRST_INITIAL_TARGET_BLOCK;
		}

		/*
		 * tbi->firstOverflowBlock
		 */
		if (!rebuild)
		{
			tbi->firstOverflowBlock = GetNeverUsedRegion(rel);
		}

		/*
		 * tbi->currentAppendCache
		 */
		tbi->prevAppendCache = tbi->cache1;

		/*
		 * tbi->nextAppendCache
		 */
		tbi->nextAppendCache = tbi->cache2;

		/*
		 * tbi->overflowCache
		 */
		tbi->overflowCache = tbi->cache3;

		/*
		 * tbi->targetBlockSlots
		 */
		sprintf(name, "targetBlockSlots %u", rel->rd_id);
		tbi->targetBlockSlots = ShmemInitStruct(name,
												sizeof(BlockNumber) * SIAS_CONCURRENT_TARGET_BLOCKS,
												&found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->targetBlockSlots[i] = InvalidBlockNumber;
		}

		/*
		 * tbi->slotsInUse
		 */
		sprintf(name, "slotsInUse %u", rel->rd_id);
		tbi->slotsInUse = ShmemInitStruct(name,
										  sizeof(int) * SIAS_CONCURRENT_TARGET_BLOCKS,
										  &found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->slotsInUse[i] = SIAS_InvalidSlotNumber;
		}

		/*
		 * tbi->unusedSlots
		 */
		sprintf(name, "unusedSlots %u", rel->rd_id);
		tbi->unusedSlots = ShmemInitStruct(name,
										   sizeof(int) * SIAS_CONCURRENT_TARGET_BLOCKS,
										   &found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->unusedSlots[i] = i;
		}

		/*
		 * tbi->previouslyUsedBlocks
		 */
		sprintf(name, "previouslyUsedBlocks %u", rel->rd_id);
		tbi->previouslyUsedBlocks = ShmemInitStruct(name,
										   	   	    sizeof(BlockNumber) * SIAS_CONCURRENT_TARGET_BLOCKS,
													&found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->previouslyUsedBlocks[i] = InvalidBlockNumber;
		}

		/*
		 * tbi->numPreviouslyUsedBlocks
		 */
		tbi->numPreviouslyUsedBlocks = 0;

		/*
		 * tbi->slotManagementMutex
		 */
		SpinLockInit(&(tbi->slotManagementMutex));

		/*
		 * tbi->slotUsers
		 */
		sprintf(name, "slotUsers %u", rel->rd_id);
		tbi->slotUsers = ShmemInitStruct(name,
										 sizeof(int) * SIAS_CONCURRENT_TARGET_BLOCKS,
										 &found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->slotUsers[i] = 0;
		}

		/*
		 * tbi->slotLocks
		 */
		sprintf(name, "slotMutexes %u", rel->rd_id);
		tbi->slotLocks = ShmemInitStruct(name,
										 sizeof(LWLockId) * SIAS_CONCURRENT_TARGET_BLOCKS,
										 &found);

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			tbi->slotLocks[i] = LWLockAssign();
		}

		/*
		 * tbi->numUnusedSlots
		 *
		 * none of the slots has been assigned
		 * to a process yet, thus all are unused
		 */
		tbi->numUnusedSlots = SIAS_CONCURRENT_TARGET_BLOCKS;

		/*
		 * tbi->nextSlotPointer
		 */
		tbi->nextSlotPointer = -1;

		/*
		 * tbi->assignmentLock
		 */
		tbi->assignmentLock = LWLockAssign();

		/*
		 * tbi->fullNonOverflowTargetBlocks
		 */
		tbi->fullNonOverflowTargetBlocks = 0;

		/*
		 * tbi->fullOverflowTargetBlocks
		 */
		tbi->fullOverflowTargetBlocks = 0;

		/*
		 * tbi->targetBlockInitialized
		 */
		sprintf(name, "targetBlockInitialized %u", rel->rd_id);
		tbi->targetBlockInitialized = ShmemInitStruct(name,
										   	   	      sizeof(bool) * SIAS_TOTAL_ACTIVE_TARGET_BLOCKS,
													  &found);

		for (i = 0; i < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS; i++)
		{
			/*
			 * none of the target blocks has been initialized yet
			 */
			tbi->targetBlockInitialized[i] = false;
		}

		/*
		 * tbi->targetBlockSlotIndex
		 */
		sprintf(name, "targetBlockSlotIndex %u", rel->rd_id);
		tbi->targetBlockSlotIndex = ShmemInitStruct(name,
										   	   	   	sizeof(int) * SIAS_TOTAL_ACTIVE_TARGET_BLOCKS,
													&found);

		for (i = 0; i < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS; i++)
		{
			/*
			 * none of the target blocks is assigned
			 * to a target block slot
			 */
			tbi->targetBlockSlotIndex[i] = SIAS_InvalidSlotNumber;
		}

		/*
		 * tbi->tbsiMutexes
		 */
		sprintf(name, "tbsiMutexes %u", rel->rd_id);
		tbi->tbsiMutexes = ShmemInitStruct(name,
										   sizeof(slock_t) * SIAS_TOTAL_ACTIVE_TARGET_BLOCKS,
										   &found);

		for (i = 0; i < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS; i++)
		{
			SpinLockInit(&(tbi->tbsiMutexes[i]));
		}

		/*
		 * tbi->highestUsedBlock
		 */
		if (!rebuild)
		{
			tbi->highestUsedBlock = InvalidBlockNumber;
		}

		/*
		 * tbi->highestUsedBlockMutex
		 */
		SpinLockInit(&(tbi->highestUsedBlockMutex));

		/*
		 * tbi->highestUsedTargetBlockIndex
		 */
		tbi->highestUsedTargetBlockIndex = -1;

		/*
		 * tbi->highestWrittenBlockPrevAppend
		 */
		tbi->highestWrittenBlockPrevAppend = InvalidBlockNumber;

		/*
		 * tbi->highestWrittenBlockPrevAppendMutex
		 */
		SpinLockInit(&(tbi->highestWrittenBlockPrevAppendMutex));

		/*
		 * tbi->appendLock
		 */
		tbi->appendLock = LWLockAssign();

		/*
		 * tbi->runningAppendLock
		 */
		tbi->runningAppendLock = LWLockAssign();

		/*
		 * tbi->replacementLock
		 */
		tbi->replacementLock = LWLockAssign();

		/*
		 * tbi->numAppends
		 */
		if (!rebuild)
		{
			tbi->numAppends = 0;
		}

		tbi->prevAppendComplete = true;
	}
}

void
RelationAllocateGarbageCollectionInfo(Relation rel)
{
	GarbageCollectionInfo *gci;
	bool 			found;
	int				i;
	HASHCTL			hashInfo;
	HTAB 			*htab;
	bool			rebuild;
	char			name[SHMEM_INDEX_KEYSIZE];

#ifndef SIAS_GC
	return;
#endif

	sprintf(name, "GarbageCollectionInfo %u", rel->rd_id);
	rel->rd_gci = ShmemInitStruct(name,
								  sizeof(GarbageCollectionInfo),
								  &found);

	gci = rel->rd_gci;

	if (!found || gci->needRebuild) {

		rebuild = found;

		RememberGciPointer(rel->rd_vmi->userRelationId, gci);

		/*
		 * gci->needRebuild
		 */
		gci->needRebuild = false;

		/*
		 * gci->coldWriteRegionLock
		 */
		gci->coldWriteRegionLock = LWLockAssign();

		/*
		 * gci->cleanedRegionsQueue
		 */
		sprintf(name, "cleanedRegionsQueue %u", rel->rd_id);
		gci->cleanedRegionsQueue.elements = ShmemInitStruct(name,
										   	  	   	   	    sizeof(BlockNumber) * SIAS_MAX_QUEUED_REGIONS,
															&found);

		if (!rebuild)
		{
			for (i = 0; i < SIAS_MIN_CLEANED_REGIONS; i++)
			{
				gci->cleanedRegionsQueue.elements[i] = (i+1) * SIAS_APPEND_SIZE;
			}

			for (i = SIAS_MIN_CLEANED_REGIONS; i < SIAS_MAX_QUEUED_REGIONS; i++)
			{
				gci->cleanedRegionsQueue.elements[i] = InvalidBlockNumber;
			}

			gci->cleanedRegionsQueue.numElements = SIAS_MIN_CLEANED_REGIONS;
			gci->cleanedRegionsQueue.head = 0;
			gci->cleanedRegionsQueue.tail = SIAS_MIN_CLEANED_REGIONS - 1;
		}

		SpinLockInit(&(gci->cleanedRegionsQueue.mutex));

		/*
		 * gci->coldWriteRegion
		 */
		if (!rebuild)
		{
			gci->coldWriteRegion = SIAS_INITIAL_COLD_REGION;
		}

		/*
		 * gci->coldWriteBlock
		 */
		gci->coldWriteBlock = gci->coldWriteRegion;

		/*
		 * gci->coldWriteBlockInited
		 */
		gci->coldWriteBlockInited = false;

		/*
		 * gci->dirtyRegions
		 */
		hashInfo.keysize = sizeof(BlockNumber);
		hashInfo.entrysize = sizeof(DirtyRegionData);
		hashInfo.hash = oid_hash;
		sprintf(name, "dirtyRegions table %u", rel->rd_id);
		htab = ShmemInitHash(name,
							 SIAS_MAX_QUEUED_REGIONS, SIAS_MAX_QUEUED_REGIONS,
							 &hashInfo,
							 HASH_ELEM | HASH_FUNCTION);
		sprintf(name, "dirtyRegions HTAB %u", rel->rd_id);
		gci->dirtyRegions = ShmemInitStruct(name,
							   	   	   	    SizeOfHTAB(),
											&found);
		memcpy(gci->dirtyRegions, htab, SizeOfHTAB());
		pfree(htab);

		if (rebuild)
		{
			DirtyRegionData *drd;
			sprintf(name, "drdList %u", rel->rd_id);
			DirtyRegionData *drdList = ShmemInitStruct(name,
   	  	   	   	    								   sizeof(DirtyRegionData) * SIAS_MAX_QUEUED_REGIONS,
													   &found);

			if (!found)
			{
				elog(ERROR, "Didn't find '%s' to rebuild gci->dirtyRegions.", name);
			}

			for (i = 0; i < gci->numDirtyRegions; i++)
			{
				drd = (DirtyRegionData*) hash_search(gci->dirtyRegions, &(drdList[i].firstBlock), HASH_ENTER, NULL);
				*drd = drdList[i];
			}
		}

		/*
		 * gci->numDirtyRegions
		 */
		if (!rebuild)
		{
			gci->numDirtyRegions = 0;
		}

		/*
		 * gci->dirtyRegionsMutex
		 */
		SpinLockInit(&(gci->dirtyRegionsMutex));

		/*
		 * gci->updateDownCounters
		 */
		sprintf(name, "updateDownCounters %u", rel->rd_id);
		gci->updateDownCounters = ShmemInitStruct(name,
										   	  	  sizeof(int) * SIAS_MAX_REGIONS,
												  &found);

		if (!rebuild)
		{
			MemSet(gci->updateDownCounters, 0, sizeof(int) * SIAS_MAX_REGIONS);
		}

		/*
		 * gci->maxUpdaterXmin
		 */
		sprintf(name, "maxUpdaterXmin %u", rel->rd_id);
		gci->maxUpdaterXmin = ShmemInitStruct(name,
										   	  sizeof(TransactionId) * SIAS_MAX_REGIONS,
											  &found);

		if (!rebuild)
		{
			MemSet(gci->maxUpdaterXmin, 0, sizeof(TransactionId) * SIAS_MAX_REGIONS);
		}

		/*
		 * gci->newlyUpdatedData
		 */
		sprintf(name, "newlyUpdatedData %u", rel->rd_id);
		gci->newlyUpdatedData = ShmemInitStruct(name,
										   	    sizeof(uint32) * SIAS_MAX_REGIONS,
												&found);

		if (!rebuild)
		{
			MemSet(gci->newlyUpdatedData, 0, sizeof(uint32) * SIAS_MAX_REGIONS);
		}

		/*
		 * gci->regionGcCountersMutexes
		 */
		sprintf(name, "regionGcCountersMutexes %u", rel->rd_id);
		gci->regionGcCountersMutexes = ShmemInitStruct(name,
										   	  	  	   sizeof(slock_t) * SIAS_MAX_REGIONS,
													   &found);

		for (i = 0; i < SIAS_MAX_REGIONS; i++)
		{
			SpinLockInit(&(gci->regionGcCountersMutexes[i]));
		}

		/*
		 * gci->coldCache
		 */
		sprintf(name, "coldCache %u", rel->rd_id);
		gci->coldCache = ShmemInitStruct(name,
									  	 BLCKSZ * SIAS_APPEND_SIZE,
										 &found);

		MemSet((char *) gci->coldCache, 0, BLCKSZ * SIAS_APPEND_SIZE);

		/*
		 * gci->highestWrittenColdBlock
		 */
		gci->highestWrittenColdBlock = InvalidBlockNumber;

		/*
		 * gci->highestWrittenColdBlockMutex
		 */
		SpinLockInit(&(gci->highestWrittenColdBlockMutex));

		/*
		 * gci->hasSiasGcWorker
		 */
		gci->hasSiasGcWorker = false;

		/*
		 * gci->numGcHot
		 */
		if (!rebuild)
		{
			gci->numGcHot = 0;
		}

		/*
		 * gci->numGcCold
		 */
		if (!rebuild)
		{
			gci->numGcCold = 0;
		}

		/*
		 * gci->totalGcDurationMillisecs
		 */
		if (!rebuild)
		{
			gci->totalGcDurationMillisecs = 0;
		}

		gci->prevAppendComplete = true;
	}
}

/*
 * IsAssignedTargetBlock
 *
 * Is blockNum currently assigned to a target block slot?
 */
bool
IsAssignedTargetBlock(Relation rel, BlockNumber blockNum)
{
	TargetBlockInfo *tbi;
	bool result;
	int slot;

	tbi = rel->rd_tbi;
	result = false;

	LWLockAcquire((tbi)->blockNumberMappingLock, LW_SHARED);

	if (IsActiveTargetBlock(tbi, blockNum))
	{
		/*
		 * There is no need to acquire the tbi->slotManagementMutex
		 * at this point, because we don't care to which slot, if any,
		 * a block is assigned, but only want to know if it is at all.
		 *
		 * That's why a possible replacement to another slot won't
		 * harm the correctness of the result.
		 */
		GetTargetBlockSlotIndex(tbi,
								GetTargetBlockOffset(tbi, blockNum),
								&slot);

		if (slot != SIAS_InvalidSlotNumber)
		{
			result = true;
		}
	}

	LWLockRelease((tbi)->blockNumberMappingLock);

	return result;
}

/*
 * AccessTargetBlockSlot
 *
 * Processes, which want to write a tuple, need a target
 * block (TB) to do so. They access this TB by acquiring
 * exclusive access to a target block slot.
 *
 * returns the index of a target block slot (TBS), which
 * has already been locked exclusively.
 *
 * In case of a bulk insert happening the caller should
 * hand over the last block it has written as otherBlock.
 * In case of an update the caller should hand over
 * the block of the to-be-updated tuple as otherBlock.
 * In both cases this method will try to access the TBS
 * holding otherBlock.
 */
int
AccessTargetBlockSlot(Relation rel, BlockNumber otherBlock, bool forInsert,
		Buffer otherBuffer, bool *otherBufferUnlocked)
{
//	printf("AccessTargetBlockSlot\n");
	TargetBlockInfo *tbi;
	int slotIndex;
	bool gotAlreadyUsedSlot;

	tbi = rel->rd_tbi;
	slotIndex = SIAS_InvalidSlotNumber;
	gotAlreadyUsedSlot = false;

	LWLockAcquire(tbi->assignmentLock, LW_EXCLUSIVE);

	SpinLockAcquire(&(tbi->slotManagementMutex));


	if (otherBlock != InvalidBlockNumber)
	{
		/*
		 * check if otherBlock is assigned to
		 * a target block slot
		 *
		 * If not, it must be full.
		 */

		if (IsActiveTargetBlock(tbi, otherBlock))
		{
			/*
			 * otherBlock is one of the current target blocks,
			 * get the slot index holding it or SIAS_InvalidSlotNumber otherwise
			 */
			GetTargetBlockSlotIndex(tbi,
									GetTargetBlockOffset(tbi, otherBlock),
									&slotIndex);
		}

		if (!forInsert
				&& slotIndex != SIAS_InvalidSlotNumber)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			*otherBufferUnlocked = true;
		}
	}

	if (slotIndex == SIAS_InvalidSlotNumber)
	{
		/*
		 * If there is any unused target block slot,
		 * get one.
		 *
		 * Otherwise get the used slot, which has been
		 * accessed least recently.
		 */

		if (tbi->numUnusedSlots > 0)
		{
			if (tbi->numPreviouslyUsedBlocks > 0)
			{
				/*
				 * get one of the unused slots, that hold a target block
				 */

				BlockNumber previouslyUsedBlock;
				previouslyUsedBlock = tbi->previouslyUsedBlocks[tbi->numPreviouslyUsedBlocks - 1];
				tbi->numPreviouslyUsedBlocks--;

				GetTargetBlockSlotIndex(tbi,
										GetTargetBlockOffset(tbi, previouslyUsedBlock),
										&slotIndex);

				if (!ValidSlotIndex(slotIndex))
				{
					elog(FATAL, "previously used block isn't assigned to a valid target block slot");
				}

				RemoveArrayEntry(tbi->unusedSlots,
								 sizeof(int),
								 slotIndex,
								 tbi->numUnusedSlots,
								 NULL, 1);
			}
			else
			{
				/*
				 * get an empty unused slot
				 */

				slotIndex = tbi->unusedSlots[tbi->numUnusedSlots - 1];

				if (!ValidSlotIndex(slotIndex))
				{
					elog(FATAL, "unused target block slot isn't a valid target block slot");
				}

				if (tbi->targetBlockSlots[slotIndex] != InvalidBlockNumber)
				{
					elog(FATAL, "unused target block slot holds block, which isn't listed in previouslyUsedBlocks");
				}
			}
		}
		else
		{
			/*
			 * get the slot, which has been accessed least recently
			 */

			Assert(ValidNextSlotPointer(tbi));
			slotIndex = tbi->slotsInUse[tbi->nextSlotPointer];
			gotAlreadyUsedSlot = true;
		}
	}
	else
	{
		if (tbi->targetBlockSlots[slotIndex] != otherBlock)
		{
			elog(FATAL, "target block slot isn't holding otherBlock");
		}

		if (tbi->slotUsers[slotIndex] == 0)
		{
			RemoveArrayEntry(tbi->previouslyUsedBlocks,
							 sizeof(BlockNumber),
							 tbi->targetBlockSlots[slotIndex],
							 tbi->numPreviouslyUsedBlocks,
							 NULL, 2);
			tbi->numPreviouslyUsedBlocks--;

			RemoveArrayEntry(tbi->unusedSlots,
							 sizeof(int),
							 slotIndex,
							 tbi->numUnusedSlots,
							 NULL, 3);
		}
		else if (tbi->slotUsers[slotIndex] > 0)
		{
			gotAlreadyUsedSlot = true;
		}
		else
		{
			elog(FATAL, "target block slot has negative user count %i", tbi->slotUsers[slotIndex]);
		}
	}

	if (!ValidSlotIndex(slotIndex))
	{
		elog(FATAL, "Didn't get valid slot.");
	}

	if (gotAlreadyUsedSlot)
	{
		advanceIndexPointer(&(tbi->nextSlotPointer), NumUsedSlots(tbi));
	}
	else
	{
		if (tbi->slotUsers[slotIndex] != 0)
		{
			elog(ERROR, "unused target block slot's user count is %i", tbi->slotUsers[slotIndex]);
		}

		tbi->slotsInUse[NumUsedSlots(tbi)] = slotIndex;
		tbi->numUnusedSlots--;

		if (NumUsedSlots(tbi) == 1)
		{
			tbi->nextSlotPointer = 0;
		}
	}

	tbi->slotUsers[slotIndex]++;

	SpinLockRelease(&(tbi->slotManagementMutex));

	LWLockRelease(tbi->assignmentLock);

	LWLockAcquire(tbi->slotLocks[slotIndex], LW_EXCLUSIVE);

//	printf("AccessTargetBlockSlot %u\n", slotIndex);
	return slotIndex;
}

/*
 * ReleaseTargetBlockSlot
 *
 * releases the lock of a target block slot
 * and manages all necessary changes if it
 * becomes unused
 */
void
ReleaseTargetBlockSlot(Relation rel, int slot)
{
//	printf("ReleaseTargetBlockSlot %u\n", slot);
	TargetBlockInfo *tbi;

	tbi = rel->rd_tbi;

	if (ValidSlotIndex(slot))
	{
		SpinLockAcquire(&(tbi->slotManagementMutex));

		tbi->slotUsers[slot]--;

		if (tbi->slotUsers[slot] == 0)
		{
			int deletionIndex;

			if (!IsOverflowBlock(tbi, tbi->targetBlockSlots[slot]))
			{
				/*
				 * non-overflow target blocks should be
				 * used preferably, that's why we add them
				 * to the right end of the list, where we
				 * take BlockNumbers away
				 */
				tbi->previouslyUsedBlocks[tbi->numPreviouslyUsedBlocks] = tbi->targetBlockSlots[slot];
			}
			else
			{
				/*
				 * overflow target blocks are added to
				 * the left end of the list
				 */
				memmove(tbi->previouslyUsedBlocks + 1,
						tbi->previouslyUsedBlocks,
						sizeof(BlockNumber) * tbi->numPreviouslyUsedBlocks);
				tbi->previouslyUsedBlocks[0] = tbi->targetBlockSlots[slot];
			}
			tbi->numPreviouslyUsedBlocks++;

			RemoveArrayEntry(tbi->slotsInUse,
							 sizeof(int),
							 slot,
							 NumUsedSlots(tbi),
							 &deletionIndex, 4);

			tbi->unusedSlots[tbi->numUnusedSlots] = slot;
			tbi->numUnusedSlots++;

			/*
			 * update nextSlotPointer
			 */
			if (deletionIndex == tbi->nextSlotPointer
					&& NumUsedSlots(tbi) == tbi->nextSlotPointer)
			{
				/*
				 * pointing to the next entry behind
				 * the last entry
				 */

				if (NumUsedSlots(tbi) == 0)
					tbi->nextSlotPointer = -1;
				else
					tbi->nextSlotPointer = 0;
			}
			else if (deletionIndex < tbi->nextSlotPointer)
			{
				tbi->nextSlotPointer--;
			}
		}

		SpinLockRelease(&(tbi->slotManagementMutex));
		LWLockRelease(tbi->slotLocks[slot]);
	}
	else
	{
		elog(ERROR, "Can't be released: invalid slot index");
	}
}

/*
 * ReplaceTargetBlock
 *
 * Processes should call this method if the target block (TB),
 * which is assigned to their target block slot (TBS), doesn't offer
 * enough space to write their data on it.
 * Consequently, such a process needs another block as replacement,
 * which will be assigned to the same TBS.
 *
 * If there is a TB, which is assigned to an unused TBS,
 * we can use that TB as replacement.
 *
 * Otherwise, if there are still empty TBs left and
 * (including the issued block) less than SIAS_APPEND_SIZE many
 * non-overflow blocks are full, we'll just provide an empty block
 * as replacement for the issued block.
 *
 * Otherwise an append has to be performed.
 */
void
ReplaceTargetBlock(Relation rel, int slotOfTargetBlock, BlockNumber targetBlock, bool usedReplacementAllowed)
{
	TargetBlockInfo *tbi;
	GarbageCollectionInfo *gci;
	int i, j;
	int *slotsOfNonOverflowBlocks;
	int numberOfNonOverflowBlocksInUse;
	bool targetBlockIsOverflowBlock;
	char *formerPrevAppendCache;
	RegionNumber regionNum;

	tbi = rel->rd_tbi;
	gci = rel->rd_gci;

	if (!ValidSlotIndex(slotOfTargetBlock))
	{
		elog(ERROR, "slotOfTargetBlock is an invalid target block slot");
	}

	LWLockAcquire(tbi->replacementLock, LW_EXCLUSIVE);

	if (targetBlock == InvalidBlockNumber)
	{
		/*
		 * The issued target block has already been
		 * replaced by an append, which has started
		 * before this call and thus made it wait
		 * for tbi->replacementMutex (right above).
		 *
		 * Nothing has to be done here. The caller
		 * can just use the new empty block that is
		 * already in the slot it owns.
		 */
		if (tbi->targetBlockSlots[slotOfTargetBlock] != InvalidBlockNumber)
		{
			elog(WARNING, "InvalidBlock shouldn't be replaced by other processes.");
			LWLockRelease(tbi->replacementLock);
			return;
		}

		targetBlockIsOverflowBlock = false;
	}
	else
	{
		/*
		 * The issued target block has already been
		 * replaced by an append, which has started
		 * before this call and thus made it wait
		 * for tbi->replacementMutex (right above).
		 *
		 * Nothing has to be done here. The caller
		 * can just use the new empty block that is
		 * already in the slot it owns.
		 */
		if (!IsActiveTargetBlock(tbi, targetBlock))
		{
			LWLockRelease(tbi->replacementLock);
			return;
		}

		targetBlockIsOverflowBlock = IsOverflowBlock(tbi, targetBlock);

		if (targetBlockIsOverflowBlock)
		{
			tbi->fullOverflowTargetBlocks++;
		}
		else
		{
			tbi->fullNonOverflowTargetBlocks++;
		}
	}

	if (usedReplacementAllowed && tbi->fullNonOverflowTargetBlocks < SIAS_APPEND_SIZE)
	{
		SpinLockAcquire(&(tbi->slotManagementMutex));
		if (tbi->numPreviouslyUsedBlocks > 0)
		{
			if (!(tbi->numUnusedSlots > 0))
			{
				elog(FATAL, "There is a previously used block, but no unused slot.");
			}

			/*
			 * There is at least one block, which should
			 * be assigned to a currently unused target
			 * block slot.
			 *
			 * Thus we can get that block for our slot
			 * and don't need to get a new block.
			 */

			BlockNumber previouslyUsedBlock;
			int oldSlot;

			previouslyUsedBlock = tbi->previouslyUsedBlocks[tbi->numPreviouslyUsedBlocks - 1];
			tbi->numPreviouslyUsedBlocks--;

			GetTargetBlockSlotIndex(tbi,
									GetTargetBlockOffset(tbi, previouslyUsedBlock),
									&oldSlot);

			if (!ValidSlotIndex(oldSlot))
			{
				elog(FATAL, "previously used block isn't assigned to a valid target block slot");
			}
			if (tbi->slotUsers[oldSlot] != 0)
			{
				elog(FATAL, "target block slot %i is holding a previously used block, thus it should be unused, but has user count %i", oldSlot, tbi->slotUsers[oldSlot]);
			}

			/*
			 * remove the block we're going to use
			 * (previouslyUsedBlock) from the slot
			 * it's currently assigned to
			 */
			tbi->targetBlockSlots[oldSlot] = InvalidBlockNumber;

			/*
			 * assign previouslyUsedBlock to our slot
			 */
			tbi->targetBlockSlots[slotOfTargetBlock] = previouslyUsedBlock;
			SetTargetBlockSlotIndex(tbi,
									GetTargetBlockOffset(tbi, previouslyUsedBlock),
									slotOfTargetBlock);

			if (targetBlock != InvalidBlockNumber)
			{
				/*
				 * targetBlock isn't assigned to
				 * a target block slot anymore
				 */
				SetTargetBlockSlotIndex(tbi,
										GetTargetBlockOffset(tbi, targetBlock),
										SIAS_InvalidSlotNumber);
			}

			SpinLockRelease(&(tbi->slotManagementMutex));
			LWLockRelease(tbi->replacementLock);

			return;
		}

		SpinLockRelease(&(tbi->slotManagementMutex));
	}



	/*
	 * As long as both
	 *
	 * - not all blocks, which will be
	 *   appended next time, are full
	 *
	 *   AND
	 *
	 * - there are still empty target blocks,
	 *
	 * we are going to offer one of the latter
	 * to the caller.
	 *
	 * Otherwise we need to start an append.
	 */
	if ((tbi->fullNonOverflowTargetBlocks < SIAS_APPEND_SIZE)
			&& (tbi->highestUsedTargetBlockIndex == -1
					|| tbi->highestUsedTargetBlockIndex < (SIAS_TOTAL_ACTIVE_TARGET_BLOCKS - 1)))
	{
		//	printf("replace begin\n");

		//	FILE *file;
		//	file = fopen("/home/richard/Documents/file1.txt", "a");
		//	fprintf(file, "replace begin %u %u\n", rel->rd_vmi->userRelationId, targetBlock);
		//	fclose(file);

		tbi->highestUsedTargetBlockIndex++;
		tbi->targetBlockSlots[slotOfTargetBlock] = GetTargetBlockNumber(tbi, tbi->highestUsedTargetBlockIndex);
		SetTargetBlockSlotIndex(tbi,
								tbi->highestUsedTargetBlockIndex,
								slotOfTargetBlock);

		if (targetBlock != InvalidBlockNumber)
		{
			/*
			 * targetBlock isn't assigned to
			 * a target block slot anymore
			 */
			SetTargetBlockSlotIndex(tbi,
									GetTargetBlockOffset(tbi, targetBlock),
									SIAS_InvalidSlotNumber);
		}

		LWLockRelease(tbi->replacementLock);

//		file = fopen("/home/richard/Documents/file1.txt", "a");
//		fprintf(file, "replace end\n");
//		fclose(file);

//		printf("replace end\n");
	}
	else
	{
//		file = fopen("/home/richard/Documents/file1.txt", "a");
//		fprintf(file, "append begin\n");
//		fclose(file);

		tbi->numAppends++;
//		printf("%u append begin\n", tbi->numAppends);


		/*
		 * halt assignment of slots to processes
		 *
		 * Without this measure, there could be a
		 * constant incoming flow of slot requests,
		 * which theirselves don't harm the append,
		 * but would delay it, because the append
		 * can only start, if one of the following
		 * criteria is fulfilled for each target
		 * block slot:
		 *
		 * a) There are no more users assigned to
		 * a target block slot.
		 *
		 * b) Some process wants to issue a slot's
		 * target block as full, but has to wait
		 * for replacementLock in this method and
		 * thus for the append to finish.
		 *
		 * Without halting the slot assingment,
		 * criteria a) would probably not be fulfilled.
		 * So the append would wait till all target
		 * blocks in use are issued as full.
		 * (This behavior is cause by the LWLock acquiring
		 * logic, which allows share locks to starve
		 * exclusive lock waiters. In this case it's
		 * about the LWLock appendLock.)
		 */
		LWLockAcquire(tbi->assignmentLock, LW_EXCLUSIVE);
//		printf("%u got assignmentLock\n", tbi->numAppends);
		/*
		 * wait till all running attempts to write target blocks,
		 * belonging to already assigned slots, have either
		 *
		 * - decided to actually write to the target block in
		 *   their slot
		 *
		 *   or
		 *
		 * - are going to issue the target block as full,
		 *   for which they have to wait for this append
		 *   here to finish.
		 */
		LWLockAcquire(tbi->appendLock, LW_EXCLUSIVE);
//		printf("%u got appendLock\n", tbi->numAppends);

//		printf("%u append starting\n", tbi->numAppends);


		/*
		 * find out which of the active, non-overflow target blocks
		 * are assigned to a target block slot
		 *
		 * All of these have to be replaced by new blocks, because
		 * they are going to be appended next.
		 */
		slotsOfNonOverflowBlocks = palloc(sizeof(int) * SIAS_CONCURRENT_TARGET_BLOCKS);
		numberOfNonOverflowBlocksInUse = 0;

		for (i = 0; i < SIAS_CONCURRENT_TARGET_BLOCKS; i++)
		{
			if (!IsOverflowBlock(tbi, tbi->targetBlockSlots[i])
					&& tbi->targetBlockSlots[i] != InvalidBlockNumber)
			{
				slotsOfNonOverflowBlocks[numberOfNonOverflowBlocksInUse] = i;
				numberOfNonOverflowBlocksInUse++;
			}
		}

		/*
		 * update targetBlockSlotIndex
		 *
		 * This can be done without locking the entries,
		 * because there can't be any concurrent access
		 * to these at this point.
		 */
		// copy slot indexes of overflow blocks
		memcpy(&tbi->targetBlockSlotIndex[0],
			   &tbi->targetBlockSlotIndex[SIAS_APPEND_SIZE],
			   SIAS_OVERFLOW_TARGET_BLOCKS * sizeof(int));
		// initialize slot indexes of new blocks
		for (i = SIAS_OVERFLOW_TARGET_BLOCKS; i < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS; i++)
		{
			tbi->targetBlockSlotIndex[i] = SIAS_InvalidSlotNumber;
		}

		/*
		 * update targetBlockInitialized
		 *
		 * This can be done without any special lock,
		 * because there can't be any concurrent access
		 * to these at this point.
		 */
		// copy values of overflow blocks
		memcpy(&tbi->targetBlockInitialized[0],
			   &tbi->targetBlockInitialized[SIAS_APPEND_SIZE],
			   SIAS_OVERFLOW_TARGET_BLOCKS * sizeof(bool));
		// initialize values of new blocks
		for (i = SIAS_OVERFLOW_TARGET_BLOCKS; i < SIAS_TOTAL_ACTIVE_TARGET_BLOCKS; i++)
		{
			tbi->targetBlockInitialized[i] = false;
		}

		/*
		 * wait for previous physical append to finish
		 */
		LWLockAcquire(tbi->runningAppendLock, LW_EXCLUSIVE);

		if (!tbi->prevAppendComplete)
		{
			elog(WARNING, "prevAppend incomplete %u %u-%u", rel->rd_id, tbi->firstPrevAppendBlock,
					tbi->firstPrevAppendBlock+SIAS_APPEND_SIZE-1);
		}

		/*
		 * mapping of BlockNumbers to cache positions
		 * is prohibited while the sequences and
		 * caches are updated
		 */
		LWLockAcquire((tbi)->blockNumberMappingLock, LW_EXCLUSIVE);

		// shift roles of caches
		formerPrevAppendCache = tbi->prevAppendCache;
		tbi->prevAppendCache = tbi->nextAppendCache;
		tbi->nextAppendCache = tbi->overflowCache;
		tbi->overflowCache = formerPrevAppendCache;

		memset(tbi->overflowCache,
			   0,
			   SIAS_APPEND_SIZE * BLCKSZ);

		tbi->firstPrevAppendBlock = tbi->firstNextAppendBlock;
		tbi->firstNextAppendBlock = tbi->firstOverflowBlock;
		tbi->firstOverflowBlock = GetWriteableRegion(rel, true);


#ifdef SIAS_GC
		ResetRegionsGcCounters(rel, tbi->firstOverflowBlock);
#endif

		SpinLockAcquire(&(rel->rd_tbi->highestWrittenBlockPrevAppendMutex));
		rel->rd_tbi->highestWrittenBlockPrevAppend = InvalidBlockNumber;
		SpinLockRelease(&(rel->rd_tbi->highestWrittenBlockPrevAppendMutex));

		tbi->highestUsedTargetBlockIndex -= SIAS_APPEND_SIZE;

		LWLockRelease((tbi)->blockNumberMappingLock);

//		printf("after mapping info update\n");

		/*
		 * former overflow blocks (now, after the append, they are part
		 * of the non-overflow blocks) aren't written to disk during this
		 * append, even if they are full
		 */
		tbi->fullNonOverflowTargetBlocks = tbi->fullOverflowTargetBlocks;
		/*
		 * all blocks, which will act as overflow blocks now,
		 * are new and haven't been accessed yet, thus they are empty
		 */
		tbi->fullOverflowTargetBlocks = 0;

		/*
		 * all former non-overflow blocks, which were in use
		 * and are going to be appended now, are not writable anymore,
		 * thus they have to be replaced by new blocks, which are empty
		 */
		int slotOfNonOverflowBlock;
		BlockNumber nonOverflowBlock;
		SpinLockAcquire(&(tbi->slotManagementMutex));
		for (i = 0; i < numberOfNonOverflowBlocksInUse; i++)
		{
			slotOfNonOverflowBlock = slotsOfNonOverflowBlocks[i];
			nonOverflowBlock = tbi->targetBlockSlots[slotOfNonOverflowBlock];

			tbi->highestUsedTargetBlockIndex++;
			tbi->targetBlockSlots[slotOfNonOverflowBlock] = GetTargetBlockNumber(tbi, tbi->highestUsedTargetBlockIndex);
			SetTargetBlockSlotIndex(tbi,
									tbi->highestUsedTargetBlockIndex,
									slotOfNonOverflowBlock);

			if (tbi->slotUsers[slotOfNonOverflowBlock] == 0)
			{
				/*
				 * We have to update previouslyUsedBlocks,
				 * because the block, which is assigned
				 * to the target block slot with index
				 * slotOfNonOverflowBlock and just got
				 * replaced by a new block, should be listed in
				 * previouslyUsedBlocks and has to be replaced
				 * by the new block in this list, too.
				 */

				for (j = 0; j < tbi->numPreviouslyUsedBlocks; j++)
				{
					if (tbi->previouslyUsedBlocks[j] == nonOverflowBlock)
						break;
				}

				if (j < tbi->numPreviouslyUsedBlocks)
				{
					tbi->previouslyUsedBlocks[j] = tbi->targetBlockSlots[slotOfNonOverflowBlock];
				}
				else
				{
					elog(FATAL, "unused target block slot's to-be-appended block isn't listed in previouslyUsedBlocks");
				}
			}
		}
		SpinLockRelease(&(tbi->slotManagementMutex));

		/*
		 * if targetBlock is one of the former overflow blocks,
		 * it hasn't been replaced (with a writable block) by the
		 * previous loop, thus this has to be done here separately
		 */
		if (targetBlockIsOverflowBlock)
		{
			tbi->highestUsedTargetBlockIndex++;
			tbi->targetBlockSlots[slotOfTargetBlock] = GetTargetBlockNumber(tbi, tbi->highestUsedTargetBlockIndex);
			SetTargetBlockSlotIndex(tbi,
									tbi->highestUsedTargetBlockIndex,
									slotOfTargetBlock);

			/*
			 * targetBlock isn't assigned to
			 * a target block slot anymore
			 */
			SetTargetBlockSlotIndex(tbi,
									GetTargetBlockOffset(tbi, targetBlock),
									SIAS_InvalidSlotNumber);
		}

		/*
		 * Filling the empty target block slot of the process,
		 * which initiated this append, with an empty block.
		 */
		if (targetBlock == InvalidBlockNumber)
		{
			tbi->highestUsedTargetBlockIndex++;
			tbi->targetBlockSlots[slotOfTargetBlock] = GetTargetBlockNumber(tbi, tbi->highestUsedTargetBlockIndex);
			SetTargetBlockSlotIndex(tbi,
									tbi->highestUsedTargetBlockIndex,
									slotOfTargetBlock);
		}


		/*
		 * All target block management related data/structures
		 * are updated now. Other processes are allowed
		 * to proceed their usage of target blocks.
		 */
		LWLockRelease(tbi->appendLock);
		LWLockRelease(tbi->assignmentLock);
		LWLockRelease(tbi->replacementLock);

//		printf("before physical append\n");
		/*
		 * begin physical append
		 *
		 * write SIAS_APPEND_SIZE many
		 * target blocks to disk
		 */
		tbi->prevAppendComplete = false;
		WriteRegion(rel, tbi->firstPrevAppendBlock, true, false);
		tbi->prevAppendComplete = true;
		/*
		 * physical append finished
		 *
		 * the next append might start now
		 */
		LWLockRelease(tbi->runningAppendLock);

		pfree(slotsOfNonOverflowBlocks);

//		printf("%u append end\n", tbi->numAppends);

//		file = fopen("/home/richard/Documents/file1.txt", "a");
//		fprintf(file, "append end\n");
//		fclose(file);
	}
}

/*
 * WriteRegion
 *
 * writes SIAS_APPEND_SIZE many blocks
 * of a SIAS relation to storage
 *
 * isOverflowRegion is only supposed to be true,
 * if the SIAS shutdown writes the current overflow
 * region. This region might only be defined for
 * SIAS_OVERFLOW_TARGET_BLOCKS many blocks, which is why the
 * remaining (SIAS_APPEND_SIZE - SIAS_OVERFLOW_TARGET_BLOCKS)
 * many blocks have to be handled separately in this case.
 */
void
WriteRegion(Relation rel, BlockNumber firstBlock, bool hotData, bool isOverflowRegion)
{
	int i;
	BufferAccessStrategy bas;
	Buffer buf;
	BlockNumber toBeAppendedBlock;
	bool rewrite;
	int numTuples;
	Page page;
	GarbageCollectionInfo *gci;
	RegionNumber regionNum;

	bas = GetAccessStrategy(BAS_BULKREAD);
	RelationOpenSmgr(rel);
	toBeAppendedBlock = firstBlock;
	rewrite = HasEverBeenAppended(rel->rd_tbi, firstBlock);
	numTuples = 0;
	gci = rel->rd_gci;
	regionNum = GetBlocksRegionNumber(firstBlock);

	for (i = 0; i < (isOverflowRegion ? SIAS_OVERFLOW_TARGET_BLOCKS : SIAS_APPEND_SIZE); i++)
	{
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, toBeAppendedBlock, RBM_NORMAL, bas);

		/*
		 * In case the region was just used by the
		 * append, it is necessary to guarantee,
		 * that nobody is writing to the buffer anymore
		 * (which is done by locking it shared, which
		 * ensures there's no exclusive lock concurrently)
		 * and to mark it dirty if it's a shared buffer.
		 */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = (Page) BufferGetPage(buf);

		if (hotData && (PageGetPageSize(page) != 0))
		{
			PageSetSiasHot(page);
		}

		//TODO testing
		if (!PageHeaderIsValid(page))
		{
//			FILE *file;
//			file = fopen("/home/richard/Documents/file1.txt", "a");
//			fprintf(file, "trying to write SIAS page %u (%u) of relation %u with invalid header\n", toBeAppendedBlock, hotData, rel->rd_id);
//			fclose(file);
			elog(ERROR, "trying to write SIAS page %u (%u) of relation %u with invalid header", toBeAppendedBlock, hotData, rel->rd_id);
		}

		/*
		 * count total number of tuples within this region
		 */
		numTuples += PageGetMaxOffsetNumber(page);

		WriteSiasBlock(rel, buf, toBeAppendedBlock, MAIN_FORKNUM, rewrite);

		if (hotData)
		{
			SpinLockAcquire(&(rel->rd_tbi->highestWrittenBlockPrevAppendMutex));
			rel->rd_tbi->highestWrittenBlockPrevAppend = toBeAppendedBlock;
			SpinLockRelease(&(rel->rd_tbi->highestWrittenBlockPrevAppendMutex));
		}
		else
		{
			SpinLockAcquire(&(rel->rd_gci->highestWrittenColdBlockMutex));
			rel->rd_gci->highestWrittenColdBlock = toBeAppendedBlock;
			SpinLockRelease(&(rel->rd_gci->highestWrittenColdBlockMutex));
		}

//		printf("appended %u\n", blockNum);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);

		toBeAppendedBlock++;
	}

	if (isOverflowRegion)
	{
		page = palloc(BLCKSZ);
		PageInit(page, BLCKSZ, 0);
		PageSetSiasHot(page);

		for (i = SIAS_OVERFLOW_TARGET_BLOCKS; i < SIAS_APPEND_SIZE; i++)
		{
			if (rewrite)
			{
				smgrwrite(rel->rd_smgr, MAIN_FORKNUM, toBeAppendedBlock, page, rel->rd_istemp);
			}
			else
			{
				smgrextend(rel->rd_smgr, MAIN_FORKNUM, toBeAppendedBlock, page, rel->rd_istemp);
			}

			toBeAppendedBlock++;
		}

		toBeAppendedBlock--;

		UpdateHighestAppendedBlock(rel->rd_tbi, toBeAppendedBlock);

		pfree(page);
	}

#ifdef SIAS_GC_TEST
	File *file;
	file = fopen("/home/richard/Documents/file1.txt", "a");
	fprintf(file, "region %u written\n", firstBlock);
	fclose(file);
#endif

#ifdef SIAS_GC
	SpinLockAcquire(&(gci->regionGcCountersMutexes[regionNum]));
	gci->updateDownCounters[regionNum] += numTuples * (hotData ? SIAS_GARBAGE_THRESHOLD_HOT : SIAS_GARBAGE_THRESHOLD_COLD) / 100;

	if (gci->updateDownCounters[regionNum] <= 0)
	{
		/*
		 * There have already been enough updates of tuples
		 * within this region to justify garbage collection.
		 *
		 * This situation is quite unfortunate, because we
		 * just wrote a region, which is going to be garbage
		 * collected soon.
		 * It would be better to avoid writing it at all and
		 * garbage collect it in memory, but this isn't
		 * implemented in the moment.
		 */
		RequestGC(rel, firstBlock);
	}
	SpinLockRelease(&(gci->regionGcCountersMutexes[regionNum]));
#endif

	FreeAccessStrategy(bas);

	/*
	 * Not necessary, but should distribute
	 * actual physical writes more equally.
	 */
//	smgrsync();
}

bool
HasEverBeenAppended(TargetBlockInfo *tbi, BlockNumber blockNum)
{
	bool hasBeenAppended;

	SpinLockAcquire(&(tbi->highestAppendedBlockMutex));
	if (tbi->highestAppendedBlock == InvalidBlockNumber
			|| tbi->highestAppendedBlock < blockNum)
	{
		hasBeenAppended = false;
	}
	else
	{
		hasBeenAppended = true;
	}
	SpinLockRelease(&(tbi->highestAppendedBlockMutex));

	return hasBeenAppended;
}

/*
 * IsCached
 *
 * Returns the cache in which block blockNum
 * is currently cached, if so.
 *
 * The caller has to make sure that the result
 * is still valid for its use after returned.
 */
CacheLocation
IsCached(Relation rel, BlockNumber blockNum, bool checkCold)
{
	CacheLocation cacheLocation;
	TargetBlockInfo *tbi;
	GarbageCollectionInfo *gci;

	cacheLocation = CACHE_NONE;
	tbi = rel->rd_tbi;
	gci = rel->rd_gci;

	LWLockAcquire(tbi->blockNumberMappingLock, LW_SHARED);

	/*
	 * check previous append region
	 */
	if (tbi->firstPrevAppendBlock != InvalidBlockNumber
			&& tbi->firstPrevAppendBlock <= blockNum
			&& blockNum < tbi->firstPrevAppendBlock + SIAS_APPEND_SIZE)
	{
		SpinLockAcquire(&(tbi->highestWrittenBlockPrevAppendMutex));
		if (tbi->highestWrittenBlockPrevAppend == InvalidBlockNumber
				|| tbi->highestWrittenBlockPrevAppend < blockNum)
		{
			/*
			 * block hasn't been written during previous (perhaps still running)
			 * append pass yet, thus it's still cached
			 */
			cacheLocation = CACHE_HOT;
		}
		SpinLockRelease(&(tbi->highestWrittenBlockPrevAppendMutex));
	}
	/*
	 * check next append region and overflow region
	 */
	else if (IsActiveTargetBlock(tbi, blockNum))
	{
		cacheLocation = CACHE_HOT;
	}

	LWLockRelease((tbi)->blockNumberMappingLock);

#ifdef SIAS_GC
	if (cacheLocation == CACHE_NONE && checkCold)
	{
		LWLockAcquire(gci->coldWriteRegionLock, LW_SHARED);

		/*
		 * check region, which is used by garbage
		 * collection to write cold data
		 */
		if (gci->coldWriteRegion <= blockNum
				&& blockNum < gci->coldWriteRegion + SIAS_APPEND_SIZE)
		{
			SpinLockAcquire(&(gci->highestWrittenColdBlockMutex));
			if (gci->highestWrittenColdBlock == InvalidBlockNumber
					|| gci->highestWrittenColdBlock < blockNum)
			{
				/*
				 * block hasn't been written yet, thus it's still cached
				 */
				cacheLocation = CACHE_COLD;
			}
			SpinLockRelease(&(gci->highestWrittenColdBlockMutex));
		}

		LWLockRelease(gci->coldWriteRegionLock);
	}
#endif

	return cacheLocation;
}

Page
GetInitBucket(void)
{
	if (!initBucket)
	{
		bool found;
		char name[] = {"SIASinitBucket"};

		initBucket = ShmemInitStruct(name, BLCKSZ, &found);

		if (!found)
		{
			PageInit(initBucket, BLCKSZ, 0);

			// prepare default entry
			ItemPointer invalidEntry = palloc(sizeof(ItemPointerData));
			ItemPointerSetBlockNumber(invalidEntry, InvalidBlockNumber);
			ItemPointerSetOffsetNumber(invalidEntry, InvalidOffsetNumber);

			// initialize bucket with invalidEntry
			OffsetNumber offset;
			for (offset = 1; offset <= SIAS_BUCKET_SIZE; offset++) {
				PageAddVidTidPair(initBucket, (Item) invalidEntry, offset);
			}
		}
	}

	return initBucket;
}

void
getItemPointerFromVidArray(Relation rel, uint32 vid, ItemPointer iptr)
{
//	printf("getItemPointerFromVidArray\n");
	Assert(vid >= SIAS_FIRST_VID);
	Assert(vid <= rel->rd_vmi->curVid.vid);

	Page 			page;
	BlockNumber 	bucketNumber;
	OffsetNumber 	bucketOffset;
	VidMappingInfo *vmi;
	Buffer 			buf;
	LWLockId		vidLock;

	vmi = rel->rd_vmi;

	bucketNumber = getBucketNumberFromVidMacro(vid);
	bucketOffset = getBucketOffsetFromVidMacro(vid);

	buf = ReadBucketBuffer(rel, bucketNumber);

	page = BucketBufferGetPage(buf, vmi->bufferBlocks);

//	vidLock = getVidLockIdMacro(tbi->bufferVidLocks, buf, bucketOffset);
//	LWLockAcquire(vidLock, LW_SHARED);
	memcpy(iptr, BucketReadItemPointer(page, bucketOffset), SIAS_BUCKET_ENTRY_SIZE);
//	LWLockRelease(vidLock);

	ReleaseBucketBuffer(vmi, buf);
}

void
insertVidTidPair(Relation rel, uint32 vid, BlockNumber blkNum, OffsetNumber offNum)
{
	Buffer 			buf;
	Page 			page;
	ItemPointer 	iptr;
	BlockNumber 	bucketNumber;
	OffsetNumber 	bucketOffset;
	VidMappingInfo *vmi;
	LWLockId		vidLock;


	vmi = rel->rd_vmi;

	bucketNumber = getBucketNumberFromVidMacro(vid);
	bucketOffset = getBucketOffsetFromVidMacro(vid);

	// prepare data to be inserted
	iptr = palloc(sizeof(ItemPointerData));
	ItemPointerSetBlockNumber(iptr, blkNum);
	ItemPointerSetOffsetNumber(iptr, offNum);

	buf = ReadBucketBuffer(rel, bucketNumber);
	page = BucketBufferGetPage(buf, vmi->bufferBlocks);

//	vidLock = getVidLockIdMacro(tbi->bufferVidLocks, buf, bucketOffset);
//	LWLockAcquire(vidLock, LW_EXCLUSIVE);
	int index = SIAS_BUCKET_SIZE * (buf - 1) + (bucketOffset - 1);
	SpinLockAcquire(&(vmi->bufferVidExclusiveLocks[index]));
	PageAddVidTidPair(page, (Item) iptr, bucketOffset);
	SpinLockRelease(&(vmi->bufferVidExclusiveLocks[index]));
//	LWLockRelease(vidLock);

	pfree(iptr);

	MarkBucketBufferDirty(buf, vmi);

	ReleaseBucketBuffer(vmi, buf);
}

ItemPointer
BucketReadItemPointer(Page bucket, OffsetNumber offsetNumber)
{
	Assert(1 <= offsetNumber);
	Assert(offsetNumber <= SIAS_BUCKET_SIZE);

	PageHeader phdr = (PageHeader) bucket;

	return ((ItemPointer) (bucket + phdr->pd_lower + (SIAS_BUCKET_ENTRY_SIZE * (offsetNumber - 1))));
}

uint32
getNewVid(Relation rel)
{
	uint32 			newVid;
	VidMappingInfo *vmi;
	Buffer 			buf;

	vmi = rel->rd_vmi;

	// TODO try to hold mutex for a shorter time

	SpinLockAcquire(&(vmi->vidIncrementMutex));

	newVid = vmi->curVid.vid + 1;

	// first VID in a new bucket
	// -> extend bucket relation
	if (getBucketOffsetFromVidMacro(newVid) == 1) {

		buf = ReadBucketBuffer(rel, P_NEW);
		ReleaseBucketBuffer(vmi, buf);
	}

	vmi->curVid.vid++;
	SpinLockRelease(&(vmi->vidIncrementMutex));

	return newVid;
}

bool
isTupleValid(Relation rel, uint32 vid, ItemPointer itemPointer1)
{
	ItemPointer itemPointer2;

	itemPointer2 = palloc(sizeof(ItemPointerData));
	getItemPointerFromVidArray(rel, vid, itemPointer2);
	return ItemPointerEquals(itemPointer1, itemPointer2);
}

BlockNumber
GetWriteableRegion(Relation rel, bool forHotData)
{
	BlockNumber firstBlock;


	firstBlock = GetCleanedRegion(rel, forHotData);

	if (firstBlock == InvalidBlockNumber)
	{
		firstBlock = GetNeverUsedRegion(rel);
	}

	return firstBlock;
}

BlockNumber
GetNeverUsedRegion(Relation rel)
{
	TargetBlockInfo *tbi;


	tbi = rel->rd_tbi;

	if (GetBlocksRegionNumber(tbi->highestAssignedRegion + SIAS_APPEND_SIZE) >= SIAS_MAX_REGIONS)
	{
		elog(PANIC, "trying to use more than SIAS_MAX_REGIONS regions. Consider increasing SIAS_MAX_REGIONS parameter");
	}

	tbi->highestAssignedRegion += SIAS_APPEND_SIZE;

	return tbi->highestAssignedRegion;
}

BlockNumber
GetCleanedRegion(Relation rel, bool forHotData)
{
	GarbageCollectionInfo *gci;
	BlockNumber cleanedRegion;
	RegionsQueue *crq;

#ifndef SIAS_GC
	return InvalidBlockNumber;
#endif

	//TODO testing
//	return InvalidBlockNumber;

	gci = rel->rd_gci;
	crq = &gci->cleanedRegionsQueue;


	SpinLockAcquire(&(crq->mutex));

	/*
	 * It is necessary to ensure that:
	 *
	 * 1) just cleaned regions don't get reused for a
	 * 	  while, which allows (long-running) transactions
	 * 	  to still access tuples on them.
	 * 	  Such a tuple has been garbage collected, but if
	 * 	  a transaction started to traverse the version
	 * 	  chain of the respective item before the tuple's
	 * 	  GC was finished, the transaction might try to
	 * 	  access the old copy of the tuple, which is
	 * 	  still located in the cleaned, but not yet
	 * 	  reused, region.
	 *
	 * 2) garbage collection (GC) always writes cold data
	 * 	  into regions, which have been written at least once,
	 * 	  thus it never does appends.
	 * 	  This would be already guaranteed, if the
	 * 	  append wouldn't be allowed to reuse the last
	 * 	  cleaned region in the queue.
	 */
	if (crq->numElements <= SIAS_MIN_CLEANED_REGIONS + forHotData)
	{
		SpinLockRelease(&(crq->mutex));
		return InvalidBlockNumber;
	}

	if (crq->head >= SIAS_MAX_QUEUED_REGIONS
			|| crq->head < 0)
	{
		elog(ERROR, "invalid index for headCRQ");
	}

	cleanedRegion = crq->elements[crq->head];
	crq->elements[crq->head] = InvalidBlockNumber;
	if (++crq->head == SIAS_MAX_QUEUED_REGIONS)
	{
		crq->head = 0;
	}
	crq->numElements--;

	if (crq->numElements < SIAS_MIN_CLEANED_REGIONS)
	{
		elog(FATAL, "There are less than SIAS_MIN_CLEANED_REGIONS many cleaned regions.");
	}

	SpinLockRelease(&(crq->mutex));

	return cleanedRegion;
}

void
AddCleanedRegion(Relation rel, BlockNumber firstBlockOfRegion)
{
	GarbageCollectionInfo *gci;
	int oldTail;
	RegionsQueue *crq;

	gci = rel->rd_gci;
	crq = &gci->cleanedRegionsQueue;

	SpinLockAcquire(&(crq->mutex));

	oldTail = crq->tail;

	if (++crq->tail == SIAS_MAX_QUEUED_REGIONS)
	{
		crq->tail = 0;
	}

	if (crq->head == crq->tail)
	{
		crq->tail = oldTail;
		SpinLockRelease(&(crq->mutex));
		elog(ERROR, "Can't add another entry to cleanedRegionsQueue. Queue is full! Consider increasing SIAS_MAX_QUEUED_REGIONS parameter.");
	}

	if (crq->elements[crq->tail] != InvalidBlockNumber)
	{
		elog(WARNING, "overwriting non-default entry in cleanedRegionsQueue");
	}

	crq->elements[crq->tail] = firstBlockOfRegion;
	crq->numElements++;

	SpinLockRelease(&(crq->mutex));
}

TargetBlockInfo*
mapRelFileNodeToTbi(RelFileNode *rfnPtr)
{
	rfnHashLookupEntry *result;
	bool found;
	TargetBlockInfo *tbi;

	result = (rfnHashLookupEntry *)
		hash_search_with_hash_value(rfnUserRelationIdHashTable,
									(void *) rfnPtr,
									get_hash_value(rfnUserRelationIdHashTable, (void *) rfnPtr),
									HASH_FIND,
									&found);

	if (!found)
	{
		elog(ERROR, "There is no entry for this RelFileNode in rfnUserRelationIdHashTable.");
	}

	tbi = userRelationTbis[result->userRelationId];

	if (!tbi)
	{
		/*
		 * This should not happen, because a user relation's TargetBlockInfo
		 * pointer should have been remembered at creation time of that
		 * user relation.
		 */
		elog(ERROR, "There is no pointer to a TargetBlockInfo for this UserRelationId in userRelationTbis.");
	}

	return tbi;
}

GarbageCollectionInfo*
mapRelFileNodeToGci(RelFileNode *rfnPtr)
{
	rfnHashLookupEntry *result;
	bool found;
	GarbageCollectionInfo *gci;

	result = (rfnHashLookupEntry *)
		hash_search_with_hash_value(rfnUserRelationIdHashTable,
									(void *) rfnPtr,
									get_hash_value(rfnUserRelationIdHashTable, (void *) rfnPtr),
									HASH_FIND,
									&found);

	if (!found)
	{
		elog(ERROR, "There is no entry for this RelFileNode in rfnUserRelationIdHashTable.");
	}

	gci = userRelationGcis[result->userRelationId];

	if (!gci)
	{
		/*
		 * This should not happen, because a user relation's GarbageCollectionInfo
		 * pointer should have been remembered at creation time of that
		 * user relation.
		 */
		elog(ERROR, "There is no pointer to a GarbageCollectionInfo for this UserRelationId in userRelationGcis.");
	}

	return gci;
}

static Size
VmiShmemSize(void)
{
	Size size;

	// first sum up amount of shmem needed to hold all bucket buffer
	// related data for one user relation;
	// then multiply it with the maximum expected number of user relations

	/* some buffer to compensate for unaccounted objects */
	size = 10000;

	/* size of VidMappingInfo struct */
	size = add_size(size, sizeof(VidMappingInfo));

	/* size of bucket relation's RelationData struct */
	size = add_size(size, sizeof(RelationData));


	// next 4 LOC resemble BufferShmemSize()

	/* size of buffer descriptors */
	size = add_size(size, mul_size(SIAS_BUFFER_POOL_SIZE, sizeof(BufferDesc)));

	/* size of buffer blocks */
	size = add_size(size, mul_size(SIAS_BUFFER_POOL_SIZE, BLCKSZ));

	/* size of lookup hash table */
	size = add_size(size, BufTableShmemSize(SIAS_BUFFER_POOL_SIZE + SIAS_BUFFER_POOL_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	/* size of the buffer partition locks array */
	size = add_size(size, mul_size(sizeof(LWLockId), SIAS_BUFFER_POOL_PARTITIONS));

	/* size of the VID lock array */
	//size = add_size(size, mul_size(sizeof(LWLockId), mul_size(SIAS_BUCKET_SIZE, SIAS_BUFFER_POOL_SIZE)));
	size = add_size(size, mul_size(sizeof(slock_t), mul_size(SIAS_BUCKET_SIZE, SIAS_BUFFER_POOL_SIZE)));

	/* size of HASHHDR */
	size = add_size(size, sizeOfHASHHDR());



	//
	// total by multiplying with SIAS_MAX_RELATIONS
	//
	size = mul_size(size, SIAS_MAX_RELATIONS);

	return size;
}

static Size
TbiShmemSize(void)
{
	Size size;

	// first sum up amount of shmem needed to hold target block
	// management related data for one user relation;
	// then multiply it with the maximum expected number of user relations

	/* some buffer to compensate for unaccounted objects */
	size = 10000;

	/* size of TargetBlockInfo struct */
	size = add_size(size, sizeof(TargetBlockInfo));

	/* size of targetBlockSlots array */
	size = add_size(size, mul_size(sizeof(BlockNumber), SIAS_CONCURRENT_TARGET_BLOCKS));

	/* size of slotUsers array */
	size = add_size(size, mul_size(sizeof(int), SIAS_CONCURRENT_TARGET_BLOCKS));

	/* size of slotLocks array */
	size = add_size(size, mul_size(sizeof(LWLockId), SIAS_CONCURRENT_TARGET_BLOCKS));

	/* size of targetBlockSlotIndex array */
	size = add_size(size, mul_size(sizeof(int), SIAS_TOTAL_ACTIVE_TARGET_BLOCKS));

	/* size of cache1, cache2, cache3 array */
	size = add_size(size, mul_size(3, mul_size(sizeof(BLCKSZ), SIAS_APPEND_SIZE)));

	/* size of tbsiMutexes array */
	size = add_size(size, mul_size(sizeof(slock_t), SIAS_TOTAL_ACTIVE_TARGET_BLOCKS));

	/* size of slotsInUse array */
	size = add_size(size, mul_size(sizeof(int), SIAS_CONCURRENT_TARGET_BLOCKS));

	/* size of unusedSlots array */
	size = add_size(size, mul_size(sizeof(int), SIAS_CONCURRENT_TARGET_BLOCKS));

	/* size of previouslyUsedBlocks array */
	size = add_size(size, mul_size(sizeof(BlockNumber), SIAS_CONCURRENT_TARGET_BLOCKS));


	//
	// total by multiplying with SIAS_MAX_RELATIONS
	//
	size = mul_size(size, SIAS_MAX_RELATIONS);

	return size;
}

static Size
GciShmemSize(void)
{
	Size size;

	// first sum up amount of shmem needed to hold garbage
	// collection related data for one user relation;
	// then multiply it with the maximum expected number of user relations

	/* some buffer to compensate for unaccounted objects */
	size = 10000;

	/* size of cleanedRegionsQueue */
	size = add_size(size, sizeof(RegionsQueue));
	size = add_size(size, mul_size(sizeof(BlockNumber), SIAS_MAX_QUEUED_REGIONS));

	/* size of dirtyRegions */
	size = add_size(size, hash_estimate_size(SIAS_MAX_QUEUED_REGIONS, sizeof(DirtyRegionData)));
	size = add_size(size, SizeOfHTAB());

	/* size of updateDownCounters array */
	size = add_size(size, mul_size(sizeof(int), SIAS_MAX_REGIONS));

	/* size of maxUpdaterXmin array */
	size = add_size(size, mul_size(sizeof(TransactionId), SIAS_MAX_REGIONS));

	/* size of newlyUpdatedData array */
	size = add_size(size, mul_size(sizeof(uint32), SIAS_MAX_REGIONS));

	/* size of regionGcCountersMutexes array */
	size = add_size(size, mul_size(sizeof(slock_t), SIAS_MAX_REGIONS));

	/* size of coldCache array */
	size = add_size(size, mul_size(BLCKSZ, SIAS_APPEND_SIZE));

	/*
	 * TODO INCLUDING UPDATING SHMEM NUM STRUCTS
	 */



	//
	// total by multiplying with SIAS_MAX_RELATIONS
	//
	size = mul_size(size, SIAS_MAX_RELATIONS);

	return size;
}

Size
SiasShmemSize(void)
{
	Size size;

	size = 0;

	// user relation specific objects

	/*
	 * size of all VidMappingInfo structures including
	 * all contained members and their possible needs
	 * of shared memory
	 */
	size = add_size(size, VmiShmemSize());

	/*
	 * size of all TargetBlockInfo structures including
	 * all contained members and their possible needs
	 * of shared memory
	 */
	size = add_size(size, TbiShmemSize());

#ifdef SIAS_GC
	/*
	 * size of all GarbageCollectionInfo structures including
	 * all contained members and their possible needs
	 * of shared memory
	 */
	size = add_size(size, GciShmemSize());
#endif




	// unique objects

	/* size of SIASinitBucket */
	size = add_size(size, BLCKSZ);

	/* size of the UserRelationIdCounter */
	size = add_size(size, MAXALIGN(sizeof(UserRelationIdCounter)));

	/* size of array of all user relation's VidMappingInfo pointers*/
	size = add_size(size, mul_size(sizeof(VidMappingInfo*), SIAS_MAX_RELATIONS));

	/* size of rfnUserRelationIdHashTable*/
	size = add_size(size, hash_estimate_size(SIAS_MAX_RELATIONS, sizeof(uint32)));

	/* size of array of all user relation's TargetBlockInfo pointers*/
	size = add_size(size, mul_size(sizeof(TargetBlockInfo*), SIAS_MAX_RELATIONS));

#ifdef SIAS_GC
	/* size of array of all user relation's GarbageCollectionInfo pointers*/
	size = add_size(size, mul_size(sizeof(GarbageCollectionInfo*), SIAS_MAX_RELATIONS));
#endif

	return size;
}

static void
InitUserRelationIdCounter(void)
{
	bool found;


	uric = ShmemInitStruct("UserRelationIdCounter",
			sizeof(UserRelationIdCounter), &found);

	if (!found)
	{
		// initialization
		uric->nextUserRelationId = 0;
		SpinLockInit(&(uric->mutex));
	}
}

static void
InitUserRelationVmis(void)
{
	bool found;


	userRelationVmis = ShmemInitStruct("userRelationVmis",
			(sizeof(VidMappingInfo*) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		// initialization
		memset(userRelationVmis, 0, sizeof(VidMappingInfo*) * SIAS_MAX_RELATIONS);
	}
}

static void
InitUserRelationTbis(void)
{
	bool found;


	userRelationTbis = ShmemInitStruct("userRelationTbis",
			(sizeof(TargetBlockInfo*) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		memset(userRelationTbis, 0, sizeof(TargetBlockInfo*) * SIAS_MAX_RELATIONS);
	}
}

static void
InitUserRelationGcis(void)
{
	bool found;


	userRelationGcis = ShmemInitStruct("userRelationGcis",
			(sizeof(GarbageCollectionInfo*) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		memset(userRelationGcis, 0, sizeof(GarbageCollectionInfo*) * SIAS_MAX_RELATIONS);
	}
}

static void
InitUserRelationOids(void)
{
	bool found;


	userRelationOids = ShmemInitStruct("userRelationOids",
			(sizeof(Oid) * SIAS_MAX_RELATIONS), &found);

	if (!found)
	{
		memset(userRelationOids, 0, sizeof(Oid) * SIAS_MAX_RELATIONS);
	}
}

void
InitRfnUserRelationIdHashTable(bool rebuild)
{
	HASHCTL		info;
	ShmemIndexEnt *shmemIndexEntry;
	rfnHashLookupEntry *rfnUrIdList;
	rfnHashLookupEntry *entry;
	int r;

	info.keysize = sizeof(RelFileNode);
	info.entrysize = sizeof(rfnHashLookupEntry);
	info.hash = tag_hash;

	rfnUserRelationIdHashTable = ShmemInitHash("rfnUrIdTable",
								  SIAS_MAX_RELATIONS, SIAS_MAX_RELATIONS,
								  &info,
								  HASH_ELEM | HASH_FUNCTION);

	if (rebuild)
	{
		shmemIndexEntry = GetShmemIndexEntry("rfnUrIdList");

		/*
		 * rfnUrIdList is used to persist all entries of
		 * rfnUserRelationIdHashTable during shutdown
		 *
		 * Just insert each of the entries in rfnUrIdList
		 * into the hash table.
		 */
		if (shmemIndexEntry)
		{
			rfnUrIdList = (rfnHashLookupEntry *) shmemIndexEntry->location;

			for (r = 0; r < SIAS_MAX_RELATIONS; r++)
			{
				/*
				 * end of valid list entries reached
				 */
				if (!OidIsValid(rfnUrIdList[r].rfn.relNode))
				{
					break;
				}

				entry = (rfnHashLookupEntry *) hash_search(rfnUserRelationIdHashTable, &(rfnUrIdList[r].rfn), HASH_ENTER, NULL);
				entry->userRelationId = rfnUrIdList[r].userRelationId;
			}
		}
		else
		{
			elog(ERROR, "There is no rfnUrIdList to rebuild rfnUserRelationIdHashTable.");
		}
	}
}

/*
 * initializes all objects, which are shared among the backends,
 * but not user relation specific and reside in shared memory
 */
void
InitSharedSIASobjects(void)
{
	InitUserRelationIdCounter();
	InitUserRelationVmis();
	InitRfnUserRelationIdHashTable(false);
	InitUserRelationTbis();
	GetInitBucket();
#ifdef SIAS_GC
	InitUserRelationGcis();
#endif
	InitUserRelationOids();
}

void
ValidateSiasParameter()
{
#ifdef SIAS
	char topic[] = {"SIAS parameter issue"};

	// target block management parameter

	if (SIAS_TOTAL_ACTIVE_TARGET_BLOCKS < SIAS_CONCURRENT_TARGET_BLOCKS)
		elog(PANIC, "%s: SIAS_TOTAL_ACTIVE_TARGET_BLOCKS must be greater or equal SIAS_CONCURRENT_TARGET_BLOCKS", topic);

	if (0 >= SIAS_APPEND_SIZE)
		elog(PANIC, "%s: SIAS_APPEND_SIZE must be greater 0", topic);

	if ((SIAS_APPEND_SIZE & (SIAS_APPEND_SIZE - 1)) != 0)
		elog(WARNING, "%s: SIAS_APPEND_SIZE should be a power of 2", topic);

	if (0 >= SIAS_CONCURRENT_TARGET_BLOCKS)
		elog(PANIC, "%s: SIAS_CONCURRENT_TARGET_BLOCKS must be greater 0", topic);

	if (0 >= SIAS_MAX_REPLACE_TRIES)
		elog(PANIC, "%s: SIAS_MAX_REPLACE_TRIES must be greater 0", topic);


	// bucket buffer management parameter

	if (SIAS_BUFFER_POOL_SIZE < SIAS_BUFFER_POOL_PARTITIONS)
		elog(PANIC, "%s: SIAS_BUFFER_POOL_SIZE must be greater or equal SIAS_BUFFER_POOL_PARTITIONS", topic);

	if ((SIAS_BUFFER_POOL_PARTITIONS & (SIAS_BUFFER_POOL_PARTITIONS - 1)) != 0)
		elog(PANIC, "%s: SIAS_BUFFER_POOL_PARTITIONS must be a power of 2", topic);


#ifdef SIAS_GC
	// garbage collection parameter

	if (SIAS_GARBAGE_THRESHOLD_HOT < 0 || 100 < SIAS_GARBAGE_THRESHOLD_HOT)
		elog(PANIC, "%s: SIAS_GARBAGE_THRESHOLD_HOT must be between 0 and 100", topic);

	if (SIAS_GARBAGE_THRESHOLD_COLD < 0 || 100 < SIAS_GARBAGE_THRESHOLD_COLD)
		elog(PANIC, "%s: SIAS_GARBAGE_THRESHOLD_COLD must be between 0 and 100", topic);

	if (0 >= SIAS_MIN_CLEANED_REGIONS)
		elog(PANIC, "%s: SIAS_MIN_CLEANED_REGIONS must be greater 0", topic);

	if (0 >= SIAS_MAX_QUEUED_REGIONS)
		elog(PANIC, "%s: SIAS_MAX_QUEUED_REGIONS must be greater 0", topic);

	if (0 >= SIAS_UPDATE_XMIN_INTERVAL)
		elog(PANIC, "%s: SIAS_UPDATE_XMIN_INTERVAL must be greater 0", topic);
#endif
#endif
}

void
PrintData(Pointer start, int length)
{
	uint32 *i;
	Pointer endOfData;
	int rowEntries = 0;


	i = start;
	endOfData = start + length;


	for (;i < endOfData; i++, rowEntries++)
	{
		if (rowEntries == 16)
		{
			printf("\n");
			rowEntries = 0;
		}
		printf("%10d", *i);
	}
	printf("\n");
}
