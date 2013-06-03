/*-------------------------------------------------------------------------
 *
 * funcapi.c
 *	  Utility and convenience functions for fmgr functions that return
 *	  sets and/or composite types.
 *
 * Copyright (c) 2002-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/fmgr/funcapi.c,v 1.45 2009/06/11 14:49:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"			/* work_mem */
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static void shutdown_MultiFuncCall(Datum arg);
static TypeFuncClass internal_get_result_type(Oid funcid,
						 Node *call_expr,
						 ReturnSetInfo *rsinfo,
						 Oid *resultTypeId,
						 TupleDesc *resultTupleDesc);
static bool resolve_polymorphic_tupdesc(TupleDesc tupdesc,
							oidvector *declared_args,
							Node *call_expr);
static TypeFuncClass get_type_func_class(Oid typid);


/*
 * init_MultiFuncCall
 * Create an empty FuncCallContext data structure
 * and do some other basic Multi-function call setup
 * and error checking
 */
FuncCallContext *
init_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	/*
	 * Bail if we're called in the wrong context
	 */
	if (!fcinfo->flinfo->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("Function attempts to return a set but was defined "
						"without RETURNS SETOF."),
				 errhint("CREATE FUNCTION should specify SETOF or TABLE in the "
						 "result type for this function."),
				 fmgr_call_errcontext(fcinfo, true)));

	if (rsi == NULL ||
		!IsA(rsi, ReturnSetInfo) ||
		!(rsi->allowedModes & (SFRM_Materialize | SFRM_ValuePerCall)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 fmgr_call_errcontext(fcinfo, false)));

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/*
		 * First call
		 */
		MemoryContext multi_call_ctx;

		/*
		 * Create a suitably long-lived context to hold cross-call data
		 */
		multi_call_ctx = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
											   "SRF multi-call context",
											   ALLOCSET_SMALL_MINSIZE,
											   ALLOCSET_SMALL_INITSIZE,
											   ALLOCSET_SMALL_MAXSIZE);

		/*
		 * Allocate suitably long-lived space and zero it
		 */
		retval = (FuncCallContext *)
			MemoryContextAllocZero(multi_call_ctx,
								   sizeof(FuncCallContext));

		/*
		 * initialize the elements
		 */
		retval->call_cntr = 0;
		retval->max_calls = 0;
		retval->slot = NULL;
		retval->user_fctx = NULL;
		retval->attinmeta = NULL;
		retval->tuple_desc = NULL;
		retval->multi_call_memory_ctx = multi_call_ctx;

		/*
		 * save the pointer for cross-call use
		 */
		fcinfo->flinfo->fn_extra = retval;

		/*
		 * Ensure we will get shut down cleanly if the exprcontext is not run
		 * to completion.
		 */
		RegisterExprContextCallback(rsi->econtext,
									shutdown_MultiFuncCall,
									PointerGetDatum(fcinfo->flinfo));
	}
	else
	{
		/* second and subsequent calls */
		elog(ERROR, "init_MultiFuncCall cannot be called more than once");

		/* never reached, but keep compiler happy */
		retval = NULL;
	}

	return retval;
}

/*
 * per_MultiFuncCall
 *
 * Do Multi-function per-call setup
 */
FuncCallContext *
per_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval = (FuncCallContext *) fcinfo->flinfo->fn_extra;

	/*
	 * Clear the TupleTableSlot, if present.  This is for safety's sake: the
	 * Slot will be in a long-lived context (it better be, if the
	 * FuncCallContext is pointing to it), but in most usage patterns the
	 * tuples stored in it will be in the function's per-tuple context. So at
	 * the beginning of each call, the Slot will hold a dangling pointer to an
	 * already-recycled tuple.	We clear it out here.
	 *
	 * Note: use of retval->slot is obsolete as of 8.0, and we expect that it
	 * will always be NULL.  This is just here for backwards compatibility in
	 * case someone creates a slot anyway.
	 */
	if (retval->slot != NULL)
		ExecClearTuple(retval->slot);

	return retval;
}

/*
 * end_MultiFuncCall
 * Clean up after init_MultiFuncCall
 */
void
end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Deregister the shutdown callback */
	UnregisterExprContextCallback(rsi->econtext,
								  shutdown_MultiFuncCall,
								  PointerGetDatum(fcinfo->flinfo));

	/* But use it to do the real work */
	shutdown_MultiFuncCall(PointerGetDatum(fcinfo->flinfo));
}

/*
 * shutdown_MultiFuncCall
 * Shutdown function to clean up after init_MultiFuncCall
 */
static void
shutdown_MultiFuncCall(Datum arg)
{
	FmgrInfo   *flinfo = (FmgrInfo *) DatumGetPointer(arg);
	FuncCallContext *funcctx = (FuncCallContext *) flinfo->fn_extra;

	/* unbind from flinfo */
	flinfo->fn_extra = NULL;

	/*
	 * Delete context that holds all multi-call data, including the
	 * FuncCallContext itself
	 */
	MemoryContextDelete(funcctx->multi_call_memory_ctx);
}


