/*-------------------------------------------------------------------------
 * trigger.c
 *
 * This file contains functions to create and process trigger objects on
 * citus tables.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "distributed/pg_version_constants.h"

#include "access/genam.h"
#if PG_VERSION_NUM >= PG_VERSION_12
#include "access/table.h"
#else
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#endif
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/commands.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/create_citus_local_table.h"
#include "distributed/listutils.h"
#include "distributed/metadata_cache.h"
#include "distributed/namespace_utils.h"
#include "distributed/shard_utils.h"
#include "distributed/worker_protocol.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* appropriate lock modes for the owner relation according to postgres */
#define CREATE_TRIGGER_LOCK_MODE ShareRowExclusiveLock
#define ALTER_TRIGGER_LOCK_MODE AccessExclusiveLock
#define DROP_TRIGGER_LOCK_MODE AccessExclusiveLock


/* local function forward declarations */
static bool IsCreateCitusTruncateTriggerStmt(CreateTrigStmt *createTriggerStmt);
static void ErrorIfUnsupportedCreateTriggerCommand(CreateTrigStmt *createTriggerStmt);
static void ErrorIfUnsupportedAlterTriggerRenameCommand(RenameStmt *renameTriggerStmt);
static void ErrorIfUnsupportedAlterTriggerDependsCommand(AlterObjectDependsStmt *
														 alterTriggerDependsStmt);
static Value * GetAlterTriggerDependsTriggerNameValue(AlterObjectDependsStmt *
													  alterTriggerDependsStmt);
static void ErrorIfUnsupportedDropTriggerCommand(DropStmt *dropTriggerStmt);
static RangeVar * GetDropTriggerStmtRelation(DropStmt *dropTriggerStmt);
static void ExtractDropStmtTriggerAndRelationName(DropStmt *dropTriggerStmt,
												  char **triggerName,
												  char **relationName);
static void ErrorIfDropStmtDropsMultipleTriggers(DropStmt *dropTriggerStmt);
static int16 GetTriggerTypeById(Oid triggerId);


/*
 * GetExplicitTriggerCommandList returns the list of DDL commands to create
 * triggers that are explicitly created for the table with relationId. See
 * comment of GetExplicitTriggerIdList function.
 */
List *
GetExplicitTriggerCommandList(Oid relationId)
{
	List *createTriggerCommandList = NIL;

	PushOverrideEmptySearchPath(CurrentMemoryContext);

	List *triggerIdList = GetExplicitTriggerIdList(relationId);

	Oid triggerId = InvalidOid;
	foreach_oid(triggerId, triggerIdList)
	{
		char *createTriggerCommand = pg_get_triggerdef_command(triggerId);

		createTriggerCommandList = lappend(createTriggerCommandList,
										   createTriggerCommand);
	}

	/* revert back to original search_path */
	PopOverrideSearchPath();

	return createTriggerCommandList;
}


/*
 * GetExplicitTriggerNameList returns a list of trigger names that are explicitly
 * created for the table with relationId. See comment of GetExplicitTriggerIdList
 * function.
 */
List *
GetExplicitTriggerNameList(Oid relationId)
{
	List *triggerNameList = NIL;

	List *triggerIdList = GetExplicitTriggerIdList(relationId);

	Oid triggerId = InvalidOid;
	foreach_oid(triggerId, triggerIdList)
	{
		char *triggerHame = GetTriggerNameById(triggerId);
		triggerNameList = lappend(triggerNameList, triggerHame);
	}

	return triggerNameList;
}


/*
 * GetTriggerNameById returns name of the trigger identified by triggerId if it
 * exists. Otherwise, errors out.
 */
char *
GetTriggerNameById(Oid triggerId)
{
	bool missingOk = false;
	HeapTuple triggerTuple = GetTriggerTupleById(triggerId, missingOk);

	Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);
	char *triggerName = pstrdup(NameStr(triggerForm->tgname));
	heap_freetuple(triggerTuple);

	return triggerName;
}


/*
 * GetTriggerTupleById returns copy of the heap tuple from pg_trigger for
 * the trigger with triggerId. If no such trigger exists, this function returns
 * NULL or errors out depending on missingOk.
 */
