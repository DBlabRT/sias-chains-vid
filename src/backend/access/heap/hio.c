/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/hio.c,v 1.78 2010/02/09 21:43:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/sias.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include <time.h>


/*
 * RelationPutHeapTuple - place tuple at specified page
 *
 * !!! EREPORT(ERROR) IS DISALLOWED HERE !!!  Must PANIC on failure!!!
 *
 * Note - caller must hold BUFFER_LOCK_EXCLUSIVE on the buffer.
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple,
					 ItemPointer pred)
{
	Page		pageHeader;
	OffsetNumber offnum;
	ItemId		itemId;
	Item		item;

	/* Add the tuple to the page */
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* Update tuple->t_self to the actual position where it was stored */
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/* Insert the correct position into CTID of the stored tuple, too */
	itemId = PageGetItemId(pageHeader, offnum);
	item = PageGetItem(pageHeader, itemId);
	if (ItemPointerIsValid(pred)) {
		((HeapTupleHeader) item)->t_ctid = *pred;
	} else {
		((HeapTupleHeader) item)->t_ctid = tuple->t_self;
	}
}

/*
 * Read in a buffer, using bulk-insert strategy if bistate isn't NULL.
 */
static Buffer
ReadBufferBI(Relation relation, BlockNumber targetBlock,
			 BulkInsertState bistate)
{
	Buffer		buffer;

	/* If not bulk-insert, exactly like ReadBuffer */
	if (!bistate)
		return ReadBuffer(relation, targetBlock);

	/* If we have the desired block already pinned, re-pin and return it */
	if (bistate->current_buf != InvalidBuffer)
	{
		if (BufferGetBlockNumber(bistate->current_buf) == targetBlock)
		{
			IncrBufferRefCount(bistate->current_buf);
			return bistate->current_buf;
		}
		/* ... else drop the old buffer */
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/* Perform a read using the buffer strategy */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								RBM_NORMAL, bistate->strategy);

	/* Save the selected block as target for future inserts */
	IncrBufferRefCount(buffer);
	bistate->current_buf = buffer;

	return buffer;
}

