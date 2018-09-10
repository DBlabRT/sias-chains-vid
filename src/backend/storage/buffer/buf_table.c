/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for mapping BufferTags to buffer indexes.
 *
 * Note: the routines in this file do no locking of their own.	The caller
 * must hold a suitable lock on the appropriate BufMappingLock, as specified
 * in the comments.  We can't do the locking inside these functions because
 * in most cases the caller needs to adjust the buffer header contents
 * before the lock is released (see notes in README).
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/buf_table.c,v 1.52 2010/04/28 16:54:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sias.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "utils/rel.h"



/* entry for buffer lookup hashtable */
typedef struct
{
	BufferTag	key;			/* Tag of a disk page */
	int			id;				/* Associated buffer ID */
} BufferLookupEnt;

static HTAB *SharedBufHash;

/*
 *  holds pointers to the HTABs of the bucket buffers
 *	of all user relations, which have been accessed at
 *	least once by this backend
 */
static HTAB	**bucketBufHashes;

/*
 * Estimate space needed for mapping hashtable
 *		size is the desired hash table size (possibly more than NBuffers)
 */
Size
BufTableShmemSize(int size)
{
	return hash_estimate_size(size, sizeof(BufferLookupEnt));
}

/*
 * Initialize shmem hash table for mapping buffers
 *		size is the desired hash table size (possibly more than NBuffers)
 */
void
InitBufTable(int size)
{
	HASHCTL		info;

	/* assume no locking is needed yet */

	/* BufferTag maps to Buffer */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.hash = tag_hash;
	info.num_partitions = NUM_BUFFER_PARTITIONS;

	SharedBufHash = ShmemInitHash("Shared Buffer Lookup Table",
								  size, size,
								  &info,
								  HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);
}

/*
 * Initialize shmem hash table for mapping bucket buffers
 *		size is the desired hash table size (possibly more than SIAS_BUFFER_POOL_SIZE),
 *		rel is the user relation for whose bucket buffer pool's access the to-be-created
 *		hash table is going to be used
 */
HTAB*
InitBucketBufTable(int size, Relation rel)
{
	HASHCTL		info;

	// assume no locking is needed yet

	// BufferTag maps to Buffer
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.hash = tag_hash;
	info.num_partitions = SIAS_BUFFER_POOL_PARTITIONS;

	char name6[(8 * sizeof(rel->rd_id) + 2) / 3 + 18];
	sprintf(name6, "bufTable %u", rel->rd_id);

	return ShmemInitHash(name6,
			  size, size,
			  &info,
			  HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);
}

/*
 * BufTableHashCode
 *		Compute the hash code associated with a BufferTag
 *
 * This must be passed to the lookup/insert/delete routines along with the
 * tag.  We do it like this because the callers need to know the hash code
 * in order to determine which buffer partition to lock, and we don't want
 * to do the hash computation twice (hash_any is a bit slow).
 */
uint32
BufTableHashCode(BufferTag *tagPtr)
{
	return get_hash_value(SharedBufHash, (void *) tagPtr);
}

/*
 * BucketBufTableHashCode
 *		Compute the hash code associated with a BufferTag
 *
 * This must be passed to the lookup/insert/delete routines along with the
 * tag.  We do it like this because the callers need to know the hash code
 * in order to determine which bucket buffer partition to lock, and we don't
 * want to do the hash computation twice (hash_any is a bit slow).
 */
uint32
BucketBufTableHashCode(BufferTag *tagPtr, HTAB *sharedBufHash)
{
	return get_hash_value(sharedBufHash, (void *) tagPtr);
}

/*
 * BufTableLookup
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
 *
 * Caller must hold at least share lock on BufMappingLock for tag's partition
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return -1;

	return result->id;
}

/*
 * BucketBufTableLookup
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
 *
 * Caller must hold at least share lock on BufMappingLock for tag's partition
 */
int
BucketBufTableLookup(BufferTag *tagPtr, uint32 hashcode, HTAB *sharedBufHash)
{
	BufferLookupEnt *result;
	result = (BufferLookupEnt *)
		hash_search_with_hash_value(sharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return -1;

	return result->id;
}

/*
 * BufTableInsert
 *		Insert a hashtable entry for given tag and buffer ID,
 *		unless an entry already exists for that tag
 *
 * Returns -1 on successful insertion.	If a conflicting entry exists
 * already, returns the buffer ID in that entry.
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
int
BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{
	BufferLookupEnt *result;
	bool		found;

	Assert(buf_id >= 0);		/* -1 is reserved for not-in-table */
	Assert(tagPtr->blockNum != P_NEW);	/* invalid tag */

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)					/* found something already in the table */
		return result->id;

	result->id = buf_id;

	return -1;
}

/*
 * BucketBufTableInsert
 *		Insert a hashtable entry for given tag and bucket buffer ID,
 *		unless an entry already exists for that tag
 *
 * Returns -1 on successful insertion.	If a conflicting entry exists
 * already, returns the bucket buffer ID in that entry.
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
int
BucketBufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id, HTAB *sharedBufHash)
{
	BufferLookupEnt *result;
	bool		found;

	Assert(buf_id >= 0);		/* -1 is reserved for not-in-table */
	Assert(tagPtr->blockNum != P_NEW);	/* invalid tag */

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(sharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)					/* found something already in the table */
		return result->id;

	result->id = buf_id;

	return -1;
}

/*
 * BufTableDelete
 *		Delete the hashtable entry for given tag (which must exist)
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
void
BufTableDelete(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_REMOVE,
									NULL);

	if (!result)				/* shouldn't happen */
		elog(ERROR, "shared buffer hash table corrupted");
}

/*
 * BucketBufTableDelete
 *		Delete the hashtable entry for given tag (which must exist)
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
void
BucketBufTableDelete(BufferTag *tagPtr, uint32 hashcode, HTAB *sharedBufHash)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(sharedBufHash,
									(void *) tagPtr,
									hashcode,
									HASH_REMOVE,
									NULL);

	if (!result)				/* shouldn't happen */
		elog(ERROR, "bucket buffer hash table corrupted");
}

HTAB*
getBucketBufHash(Relation rel)
{
	uint32	userRelationId;


	userRelationId = rel->rd_vmi->userRelationId;

	Assert(userRelationId < SIAS_MAX_RELATIONS);

	if (bucketBufHashes == NULL)
	{
		// init bucketBufHashes
		bucketBufHashes = (HTAB**) calloc(SIAS_MAX_RELATIONS, sizeof(HTAB*));
	}

	if (bucketBufHashes[userRelationId] == NULL)
	{
		// first access of this backend to this user relation
		// -> create own, locally kept HTAB pointer

		/*
		 * Initialize the shared bucket buffer lookup hashtable.
		 *
		 * Since we can't tolerate running out of lookup table entries, we must be
		 * sure to specify an adequate table size here.  The maximum steady-state
		 * usage is of course SIAS_BUFFER_POOL_SIZE entries, but BucketBufferAlloc()
		 * tries to insert a new entry before deleting the old.  In principle this could be
		 * happening in each partition concurrently, so we could need as many as
		 * SIAS_BUFFER_POOL_SIZE + SIAS_BUFFER_POOL_PARTITIONS entries.
		 */
		bucketBufHashes[userRelationId] = InitBucketBufTable(SIAS_BUFFER_POOL_SIZE + SIAS_BUFFER_POOL_PARTITIONS, rel);
	}

	return bucketBufHashes[userRelationId];
}

long
EntriesInBufTable(void)
{
	return hash_get_num_entries(SharedBufHash);
}
