/*-------------------------------------------------------------------------
 *
 * execBitmapTableScan.c
 *	  Support routines for nodeBitmapTableScan.c
 *
 * Portions Copyright (c) 2014-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	    src/backend/executor/execBitmapTableScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/relscan.h"
#include "access/transam.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapTableScan.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "cdb/cdbvars.h" /* gp_select_invisible */
#include "nodes/tidbitmap.h"

/*
 * Returns BitmapTableScanMethod for a given table type.
 */
static const ScanMethod *
getBitmapTableScanMethod(TableType tableType)
{
	Assert(tableType >= TableTypeHeap && tableType < TableTypeInvalid);

	/*
	 * scanMethods
	 *    Array that specifies different scan methods for various table types.
	 *
	 * The index in this array for a specific table type should match the enum value
	 * defined in TableType.
	 */
	static const ScanMethod scanMethods[] =
	{
		{
			&BitmapHeapScanNext, &BitmapHeapScanRecheck, &BitmapHeapScanBegin, &BitmapHeapScanEnd,
			&BitmapHeapScanReScan
		},
		/*
		 * AO and AOCS tables don't need a recheck-method, because they never
		 * participate in EvalPlanQual rechecks. (They don't have a ctid
		 * field, so UPDATE in REPEATABLE READ mode cannot follow the chain
		 * to the updated tuple.
		 */
		{
			&BitmapAOScanNext, NULL, &BitmapAOScanBegin, &BitmapAOScanEnd,
			&BitmapAOScanReScan
		},
		{
			/* The same set of methods serve both AO and AOCO scans */
			&BitmapAOScanNext, NULL, &BitmapAOScanBegin, &BitmapAOScanEnd,
			&BitmapAOScanReScan
		}
	};

	COMPILE_ASSERT(ARRAY_SIZE(scanMethods) == TableTypeInvalid);

	return &scanMethods[tableType];
}

/*
 * Frees the state relevant to bitmaps.
 */
static inline void
freeBitmapState(BitmapTableScanState *scanstate)
{
	/* BitmapIndexScan is the owner of the bitmap memory. Don't free it here */
	scanstate->tbm = NULL;

	/* BitmapTableScan created the iterator, so it is responsible to free its
	 * iterator. We free our GenericBMIterator here, though. It owns the tbmres
	 * memory. */
	if (scanstate->tbmiterator != NULL)
	{
		tbm_generic_end_iterate(scanstate->tbmiterator);
		scanstate->tbmiterator = NULL;
		scanstate->tbmres = NULL;
	}
}

static TupleTableSlot*
BitmapTableScanPlanQualTuple(BitmapTableScanState *node)
{
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	return ExecClearTuple(slot);
}

/*
 * Reads a bitmap (with possibly many pages) from the underlying node.
 */
static void
readBitmap(BitmapTableScanState *scanState)
{
	if (scanState->tbm != NULL)
	{
		return;
	}

	Node *tbm = (Node *) MultiExecProcNode(outerPlanState(scanState));

	if (!tbm || !(IsA(tbm, TIDBitmap) || IsA(tbm, StreamBitmap)))
		elog(ERROR, "unrecognized result from subplan");

	scanState->tbm = tbm;
	scanState->needNewBitmapPage = true;
}

/*
 * Reads the next bitmap page from the current bitmap.
 */
static bool
fetchNextBitmapPage(BitmapTableScanState *scanState)
{
	TBMIterateResult *tbmres;

	if (scanState->tbm == NULL)
	{
		return false;
	}

	Assert(scanState->needNewBitmapPage);

	if (scanState->tbmiterator == NULL)
	{
		scanState->tbmiterator = tbm_generic_begin_iterate(scanState->tbm);
	}

	do
	{
		tbmres = tbm_generic_iterate(scanState->tbmiterator);
	} while (tbmres && (tbmres->ntuples == 0));

	if (tbmres)
	{
		scanState->tbmres = tbmres;
		scanState->iterator = NULL;
		scanState->needNewBitmapPage = false;

		if (tbmres->ntuples == BITMAP_IS_LOSSY)
			scanState->isLossyBitmapPage = true;
		else
			scanState->isLossyBitmapPage = false;
		scanState->recheckTuples = tbmres->recheck;
	}

	return (tbmres != NULL);
}

/*
 * Checks eligibility of a tuple.
 *
 * Note, a tuple may fail to meet visibility requirement. Moreover,
 * for a lossy bitmap, we need to check for every tuple to make sure
 * that it satisfies the qual.
 */
bool
BitmapTableScanRecheckTuple(BitmapTableScanState *scanState, TupleTableSlot *slot)
{
	/*
	 * If we are using lossy info or we are required to recheck each tuple
	 * because of visibility or other causes, then evaluate the tuple
	 * eligibility.
	 */
	if (scanState->isLossyBitmapPage || scanState->recheckTuples)
	{
		ExprContext *econtext = scanState->ss.ps.ps_ExprContext;

		econtext->ecxt_scantuple = slot;
		ResetExprContext(econtext);

		return ExecQual(scanState->bitmapqualorig, econtext, false);
	}

	return true;
}