/*
 * RelationGetBufferForTuple
 *
 *	Returns pinned and exclusive-locked buffer of a page in given relation
 *	with free space >= given len.
 *
 *	If otherBuffer is not InvalidBuffer, then it references a previously
 *	pinned buffer of another page in the same relation; on return, this
 *	buffer will also be exclusive-locked.  (This case is used by heap_update;
 *	the otherBuffer contains the tuple being updated.)
 *
 *	The reason for passing otherBuffer is that if two backends are doing
 *	concurrent heap_update operations, a deadlock could occur if they try
 *	to lock the same two buffers in opposite orders.  To ensure that this
 *	can't happen, we impose the rule that buffers of a relation must be
 *	locked in increasing page number order.  This is most conveniently done
 *	by having RelationGetBufferForTuple lock them both, with suitable care
 *	for ordering.
 *
 *	NOTE: it is unlikely, but not quite impossible, for otherBuffer to be the
 *	same buffer we select for insertion of the new tuple (this could only
 *	happen if space is freed in that page after heap_update finds there's not
 *	enough there).	In that case, the page will be pinned and locked only once.
 *
 *	We normally use FSM to help us find free space.  However,
 *	if HEAP_INSERT_SKIP_FSM is specified, we just append a new empty page to
 *	the end of the relation if the tuple won't fit on the current target page.
 *	This can save some cycles when we know the relation is new and doesn't
 *	contain useful amounts of free space.
 *
 *	HEAP_INSERT_SKIP_FSM is also useful for non-WAL-logged additions to a
 *	relation, if the caller holds exclusive lock and is careful to invalidate
 *	relation's smgr_targblock before the first insertion --- that ensures that
 *	all insertions will occur into newly added pages and not be intermixed
 *	with tuples from other transactions.  That way, a crash can't risk losing
 *	any committed data of other transactions.  (See heap_insert's comments
 *	for additional constraints needed for safe usage of this behavior.)
 *
 *	The caller can also provide a BulkInsertState object to optimize many
 *	insertions into the same relation.	This keeps a pin on the current
 *	insertion target page (to save pin/unpin cycles) and also passes a
 *	BULKWRITE buffer selection strategy object to the buffer manager.
 *	Passing NULL for bistate selects the default behavior.
 *
 *	We always try to avoid filling existing pages further than the fillfactor.
 *	This is OK since this routine is not consulted when updating a tuple and
 *	keeping it on the same page, which is the scenario fillfactor is meant
 *	to reserve space for.
 *
 *	ereport(ERROR) is allowed here, so this routine *must* be called
 *	before any (unlogged) changes are made in buffer pool.
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  struct BulkInsertStateData *bistate)
{
	bool		use_fsm = !(options & HEAP_INSERT_SKIP_FSM);
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		pageFreeSpace,
				saveFreeSpace;
	BlockNumber targetBlock,
				otherBlock;
	bool		needLock;

	len = MAXALIGN(len);		/* be conservative */

	/* Bulk insert is not supported for updates, only inserts. */
	Assert(otherBuffer == InvalidBuffer || !bistate);

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) len,
						(unsigned long) MaxHeapTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;		/* just to keep compiler quiet */

	/*
	 * We first try to put the tuple on the same page we last inserted a tuple
	 * on, as cached in the BulkInsertState or relcache entry.	If that
	 * doesn't work, we ask the Free Space Map to locate a suitable page.
	 * Since the FSM's info might be out of date, we have to be prepared to
	 * loop around and retry multiple times. (To insure this isn't an infinite
	 * loop, we must update the FSM with the correct amount of free space on
	 * each page that proves not to be suitable.)  If the FSM has no record of
	 * a page with enough free space, we give up and extend the relation.
	 *
	 * When use_fsm is false, we either put the tuple onto the existing target
	 * page or extend the relation.
	 */
	if (len + saveFreeSpace > MaxHeapTupleSize)
	{
		/* can't fit, don't bother asking FSM */
		targetBlock = InvalidBlockNumber;
		use_fsm = false;
	}
	else if (bistate && bistate->current_buf != InvalidBuffer)
		targetBlock = BufferGetBlockNumber(bistate->current_buf);
	else
		targetBlock = RelationGetTargetBlock(relation);

	if (targetBlock == InvalidBlockNumber && use_fsm)
	{
		/*
		 * We have no cached target page, so ask the FSM for an initial
		 * target.
		 */
		targetBlock = GetPageWithFreeSpace(relation, len + saveFreeSpace);

		/*
		 * If the FSM knows nothing of the rel, try the last page before we
		 * give up and extend.	This avoids one-tuple-per-page syndrome during
		 * bootstrapping or in a recently-started system.
		 */
		if (targetBlock == InvalidBlockNumber)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

			if (nblocks > 0)
				targetBlock = nblocks - 1;
		}
	}

	while (targetBlock != InvalidBlockNumber)
	{
		/*
		 * Read and exclusive-lock the target block, as well as the other
		 * block if one was given, taking suitable care with lock ordering and
		 * the possibility they are the same block.
		 */
		if (otherBuffer == InvalidBuffer)
		{
			/* easy case */
			buffer = ReadBufferBI(relation, targetBlock, bistate);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/* also easy case */
			buffer = otherBuffer;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock < targetBlock)
		{
			/* lock other buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* lock target buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * Now we can check to see if there's enough free space here. If so,
		 * we're done.
		 */
		page = BufferGetPage(buffer);
		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (len + saveFreeSpace <= pageFreeSpace)
		{
			/* use this page as future insert target, too */
			RelationSetTargetBlock(relation, targetBlock);
			return buffer;
		}

		/*
		 * Not enough space, so we must give up our page locks and pin (if
		 * any) and prepare to look elsewhere.	We don't care which order we
		 * unlock the two buffers in, so this can be slightly simpler than the
		 * code above.
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (otherBuffer == InvalidBuffer)
			ReleaseBuffer(buffer);
		else if (otherBlock != targetBlock)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}

		/* Without FSM, always fall out of the loop and extend */
		if (!use_fsm)
			break;

		/*
		 * Update FSM as to condition of this page, and ask for another page
		 * to try.
		 */
		targetBlock = RecordAndGetPageWithFreeSpace(relation,
													targetBlock,
													pageFreeSpace,
													len + saveFreeSpace);
	}

	/*
	 * Have to extend the relation.
	 *
	 * We have to use a lock to ensure no one else is extending the rel at the
	 * same time, else we will both try to initialize the same new page.  We
	 * can skip locking for new or temp relations, however, since no one else
	 * could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(relation);

	if (needLock)
		LockRelationForExtension(relation, ExclusiveLock);

	/*
	 * XXX This does an lseek - rather expensive - but at the moment it is the
	 * only way to accurately determine how many blocks are in a relation.	Is
	 * it worth keeping an accurate file length in shared memory someplace,
	 * rather than relying on the kernel to do it for us?
	 */
	buffer = ReadBufferBI(relation, P_NEW, bistate);

	/*
	 * We can be certain that locking the otherBuffer first is OK, since it
	 * must have a lower page number.
	 */
	if (otherBuffer != InvalidBuffer)
		LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Now acquire lock on the new page.
	 */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Release the file-extension lock; it's now OK for someone else to extend
	 * the relation some more.	Note that we cannot release this lock before
	 * we have buffer lock on the new page, or we risk a race condition
	 * against vacuumlazy.c --- see comments therein.
	 */
	if (needLock)
		UnlockRelationForExtension(relation, ExclusiveLock);

	/*
	 * We need to initialize the empty new page.  Double-check that it really
	 * is empty (this should never happen, but if it does we don't want to
	 * risk wiping out valid data).
	 */
	page = BufferGetPage(buffer);

	if (!PageIsNew(page))
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 BufferGetBlockNumber(buffer),
			 RelationGetRelationName(relation));

	PageInit(page, BufferGetPageSize(buffer), 0);

	if (len > PageGetHeapFreeSpace(page))
	{
		/* We should not get here given the test at the top */
		elog(PANIC, "tuple is too big: size %lu", (unsigned long) len);
	}

	/*
	 * Remember the new page as our target for future insertions.
	 *
	 * XXX should we enter the new page into the free space map immediately,
	 * or just keep it for this backend's exclusive use in the short run
	 * (until VACUUM sees it)?	Seems to depend on whether you expect the
	 * current backend to make more insertions or not, which is probably a
	 * good bet most of the time.  So for now, don't add it to FSM yet.
	 */
	RelationSetTargetBlock(relation, BufferGetBlockNumber(buffer));

	return buffer;
}

