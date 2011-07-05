/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/lockcmds.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/syscache.h"

#ifdef PGXC
#include "pgxc/barrier.h"
#include "pgxc/execRemote.h"
#include "pgxc/locator.h"
#include "pgxc/pgxc.h"
#include "pgxc/planner.h"
#include "pgxc/poolutils.h"
#include "pgxc/poolmgr.h"

static void ExecUtilityStmtOnNodes(const char *queryString, ExecNodes *nodes,
								   bool force_autocommit, RemoteQueryExecType exec_type);
#endif


/* Hook for plugins to get control in ProcessUtility() */
ProcessUtility_hook_type ProcessUtility_hook = NULL;


/*
 * Verify user has ownership of specified relation, else ereport.
 *
 * If noCatalogs is true then we also deny access to system catalogs,
 * except when allowSystemTableMods is true.
 */
void
CheckRelationOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	relOid = RangeVarGetRelid(rel, false);
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (noCatalogs)
	{
		if (!allowSystemTableMods &&
			IsSystemClass((Form_pg_class) GETSTRUCT(tuple)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: \"%s\" is a system catalog",
							rel->relname)));
	}

	ReleaseSysCache(tuple);
}


/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * Note: currently no need to support Query nodes here
 */
bool
CommandIsReadOnly(Node *parsetree)
{
	if (IsA(parsetree, PlannedStmt))
	{
		PlannedStmt *stmt = (PlannedStmt *) parsetree;

		switch (stmt->commandType)
		{
			case CMD_SELECT:
				if (stmt->intoClause != NULL)
					return false;		/* SELECT INTO */
				else if (stmt->rowMarks != NIL)
					return false;		/* SELECT FOR UPDATE/SHARE */
				else if (stmt->hasModifyingCTE)
					return false;		/* data-modifying CTE */
				else
					return true;
			case CMD_UPDATE:
			case CMD_INSERT:
			case CMD_DELETE:
				return false;
			default:
				elog(WARNING, "unrecognized commandType: %d",
					 (int) stmt->commandType);
				break;
		}
	}
	/* For now, treat all utility commands as read/write */
	return false;
}

/*
 * check_xact_readonly: is a utility command read-only?
 *
 * Here we use the loose rules of XactReadOnly mode: no permanent effects
 * on the database are allowed.
 */
static void
check_xact_readonly(Node *parsetree)
{
	if (!XactReadOnly)
		return;

	/*
	 * Note: Commands that need to do more complicated checking are handled
	 * elsewhere, in particular COPY and plannable statements do their own
	 * checking.  However they should all call PreventCommandIfReadOnly to
	 * actually throw the error.
	 */

	switch (nodeTag(parsetree))
	{
		case T_AlterDatabaseStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDomainStmt:
		case T_AlterFunctionStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOwnerStmt:
		case T_AlterSeqStmt:
		case T_AlterTableStmt:
		case T_RenameStmt:
		case T_CommentStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreatedbStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_CreateRoleStmt:
		case T_IndexStmt:
		case T_CreatePLangStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_AlterOpFamilyStmt:
		case T_RuleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTrigStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_AlterEnumStmt:
		case T_ViewStmt:
		case T_DropCastStmt:
		case T_DropStmt:
		case T_DropdbStmt:
		case T_DropTableSpaceStmt:
		case T_RemoveFuncStmt:
		case T_DropRoleStmt:
		case T_DropPLangStmt:
		case T_RemoveOpClassStmt:
		case T_RemoveOpFamilyStmt:
		case T_DropPropertyStmt:
		case T_GrantStmt:
		case T_GrantRoleStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_TruncateStmt:
		case T_DropOwnedStmt:
		case T_ReassignOwnedStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTSConfigurationStmt:
		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_DropFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_DropForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_CreateForeignTableStmt:
		case T_SecLabelStmt:
			PreventCommandIfReadOnly(CreateCommandTag(parsetree));
			break;
		default:
			/* do nothing */
			break;
	}
}

/*
 * PreventCommandIfReadOnly: throw error if XactReadOnly
 *
 * This is useful mainly to ensure consistency of the error message wording;
 * most callers have checked XactReadOnly for themselves.
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery: throw error if RecoveryInProgress
 *
 * The majority of operations that are unsafe in a Hot Standby slave
 * will be rejected by XactReadOnly tests.	However there are a few
 * commands that are allowed in "read-only" xacts but cannot be allowed
 * in Hot Standby mode.  Those commands should call this function.
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/* translator: %s is name of a SQL command, eg PREPARE */
			 errmsg("cannot execute %s within security-restricted operation",
					cmdname)));
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	queryString: original source text of command
 *	params: parameters to use during execution
 *	isTopLevel: true if executing a "top level" (interactively issued) command
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * Notes: as of PG 8.4, caller MUST supply a queryString; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * completionTag is only set nonempty if we want to return a nondefault status.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   const char *queryString,
			   ParamListInfo params,
			   bool isTopLevel,
			   DestReceiver *dest,
			   char *completionTag)
{
	Assert(queryString != NULL);	/* required as of 8.4 */

	/*
	 * We provide a function hook variable that lets loadable plugins get
	 * control when ProcessUtility is called.  Such a plugin would normally
	 * call standard_ProcessUtility().
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (parsetree, queryString, params,
								isTopLevel, dest, completionTag);
	else
		standard_ProcessUtility(parsetree, queryString, params,
								isTopLevel, dest, completionTag);
}

void
standard_ProcessUtility(Node *parsetree,
						const char *queryString,
						ParamListInfo params,
						bool isTopLevel,
						DestReceiver *dest,
						char *completionTag)
{
	bool operation_local = false;

	check_xact_readonly(parsetree);

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;
#ifdef PGXC
							if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
								PGXCNodeBegin();
#endif

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
							}

#ifdef PGXC
							/*
							 * Now that all the local variables have been set,
							 * it is time to rebuild the query.
							 */
							if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
							{
								char *begin_string = NULL;

								/* Result is palloc'd */
								foreach(lc, stmt->options)
								{
									DefElem    *item = (DefElem *) lfirst(lc);

									if (strcmp(item->defname, "transaction_isolation") == 0)
										begin_string = RewriteBeginQuery(begin_string,
																		 "transaction_isolation",
																		 list_make1(item->arg));
									else if (strcmp(item->defname, "transaction_read_only") == 0)
										begin_string = RewriteBeginQuery(begin_string,
																		 "transaction_read_only",
																		 list_make1(item->arg));
								}
								PGXCNodeSetBeginQuery(begin_string);
							}
#endif
						}
						break;

					case TRANS_STMT_COMMIT:
