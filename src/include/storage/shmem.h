/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/shmem.h,v 1.57 2010/01/02 16:58:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "utils/hsearch.h"


/* shmqueue.c */
typedef struct SHM_QUEUE
{
	struct SHM_QUEUE *prev;
	struct SHM_QUEUE *next;
} SHM_QUEUE;

/* size constants for the shmem index table */
 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* estimated size of the shmem index table (not a hard limit) */
#define SHMEM_INDEX_SIZE		 (1024)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];		/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes allocated for the structure */
} ShmemIndexEnt;

/* shmem.c */
extern void InitShmemAccess(void *seghdr);
extern void InitShmemAllocation(void);
extern void *ShmemAlloc(Size size);
extern bool ShmemAddrIsValid(void *addr);
extern void InitShmemIndex(void);
extern HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
			  HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);
extern ShmemIndexEnt* GetShmemIndexEntry(const char *name);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);



/*
 * prototypes for functions in shmqueue.c
 */
extern void SHMQueueInit(SHM_QUEUE *queue);
extern void SHMQueueElemInit(SHM_QUEUE *queue);
extern void SHMQueueDelete(SHM_QUEUE *queue);
extern void SHMQueueInsertBefore(SHM_QUEUE *queue, SHM_QUEUE *elem);
extern Pointer SHMQueueNext(SHM_QUEUE *queue, SHM_QUEUE *curElem,
			 Size linkOffset);
extern bool SHMQueueEmpty(SHM_QUEUE *queue);

#endif   /* SHMEM_H */