HeapTuple
GetTriggerTupleById(Oid triggerId, bool missingOk)
{
	Relation pgTrigger = heap_open(TriggerRelationId, AccessShareLock);

	int scanKeyCount = 1;
	ScanKeyData scanKey[1];

#if PG_VERSION_NUM >= PG_VERSION_12
	AttrNumber attrNumber = Anum_pg_trigger_oid;
#else
	AttrNumber attrNumber = ObjectIdAttributeNumber;
#endif

	ScanKeyInit(&scanKey[0], attrNumber, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(triggerId));

	bool useIndex = true;
	SysScanDesc scanDescriptor = systable_beginscan(pgTrigger, TriggerOidIndexId,
													useIndex, NULL, scanKeyCount,
													scanKey);

	HeapTuple targetHeapTuple = NULL;

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		targetHeapTuple = heap_copytuple(heapTuple);
	}

	systable_endscan(scanDescriptor);
	heap_close(pgTrigger, NoLock);

	if (targetHeapTuple == NULL && missingOk == false)
	{
		ereport(ERROR, (errmsg("could not find heap tuple for trigger with "
							   "OID %d", triggerId)));
	}

	return targetHeapTuple;
}


/*
 * GetExplicitTriggerIdList returns a list of OIDs corresponding to the triggers
 * that are explicitly created on the relation with relationId. That means,
 * this function discards internal triggers implicitly created by postgres for
 * foreign key constraint validation and the citus_truncate_trigger.
 */
List *
GetExplicitTriggerIdList(Oid relationId)
{
	List *triggerIdList = NIL;

	Relation pgTrigger = heap_open(TriggerRelationId, AccessShareLock);

	int scanKeyCount = 1;
	ScanKeyData scanKey[1];

	ScanKeyInit(&scanKey[0], Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ, relationId);

	bool useIndex = true;
	SysScanDesc scanDescriptor = systable_beginscan(pgTrigger, TriggerRelidNameIndexId,
													useIndex, NULL, scanKeyCount,
													scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);

		/*
		 * Note that we mark truncate trigger that we create on citus tables as
		 * internal. Hence, below we discard citus_truncate_trigger as well as
		 * the implicit triggers created by postgres for foreign key validation.
		 */
		if (!triggerForm->tgisinternal)
		{
			Oid triggerId = get_relation_trigger_oid_compat(heapTuple);
			triggerIdList = lappend_oid(triggerIdList, triggerId);
		}

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	heap_close(pgTrigger, NoLock);

	return triggerIdList;
}


/*
 * get_relation_trigger_oid_compat returns OID of the trigger represented
 * by the constraintForm, which is passed as an heapTuple. OID of the
 * trigger is already stored in the triggerForm struct if major PostgreSQL
 * version is 12. However, in the older versions, we should utilize
 * HeapTupleGetOid to deduce that OID with no cost.
 */
Oid
get_relation_trigger_oid_compat(HeapTuple heapTuple)
{
	Assert(HeapTupleIsValid(heapTuple));

	Oid triggerOid = InvalidOid;

#if PG_VERSION_NUM >= PG_VERSION_12
	Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);
	triggerOid = triggerForm->oid;
#else
	triggerOid = HeapTupleGetOid(heapTuple);
#endif

	return triggerOid;
}


/*
 * PostprocessCreateTriggerStmt is called after a CREATE TRIGGER command has
 * been executed by standard process utility. This function errors out for
 * unsupported commands or creates ddl job for supported CREATE TRIGGER commands.
 */
List *
PostprocessCreateTriggerStmt(Node *node, const char *queryString)
{
	CreateTrigStmt *createTriggerStmt = castNode(CreateTrigStmt, node);

	RangeVar *relation = createTriggerStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, CREATE_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedCreateTriggerCommand(createTriggerStmt);

	if (IsCreateCitusTruncateTriggerStmt(createTriggerStmt))
	{
		return NIL;
	}

	if (IsCitusLocalTable(relationId))
	{
		char *triggerName = createTriggerStmt->trigname;
		return CitusLocalTableTriggerCommandDDLJob(relationId, triggerName,
												   queryString);
	}

	return NIL;
}


/*
 * IsCreateCitusTruncateTriggerStmt returns true if given createTriggerStmt
 * creates citus_truncate_trigger.
 */
static bool
IsCreateCitusTruncateTriggerStmt(CreateTrigStmt *createTriggerStmt)
{
	List *functionNameList = createTriggerStmt->funcname;
	RangeVar *functionRangeVar = makeRangeVarFromNameList(functionNameList);
	char *functionName = functionRangeVar->relname;
	if (strncmp(functionName, CITUS_TRUNCATE_TRIGGER_NAME, NAMEDATALEN) == 0)
	{
		return true;
	}

	return false;
}