Buffer
RelationGetBufferForSiasTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  struct BulkInsertStateData *bistate,
						  BlockNumber preferredBlock)
{
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		pageFreeSpace,
				saveFreeSpace;
	BlockNumber targetBlock,
				otherBlock;
	bool		needLock;
	volatile BufferDesc *bufHdr;
	int			targetBlockSlot = SIAS_InvalidSlotNumber;
	TargetBlockInfo *tbi;
	bool		otherBufferUnlocked;
	int			numReplacements;

	tbi = relation->rd_tbi;

	len = MAXALIGN(len);		/* be conservative */

	/* Bulk insert is not supported for updates, only inserts. */
	Assert(otherBuffer == InvalidBuffer || !bistate);


	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) len,
						(unsigned long) MaxHeapTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/*
	 * In case of an update otherBuffer was
	 * handed over BUFFER_LOCK_SHARE locked.
	 *
	 * Otherwise, in case of an insertion,
	 * otherBuffer equals InvalidBuffer and
	 * setting otherBufferUnlocked to false
	 * is the way to go to avoid any locking
	 * of otherBuffer.
	 */
	otherBufferUnlocked = false;

	if (otherBuffer != InvalidBuffer)
	{
		// update case
		otherBlock = BufferGetBlockNumber(otherBuffer);
	}
	else
	{
		// insert case
		otherBlock = InvalidBlockNumber;		/* just to keep compiler quiet */
	}

	/*
	 * Either we first try to put the tuple on the same page we last
	 * inserted a tuple on, as cached in the BulkInsertState or we get
	 * a target block via requesting a target block slot.
	 * In either way, if the respective target block is too full for us
	 * to add our data, we'll issue the target block to get a replacement.
	 */
	if (otherBuffer == InvalidBuffer)
	{
		if (bistate && bistate->current_buf != InvalidBuffer)
		{
			targetBlockSlot = AccessTargetBlockSlot(relation, BufferGetBlockNumber(bistate->current_buf),
								true, InvalidBuffer, &otherBufferUnlocked);
		}
		else if (preferredBlock != InvalidBlockNumber)
		{
			/*
			 * This case allows processes, that want to insert a tuple,
			 * but don't use bistate to do so, to preferably get a
			 * specific block (preferredBlock) to write to.
			 */
			targetBlockSlot = AccessTargetBlockSlot(relation, preferredBlock,
								true, InvalidBuffer, &otherBufferUnlocked);
		}
		else
		{
			targetBlockSlot = AccessTargetBlockSlot(relation, InvalidBlockNumber, true, InvalidBuffer, &otherBufferUnlocked);
		}
	}
	else
	{
		targetBlockSlot = AccessTargetBlockSlot(relation, otherBlock, false, otherBuffer, &otherBufferUnlocked);
	}

	// prevent append from running
	LWLockAcquire(tbi->appendLock, LW_SHARED);

	targetBlock = tbi->targetBlockSlots[targetBlockSlot];

	if (targetBlock == InvalidBlockNumber)
	{
		LWLockRelease(tbi->appendLock);

		ReplaceTargetBlock(relation, targetBlockSlot, targetBlock, true);

		// prevent append from running
		LWLockAcquire(tbi->appendLock, LW_SHARED);

		targetBlock = tbi->targetBlockSlots[targetBlockSlot];
	}

	if (!IsActiveTargetBlock(tbi, targetBlock))
	{
		elog(FATAL, "Target block slot doesn't contain active target block. "
				"holds %u, but %u-%u and %u-%u are the active target blocks",
				targetBlock, tbi->firstNextAppendBlock, tbi->firstNextAppendBlock + SIAS_APPEND_SIZE - 1,
							 tbi->firstOverflowBlock,   tbi->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS - 1);
	}