/*
 * srf_check_context
 *
 * A set-returning function can call this convenience routine to verify that
 * the function has been properly registered as a set-returning function in
 * the system catalog; and that its invocation comes from a calling context in
 * which the set-returning protocol is supported.
 *
 * We recommend that set-returning functions call this routine when called for
 * the first time.  (Calling more than once is ok but redundant.)  However,
 * it's not necessary when using srf_init_materialize_mode, SRF_FIRSTCALL_INIT
 * or init_MultiFuncCall because those have the same checking built in.
 *
 * On return, the caller can assume that fcinfo->resultinfo points to a
 * ReturnSetInfo structure.
 */
void
srf_check_context(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Fail if single-valued result type in function's catalog definition. */
	if (!fcinfo->flinfo->fn_retset)
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("Function attempts to return a set but was defined "
						"without RETURNS SETOF."),
				 errhint("CREATE FUNCTION should specify SETOF or TABLE in the "
						 "result type for this function."),
				 fmgr_call_errcontext(fcinfo, true)));
	}

	/*
	 * Fail if function's caller doesn't support the set-returning protocol.
	 * When invoked via the expression evaluator, this test is redundant
	 * because this has already been checked by ExecEvalSRF, and ReturnSetInfo
	 * is always supplied.  We keep the test here for the benefit of direct
	 * call sites that cut corners and bypass the expression evaluator.
	 */
 	if (rsinfo == NULL ||
		!IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & (SFRM_ValuePerCall | SFRM_Materialize)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 fmgr_call_errcontext(fcinfo, false)));
	}
}


/*
 * srf_init_materialize_mode
 *
 * A set-returning function can call this convenience routine to declare that
 * it will return a set of result tuples via the "materialize mode" protocol,
 * and to obtain a tuplestore into which it should insert the result tuples.
 *
 * After srf_init_materialize_mode has been called, the set-returning function
 * should append all of its result tuples (zero or more) to the tuplestore by
 * calling a suitable tuplestore_put* routine, and return (Datum) 0 to exit.
 *
 * Optionally, instead of returning all of the results at once, the
 * set-returning function can return a series of nonempty partial results
 * having any desired number of tuples.  Thus the function can balance latency
 * with efficiency.  This is done by appending a partial result of one or more
 * tuples to the tuplestore and setting rsinfo->isDone = ExprMultipleResult
 * before returning.  Then after the function's caller has processed the
 * partial result, it will call the function again with unchanged arguments,
 * still with isDone == ExprMultipleResult, to produce the next portion of the
 * result.  When there are no more results, the set-returning function should
 * end the iteration by returning with nothing added to the tuplestore; or,
 * alternatively, by setting rsinfo->isDone = ExprSingleResult when returning
 * the final installment of zero or more tuples.
 *
 * After the function has finished returning the results for the given
 * argument values, the caller can call again with new argument values.
 * Each time the set-returning function is called, it may optionally call
 * srf_init_materialize_mode again to get the tuplestore pointer; or it may
 * simply get the pointer from rsinfo->setResult itself.
 *
 * The set-returning function's caller takes ownership of the tuplestore and
 * will be responsible for freeing it when it is no longer needed.  The
 * function should not change the tuplestore's read pointer position.  A
 * set-returning function must not call tuplestore_end(), tuplestore_clear(),
 * or tuplestore_trim() on a pointer which it has received or returned in the
 * rsinfo->setResult field.
 *
 * It's ok for a set-returning function to call this routine more than once.
 *
 * On return, the caller can assume that fcinfo->resultinfo points to a
 * ReturnSetInfo structure.
 */
Tuplestorestate *
srf_init_materialize_mode(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Fail if function improperly cataloged or caller requires single value. */
	srf_check_context(fcinfo);

	/*
	 * Create tuplestore, unless the function's caller has provided one.
	 *
	 * When a set-returning function has a set-valued argument, a tuplestore
	 * created by the function on the first iteration could be passed in again
	 * to gather additional results on subsequent iterations.  The function is
	 * expected to append its new results (if any) to the existing contents.
	 */
	if (!rsinfo->setResult)
	{
		MemoryContext oldcontext;
		bool randomAccess = false;

		/* Switch to per-query memory context. */
		oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		/* Does function's caller want to read tuplestore bidirectionally? */
		if (rsinfo->allowedModes & SFRM_Materialize_Random)
			randomAccess = true;

		/* Create the tuplestore. */
		rsinfo->setResult = tuplestore_begin_heap(randomAccess, false, work_mem);

		/* Switch back to caller's memory context. */
		MemoryContextSwitchTo(oldcontext);
	}

	/* Tell function's caller to obtain the result from the tuplestore. */
	rsinfo->returnMode = SFRM_Materialize;

	/* Return the tuplestore. */
	return rsinfo->setResult;
}