/*
 * Prepares for a new scan such as initializing bitmap states, preparing
 * the corresponding scan method etc.
 */
void
BitmapTableScanBegin(BitmapTableScanState *scanState, Plan *plan, EState *estate, int eflags)
{
	DynamicScan_Begin((ScanState *)scanState, plan, estate, eflags);
}

/*
 * Prepares for scanning of a new partition/relation.
 */
void
BitmapTableScanBeginPartition(ScanState *node, AttrNumber *attMap)
{
	Assert(NULL != node);
	BitmapTableScanState *scanState = (BitmapTableScanState *)node;
	BitmapTableScan *plan = (BitmapTableScan*)(node->ps.plan);

	/* Remap the bitmapqualorig as we might have dropped column problem */
	DynamicScan_RemapExpression(node, attMap, (Node *) plan->bitmapqualorig);

	if (NULL == scanState->bitmapqualorig || NULL != attMap)
	{
		/* Always initialize new expressions in the per-partition memory context to prevent leaking */
		MemoryContext partitionContext = DynamicScan_GetPartitionMemoryContext(node);
		MemoryContext oldCxt = NULL;
		if (NULL != partitionContext)
		{
			oldCxt = MemoryContextSwitchTo(partitionContext);
		}

		scanState->bitmapqualorig = (List *)
			ExecInitExpr((Expr *) plan->bitmapqualorig,
						 (PlanState *) scanState);

		if (NULL != oldCxt)
		{
			MemoryContextSwitchTo(oldCxt);
		}
	}

	scanState->needNewBitmapPage = true;
	/*
	 * In some cases, the BitmapTableScan needs to re-evaluate the
	 * bitmap qual. This is determined by the recheckTuples and
	 * isLossyBitmapPage flags, as well as the type of table.
	 * The appropriate type of BitmapIndexScan will set this flag
	 * as follows:
	 *  Table/Index Type  Lossy    Recheck
	 *	Heap                1        1
	 * 	Ao/Lossy            1        0
	 *	Ao/Non-Lossy        0        0
	 *	Aocs/Lossy          1        0
	 *	Aocs/Non-Lossy      0        0
	 */
	getBitmapTableScanMethod(node->tableType)->beginScanMethod(node);

	/*
	 * Prepare child node to produce new bitmaps for the new partition (and cleanup
	 * any leftover state from old partition).
	 */
	ExecReScan(outerPlanState(node));
}

/*
 * ReScan a partition
 */
void
BitmapTableScanReScanPartition(ScanState *node)
{
	BitmapTableScanState *scanState = (BitmapTableScanState *) node;

	freeBitmapState(scanState);
	Assert(scanState->tbm == NULL);

	scanState->needNewBitmapPage = true;

	getBitmapTableScanMethod(node->tableType)->reScanMethod(node);
}

/*
 * Cleans up once scanning of a partition/relation is done.
 */
void
BitmapTableScanEndPartition(ScanState *node)
{
	BitmapTableScanState *scanState = (BitmapTableScanState *) node;

	freeBitmapState(scanState);

	getBitmapTableScanMethod(node->tableType)->endScanMethod(node);

	Assert(scanState->tbm == NULL);
}

/*
 * Executes underlying scan method to fetch the next matching tuple.
 */
TupleTableSlot *
BitmapTableScanFetchNext(ScanState *node)
{
	BitmapTableScanState *scanState = (BitmapTableScanState *) node;
	TupleTableSlot *slot = BitmapTableScanPlanQualTuple(scanState);

	while (TupIsNull(slot))
	{
		/* If we haven't already obtained the required bitmap, do so */
		readBitmap(scanState);

		/* If we have exhausted the current bitmap page, fetch the next one */
		if (!scanState->needNewBitmapPage || fetchNextBitmapPage(scanState))
		{
			const ScanMethod *scanMethods = getBitmapTableScanMethod(scanState->ss.tableType);

			slot = ExecScan(&scanState->ss,
							scanMethods->accessMethod,
							scanMethods->recheckMethod);
		}
		else
		{
			/*
			 * Needed a new bitmap page, but couldn't fetch one. Therefore,
			 * try the next partition.
			 */
			break;
		}
	}

	return slot;
}

/*
 * Cleans up after the scanning has finished.
 */
void
BitmapTableScanEnd(BitmapTableScanState *scanState)
{
	DynamicScan_End((ScanState *)scanState, BitmapTableScanEndPartition);
}

/*
 * Prepares for a rescan.
 */
void
BitmapTableScanReScan(BitmapTableScanState *node)
{
	ScanState *scanState = &node->ss;
	DynamicScan_ReScan(scanState);

	freeBitmapState(node);

	ExecReScan(outerPlanState(node));
}