#ifdef PGXC
						if (!EndTransactionBlock(true))
#else
						if (!EndTransactionBlock())
#endif
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_PREPARE:
						PreventCommandDuringRecovery("PREPARE TRANSACTION");
#ifdef PGXC
						/*
						 * If 2PC is invoked from application, transaction is first prepared on Datanodes.
						 * 2PC file is not written for Coordinators to keep the possiblity
						 * of a COMMIT PREPARED on a separate Coordinator.
						 */
						if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
							operation_local = PGXCNodePrepare(stmt->gid);

						/*
						 * On a Postgres-XC Datanode, a prepare command coming from Coordinator
						 * has always to be executed.
						 * On a Coordinator also when a DDL has been involved in the prepared transaction
						 */
						if (IsConnFromCoord())
							operation_local = true;

						if (operation_local)
						{
#endif
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
#ifdef PGXC
						}
						else
						{
							/*
							 * In this case commit locally to erase the transaction traces
							 * but do not contact GTM
							 */
							if (!EndTransactionBlock(false))
							{
								/* report unsuccessful commit in completionTag */
								if (completionTag)
									strcpy(completionTag, "ROLLBACK");
							}
						}
#endif
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventTransactionChain(isTopLevel, "COMMIT PREPARED");
						PreventCommandDuringRecovery("COMMIT PREPARED");
#ifdef PGXC
						/*
						 * If a COMMIT PREPARED message is received from another Coordinator,
						 * Don't send it down to Datanodes.
						 *
						 * XXX We call FinishPreparedTransaction inside
						 * PGXCNodeCommitPrepared if we are doing a local
						 * operation. This is convenient because we want to
						 * hold on to the BarrierLock until local transaction
						 * is committed too.
						 *  
						 */
						if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
							PGXCNodeCommitPrepared(stmt->gid);
						else if (IsConnFromCoord())
						{
							/*
							 * A local Coordinator always commits if involved in Prepare.
							 * 2PC file is created and flushed if a DDL has been involved in the transaction.
							 * If remote connection is a Coordinator type, the commit prepared has to be done locally
							 * if and only if the Coordinator number was in the node list received from GTM.
							 */
#endif
						FinishPreparedTransaction(stmt->gid, true);
#ifdef PGXC
						}
#endif
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventTransactionChain(isTopLevel, "ROLLBACK PREPARED");
						PreventCommandDuringRecovery("ROLLBACK PREPARED");
#ifdef PGXC
						/*
						 * If a ROLLBACK PREPARED message is received from another Coordinator,
						 * Don't send it down to Datanodes.
						 */
						if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
							operation_local = PGXCNodeRollbackPrepared(stmt->gid);
						/*
						 * Local coordinator rollbacks if involved in PREPARE
						 * If remote connection is a Coordinator type, the commit prepared has to be done locally also.
						 * This works for both Datanodes and Coordinators.
						 */
						if (operation_local || IsConnFromCoord())
						{
#endif
						FinishPreparedTransaction(stmt->gid, false);
#ifdef PGXC
						}
#endif
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock();
						break;

					case TRANS_STMT_SAVEPOINT:
						{
							ListCell   *cell;
							char	   *name = NULL;

#ifdef PGXC
							ereport(ERROR,
									(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
									 (errmsg("SAVEPOINT is not yet supported."))));
#endif

							RequireTransactionChain(isTopLevel, "SAVEPOINT");

							foreach(cell, stmt->options)
							{
								DefElem    *elem = lfirst(cell);

								if (strcmp(elem->defname, "savepoint_name") == 0)
									name = strVal(elem->arg);
							}

							Assert(PointerIsValid(name));

							DefineSavepoint(name);
						}
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionChain(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->options);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionChain(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->options);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 *
			 * Note: DECLARE CURSOR is processed mostly as a SELECT, and
			 * therefore what we will get here is a PlannedStmt not a bare
			 * DeclareCursorStmt.
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				if (stmt->utilityStmt == NULL ||
					!IsA(stmt->utilityStmt, DeclareCursorStmt))
					elog(ERROR, "non-DECLARE CURSOR PlannedStmt passed to ProcessUtility");
				PerformCursorOpen(stmt, params, queryString, isTopLevel);
			}
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest,
							   completionTag);
			break;

			/*
			 * relation and attribute manipulation
			 */
		case T_CreateSchemaStmt:
#ifdef PGXC
			CreateSchemaCommand((CreateSchemaStmt *) parsetree,
								queryString, isTopLevel);
#else
			CreateSchemaCommand((CreateSchemaStmt *) parsetree,
								queryString);
#endif
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			{
				List	   *stmts;
				ListCell   *l;
				Oid			relOid;

				/* Run parse analysis ... */
				stmts = transformCreateStmt((CreateStmt *) parsetree,
											queryString);

#ifdef PGXC
				/*
				 * Add a RemoteQuery node for a query at top level on a remote Coordinator
				 */
				if (isTopLevel)
					stmts = AddRemoteQueryNode(stmts, queryString, EXEC_ON_ALL_NODES);
#endif

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, CreateStmt))
					{
						Datum		toast_options;
						static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

						/* Create the table itself */
						relOid = DefineRelation((CreateStmt *) stmt,
												RELKIND_RELATION,
												InvalidOid);

						/*
						 * Let AlterTableCreateToastTable decide if this one
						 * needs a secondary relation too.
						 */
						CommandCounterIncrement();

						/* parse and validate reloptions for the toast table */
						toast_options = transformRelOptions((Datum) 0,
											  ((CreateStmt *) stmt)->options,
															"toast",
															validnsps,
															true, false);
						(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options,
											   true);

						AlterTableCreateToastTable(relOid, toast_options);
					}
					else if (IsA(stmt, CreateForeignTableStmt))
					{
						/* Create the table itself */
						relOid = DefineRelation((CreateStmt *) stmt,
												RELKIND_FOREIGN_TABLE,
												InvalidOid);
						CreateForeignTable((CreateForeignTableStmt *) stmt,
										   relOid);
					}
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}

					/* Need CCI between commands */
					if (lnext(l) != NULL)
						CommandCounterIncrement();
				}
			}
			break;

		case T_CreateTableSpaceStmt:
#ifdef PGXC
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Postgres-XC does not support TABLESPACE yet"),
					 errdetail("The feature is not currently supported")));