/*
 * srf_set_tupdesc
 *
 * A set-returning function can call this convenience routine to specify a
 * tuple descriptor defining the format of the function's result tuples.  This
 * routine stores the 'actualtupdesc' argument into the rsinfo->setDesc field.
 *
 * In most cases the result tuples must be binary compatible with an expected
 * format (rsinfo->expectedDesc) which has been predetermined from the SQL
 * procedure definition (with ANY* types resolved from context) or specified
 * explicitly in the query.
 *
 * However, for some functions the caller might not always provide an expected
 * tuple descriptor: rsinfo->expectedDesc can be NULL if the function returns
 * SETOF RECORD and the result types were not specified explicitly in the
 * query.  When materialize mode is used and there is no expected descriptor,
 * the set-returning function is required to specify its result tuple
 * descriptor; otherwise the query fails.
 *
 * Tuples in memory generally carry header fields identifying the tuple type,
 * enabling callers to access the columns; this is enough when value-per-call
 * mode is used.  But tuples in a tuplestore may get squashed to MinimalTuple
 * format, eliminating that part of the header.  Columns of a MinimalTuple can
 * be picked out only with the help of a tuple descriptor that is passed along
 * separately.  To use materialize mode, there has to be a descriptor for the
 * result tuples: the expected descriptor is used if available; otherwise the
 * descriptor specified by the function (rsinfo->setDesc) is used, and must be
 * non-NULL or the query fails.
 *
 * As an optional (but recommended) sanity check, a set-returning function
 * can specify a tuple descriptor which the function's caller will check for
 * compatibility with the expected tuple descriptor if any.  This applies
 * whether the function chooses materialize mode or value-per-call mode.
 *
 * Upon return from the set-returning function, its caller takes ownership of
 * 'actualtupdesc' and will free it promptly; there is no need for it to be
 * allocated in a special long-lived memory context.  When a set-returning
 * function is called, rsinfo->setDesc is  always NULL; the descriptor is not
 * retained from call to call (although it is copied to fill in a missing
 * rsinfo->expectedDesc if there was no expected descriptor).
 *
 * Consider using srf_verify_expected_tupdesc() instead of this routine in the
 * following cases: (a) if you want early notification of discrepancies between
 * the actual and expected descriptors; (b) if you want to keep ownership of
 * actualtupdesc; (c) if actualtupdesc may have been created in a short-lived
 * memory context that will be reset before your function returns; (d) if your
 * function is not a set-returning function but returns a RECORD type that
 * can't be checked until runtime.
 *
 * On return, the caller can assume that fcinfo->resultinfo points to a
 * ReturnSetInfo structure.
 */
void
srf_set_tupdesc(FunctionCallInfo fcinfo, TupleDesc actualtupdesc)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function called in an unsupported context"),
				 fmgr_call_errcontext(fcinfo, false)));

	rsinfo->setDesc = actualtupdesc;
}

/*
 * srf_get_expected_tupdesc
 *
 * A set-returning function, or a function returning RECORD, can call this
 * convenience routine to obtain rsinfo->expectedDesc, a descriptor for the
 * function's expected result tuple format.
 *
 * If 'required' is true, and rsinfo->expectedDesc == NULL, then this routine
 * signals an error.  The rsinfo->expectedDesc field could be NULL if the
 * function returns RECORD or SETOF RECORD and the result types were not
 * specified explicitly in the query.
 *
 * On return, the caller can assume that fcinfo->resultinfo points to a
 * ReturnSetInfo structure.  If 'required' is true, the caller can assume that
 * the returned tuple descriptor pointer is non-NULL.
 */
TupleDesc
srf_get_expected_tupdesc(FunctionCallInfo fcinfo, bool required)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function called in an unsupported context"),
				 fmgr_call_errcontext(fcinfo, false)));

	if (required &&
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record"),
				 fmgr_call_errcontext(fcinfo, false)));

	return rsinfo->expectedDesc;
}

/*
 * srf_verify_expected_tupdesc
 *
 * This routine raises an error if the tuple descriptor 'actualtupdesc'
 * specifies a format that is not binary compatible with the calling
 * function's expected result tuple descriptor (rsinfo->expectedDesc).  But
 * when no expected tuple descriptor is available, this routine stores a copy
 * of the actualtupdesc into the expected descriptor field; in which case, for
 * a set-returning function using materialize mode, the copy will eventually
 * be used to read the results from the tuplestore.  If both actualtupdesc and
 * rsinfo->expectedDesc are null, an error is raised.
 *
 * If 'free' is true, then this routine frees 'actualtupdesc' unless it
 * resides in the type cache.  If 'free' is false, the caller remains
 * responsible for the disposition of actualtupdesc.
 *
 * This duplicates the handling of rsinfo->setDesc upon return from
 * set-returning functions (in ExecEvalSRF).  Thus, set-returning functions
 * that use this srf_verify_expected_tupdesc routine need not fill in the
 * rsinfo->setDesc field.
 *
 * When the result format is dynamic and not known in advance, this routine
 * allows early detection of descriptor errors, before embarking on a possibly
 * expensive but doomed computation.  Also, in case the actualtupdesc has been
 * created in a very short-lived memory context, this routine can save the
 * caller the trouble of copying it to a longer-lived context merely to be
 * checked and deleted immediately afterward.
 *
 * On return, the caller can assume that fcinfo->resultinfo points to a
 * ReturnSetInfo structure.
 */
void
srf_verify_expected_tupdesc(FunctionCallInfo fcinfo, TupleDesc actualtupdesc, bool free)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function called in an unsupported context"),
				 fmgr_call_errcontext(fcinfo, false)));

	/* Verify the actual result tuple format matches the expected layout. */
	if (actualtupdesc)
		ExecVerifyExpectedTupleDesc(fcinfo, actualtupdesc, free);

	/*
	 * Fail if function's caller was not able to determine the expected result
	 * rowtype from context, and the function itself hasn't created a tupdesc,
	 * and the function has indicated that it wants to use materialize mode to
	 * return a set.  The function's caller won't be able to read results from
	 * the tuplestore in MinimalTuple format without a descriptor.  Again,
	 * we're providing early notification of an error that will also be
	 * checked on return from the set-returning function.
	 */
	else if (rsinfo->expectedDesc == NULL &&
		     rsinfo->returnMode == SFRM_Materialize)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in "
							"context that cannot accept type record"),
					fmgr_call_errcontext(fcinfo, false)));
}