/*
 * ErrorIfUnsupportedCreateTriggerCommand errors out for unsupported
 * "CREATE TRIGGER triggerName ON relationName" commands.
 */
static void
ErrorIfUnsupportedCreateTriggerCommand(CreateTrigStmt *createTriggerStmt)
{
	if (IsCreateCitusTruncateTriggerStmt(createTriggerStmt))
	{
		return;
	}

	RangeVar *relation = createTriggerStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, CREATE_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return;
	}

	EnsureCoordinator();
	ErrorIfTriggerCommandExecutedForUnsupportedCitusTable(relationId);
}


/*
 * CreateTriggerEventExtendNames extends relation name and trigger name with
 * shardId, and sets schema name in given CreateTrigStmt.
 */
void
CreateTriggerEventExtendNames(CreateTrigStmt *createTriggerStmt, char *schemaName,
							  uint64 shardId)
{
	RangeVar *relation = createTriggerStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	char **triggerName = &(createTriggerStmt->trigname);
	AppendShardIdToName(triggerName, shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * PostprocessAlterTriggerRenameStmt is called after a ALTER TRIGGER RENAME
 * command has been executed by standard process utility. This function errors
 * out for unsupported commands or creates ddl job for supported ALTER TRIGGER
 * RENAME commands.
 */
List *
PostprocessAlterTriggerRenameStmt(Node *node, const char *queryString)
{
	RenameStmt *renameTriggerStmt = castNode(RenameStmt, node);
	Assert(renameTriggerStmt->renameType == OBJECT_TRIGGER);

	RangeVar *relation = renameTriggerStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, ALTER_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedAlterTriggerRenameCommand(renameTriggerStmt);

	if (IsCitusLocalTable(relationId))
	{
		/* use newname as standard process utility already renamed it */
		char *triggerName = renameTriggerStmt->newname;
		return CitusLocalTableTriggerCommandDDLJob(relationId, triggerName,
												   queryString);
	}

	return NIL;
}


/*
 * ErrorIfUnsupportedAlterTriggerRenameCommand errors out for unsupported
 * "ALTER TRIGGER oldName ON relationName RENAME TO newName" commands.
 */
static void
ErrorIfUnsupportedAlterTriggerRenameCommand(RenameStmt *renameTriggerStmt)
{
	RangeVar *relation = renameTriggerStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, ALTER_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return;
	}

	EnsureCoordinator();
	ErrorIfTriggerCommandExecutedForUnsupportedCitusTable(relationId);
}


/*
 * AlterTriggerRenameEventExtendNames extends relation name, old and new trigger
 * name with shardId, and sets schema name in given RenameStmt.
 */
void
AlterTriggerRenameEventExtendNames(RenameStmt *renameTriggerStmt, char *schemaName,
								   uint64 shardId)
{
	Assert(renameTriggerStmt->renameType == OBJECT_TRIGGER);

	RangeVar *relation = renameTriggerStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	char **triggerOldName = &(renameTriggerStmt->subname);
	AppendShardIdToName(triggerOldName, shardId);

	char **triggerNewName = &(renameTriggerStmt->newname);
	AppendShardIdToName(triggerNewName, shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * PostprocessAlterTriggerDependsStmt is called after a ALTER TRIGGER DEPENDS ON
 * command has been executed by standard process utility. This function errors out
 * for unsupported commands or creates ddl job for supported ALTER TRIGGER DEPENDS
 * ON commands.
 */
List *
PostprocessAlterTriggerDependsStmt(Node *node, const char *queryString)
{
	AlterObjectDependsStmt *alterTriggerDependsStmt =
		castNode(AlterObjectDependsStmt, node);
	Assert(alterTriggerDependsStmt->objectType == OBJECT_TRIGGER);

	RangeVar *relation = alterTriggerDependsStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, ALTER_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedAlterTriggerDependsCommand(alterTriggerDependsStmt);

	if (IsCitusLocalTable(relationId))
	{
		Value *triggerNameValue =
			GetAlterTriggerDependsTriggerNameValue(alterTriggerDependsStmt);
		return CitusLocalTableTriggerCommandDDLJob(relationId, strVal(triggerNameValue),
												   queryString);
	}

	return NIL;
}


/*
 * ErrorIfUnsupportedAlterTriggerDependsCommand errors out for unsupported
 * "ALTER TRIGGER triggerName ON relationName DEPENDS ON extensionName"
 * commands.
 */
static void
ErrorIfUnsupportedAlterTriggerDependsCommand(
	AlterObjectDependsStmt *alterTriggerDependsStmt)
{
	RangeVar *relation = alterTriggerDependsStmt->relation;

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, ALTER_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return;
	}

	EnsureCoordinator();
	ErrorIfTriggerCommandExecutedForUnsupportedCitusTable(relationId);
}


/*
 * AlterTriggerDependsEventExtendNames extends relation name and trigger name
 * with shardId, and sets schema name in given AlterObjectDependsStmt.
 */
void
AlterTriggerDependsEventExtendNames(AlterObjectDependsStmt *alterTriggerDependsStmt,
									char *schemaName, uint64 shardId)
{
	Assert(alterTriggerDependsStmt->objectType == OBJECT_TRIGGER);

	RangeVar *relation = alterTriggerDependsStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	Value *triggerNameValue =
		GetAlterTriggerDependsTriggerNameValue(alterTriggerDependsStmt);
	AppendShardIdToName(&strVal(triggerNameValue), shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * GetAlterTriggerDependsTriggerName returns Value object for the trigger
 * name that given AlterObjectDependsStmt is executed for.
 */
static Value *
GetAlterTriggerDependsTriggerNameValue(AlterObjectDependsStmt *alterTriggerDependsStmt)
{
	List *triggerObjectNameList = (List *) alterTriggerDependsStmt->object;
	Value *triggerNameValue = llast(triggerObjectNameList);
	return triggerNameValue;
}


/*
 * PreprocessDropTriggerStmt is called before a DROP TRIGGER command has been
 * executed by standard process utility. This function errors out for
 * unsupported commands or creates ddl job for supported DROP TRIGGER commands.
 * The reason we process drop trigger commands before standard process utility
 * (unlike the other type of trigger commands) is that we act according to trigger
 * type in CitusLocalTableTriggerCommandDDLJob but trigger wouldn't exist after
 * standard process utility.
 */
List *
PreprocessDropTriggerStmt(Node *node, const char *queryString)
{
	DropStmt *dropTriggerStmt = castNode(DropStmt, node);
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	RangeVar *relation = GetDropTriggerStmtRelation(dropTriggerStmt);

	bool missingOk = true;
	Oid relationId = RangeVarGetRelid(relation, DROP_TRIGGER_LOCK_MODE, missingOk);

	if (!OidIsValid(relationId))
	{
		/* let standard process utility to error out */
		return NIL;
	}

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedDropTriggerCommand(dropTriggerStmt);

	if (IsCitusLocalTable(relationId))
	{
		char *triggerName = NULL;
		ExtractDropStmtTriggerAndRelationName(dropTriggerStmt, &triggerName, NULL);
		return CitusLocalTableTriggerCommandDDLJob(relationId, triggerName,
												   queryString);
	}

	return NIL;
}


/*
 * ErrorIfUnsupportedDropTriggerCommand errors out for unsupported
 * "DROP TRIGGER triggerName ON relationName" commands.
 */
static void
ErrorIfUnsupportedDropTriggerCommand(DropStmt *dropTriggerStmt)
{
	RangeVar *relation = GetDropTriggerStmtRelation(dropTriggerStmt);

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, DROP_TRIGGER_LOCK_MODE, missingOk);

	if (!IsCitusTable(relationId))
	{
		return;
	}

	EnsureCoordinator();
	ErrorIfTriggerCommandExecutedForUnsupportedCitusTable(relationId);
}


/*
 * ErrorIfTriggerCommandExecutedForUnsupportedCitusTable is an helper function
 * to error out for unsupported trigger commands depending on the citus table
 * type.
 */
void
ErrorIfTriggerCommandExecutedForUnsupportedCitusTable(Oid relationId)
{
	if (IsCitusLocalTable(relationId))
	{
		return;
	}

	char *relationName = get_rel_name(relationId);
	ereport(ERROR, (errmsg("cannot execute command on table \"%s\" because "
						   "triggers are not supported for distributed and "
						   "reference tables", relationName)));
}


/*
 * GetDropTriggerStmtRelation takes a DropStmt for a trigger
 * object and returns RangeVar for the relation that owns the trigger.
 */
static RangeVar *
GetDropTriggerStmtRelation(DropStmt *dropTriggerStmt)
{
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	ErrorIfDropStmtDropsMultipleTriggers(dropTriggerStmt);

	List *targetObjectList = dropTriggerStmt->objects;
	List *triggerObjectNameList = linitial(targetObjectList);

	List *relationNameList = NIL;
	SplitLastPointerElement(triggerObjectNameList, &relationNameList, NULL);

	return makeRangeVarFromNameList(relationNameList);
}


/*
 * DropTriggerEventExtendNames extends relation name and trigger name with
 * shardId, and sets schema name in given DropStmt by recreating "objects"
 * list.
 */
void
DropTriggerEventExtendNames(DropStmt *dropTriggerStmt, char *schemaName, uint64 shardId)
{
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	char *triggerName = NULL;
	char *relationName = NULL;
	ExtractDropStmtTriggerAndRelationName(dropTriggerStmt, &triggerName, &relationName);

	AppendShardIdToName(&triggerName, shardId);
	Value *triggerNameValue = makeString(triggerName);

	AppendShardIdToName(&relationName, shardId);
	Value *relationNameValue = makeString(relationName);

	Value *schemaNameValue = makeString(pstrdup(schemaName));

	List *shardTriggerNameList =
		list_make3(schemaNameValue, relationNameValue, triggerNameValue);
	dropTriggerStmt->objects = list_make1(shardTriggerNameList);
}


/*
 * ExtractDropStmtTriggerAndRelationName extracts triggerName and relationName
 * from given dropTriggerStmt if arguments are passed as non-null pointers.
 */
static void
ExtractDropStmtTriggerAndRelationName(DropStmt *dropTriggerStmt, char **triggerName,
									  char **relationName)
{
	ErrorIfDropStmtDropsMultipleTriggers(dropTriggerStmt);

	List *targetObjectList = dropTriggerStmt->objects;
	List *triggerObjectNameList = linitial(targetObjectList);

	List *relationNameList = NIL;
	Value *triggerNameValue = NULL;
	SplitLastPointerElement(triggerObjectNameList, &relationNameList,
							(void **) &triggerNameValue);

	if (triggerName)
	{
		*triggerName = strVal(triggerNameValue);
	}

	if (relationName)
	{
		Value *relationNameValue = llast(relationNameList);
		*relationName = strVal(relationNameValue);
	}
}


/*
 * ErrorIfDropStmtDropsMultipleTriggers errors out if given drop trigger command
 * drops more than one triggers. Actually, this can't be the case as postgres
 * doesn't support dropping multiple triggers, but we should be on the safe side.
 */
static void
ErrorIfDropStmtDropsMultipleTriggers(DropStmt *dropTriggerStmt)
{
	List *targetObjectList = dropTriggerStmt->objects;
	if (list_length(targetObjectList) > 1)
	{
		ereport(ERROR, (errmsg("bug: cannot execute DROP TRIGGER command "
							   "for multiple triggers")));
	}
}


/*
 * CitusLocalTableTriggerCommandDDLJob creates a ddl job to execute given
 * queryString trigger command on shell relation(s) in mx worker(s) and to
 * execute necessary ddl task on citus local table shard (if needed).
 */
List *
CitusLocalTableTriggerCommandDDLJob(Oid relationId, char *triggerName,
									const char *queryString)
{
	DDLJob *ddlJob = palloc0(sizeof(DDLJob));
	ddlJob->targetRelationId = relationId;
	ddlJob->commandString = queryString;

	if (!triggerName)
	{
		/*
		 * ENABLE/DISABLE TRIGGER ALL/USER commands does not specify trigger
		 * name.
		 */
		ddlJob->taskList = DDLTaskList(relationId, queryString);
		return list_make1(ddlJob);
	}

	bool missingOk = true;
	Oid triggerId = get_trigger_oid(relationId, triggerName, missingOk);
	if (!OidIsValid(triggerId))
	{
		/*
		 * For DROP, ENABLE/DISABLE, ENABLE REPLICA/ALWAYS TRIGGER commands,
		 * we create ddl job for below commands in preprocess. So trigger may
		 * not exist.
		 */
		return NIL;
	}

	/* we don't have truncate triggers on shard relations */
	int16 triggerType = GetTriggerTypeById(triggerId);
	if (!TRIGGER_FOR_TRUNCATE(triggerType))
	{
		ddlJob->taskList = DDLTaskList(relationId, queryString);
	}

	return list_make1(ddlJob);
}


/*
 * GetTriggerTypeById returns trigger type (tgtype) of the trigger identified
 * by triggerId if it exists. Otherwise, errors out.
 */
static int16
GetTriggerTypeById(Oid triggerId)
{
	bool missingOk = false;
	HeapTuple triggerTuple = GetTriggerTupleById(triggerId, missingOk);

	Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);
	int16 triggerType = triggerForm->tgtype;
	heap_freetuple(triggerTuple);

	return triggerType;
}