#endif
			PreventTransactionChain(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropTableSpaceStmt:
			PreventTransactionChain(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
			break;

		case T_AlterTableSpaceOptionsStmt:
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
			break;

		case T_CreateExtensionStmt:
			CreateExtension((CreateExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionStmt:
			ExecAlterExtensionStmt((AlterExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionContentsStmt:
			ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree);
			break;

		case T_CreateFdwStmt:
			CreateForeignDataWrapper((CreateFdwStmt *) parsetree);
			break;

		case T_AlterFdwStmt:
			AlterForeignDataWrapper((AlterFdwStmt *) parsetree);
			break;

		case T_DropFdwStmt:
			RemoveForeignDataWrapper((DropFdwStmt *) parsetree);
			break;

		case T_CreateForeignServerStmt:
			CreateForeignServer((CreateForeignServerStmt *) parsetree);
			break;

		case T_AlterForeignServerStmt:
			AlterForeignServer((AlterForeignServerStmt *) parsetree);
			break;

		case T_DropForeignServerStmt:
			RemoveForeignServer((DropForeignServerStmt *) parsetree);
			break;

		case T_CreateUserMappingStmt:
			CreateUserMapping((CreateUserMappingStmt *) parsetree);
			break;

		case T_AlterUserMappingStmt:
			AlterUserMapping((AlterUserMappingStmt *) parsetree);
			break;

		case T_DropUserMappingStmt:
			RemoveUserMapping((DropUserMappingStmt *) parsetree);
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;
				switch (stmt->removeType)
				{
					case OBJECT_TABLE:
					case OBJECT_SEQUENCE:
					case OBJECT_VIEW:
					case OBJECT_INDEX:
					case OBJECT_FOREIGN_TABLE:
						RemoveRelations(stmt);
						break;

					case OBJECT_TYPE:
					case OBJECT_DOMAIN:
						RemoveTypes(stmt);
						break;

					case OBJECT_COLLATION:
						DropCollationsCommand(stmt);
						break;

					case OBJECT_CONVERSION:
						DropConversionsCommand(stmt);
						break;

					case OBJECT_SCHEMA:
						RemoveSchemas(stmt);
						break;

					case OBJECT_TSPARSER:
						RemoveTSParsers(stmt);
						break;

					case OBJECT_TSDICTIONARY:
						RemoveTSDictionaries(stmt);
						break;

					case OBJECT_TSTEMPLATE:
						RemoveTSTemplates(stmt);
						break;

					case OBJECT_TSCONFIGURATION:
						RemoveTSConfigurations(stmt);
						break;

					case OBJECT_EXTENSION:
						RemoveExtensions(stmt);
						break;

					default:
						elog(ERROR, "unrecognized drop object type: %d",
							 (int) stmt->removeType);
						break;
				}
#ifdef PGXC
				/*
				 * We need to check details of the object being dropped and
				 * run command on correct nodes
				 */
				if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
				{
					/* Sequence and views exists only on Coordinators */
					if (stmt->removeType == OBJECT_SEQUENCE ||
						stmt->removeType == OBJECT_VIEW)
						ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_COORDS);
					else
						ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
				}
#endif
			}
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
#ifdef PGXC
			/*
			 * PGXCTODO
			 * We may need to check details of the object being truncated and
			 * run command on correct nodes
			 */
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CommentStmt:
			CommentObject((CommentStmt *) parsetree);

#ifdef PGXC
			/*
			 * We need to check details of the object being dropped and
			 * run command on correct nodes
			 */
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				/* Sequence and views exists only on Coordinators */
				if (stmt->objtype == OBJECT_SEQUENCE ||
					stmt->objtype == OBJECT_VIEW)
					ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_COORDS);
				else if (stmt->objtype == OBJECT_RULE)
				{
					/*
					 * Sometimes rules are created only on Coordinator (views), sometimes
					 * on all nodes (other relations), so block it for the moment.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("Postgres-XC does not support COMMENT on RULE yet"),
							 errdetail("The feature is not currently supported")));
				}
				else
					ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
			}
#endif
			break;

		case T_SecLabelStmt:
			ExecSecLabelStmt((SecLabelStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;
				processed = DoCopy((CopyStmt *) parsetree, queryString);
				if (completionTag)
					snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
							 "COPY " UINT64_FORMAT, processed);
			}
			break;

		case T_PrepareStmt:
#ifdef PGXC
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Postgres-XC does not support PREPARE yet"),
					 errdetail("The feature is not currently supported")));
#endif
			CheckRestrictedOperation("PREPARE");
			PrepareQuery((PrepareStmt *) parsetree, queryString);
			break;

		case T_ExecuteStmt:
#ifdef PGXC
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Postgres-XC does not support EXECUTE yet"),
					 errdetail("The feature is not currently supported")));
#endif
			ExecuteQuery((ExecuteStmt *) parsetree, queryString, params,
						 dest, completionTag);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

			/*
			 * schema
			 */
		case T_RenameStmt:
#ifdef PGXC
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				RemoteQueryExecType remoteExecType = EXEC_ON_ALL_NODES;
				RenameStmt *stmt = (RenameStmt *) parsetree;

				if (stmt->renameType == OBJECT_SEQUENCE ||
					stmt->renameType == OBJECT_VIEW)
					remoteExecType = EXEC_ON_COORDS;
				else if (stmt->renameType == OBJECT_TABLE)
				{
					Oid relid = RangeVarGetRelid(stmt->relation, false);

					if (get_rel_relkind(relid) == RELKIND_SEQUENCE)
						remoteExecType = EXEC_ON_COORDS;
				}

				ExecUtilityStmtOnNodes(queryString, NULL, false, remoteExecType);
			}
#endif
			ExecRenameStmt((RenameStmt *) parsetree);
			break;

		case T_AlterObjectSchemaStmt:
#ifdef PGXC
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				RemoteQueryExecType remoteExecType = EXEC_ON_ALL_NODES;
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				if (stmt->objectType == OBJECT_SEQUENCE ||
					stmt->objectType == OBJECT_VIEW)
					remoteExecType = EXEC_ON_COORDS;
				else if (stmt->objectType == OBJECT_TABLE)
				{
					Oid relid = RangeVarGetRelid(stmt->relation, false);

					if (get_rel_relkind(relid) == RELKIND_SEQUENCE)
						remoteExecType = EXEC_ON_COORDS;
				}
				ExecUtilityStmtOnNodes(queryString, NULL, false, remoteExecType);
			}
#endif
			ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree);
			break;

		case T_AlterOwnerStmt:
			ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);