/*
 * get_call_result_type
 *		Given a function's call info record, determine the kind of datatype
 *		it is supposed to return.  If resultTypeId isn't NULL, *resultTypeId
 *		receives the actual datatype OID (this is mainly useful for scalar
 *		result types).	If resultTupleDesc isn't NULL, *resultTupleDesc
 *		receives a pointer to a TupleDesc when the result is of a composite
 *		type, or NULL when it's a scalar result.  The TupleDesc is newly
 *		palloc'ed in the current memory context; the caller may pfree it
 *		when no longer needed.
 *
 * One hard case that this handles is resolution of actual rowtypes for
 * functions returning RECORD (from either the function's OUT parameter
 * list, or a ReturnSetInfo context node).	TYPEFUNC_RECORD is returned
 * only when we couldn't resolve the actual rowtype for lack of information.
 *
 * The other hard case that this handles is resolution of polymorphism.
 * We will never return polymorphic pseudotypes (ANYELEMENT etc), either
 * as a scalar result type or as a component of a rowtype.
 *
 * This function is relatively expensive --- in a function returning set,
 * try to call it only the first time through.
 */
TypeFuncClass
get_call_result_type(FunctionCallInfo fcinfo,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(fcinfo->flinfo->fn_oid,
									fcinfo->flinfo->fn_expr,
									(ReturnSetInfo *) fcinfo->resultinfo,
									resultTypeId,
									resultTupleDesc);
}

/*
 * get_expr_result_type
 *		As above, but work from a calling expression node tree
 */
TypeFuncClass
get_expr_result_type(Node *expr,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;

	if (expr && IsA(expr, FuncExpr))
		result = internal_get_result_type(((FuncExpr *) expr)->funcid,
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else if (expr && IsA(expr, OpExpr))
		result = internal_get_result_type(get_opcode(((OpExpr *) expr)->opno),
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else
	{
		/* handle as a generic expression; no chance to resolve RECORD */
		Oid			typid = exprType(expr);

		if (resultTypeId)
			*resultTypeId = typid;
		if (resultTupleDesc)
			*resultTupleDesc = NULL;
		result = get_type_func_class(typid);
		if (result == TYPEFUNC_COMPOSITE && resultTupleDesc)
			*resultTupleDesc = lookup_rowtype_tupdesc_copy(typid, -1);
	}

	return result;
}

/*
 * get_func_result_type
 *		As above, but work from a function's OID only
 *
 * This will not be able to resolve pure-RECORD results nor polymorphism.
 */
TypeFuncClass
get_func_result_type(Oid functionId,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(functionId,
									NULL,
									NULL,
									resultTypeId,
									resultTupleDesc);
}

/*
 * internal_get_result_type -- workhorse code implementing all the above
 *
 * funcid must always be supplied.	call_expr and rsinfo can be NULL if not
 * available.  We will return TYPEFUNC_RECORD, and store NULL into
 * *resultTupleDesc, if we cannot deduce the complete result rowtype from
 * the available information.
 */
static TypeFuncClass
internal_get_result_type(Oid funcid,
						 Node *call_expr,
						 ReturnSetInfo *rsinfo,
						 Oid *resultTypeId,
						 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;
	HeapTuple	tp;
	Form_pg_proc procform;
	Oid			rettype;
	TupleDesc	tupdesc;

	/* First fetch the function's pg_proc row to inspect its rettype */
	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	rettype = procform->prorettype;

	/* Check for OUT parameters defining a RECORD result */
	tupdesc = build_function_result_tupdesc_t(tp);
	if (tupdesc)
	{
		/*
		 * It has OUT parameters, so it's basically like a regular composite
		 * type, except we have to be able to resolve any polymorphic OUT
		 * parameters.
		 */
		if (resultTypeId)
			*resultTypeId = rettype;

		if (resolve_polymorphic_tupdesc(tupdesc,
										&procform->proargtypes,
										call_expr))
		{
			if (tupdesc->tdtypeid == RECORDOID &&
				tupdesc->tdtypmod < 0)
				assign_record_type_typmod(tupdesc);
			if (resultTupleDesc)
				*resultTupleDesc = tupdesc;
			result = TYPEFUNC_COMPOSITE;
		}
		else
		{
			if (resultTupleDesc)
				*resultTupleDesc = NULL;
			result = TYPEFUNC_RECORD;
		}

		ReleaseSysCache(tp);

		/* tupdesc is unshared and newly palloc'ed in caller's context */
		Assert(resultTupleDesc == NULL ||
			   *resultTupleDesc == NULL ||
			   ((*resultTupleDesc)->tdrefcount == -1 &&
		   GetMemoryChunkContext(*resultTupleDesc) == CurrentMemoryContext));

		return result;
	}

	/*
	 * If scalar polymorphic result, try to resolve it.
	 */
	if (IsPolymorphicType(rettype))
	{
		Oid			newrettype = exprType(call_expr);

		if (newrettype == InvalidOid)	/* this probably should not happen */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not determine actual result type for function \"%s\" declared to return type %s",
							NameStr(procform->proname),
							format_type_be(rettype))));
		rettype = newrettype;
	}

	if (resultTypeId)
		*resultTypeId = rettype;
	if (resultTupleDesc)
		*resultTupleDesc = NULL;	/* default result */

	/* Classify the result type */
	result = get_type_func_class(rettype);
	switch (result)
	{
		case TYPEFUNC_COMPOSITE:
			if (resultTupleDesc)
				*resultTupleDesc = lookup_rowtype_tupdesc_copy(rettype, -1);
			/* Named composite types can't have any polymorphic columns */
			break;
		case TYPEFUNC_SCALAR:
			break;
		case TYPEFUNC_RECORD:
			/* We must get the tupledesc from call context */
			if (rsinfo && IsA(rsinfo, ReturnSetInfo) &&
				rsinfo->expectedDesc != NULL)
			{
				result = TYPEFUNC_COMPOSITE;
				if (resultTupleDesc)
					*resultTupleDesc = CreateTupleDescCopyConstr(rsinfo->expectedDesc);
				/* Assume no polymorphic columns here, either */
			}
			break;
		default:
			break;
	}

	ReleaseSysCache(tp);

	/* tupdesc is unshared and newly palloc'ed in caller's context */
	Assert(!resultTupleDesc ||
		   !*resultTupleDesc ||
		   ((*resultTupleDesc)->tdrefcount == -1 &&
			GetMemoryChunkContext(*resultTupleDesc) == CurrentMemoryContext));

	return result;
}

