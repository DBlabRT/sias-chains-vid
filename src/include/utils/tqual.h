/*-------------------------------------------------------------------------
 *
 * tqual.h
 *	  POSTGRES "time qualification" definitions, ie, tuple visibility rules.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tqual.h,v 1.75 2010/01/02 16:58:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include "utils/snapshot.h"


/* Static variables representing various special snapshot semantics */
extern PGDLLIMPORT SnapshotData SnapshotNowData;
extern PGDLLIMPORT SnapshotData SnapshotSelfData;
extern PGDLLIMPORT SnapshotData SnapshotAnyData;
extern PGDLLIMPORT SnapshotData SnapshotToastData;

#define SnapshotNow			(&SnapshotNowData)
#define SnapshotSelf		(&SnapshotSelfData)
#define SnapshotAny			(&SnapshotAnyData)
#define SnapshotToast		(&SnapshotToastData)

/*
 * We don't provide a static SnapshotDirty variable because it would be
 * non-reentrant.  Instead, users of that snapshot type should declare a
 * local variable of type SnapshotData, and initialize it with this macro.
 */
#define InitDirtySnapshot(snapshotdata)  \
	((snapshotdata).satisfies = HeapTupleSatisfiesDirty)

/* This macro encodes the knowledge of which snapshots are MVCC-safe */
#define IsMVCCSnapshot(snapshot)  \
	((snapshot)->satisfies == HeapTupleSatisfiesMVCC)

/*
 * HeapTupleSatisfiesVisibility
 *		True iff heap tuple satisfies a time qual.
 *
 * Notes:
 *	Assumes heap tuple is valid.
 *	Beware of multiple evaluations of snapshot argument.
 *	Hint bits in the HeapTuple's t_infomask may be updated as a side effect;
 *	if so, the indicated buffer is marked dirty.
 */
#define HeapTupleSatisfiesVisibility(rel, tuple, snapshot, buffer, isXminValid) \
	((*(snapshot)->satisfies) (rel, tuple, snapshot, buffer, isXminValid))

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,		/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

/* These are the "satisfies" test routines for the various snapshot types */
extern bool HeapTupleSatisfiesMVCC(Relation rel, HeapTuple tuple, Snapshot snapshot,
								   Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesSiasMVCC(Relation rel, HeapTuple tuple, Snapshot snapshot,
									   Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesPostgresMVCCSystemRelation(HeapTupleHeader tuple,
														 Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesPostgresMVCCUserRelation(HeapTupleHeader tuple,
													   Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesNow(Relation rel, HeapTuple tuple, Snapshot snapshot,
								  Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesSiasNow(Relation rel, HeapTuple tuple, Snapshot snapshot,
									  Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesPostgresNow(HeapTupleHeader tuple,
										  Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesSelf(Relation rel, HeapTuple tuple, Snapshot snapshot,
								   Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesSiasSelf(Relation rel, HeapTuple tuple, Snapshot snapshot,
									   Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesPostgresSelf(HeapTupleHeader tuple,
										   Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesAny(Relation rel, HeapTuple tuple, Snapshot snapshot, Buffer buffer);

extern bool HeapTupleSatisfiesToast(Relation rel, HeapTuple tuple, Snapshot snapshot, Buffer buffer);

extern bool HeapTupleSatisfiesDirty(Relation rel, HeapTuple tuple, Snapshot snapshot,
									Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesSiasDirty(Relation rel, HeapTuple tuple, Snapshot snapshot,
										Buffer buffer, bool* isXminValid);
extern bool HeapTupleSatisfiesPostgresDirty(HeapTupleHeader tuple,
									Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesSiasActiveTombstone(Relation rel, HeapTuple tuple, Snapshot snapshot,
												  Buffer buffer, bool* isXminValid);

/* Special "satisfies" routines with different APIs */
extern HTSU_Result HeapTupleSatisfiesUpdate(Relation rel, HeapTuple tuple,
						 CommandId curcid, Buffer buffer);
extern HTSU_Result HeapTupleSatisfiesSiasUpdate(Relation rel, HeapTuple tuple,
						 CommandId curcid, Buffer buffer);
extern HTSU_Result HeapTupleSatisfiesPostgresUpdateSystemRelation(HeapTupleHeader tuple,
						 CommandId curcid, Buffer buffer);
extern HTSU_Result HeapTupleSatisfiesPostgresUpdateUserRelation(HeapTupleHeader tuple,
						 CommandId curcid, Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTupleHeader tuple,
						 TransactionId OldestXmin, Buffer buffer);

extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid);

#endif   /* TQUAL_H */