#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterTableStmt:
			{
				List	   *stmts;
				ListCell   *l;

				/* Run parse analysis ... */
				stmts = transformAlterTableStmt((AlterTableStmt *) parsetree,
												queryString);
#ifdef PGXC
				/*
				 * Add a RemoteQuery node for a query at top level on a remote Coordinator
				 */
				if (isTopLevel)
				{
					RemoteQueryExecType remoteExecType = EXEC_ON_ALL_NODES;
					AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

					if (stmt->relkind == OBJECT_VIEW ||
						stmt->relkind == OBJECT_SEQUENCE)
						remoteExecType = EXEC_ON_COORDS;
					else if (stmt->relkind == OBJECT_TABLE)
					{
						Oid relid = RangeVarGetRelid(stmt->relation, false);

						if (get_rel_relkind(relid) == RELKIND_SEQUENCE)
							remoteExecType = EXEC_ON_COORDS;
					}

					stmts = AddRemoteQueryNode(stmts, queryString, remoteExecType);
				}
#endif

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, AlterTableStmt))
					{
						/* Do the table alteration proper */
						AlterTable((AlterTableStmt *) stmt);
					}
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}

					/* Need CCI between commands */
					if (lnext(l) != NULL)
						CommandCounterIncrement();
				}
			}
			break;

		case T_AlterDomainStmt:
			{
				AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'T':	/* ALTER DOMAIN DEFAULT */

						/*
						 * Recursively alter column default for table and, if
						 * requested, for descendants
						 */
						AlterDomainDefault(stmt->typeName,
										   stmt->def);
						break;
					case 'N':	/* ALTER DOMAIN DROP NOT NULL */
						AlterDomainNotNull(stmt->typeName,
										   false);
						break;
					case 'O':	/* ALTER DOMAIN SET NOT NULL */
						AlterDomainNotNull(stmt->typeName,
										   true);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterDomainAddConstraint(stmt->typeName,
												 stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterDomainDropConstraint(stmt->typeName,
												  stmt->name,
												  stmt->behavior);
						break;
					default:	/* oops */
						elog(ERROR, "unrecognized alter domain type: %d",
							 (int) stmt->subtype);
						break;
				}
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_GrantStmt:
#ifdef PGXC
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				RemoteQueryExecType remoteExecType = EXEC_ON_ALL_NODES;
				GrantStmt *stmt = (GrantStmt *) parsetree;

				/* Launch GRANT on Coordinator if object is a sequence */
				if (stmt->objtype == ACL_OBJECT_SEQUENCE)
					remoteExecType = EXEC_ON_COORDS;
				else if (stmt->objtype == ACL_OBJECT_RELATION &&
						 stmt->targtype == ACL_TARGET_OBJECT)
				{
					/*
					 * In case object is a relation, differenciate the case
					 * of a sequence, a view and a table
					 */
					ListCell   *cell;
					/* Check the list of objects */
					bool		first = true;
					RemoteQueryExecType type_local = remoteExecType;

					foreach (cell, stmt->objects)
					{
						RangeVar   *relvar = (RangeVar *) lfirst(cell);
						Oid			relid = RangeVarGetRelid(relvar, false);

						if (get_rel_relkind(relid) == RELKIND_SEQUENCE ||
							get_rel_relkind(relid) == RELKIND_VIEW)
							remoteExecType = EXEC_ON_COORDS;
						else
							remoteExecType = EXEC_ON_ALL_NODES;

						/* Check if objects can be launched at the same place as 1st one */
						if (first)
						{
							type_local = remoteExecType;
							first = false;
						}
						else
						{
							if (type_local != remoteExecType)
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("PGXC does not support GRANT on multiple object types"),
										 errdetail("Grant VIEW/SEQUENCE and relations on separate queries")));
						}
					}
				}
				ExecUtilityStmtOnNodes(queryString, NULL, false, remoteExecType);
			}