/*
 * Given the result tuple descriptor for a function with OUT parameters,
 * replace any polymorphic columns (ANYELEMENT etc) with correct data types
 * deduced from the input arguments. Returns TRUE if able to deduce all types,
 * FALSE if not.
 */
static bool
resolve_polymorphic_tupdesc(TupleDesc tupdesc, oidvector *declared_args,
							Node *call_expr)
{
	int			natts = tupdesc->natts;
	int			nargs = declared_args->dim1;
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	bool		have_anynonarray = false;
	bool		have_anyenum = false;
	Oid			anyelement_type = InvalidOid;
	Oid			anyarray_type = InvalidOid;
	int			i;

	/* See if there are any polymorphic outputs; quick out if not */
	for (i = 0; i < natts; i++)
	{
		switch (tupdesc->attrs[i]->atttypid)
		{
			case ANYELEMENTOID:
				have_anyelement_result = true;
				break;
			case ANYARRAYOID:
				have_anyarray_result = true;
				break;
			case ANYNONARRAYOID:
				have_anyelement_result = true;
				have_anynonarray = true;
				break;
			case ANYENUMOID:
				have_anyelement_result = true;
				have_anyenum = true;
				break;
			default:
				break;
		}
	}
	if (!have_anyelement_result && !have_anyarray_result)
		return true;

	/*
	 * Otherwise, extract actual datatype(s) from input arguments.	(We assume
	 * the parser already validated consistency of the arguments.)
	 */
	if (!call_expr)
		return false;			/* no hope */

	for (i = 0; i < nargs; i++)
	{
		switch (declared_args->values[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (!OidIsValid(anyelement_type))
					anyelement_type = get_call_expr_argtype(call_expr, i);
				break;
			case ANYARRAYOID:
				if (!OidIsValid(anyarray_type))
					anyarray_type = get_call_expr_argtype(call_expr, i);
				break;
			default:
				break;
		}
	}

	/* If nothing found, parser messed up */
	if (!OidIsValid(anyelement_type) && !OidIsValid(anyarray_type))
		return false;

	/* If needed, deduce one polymorphic type from the other */
	if (have_anyelement_result && !OidIsValid(anyelement_type))
		anyelement_type = resolve_generic_type(ANYELEMENTOID,
											   anyarray_type,
											   ANYARRAYOID);
	if (have_anyarray_result && !OidIsValid(anyarray_type))
		anyarray_type = resolve_generic_type(ANYARRAYOID,
											 anyelement_type,
											 ANYELEMENTOID);

	/* Enforce ANYNONARRAY if needed */
	if (have_anynonarray && type_is_array(anyelement_type))
		return false;

	/* Enforce ANYENUM if needed */
	if (have_anyenum && !type_is_enum(anyelement_type))
		return false;

	/* And finally replace the tuple column types as needed */
	for (i = 0; i < natts; i++)
	{
		switch (tupdesc->attrs[i]->atttypid)
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(tupdesc->attrs[i]->attname),
								   anyelement_type,
								   -1,
								   0);
				break;
			case ANYARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(tupdesc->attrs[i]->attname),
								   anyarray_type,
								   -1,
								   0);
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * Given the declared argument types and modes for a function, replace any
 * polymorphic types (ANYELEMENT etc) with correct data types deduced from the
 * input arguments.  Returns TRUE if able to deduce all types, FALSE if not.
 * This is the same logic as resolve_polymorphic_tupdesc, but with a different
 * argument representation.
 *
 * argmodes may be NULL, in which case all arguments are assumed to be IN mode.
 */
bool
resolve_polymorphic_argtypes(int numargs, Oid *argtypes, char *argmodes,
							 Node *call_expr)
{
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	Oid			anyelement_type = InvalidOid;
	Oid			anyarray_type = InvalidOid;
	int			inargno;
	int			i;

	/* First pass: resolve polymorphic inputs, check for outputs */
	inargno = 0;
	for (i = 0; i < numargs; i++)
	{
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
					have_anyelement_result = true;
				else
				{
					if (!OidIsValid(anyelement_type))
					{
						anyelement_type = get_call_expr_argtype(call_expr,
																inargno);
						if (!OidIsValid(anyelement_type))
							return false;
					}
					argtypes[i] = anyelement_type;
				}
				break;
			case ANYARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
					have_anyarray_result = true;
				else
				{
					if (!OidIsValid(anyarray_type))
					{
						anyarray_type = get_call_expr_argtype(call_expr,
															  inargno);
						if (!OidIsValid(anyarray_type))
							return false;
					}
					argtypes[i] = anyarray_type;
				}
				break;
			default:
				break;
		}
		if (argmode != PROARGMODE_OUT && argmode != PROARGMODE_TABLE)
			inargno++;
	}

	/* Done? */
	if (!have_anyelement_result && !have_anyarray_result)
		return true;

	/* If no input polymorphics, parser messed up */
	if (!OidIsValid(anyelement_type) && !OidIsValid(anyarray_type))
		return false;

	/* If needed, deduce one polymorphic type from the other */
	if (have_anyelement_result && !OidIsValid(anyelement_type))
		anyelement_type = resolve_generic_type(ANYELEMENTOID,
											   anyarray_type,
											   ANYARRAYOID);
	if (have_anyarray_result && !OidIsValid(anyarray_type))
		anyarray_type = resolve_generic_type(ANYARRAYOID,
											 anyelement_type,
											 ANYELEMENTOID);

	/* XXX do we need to enforce ANYNONARRAY or ANYENUM here?  I think not */

	/* And finally replace the output column types as needed */
	for (i = 0; i < numargs; i++)
	{
		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				argtypes[i] = anyelement_type;
				break;
			case ANYARRAYOID:
				argtypes[i] = anyarray_type;
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * get_type_func_class
 *		Given the type OID, obtain its TYPEFUNC classification.
 *
 * This is intended to centralize a bunch of formerly ad-hoc code for
 * classifying types.  The categories used here are useful for deciding
 * how to handle functions returning the datatype.
 */
static TypeFuncClass
get_type_func_class(Oid typid)
{
	switch (get_typtype(typid))
	{
		case TYPTYPE_COMPOSITE:
			return TYPEFUNC_COMPOSITE;
		case TYPTYPE_BASE:
		case TYPTYPE_DOMAIN:
		case TYPTYPE_ENUM:
			return TYPEFUNC_SCALAR;
		case TYPTYPE_PSEUDO:
			if (typid == RECORDOID)
				return TYPEFUNC_RECORD;

			/*
			 * We treat VOID and CSTRING as legitimate scalar datatypes,
			 * mostly for the convenience of the JDBC driver (which wants to
			 * be able to do "SELECT * FROM foo()" for all legitimately
			 * user-callable functions).
			 */
			if (typid == VOIDOID || typid == CSTRINGOID)
				return TYPEFUNC_SCALAR;
			return TYPEFUNC_OTHER;
	}
	/* shouldn't get here, probably */
	return TYPEFUNC_OTHER;
}


/*
 * get_func_arg_info
 *
 * Fetch info about the argument types, names, and IN/OUT modes from the
 * pg_proc tuple.  Return value is the total number of arguments.
 * Other results are palloc'd.  *p_argtypes is always filled in, but
 * *p_argnames and *p_argmodes will be set NULL in the default cases
 * (no names, and all IN arguments, respectively).
 *
 * Note that this function simply fetches what is in the pg_proc tuple;
 * it doesn't do any interpretation of polymorphic types.
 */
int
get_func_arg_info(HeapTuple procTup,
				  Oid **p_argtypes, char ***p_argnames, char **p_argmodes)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isNull;
	ArrayType  *arr;
	int			numargs;
	Datum	   *elems;
	int			nelems;
	int			i;

	/* First discover the total number of parameters and get their types */
	proallargtypes = SysCacheGetAttr(PROCOID, procTup,
									 Anum_pg_proc_proallargtypes,
									 &isNull);
	if (!isNull)
	{
		/*
		 * We expect the arrays to be 1-D arrays of the right types; verify
		 * that.  For the OID and char arrays, we don't need to use
		 * deconstruct_array() since the array data is just going to look like
		 * a C array of values.
		 */
		arr = DatumGetArrayTypeP(proallargtypes);		/* ensure not toasted */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "proallargtypes is not a 1-D Oid array");
		Assert(numargs >= procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, ARR_DATA_PTR(arr),
			   numargs * sizeof(Oid));
	}
	else
	{
		/* If no proallargtypes, use proargtypes */
		numargs = procStruct->proargtypes.dim1;
		Assert(numargs == procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, procStruct->proargtypes.values,
			   numargs * sizeof(Oid));
	}

	/* Get argument names, if available */
	proargnames = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargnames,
								  &isNull);
	if (isNull)
		*p_argnames = NULL;
	else
	{
		deconstruct_array(DatumGetArrayTypeP(proargnames),
						  TEXTOID, -1, false, 'i',
						  &elems, NULL, &nelems);
		if (nelems != numargs)	/* should not happen */
			elog(ERROR, "proargnames must have the same number of elements as the function has arguments");
		*p_argnames = (char **) palloc(sizeof(char *) * numargs);
		for (i = 0; i < numargs; i++)
			(*p_argnames)[i] = TextDatumGetCString(elems[i]);
	}

	/* Get argument modes, if available */
	proargmodes = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (isNull)
		*p_argmodes = NULL;
	else
	{
		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array");
		*p_argmodes = (char *) palloc(numargs * sizeof(char));
		memcpy(*p_argmodes, ARR_DATA_PTR(arr),
			   numargs * sizeof(char));
	}

	return numargs;
}