//	printf("target block %u\n", targetBlock);

	/*
	 * Read and exclusive-lock the target block, taking suitable
	 * care with the possibility that it's the same block as otherBlock.
	 */
	if (otherBuffer == InvalidBuffer)
	{
		// insert case
		buffer = ReadBufferBI(relation, targetBlock, bistate);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}
	else if (otherBlock == targetBlock)
	{
		// update case I: targetBlock already buffered
		buffer = otherBuffer;
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		otherBufferUnlocked = false;
	}
	else
	{
		// update case II: targetBlock has to be fetched
		buffer = ReadBuffer(relation, targetBlock);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	page = BufferGetPage(buffer);

	if (!IsTargetBlockInitialized(relation, targetBlock))
	{
		InitSiasBuffer(buffer);
		SetTargetBlockInitialized(relation, targetBlock);
		UpdateHighestUsedBlock(tbi, targetBlock);
	}

	/*
	 * The buffer is pinned and locked exclusively.
	 *
	 * We can allow a potentially waiting append to start,
	 * because the exclusive content lock on the buffer
	 * ensures that an append would have to wait for
	 * us to finish writing the buffer, since it needs
	 * to acquire a shared content lock of a buffer holding
	 * a to-be-appended target block.
	 */
	LWLockRelease(tbi->appendLock);

	/*
	 * Now we can check to see if there's enough free space here.
	 * If so, we're done.
	 */
	pageFreeSpace = PageGetHeapFreeSpace(page);
	if (len + saveFreeSpace <= pageFreeSpace)
	{
		/*
		 * Now we know that we won't change the target block
		 * in the slot (by issuing it as full).
		 * That's why we can allow another process,
		 * waiting to access the slot's target block,
		 * to proceed.
		 * If we'd release this lock earlier, it would be
		 * possible that two processes issue the same
		 * target block as full.
		 * With the current approach each target block
		 * is issued at most once until it is appended.
		 *
		 * Other than the owner of a slot, only an append
		 * can change a slot's target block, but another
		 * process waiting for our slot will prevent an
		 * append from running before accessing the slot's
		 * target block just as we did.
		 */
		ReleaseTargetBlockSlot(relation, targetBlockSlot);

		/*
		 * OtherBuffer should at least be shared
		 * locked on return as this is assumed from
		 * callers of this method.
		 *
		 * This case is only possible when you got the
		 * target block slot of otherBuffer's block,
		 * which causes it to get unlocked, but then
		 * an append replaced the slot's block and
		 * you locked a buffer different than otherBuffer.
		 */
		if (otherBufferUnlocked)
			LockBuffer(otherBuffer, BUFFER_LOCK_SHARE);

		return buffer;
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	if (otherBuffer == InvalidBuffer || otherBlock != targetBlock)
		ReleaseBuffer(buffer);
	/*
	 * If otherBuffer equals buffer at this point,
	 * we aren't holding the content_lock of
	 * otherBuffer anymore, because we just
	 * unlocked buffer.
	 * But otherBuffer should at least be shared
	 * locked on return as this is assumed from
	 * callers of this method.
	 * We have to remember to lock otherBuffer
	 * again later.
	 */
	if (otherBuffer != InvalidBuffer && otherBlock == targetBlock)
	{
		otherBufferUnlocked = true;
	}



	/*
	 * The given target block doesn't offer enough space
	 * to place the data, which should be written.
	 * -> We have to issue this block as full.
	 *
	 * This way we'll get another target block
	 * assigned to the same slot.
	 *
	 * Replacing a target block doesn't guarantee
	 * to receive an empty block, but the
	 * replacement block might have already been
	 * written and thus could be too full for us
	 * to write our data.
	 *
	 * That's why we might need to replace that
	 * block as well.
	 *
	 * But, after at most SIAS_MAX_REPLACE_TRIES
	 * many replacements we'll request an empty
	 * block, which should offer enough space for
	 * the data to be written.
	 */



	numReplacements = 0;

	while (numReplacements <= SIAS_MAX_REPLACE_TRIES)
	{
		if (numReplacements < SIAS_MAX_REPLACE_TRIES)
		{
			/*
			 * try out another target block, which might have
			 * already been used and thus could be full
			 */
			ReplaceTargetBlock(relation, targetBlockSlot, targetBlock, true);
		}
		else
		{
			/*
			 * Finally get a new, empty block which should
			 * offer enough free space to write our data.
			 */
			ReplaceTargetBlock(relation, targetBlockSlot, targetBlock, false);
		}

		numReplacements++;

		/*
		 * The new block, which is assigned to our target
		 * block slot now, isn't guaranteed to be empty.
		 *
		 * We have to proceed similarly as before, when we
		 * tried to write to the other, just replaced
		 * target block.
		 */

		// prevent append from running
		LWLockAcquire(tbi->appendLock, LW_SHARED);

		targetBlock = tbi->targetBlockSlots[targetBlockSlot];

		if (!IsActiveTargetBlock(tbi, targetBlock))
		{
			elog(FATAL, "Target block slot doesn't contain active target block. "
					"holds %u, but %u-%u and %u-%u are the active target blocks",
					targetBlock, tbi->firstNextAppendBlock, tbi->firstNextAppendBlock + SIAS_APPEND_SIZE - 1,
								 tbi->firstOverflowBlock,   tbi->firstOverflowBlock + SIAS_OVERFLOW_TARGET_BLOCKS - 1);
		}

//		printf("new target block %u\n", targetBlock);

		/*
		 * Read and exclusive-lock the target block.
		 */
		if (otherBuffer == InvalidBuffer)
		{
			// insert case
			buffer = ReadBufferBI(relation, targetBlock, bistate);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/*
			 * update case I: targetBlock already buffered
			 *
			 * This case should be impossible, because if
			 * otherBlock was a non-full, active target block
			 * when we accessed our slot in the first place,
			 * we would have gotten a slot holding otherBlock
			 * and tried to write to it.
			 *
			 * The fact that we are at this point now proves
			 * that otherBlock isn't a non-full, active target
			 * block anymore, because we would have chosen it
			 * as the block to be written to and wouldn't have
			 * replaced it and got to this point.
			 *
			 * That's why the replacement routine shouldn't
			 * offer us otherBlock as a target block here.
			 */
			elog(FATAL, "replacement block equals the block holding the to-be-updated tuple");
		}
		else
		{
			// update case II: targetBlock has to be fetched
			buffer = ReadBuffer(relation, targetBlock);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}

		page = BufferGetPage(buffer);

		if (!IsTargetBlockInitialized(relation, targetBlock))
		{
			InitSiasBuffer(buffer);
			SetTargetBlockInitialized(relation, targetBlock);
			UpdateHighestUsedBlock(tbi, targetBlock);
		}

		/*
		 * The buffer is pinned and locked exclusively.
		 *
		 * We can allow a potentially waiting append to start,
		 * because the exclusive content lock on the buffer
		 * ensures that an append would have to wait for
		 * us to finish writing the buffer, since it needs
		 * to acquire a shared content lock of a buffer holding
		 * a to-be-appended target block.
		 */
		LWLockRelease(tbi->appendLock);

		/*
		 * Now we can check to see if there's enough free space here.
		 * If so, we're done.
		 */
		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (len + saveFreeSpace <= pageFreeSpace)
		{
			/*
			 * Now we know that we won't change the target block
			 * in the slot (by issuing it as full).
			 * That's why we can allow another process,
			 * waiting to access the slot's target block,
			 * to proceed.
			 * If we'd release this lock earlier, it would be
			 * possible that two processes issue the same
			 * target block as full.
			 * With the current approach each target block
			 * is issued at most once until it is appended.
			 *
			 * Other than the owner of a slot, only an append
			 * can change a slot's target block, but another
			 * process waiting for our slot will prevent an
			 * append from running before accessing the slot's
			 * target block just as we did.
			 */
			ReleaseTargetBlockSlot(relation, targetBlockSlot);

			/*
			 * OtherBuffer should at least be shared
			 * locked on return as this is assumed from
			 * callers of this method.
			 */
			if (otherBufferUnlocked)
				LockBuffer(otherBuffer, BUFFER_LOCK_SHARE);

			return buffer;
		}

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	elog(FATAL, "Didn't find block with enough space.");

	return buffer;		/* just to keep compiler quiet */
}