#endif
			ExecuteGrantStmt((GrantStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			GrantRole((GrantRoleStmt *) parsetree);

#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterDefaultPrivilegesStmt:
			ExecAlterDefaultPrivilegesStmt((AlterDefaultPrivilegesStmt *) parsetree);

#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

			/*
			 * **************** object creation / destruction *****************
			 */
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_AGGREGATE:
						DefineAggregate(stmt->defnames, stmt->args,
										stmt->oldstyle, stmt->definition);
						break;
					case OBJECT_OPERATOR:
						Assert(stmt->args == NIL);
						DefineOperator(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TYPE:
						Assert(stmt->args == NIL);
						DefineType(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSPARSER:
						Assert(stmt->args == NIL);
						DefineTSParser(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSDICTIONARY:
						Assert(stmt->args == NIL);
						DefineTSDictionary(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSTEMPLATE:
						Assert(stmt->args == NIL);
						DefineTSTemplate(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSCONFIGURATION:
						Assert(stmt->args == NIL);
						DefineTSConfiguration(stmt->defnames, stmt->definition);
						break;
					case OBJECT_COLLATION:
						Assert(stmt->args == NIL);
						DefineCollation(stmt->defnames, stmt->definition);
						break;
					default:
						elog(ERROR, "unrecognized define stmt type: %d",
							 (int) stmt->kind);
						break;
				}
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
			{
				CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

				DefineCompositeType(stmt->typevar, stmt->coldeflist);
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CreateEnumStmt:	/* CREATE TYPE (enum) */
			DefineEnum((CreateEnumStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterEnumStmt:	/* ALTER TYPE (enum) */

			/*
			 * We disallow this in transaction blocks, because we can't cope
			 * with enum OID values getting into indexes and then having their
			 * defining pg_enum entries go away.
			 */
			PreventTransactionChain(isTopLevel, "ALTER TYPE ... ADD");
			AlterEnum((AlterEnumStmt *) parsetree);
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			DefineView((ViewStmt *) parsetree, queryString);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_COORDS);
#endif
			break;

		case T_CreateFunctionStmt:		/* CREATE FUNCTION */
			CreateFunction((CreateFunctionStmt *) parsetree, queryString);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterFunctionStmt:		/* ALTER FUNCTION */
			AlterFunction((AlterFunctionStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

#ifdef PGXC
				if (stmt->concurrent)
				{
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PGXC does not support concurrent INDEX yet"),
						 errdetail("The feature is not currently supported")));
				}
#endif

				if (stmt->concurrent)
					PreventTransactionChain(isTopLevel,
											"CREATE INDEX CONCURRENTLY");

				CheckRelationOwnership(stmt->relation, true);

				/* Run parse analysis ... */
				stmt = transformIndexStmt(stmt, queryString);

				/* ... and do it */
				DefineIndex(stmt->relation,		/* relation */
							stmt->idxname,		/* index name */
							InvalidOid, /* no predefined OID */
							stmt->accessMethod, /* am name */
							stmt->tableSpace,
							stmt->indexParams,	/* parameters */
							(Expr *) stmt->whereClause,
							stmt->options,
							stmt->excludeOpNames,
							stmt->unique,
							stmt->primary,
							stmt->isconstraint,
							stmt->deferrable,
							stmt->initdeferred,
							false,		/* is_alter_table */
							true,		/* check_rights */
							false,		/* skip_build */
							false,		/* quiet */
							stmt->concurrent);	/* concurrent */
#ifdef PGXC
				if (IS_PGXC_COORDINATOR && !stmt->isconstraint && !IsConnFromCoord())
					ExecUtilityStmtOnNodes(queryString, NULL,
										   stmt->concurrent, EXEC_ON_ALL_NODES);
#endif
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			DefineRule((RuleStmt *) parsetree, queryString);
#ifdef PGXC
			/* If a rule is created on a view, define it only on Coordinator */
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				RemoteQueryExecType remoteExecType;
				Oid relid = RangeVarGetRelid(((RuleStmt *) parsetree)->relation, false);

				if (get_rel_relkind(relid) == RELKIND_VIEW)
					remoteExecType = EXEC_ON_COORDS;
				else
					remoteExecType = EXEC_ON_ALL_NODES;

				ExecUtilityStmtOnNodes(queryString, NULL, false, remoteExecType);
			}
#endif
			break;

		case T_CreateSeqStmt:
			DefineSequence((CreateSeqStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_COORDS);
#endif
			break;

		case T_AlterSeqStmt:
			AlterSequence((AlterSeqStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_COORDS);
#endif
			break;

		case T_RemoveFuncStmt:
			{
				RemoveFuncStmt *stmt = (RemoveFuncStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_FUNCTION:
						RemoveFunction(stmt);
						break;
					case OBJECT_AGGREGATE:
						RemoveAggregate(stmt);
						break;
					case OBJECT_OPERATOR:
						RemoveOperator(stmt);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DoStmt:
			ExecuteDoStmt((DoStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			PreventTransactionChain(isTopLevel, "CREATE DATABASE");
			createdb((CreatedbStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterDatabaseStmt:
			AlterDatabase((AlterDatabaseStmt *) parsetree, isTopLevel);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterDatabaseSetStmt:
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

#ifdef PGXC
				/* Clean connections before dropping a database on local node */
				if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
				{
					char query[256];
					DropDBCleanConnection(stmt->dbname);

					/* Clean also remote Coordinators */
					sprintf(query, "CLEAN CONNECTION TO ALL FOR DATABASE %s;", stmt->dbname);

					ExecUtilityStmtOnNodes(query, NULL, true, EXEC_ON_COORDS);
				}
#endif

				PreventTransactionChain(isTopLevel, "DROP DATABASE");
				dropdb(stmt->dbname, stmt->missing_ok);
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_ALL_NODES);
#endif
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				PreventCommandDuringRecovery("NOTIFY");
				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				PreventCommandDuringRecovery("LISTEN");
				CheckRestrictedOperation("LISTEN");
				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				PreventCommandDuringRecovery("UNLISTEN");
				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				/* Allowed names are restricted if you're not superuser */
				load_file(stmt->filename, !superuser());
			}
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_DATANODES);
#endif
			break;

		case T_ClusterStmt:
			/* we choose to allow this during "read only" transactions */
			PreventCommandDuringRecovery("CLUSTER");
			cluster((ClusterStmt *) parsetree, isTopLevel);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_DATANODES);
#endif
			break;

		case T_VacuumStmt:
			/* we choose to allow this during "read only" transactions */
			PreventCommandDuringRecovery("VACUUM");
#ifdef PGXC
			/*
			 * We have to run the command on nodes before coordinator because
			 * vacuum() pops active snapshot and we can not send it to nodes
			 */
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_DATANODES);
#endif
			vacuum((VacuumStmt *) parsetree, InvalidOid, true, NULL, false,
				   isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, queryString, params, dest);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree);
#ifdef PGXC
			/* Let the pooler manage the statement */
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				VariableSetStmt *stmt = (VariableSetStmt *) parsetree;
				/*
				 * If command is local and we are not in a transaction block do NOT
				 * send this query to backend nodes
				 */
				if (!stmt->is_local || !IsTransactionBlock())
					if (PoolManagerSetCommand(stmt->is_local, queryString) < 0)
						elog(ERROR, "Postgres-XC: ERROR SET query");
			}
#endif
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* should we allow DISCARD PLANS? */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
#ifdef PGXC
			/* Let the pooler manage the statement */
			if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
			{
				/*
				 * If command is local and we are not in a transaction block do NOT
				 * send this query to backend nodes
				 */
				if (!IsTransactionBlock())
					if (PoolManagerSetCommand(false, queryString) < 0)
						elog(ERROR, "Postgres-XC: ERROR DISCARD query");
			}
#endif
			break;

		case T_CreateTrigStmt:
			(void) CreateTrigger((CreateTrigStmt *) parsetree, queryString,
								 InvalidOid, InvalidOid, false);
#ifdef PGXC
			/* Postgres-XC does not support yet triggers */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Postgres-XC does not support TRIGGER yet"),
					 errdetail("The feature is not currently supported")));

			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropPropertyStmt:
			{
				DropPropertyStmt *stmt = (DropPropertyStmt *) parsetree;
				Oid			relId;

				relId = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->removeType)
				{
					case OBJECT_RULE:
						/* RemoveRewriteRule checks permissions */
						RemoveRewriteRule(relId, stmt->property,
										  stmt->behavior, stmt->missing_ok);
#ifdef PGXC
						/* If rule is defined on a view, drop it only on Coordinators */
						if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
						{
							RemoteQueryExecType remoteExecType;
							Oid relid = RangeVarGetRelid(stmt->relation, false);

							if (get_rel_relkind(relid) == RELKIND_VIEW)
								remoteExecType = EXEC_ON_COORDS;
							else
								remoteExecType = EXEC_ON_ALL_NODES;

							ExecUtilityStmtOnNodes(queryString, NULL, false, remoteExecType);
						}
#endif
						break;
					case OBJECT_TRIGGER:
						/* DropTrigger checks permissions */
						DropTrigger(relId, stmt->property,
									stmt->behavior, stmt->missing_ok);
#ifdef PGXC
						if (IS_PGXC_COORDINATOR)
							ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->removeType);
						break;
				}
			}
			break;

		case T_CreatePLangStmt:
			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropPLangStmt:
			DropProceduralLanguage((DropPLangStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

			/*
			 * ******************************** DOMAIN statements ****
			 */
		case T_CreateDomainStmt:
			DefineDomain((CreateDomainStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

			/*
			 * ******************************** ROLE statements ****
			 */
		case T_CreateRoleStmt:
			CreateRole((CreateRoleStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterRoleStmt:
			AlterRole((AlterRoleStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterRoleSetStmt:
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropRoleStmt:
			DropRole((DropRoleStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropOwnedStmt:
			DropOwnedObjects((DropOwnedStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_ReassignOwnedStmt:
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_LockStmt:

			/*
			 * Since the lock would just get dropped immediately, LOCK TABLE
			 * outside a transaction block is presumed to be user error.
			 */
			RequireTransactionChain(isTopLevel, "LOCK TABLE");
			LockTableCommand((LockStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_ConstraintsSetStmt:
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);

			/*
			 * PGXCTODO: SET CONSTRAINT management
			 * This can just be done inside a transaction block,
			 * so just launch it on all the Datanodes.
			 * For the time being only IMMEDIATE constraints are supported
			 * so this is not really useful...
			 */
			break;

		case T_CheckPointStmt:
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to do CHECKPOINT")));
			/*
			 * You might think we should have a PreventCommandDuringRecovery()
			 * here, but we interpret a CHECKPOINT command during recovery as
			 * a request for a restartpoint instead. We allow this since it
			 * can be a useful way of reducing switchover time when using
			 * various forms of replication.
			 */
			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_DATANODES);
#endif
			break;

#ifdef PGXC
		case T_BarrierStmt:
			RequestBarrier(((BarrierStmt *) parsetree)->id, completionTag);
			break;
#endif

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				/* we choose to allow this during "read only" transactions */
				PreventCommandDuringRecovery("REINDEX");
				switch (stmt->kind)
				{
					case OBJECT_INDEX:
						ReindexIndex(stmt->relation);
						break;
					case OBJECT_TABLE:
						ReindexTable(stmt->relation);
						break;
					case OBJECT_DATABASE:

						/*
						 * This cannot run inside a user transaction block; if
						 * we were inside a transaction, then its commit- and
						 * start-transaction-command calls would not have the
						 * intended effect!
						 */
						PreventTransactionChain(isTopLevel,
												"REINDEX DATABASE");
						ReindexDatabase(stmt->name,
										stmt->do_system, stmt->do_user);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
#ifdef PGXC
				if (IS_PGXC_COORDINATOR)
					ExecUtilityStmtOnNodes(queryString, NULL,
										   stmt->kind == OBJECT_DATABASE, EXEC_ON_ALL_NODES);
#endif
				break;
			}
			break;

		case T_CreateConversionStmt:
			CreateConversionCommand((CreateConversionStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CreateCastStmt:
			CreateCast((CreateCastStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_DropCastStmt:
			DropCast((DropCastStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CreateOpClassStmt:
			DefineOpClass((CreateOpClassStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_CreateOpFamilyStmt:
			DefineOpFamily((CreateOpFamilyStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterOpFamilyStmt:
			AlterOpFamily((AlterOpFamilyStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_RemoveOpClassStmt:
			RemoveOpClass((RemoveOpClassStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_RemoveOpFamilyStmt:
			RemoveOpFamily((RemoveOpFamilyStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterTSDictionaryStmt:
			AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;

		case T_AlterTSConfigurationStmt:
			AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);
#ifdef PGXC
			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, false, EXEC_ON_ALL_NODES);
#endif
			break;
#ifdef PGXC
		case T_RemoteQuery:
			Assert(IS_PGXC_COORDINATOR);
			/*
			 * Do not launch query on Other Datanodes if remote connection is a coordinator one
			 * it will cause a deadlock in the cluster at Datanode levels.
			 */
			if (!IsConnFromCoord())
				ExecRemoteUtility((RemoteQuery *) parsetree);
			break;

		case T_CleanConnStmt:
			Assert(IS_PGXC_COORDINATOR);
			CleanConnection((CleanConnStmt *) parsetree);

			if (IS_PGXC_COORDINATOR)
				ExecUtilityStmtOnNodes(queryString, NULL, true, EXEC_ON_COORDS);
			break;
#endif
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			break;
	}
}

#ifdef PGXC
/*
 * Execute a Utility statement on nodes, including Coordinators
 * If the DDL is received from a remote Coordinator,
 * it is not possible to push down DDL to Datanodes
 * as it is taken in charge by the remote Coordinator.
 */
static void
ExecUtilityStmtOnNodes(const char *queryString, ExecNodes *nodes,
					   bool force_autocommit, RemoteQueryExecType exec_type)
{
	if (!IsConnFromCoord())
	{
		RemoteQuery *step = makeNode(RemoteQuery);
		step->combine_type = COMBINE_TYPE_SAME;
		step->exec_nodes = nodes;
		step->sql_statement = pstrdup(queryString);
		step->force_autocommit = force_autocommit;
		step->exec_type = exec_type;
		ExecRemoteUtility(step);
		pfree(step->sql_statement);
		pfree(step);
	}
}
#endif

/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;		/* not our business to raise error */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				if (stmt->into)
					return false;
				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;		/* not our business to raise error */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				if (stmt->into)
					return NULL;
				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		Return "true" if this Query will send output to the destination.
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/* returns tuples ... unless it's DECLARE CURSOR or SELECT INTO */
			if (parsetree->utilityStmt == NULL &&
				parsetree->intoClause == NULL)
				return true;
			break;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* the forms with RETURNING return tuples */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/* probably shouldn't get here */
			break;
	}
	return false;				/* default */
}
#endif


/*
 * AlterObjectTypeCommandTag
 *		helper function for CreateCommandTag
 *
 * This covers most cases where ALTER is used with an ObjectType enum.
 */
static const char *
AlterObjectTypeCommandTag(ObjectType objtype)
{
	const char *tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = "ALTER AGGREGATE";
			break;
		case OBJECT_ATTRIBUTE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_CAST:
			tag = "ALTER CAST";
			break;
		case OBJECT_COLLATION:
			tag = "ALTER COLLATION";
			break;
		case OBJECT_COLUMN:
			tag = "ALTER TABLE";
			break;
		case OBJECT_CONSTRAINT:
			tag = "ALTER TABLE";
			break;
		case OBJECT_CONVERSION:
			tag = "ALTER CONVERSION";
			break;
		case OBJECT_DATABASE:
			tag = "ALTER DATABASE";
			break;
		case OBJECT_DOMAIN:
			tag = "ALTER DOMAIN";
			break;
		case OBJECT_EXTENSION:
			tag = "ALTER EXTENSION";
			break;
		case OBJECT_FDW:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = "ALTER SERVER";
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = "ALTER FOREIGN TABLE";
			break;
		case OBJECT_FUNCTION:
			tag = "ALTER FUNCTION";
			break;
		case OBJECT_INDEX:
			tag = "ALTER INDEX";
			break;
		case OBJECT_LANGUAGE:
			tag = "ALTER LANGUAGE";
			break;
		case OBJECT_LARGEOBJECT:
			tag = "ALTER LARGE OBJECT";
			break;
		case OBJECT_OPCLASS:
			tag = "ALTER OPERATOR CLASS";
			break;
		case OBJECT_OPERATOR:
			tag = "ALTER OPERATOR";
			break;
		case OBJECT_OPFAMILY:
			tag = "ALTER OPERATOR FAMILY";
			break;
		case OBJECT_ROLE:
			tag = "ALTER ROLE";
			break;
		case OBJECT_RULE:
			tag = "ALTER RULE";
			break;
		case OBJECT_SCHEMA:
			tag = "ALTER SCHEMA";
			break;
		case OBJECT_SEQUENCE:
			tag = "ALTER SEQUENCE";
			break;
		case OBJECT_TABLE:
			tag = "ALTER TABLE";
			break;
		case OBJECT_TABLESPACE:
			tag = "ALTER TABLESPACE";
			break;
		case OBJECT_TRIGGER:
			tag = "ALTER TRIGGER";
			break;
		case OBJECT_TSCONFIGURATION:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;
		case OBJECT_TSDICTIONARY:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;
		case OBJECT_TSPARSER:
			tag = "ALTER TEXT SEARCH PARSER";
			break;
		case OBJECT_TSTEMPLATE:
			tag = "ALTER TEXT SEARCH TEMPLATE";
			break;
		case OBJECT_TYPE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_VIEW:
			tag = "ALTER VIEW";
			break;
		default:
			tag = "???";
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		utility to get a string representation of the command operation,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * NB: all result strings must be shorter than COMPLETION_TAG_BUFSIZE.
 * Also, the result must point at a true constant (permanent storage).
 */
const char *
CreateCommandTag(Node *parsetree)
{
	const char *tag;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
			tag = "INSERT";
			break;

		case T_DeleteStmt:
			tag = "DELETE";
			break;

		case T_UpdateStmt:
			tag = "UPDATE";
			break;

		case T_SelectStmt:
			tag = "SELECT";
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = "BEGIN";
						break;

					case TRANS_STMT_START:
						tag = "START TRANSACTION";
						break;

					case TRANS_STMT_COMMIT:
						tag = "COMMIT";
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = "ROLLBACK";
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = "SAVEPOINT";
						break;

					case TRANS_STMT_RELEASE:
						tag = "RELEASE";
						break;

					case TRANS_STMT_PREPARE:
						tag = "PREPARE TRANSACTION";
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = "COMMIT PREPARED";
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = "ROLLBACK PREPARED";
						break;

					default:
						tag = "???";
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = "DECLARE CURSOR";
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = "CLOSE CURSOR ALL";
				else
					tag = "CLOSE CURSOR";
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? "MOVE" : "FETCH";
			}
			break;

		case T_CreateDomainStmt:
			tag = "CREATE DOMAIN";
			break;

		case T_CreateSchemaStmt:
			tag = "CREATE SCHEMA";
			break;

		case T_CreateStmt:
			tag = "CREATE TABLE";
			break;

		case T_CreateTableSpaceStmt:
			tag = "CREATE TABLESPACE";
			break;

		case T_DropTableSpaceStmt:
			tag = "DROP TABLESPACE";
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = "ALTER TABLESPACE";
			break;

		case T_CreateExtensionStmt:
			tag = "CREATE EXTENSION";
			break;

		case T_AlterExtensionStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_AlterExtensionContentsStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_CreateFdwStmt:
			tag = "CREATE FOREIGN DATA WRAPPER";
			break;

		case T_AlterFdwStmt:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;

		case T_DropFdwStmt:
			tag = "DROP FOREIGN DATA WRAPPER";
			break;

		case T_CreateForeignServerStmt:
			tag = "CREATE SERVER";
			break;

		case T_AlterForeignServerStmt:
			tag = "ALTER SERVER";
			break;

		case T_DropForeignServerStmt:
			tag = "DROP SERVER";
			break;

		case T_CreateUserMappingStmt:
			tag = "CREATE USER MAPPING";
			break;

		case T_AlterUserMappingStmt:
			tag = "ALTER USER MAPPING";
			break;

		case T_DropUserMappingStmt:
			tag = "DROP USER MAPPING";
			break;

		case T_CreateForeignTableStmt:
			tag = "CREATE FOREIGN TABLE";
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = "DROP TABLE";
					break;
				case OBJECT_SEQUENCE:
					tag = "DROP SEQUENCE";
					break;
				case OBJECT_VIEW:
					tag = "DROP VIEW";
					break;
				case OBJECT_INDEX:
					tag = "DROP INDEX";
					break;
				case OBJECT_TYPE:
					tag = "DROP TYPE";
					break;
				case OBJECT_DOMAIN:
					tag = "DROP DOMAIN";
					break;
				case OBJECT_COLLATION:
					tag = "DROP COLLATION";
					break;
				case OBJECT_CONVERSION:
					tag = "DROP CONVERSION";
					break;
				case OBJECT_SCHEMA:
					tag = "DROP SCHEMA";
					break;
				case OBJECT_TSPARSER:
					tag = "DROP TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "DROP TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "DROP TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "DROP TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = "DROP FOREIGN TABLE";
					break;
				case OBJECT_EXTENSION:
					tag = "DROP EXTENSION";
					break;
				default:
					tag = "???";
			}
			break;

		case T_TruncateStmt:
			tag = "TRUNCATE TABLE";
			break;

		case T_CommentStmt:
			tag = "COMMENT";
			break;

		case T_SecLabelStmt:
			tag = "SECURITY LABEL";
			break;

		case T_CopyStmt:
			tag = "COPY";
			break;

		case T_RenameStmt:
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->relkind);
			break;

		case T_AlterDomainStmt:
			tag = "ALTER DOMAIN";
			break;

		case T_AlterFunctionStmt:
			tag = "ALTER FUNCTION";
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT" : "REVOKE";
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT ROLE" : "REVOKE ROLE";
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = "ALTER DEFAULT PRIVILEGES";
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = "CREATE AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "CREATE OPERATOR";
					break;
				case OBJECT_TYPE:
					tag = "CREATE TYPE";
					break;
				case OBJECT_TSPARSER:
					tag = "CREATE TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "CREATE TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "CREATE TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "CREATE TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_COLLATION:
					tag = "CREATE COLLATION";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CompositeTypeStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateEnumStmt:
			tag = "CREATE TYPE";
			break;

		case T_AlterEnumStmt:
			tag = "ALTER TYPE";
			break;

		case T_ViewStmt:
			tag = "CREATE VIEW";
			break;

		case T_CreateFunctionStmt:
			tag = "CREATE FUNCTION";
			break;

		case T_IndexStmt:
			tag = "CREATE INDEX";
			break;

		case T_RuleStmt:
			tag = "CREATE RULE";
			break;

		case T_CreateSeqStmt:
			tag = "CREATE SEQUENCE";
			break;

		case T_AlterSeqStmt:
			tag = "ALTER SEQUENCE";
			break;

		case T_RemoveFuncStmt:
			switch (((RemoveFuncStmt *) parsetree)->kind)
			{
				case OBJECT_FUNCTION:
					tag = "DROP FUNCTION";
					break;
				case OBJECT_AGGREGATE:
					tag = "DROP AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "DROP OPERATOR";
					break;
				default:
					tag = "???";
			}
			break;

		case T_DoStmt:
			tag = "DO";
			break;

		case T_CreatedbStmt:
			tag = "CREATE DATABASE";
			break;

		case T_AlterDatabaseStmt:
			tag = "ALTER DATABASE";
			break;

		case T_AlterDatabaseSetStmt:
			tag = "ALTER DATABASE";
			break;

		case T_DropdbStmt:
			tag = "DROP DATABASE";
			break;

		case T_NotifyStmt:
			tag = "NOTIFY";
			break;

		case T_ListenStmt:
			tag = "LISTEN";
			break;

		case T_UnlistenStmt:
			tag = "UNLISTEN";
			break;

		case T_LoadStmt:
			tag = "LOAD";
			break;

		case T_ClusterStmt:
			tag = "CLUSTER";
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->options & VACOPT_VACUUM)
				tag = "VACUUM";
			else
				tag = "ANALYZE";
			break;

		case T_ExplainStmt:
			tag = "EXPLAIN";
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = "SET";
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = "RESET";
					break;
				default:
					tag = "???";
			}
			break;

		case T_VariableShowStmt:
			tag = "SHOW";
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = "DISCARD ALL";
					break;
				case DISCARD_PLANS:
					tag = "DISCARD PLANS";
					break;
				case DISCARD_TEMP:
					tag = "DISCARD TEMP";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreateTrigStmt:
			tag = "CREATE TRIGGER";
			break;

		case T_DropPropertyStmt:
			switch (((DropPropertyStmt *) parsetree)->removeType)
			{
				case OBJECT_TRIGGER:
					tag = "DROP TRIGGER";
					break;
				case OBJECT_RULE:
					tag = "DROP RULE";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreatePLangStmt:
			tag = "CREATE LANGUAGE";
			break;

		case T_DropPLangStmt:
			tag = "DROP LANGUAGE";
			break;

		case T_CreateRoleStmt:
			tag = "CREATE ROLE";
			break;

		case T_AlterRoleStmt:
			tag = "ALTER ROLE";
			break;

		case T_AlterRoleSetStmt:
			tag = "ALTER ROLE";
			break;

		case T_DropRoleStmt:
			tag = "DROP ROLE";
			break;

		case T_DropOwnedStmt:
			tag = "DROP OWNED";
			break;

		case T_ReassignOwnedStmt:
			tag = "REASSIGN OWNED";
			break;

		case T_LockStmt:
			tag = "LOCK TABLE";
			break;

		case T_ConstraintsSetStmt:
			tag = "SET CONSTRAINTS";
			break;

		case T_CheckPointStmt:
			tag = "CHECKPOINT";
			break;

#ifdef PGXC
		case T_BarrierStmt:
			tag = "BARRIER";
			break;
#endif

		case T_ReindexStmt:
			tag = "REINDEX";
			break;

		case T_CreateConversionStmt:
			tag = "CREATE CONVERSION";
			break;

		case T_CreateCastStmt:
			tag = "CREATE CAST";
			break;

		case T_DropCastStmt:
			tag = "DROP CAST";
			break;

		case T_CreateOpClassStmt:
			tag = "CREATE OPERATOR CLASS";
			break;

		case T_CreateOpFamilyStmt:
			tag = "CREATE OPERATOR FAMILY";
			break;

		case T_AlterOpFamilyStmt:
			tag = "ALTER OPERATOR FAMILY";
			break;

		case T_RemoveOpClassStmt:
			tag = "DROP OPERATOR CLASS";
			break;

		case T_RemoveOpFamilyStmt:
			tag = "DROP OPERATOR FAMILY";
			break;

		case T_AlterTSDictionaryStmt:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;

		case T_AlterTSConfigurationStmt:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;

		case T_PrepareStmt:
			tag = "PREPARE";
			break;

		case T_ExecuteStmt:
			tag = "EXECUTE";
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = "DEALLOCATE ALL";
				else
					tag = "DEALLOCATE";
			}
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->intoClause != NULL)
							tag = "SELECT INTO";
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							if (((PlanRowMark *) linitial(stmt->rowMarks))->markType == ROW_MARK_EXCLUSIVE)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->intoClause != NULL)
							tag = "SELECT INTO";
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							if (((RowMarkClause *) linitial(stmt->rowMarks))->forUpdate)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

		case T_ExecDirectStmt:
			tag = "EXECUTE DIRECT";
			break;
		case T_CleanConnStmt:
			tag = "CLEAN CONNECTION";
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = "???";
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;		/* CREATE AS, SELECT INTO */
			else
				lev = LOGSTMT_ALL;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_DropFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_DropForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* Look through a PREPARE to the contained stmt */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* Look through an EXECUTE to the referenced stmt */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveFuncStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/* Look through an EXPLAIN ANALYZE to the contained stmt */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/* don't "break", as explain.c will use the last value */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/* Plain EXPLAIN isn't so interesting */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropPropertyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropPLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL? */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						if (stmt->intoClause != NULL)
							lev = LOGSTMT_DDL;	/* CREATE AS, SELECT INTO */
						else
							lev = LOGSTMT_ALL;	/* SELECT or DECLARE CURSOR */
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						if (stmt->intoClause != NULL)
							lev = LOGSTMT_DDL;	/* CREATE AS, SELECT INTO */
						else
							lev = LOGSTMT_ALL;	/* SELECT or DECLARE CURSOR */
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}

			}
			break;
#ifdef PGXC
		case T_CleanConnStmt:
			lev = LOGSTMT_DDL;
			break;
#endif

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}