/*
 * get_func_result_name
 *
 * If the function has exactly one output parameter, and that parameter
 * is named, return the name (as a palloc'd string).  Else return NULL.
 *
 * This is used to determine the default output column name for functions
 * returning scalar types.
 */
char *
get_func_result_name(Oid functionId)
{
	char	   *result;
	HeapTuple	procTuple;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isnull;
	ArrayType  *arr;
	int			numargs;
	char	   *argmodes;
	Datum	   *argnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* First fetch the function's pg_proc row */
	procTuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(functionId),
							   0, 0, 0);
	if (!HeapTupleIsValid(procTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);

	/* If there are no named OUT parameters, return NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proargmodes) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargnames))
		result = NULL;
	else
	{
		/* Get the data out of the tuple */
		proargmodes = SysCacheGetAttr(PROCOID, procTuple,
									  Anum_pg_proc_proargmodes,
									  &isnull);
		Assert(!isnull);
		proargnames = SysCacheGetAttr(PROCOID, procTuple,
									  Anum_pg_proc_proargnames,
									  &isnull);
		Assert(!isnull);

		/*
		 * We expect the arrays to be 1-D arrays of the right types; verify
		 * that.  For the char array, we don't need to use deconstruct_array()
		 * since the array data is just going to look like a C array of
		 * values.
		 */
		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array");
		argmodes = (char *) ARR_DATA_PTR(arr);
		arr = DatumGetArrayTypeP(proargnames);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array");
		deconstruct_array(arr, TEXTOID, -1, false, 'i',
						  &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);

		/* scan for output argument(s) */
		result = NULL;
		numoutargs = 0;
		for (i = 0; i < numargs; i++)
		{
			if (argmodes[i] == PROARGMODE_IN ||
				argmodes[i] == PROARGMODE_VARIADIC)
				continue;
			Assert(argmodes[i] == PROARGMODE_OUT ||
				   argmodes[i] == PROARGMODE_INOUT ||
				   argmodes[i] == PROARGMODE_TABLE);
			if (++numoutargs > 1)
			{
				/* multiple out args, so forget it */
				result = NULL;
				break;
			}
			result = TextDatumGetCString(argnames[i]);
			if (result == NULL || result[0] == '\0')
			{
				/* Parameter is not named, so forget it */
				result = NULL;
				break;
			}
		}
	}

	ReleaseSysCache(procTuple);

	return result;
}


/*
 * build_function_result_tupdesc_t
 *
 * Given a pg_proc row for a function, return a tuple descriptor for the
 * result rowtype, or NULL if the function does not have OUT parameters.
 *
 * Note that this does not handle resolution of polymorphic types;
 * that is deliberate.
 */
TupleDesc
build_function_result_tupdesc_t(HeapTuple procTuple)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(procTuple);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isnull;

	/* Return NULL if the function isn't declared to return RECORD */
	if (procform->prorettype != RECORDOID)
		return NULL;

	/* If there are no OUT parameters, return NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proallargtypes) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargmodes))
		return NULL;

	/* Get the data out of the tuple */
	proallargtypes = SysCacheGetAttr(PROCOID, procTuple,
									 Anum_pg_proc_proallargtypes,
									 &isnull);
	Assert(!isnull);
	proargmodes = SysCacheGetAttr(PROCOID, procTuple,
								  Anum_pg_proc_proargmodes,
								  &isnull);
	Assert(!isnull);
	proargnames = SysCacheGetAttr(PROCOID, procTuple,
								  Anum_pg_proc_proargnames,
								  &isnull);
	if (isnull)
		proargnames = PointerGetDatum(NULL);	/* just to be sure */

	return build_function_result_tupdesc_d(proallargtypes,
										   proargmodes,
										   proargnames);
}

/*
 * build_function_result_tupdesc_d
 *
 * Build a RECORD function's tupledesc from the pg_proc proallargtypes,
 * proargmodes, and proargnames arrays.  This is split out for the
 * convenience of ProcedureCreate, which needs to be able to compute the
 * tupledesc before actually creating the function.
 *
 * Returns NULL if there are not at least two OUT or INOUT arguments.
 */
TupleDesc
build_function_result_tupdesc_d(Datum proallargtypes,
								Datum proargmodes,
								Datum proargnames)
{
	TupleDesc	desc;
	ArrayType  *arr;
	int			numargs;
	Oid		   *argtypes;
	char	   *argmodes;
	Datum	   *argnames = NULL;
	Oid		   *outargtypes;
	char	  **outargnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* Can't have output args if columns are null */
	if (proallargtypes == PointerGetDatum(NULL) ||
		proargmodes == PointerGetDatum(NULL))
		return NULL;

	/*
	 * We expect the arrays to be 1-D arrays of the right types; verify that.
	 * For the OID and char arrays, we don't need to use deconstruct_array()
	 * since the array data is just going to look like a C array of values.
	 */
	arr = DatumGetArrayTypeP(proallargtypes);	/* ensure not toasted */
	numargs = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numargs < 0 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "proallargtypes is not a 1-D Oid array");
	argtypes = (Oid *) ARR_DATA_PTR(arr);
	arr = DatumGetArrayTypeP(proargmodes);		/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_DIMS(arr)[0] != numargs ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "proargmodes is not a 1-D char array");
	argmodes = (char *) ARR_DATA_PTR(arr);
	if (proargnames != PointerGetDatum(NULL))
	{
		arr = DatumGetArrayTypeP(proargnames);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array");
		deconstruct_array(arr, TEXTOID, -1, false, 'i',
						  &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);
	}

	/* zero elements probably shouldn't happen, but handle it gracefully */
	if (numargs <= 0)
		return NULL;

	/* extract output-argument types and names */
	outargtypes = (Oid *) palloc(numargs * sizeof(Oid));
	outargnames = (char **) palloc(numargs * sizeof(char *));
	numoutargs = 0;
	for (i = 0; i < numargs; i++)
	{
		char	   *pname;

		if (argmodes[i] == PROARGMODE_IN ||
			argmodes[i] == PROARGMODE_VARIADIC)
			continue;
		Assert(argmodes[i] == PROARGMODE_OUT ||
			   argmodes[i] == PROARGMODE_INOUT ||
			   argmodes[i] == PROARGMODE_TABLE);
		outargtypes[numoutargs] = argtypes[i];
		if (argnames)
			pname = TextDatumGetCString(argnames[i]);
		else
			pname = NULL;
		if (pname == NULL || pname[0] == '\0')
		{
			/* Parameter is not named, so gin up a column name */
			pname = (char *) palloc(32);
			snprintf(pname, 32, "column%d", numoutargs + 1);
		}
		outargnames[numoutargs] = pname;
		numoutargs++;
	}

	/*
	 * If there is no output argument, or only one, the function does not
	 * return tuples.
	 */
	if (numoutargs < 2)
		return NULL;

	desc = CreateTemplateTupleDesc(numoutargs, false);
	for (i = 0; i < numoutargs; i++)
	{
		TupleDescInitEntry(desc, i + 1,
						   outargnames[i],
						   outargtypes[i],
						   -1,
						   0);
	}

	return desc;
}


/*
 * RelationNameGetTupleDesc
 *
 * Given a (possibly qualified) relation name, build a TupleDesc.
 *
 * Note: while this works as advertised, it's seldom the best way to
 * build a tupdesc for a function's result type.  It's kept around
 * only for backwards compatibility with existing user-written code.
 */
TupleDesc
RelationNameGetTupleDesc(const char *relname)
{
	RangeVar   *relvar;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *relname_list;

	/* Open relation and copy the tuple description */
	relname_list = stringToQualifiedNameList(relname);
	relvar = makeRangeVarFromNameList(relname_list);
	rel = relation_openrv(relvar, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	relation_close(rel, AccessShareLock);

	return tupdesc;
}

/*
 * TypeGetTupleDesc
 *
 * Given a type Oid, build a TupleDesc.  (In most cases you should be
 * using get_call_result_type or one of its siblings instead of this
 * routine, so that you can handle OUT parameters, RECORD result type,
 * and polymorphic results.)
 *
 * If the type is composite, *and* a colaliases List is provided, *and*
 * the List is of natts length, use the aliases instead of the relation
 * attnames.  (NB: this usage is deprecated since it may result in
 * creation of unnecessary transient record types.)
 *
 * If the type is a base type, a single item alias List is required.
 */
TupleDesc
TypeGetTupleDesc(Oid typeoid, List *colaliases)
{
	TypeFuncClass functypclass = get_type_func_class(typeoid);
	TupleDesc	tupdesc = NULL;

	/*
	 * Build a suitable tupledesc representing the output rows
	 */
	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		tupdesc = lookup_rowtype_tupdesc_copy(typeoid, -1);

		if (colaliases != NIL)
		{
			int			natts = tupdesc->natts;
			int			varattno;

			/* does the list length match the number of attributes? */
			if (list_length(colaliases) != natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("number of aliases does not match number of columns")));

			/* OK, use the aliases instead */
			for (varattno = 0; varattno < natts; varattno++)
			{
				char	   *label = strVal(list_nth(colaliases, varattno));

				if (label != NULL)
					namestrcpy(&(tupdesc->attrs[varattno]->attname), label);
			}

			/* The tuple type is now an anonymous record type */
			tupdesc->tdtypeid = RECORDOID;
			tupdesc->tdtypmod = -1;
		}
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		char	   *attname;

		/* the alias list is required for base types */
		if (colaliases == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("no column alias was provided")));

		/* the alias list length must be 1 */
		if (list_length(colaliases) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			  errmsg("number of aliases does not match number of columns")));

		/* OK, get the column alias */
		attname = strVal(linitial(colaliases));

		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   typeoid,
						   -1,
						   0);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		/* XXX can't support this because typmod wasn't passed in ... */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine row description for function returning record")));
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	return tupdesc;
}
