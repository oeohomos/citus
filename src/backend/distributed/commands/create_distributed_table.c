/*-------------------------------------------------------------------------
 *
 * create_distributed_relation.c
 *	  Routines relation to the creation of distributed relations.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#if (PG_VERSION_NUM >= 90600)
#include "catalog/pg_constraint_fn.h"
#endif
#include "catalog/pg_enum.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "distributed/colocation_utils.h"
#include "distributed/distribution_column.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/master_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_copy.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/pg_dist_colocation.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/reference_table_utils.h"
#include "distributed/worker_protocol.h"
#include "distributed/worker_transaction.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/inval.h"


/* Replication model to use when creating distributed tables */
int ReplicationModel = REPLICATION_MODEL_COORDINATOR;


/* local function forward declarations */
static void CreateReferenceTable(Oid distributedRelationId);
static void ConvertToDistributedTable(Oid relationId, char *distributionColumnName,
									  char distributionMethod, char replicationModel,
									  uint32 colocationId, bool requireEmpty);
static char LookupDistributionMethod(Oid distributionMethodOid);
static Oid SupportFunctionForColumn(Var *partitionColumn, Oid accessMethodId,
									int16 supportFunctionNumber);
static bool LocalTableEmpty(Oid tableId);
static void ErrorIfNotSupportedConstraint(Relation relation, char distributionMethod,
										  Var *distributionColumn, uint32 colocationId);
static void ErrorIfNotSupportedForeignConstraint(Relation relation,
												 char distributionMethod,
												 Var *distributionColumn,
												 uint32 colocationId);
static void CreateHashDistributedTable(Oid relationId, char *distributionColumnName,
									   char *colocateWithTableName,
									   int shardCount, int replicationFactor);
static Oid ColumnType(Oid relationId, char *columnName);
static void CopyLocalDataIntoShards(Oid relationId);
static List * TupleDescColumnNameList(TupleDesc tupleDescriptor);

/* exports for SQL callable functions */
PG_FUNCTION_INFO_V1(master_create_distributed_table);
PG_FUNCTION_INFO_V1(create_distributed_table);
PG_FUNCTION_INFO_V1(create_reference_table);


/*
 * master_create_distributed_table accepts a table, distribution column and
 * method and performs the corresponding catalog changes.
 *
 * Note that this udf is depreciated and cannot create colocated tables, so we
 * always use INVALID_COLOCATION_ID.
 */
Datum
master_create_distributed_table(PG_FUNCTION_ARGS)
{
	Oid distributedRelationId = PG_GETARG_OID(0);
	text *distributionColumnText = PG_GETARG_TEXT_P(1);
	Oid distributionMethodOid = PG_GETARG_OID(2);

	char *distributionColumnName = text_to_cstring(distributionColumnText);
	char distributionMethod = LookupDistributionMethod(distributionMethodOid);
	bool requireEmpty = true;

	EnsureCoordinator();

	if (ReplicationModel != REPLICATION_MODEL_COORDINATOR)
	{
		ereport(NOTICE, (errmsg("using statement-based replication"),
						 errdetail("The current replication_model setting is "
								   "'streaming', which is not supported by "
								   "master_create_distributed_table."),
						 errhint("Use create_distributed_table to use the streaming "
								 "replication model.")));
	}

	ConvertToDistributedTable(distributedRelationId, distributionColumnName,
							  distributionMethod, REPLICATION_MODEL_COORDINATOR,
							  INVALID_COLOCATION_ID, requireEmpty);

	PG_RETURN_VOID();
}


/*
 * create_distributed_table gets a table name, distribution column,
 * distribution method and colocate_with option, then it creates a
 * distributed table.
 */
Datum
create_distributed_table(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	text *distributionColumnText = PG_GETARG_TEXT_P(1);
	Oid distributionMethodOid = PG_GETARG_OID(2);

	char *distributionColumnName = text_to_cstring(distributionColumnText);
	char distributionMethod = LookupDistributionMethod(distributionMethodOid);
	text *colocateWithTableNameText = NULL;
	char *colocateWithTableName = NULL;

	EnsureCoordinator();

	/* guard against a binary update without a function update */
	if (PG_NARGS() >= 4)
	{
		colocateWithTableNameText = PG_GETARG_TEXT_P(3);
		colocateWithTableName = text_to_cstring(colocateWithTableNameText);
	}
	else
	{
		colocateWithTableName = "default";
	}

	/* check if we try to colocate with hash distributed tables */
	if (pg_strncasecmp(colocateWithTableName, "default", NAMEDATALEN) != 0 &&
		pg_strncasecmp(colocateWithTableName, "none", NAMEDATALEN) != 0)
	{
		Oid colocateWithTableOid = ResolveRelationId(colocateWithTableNameText);
		char colocateWithTableDistributionMethod = PartitionMethod(colocateWithTableOid);

		if (colocateWithTableDistributionMethod != DISTRIBUTE_BY_HASH ||
			distributionMethod != DISTRIBUTE_BY_HASH)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot distribute relation"),
							errdetail("Currently, colocate_with option is only supported "
									  "for hash distributed tables.")));
		}
	}

	/* if distribution method is not hash, just create partition metadata */
	if (distributionMethod != DISTRIBUTE_BY_HASH)
	{
		bool requireEmpty = true;

		if (ReplicationModel != REPLICATION_MODEL_COORDINATOR)
		{
			ereport(NOTICE, (errmsg("using statement-based replication"),
							 errdetail("Streaming replication is supported only for "
									   "hash-distributed tables.")));
		}

		ConvertToDistributedTable(relationId, distributionColumnName,
								  distributionMethod, REPLICATION_MODEL_COORDINATOR,
								  INVALID_COLOCATION_ID, requireEmpty);
		PG_RETURN_VOID();
	}

	/* use configuration values for shard count and shard replication factor */
	CreateHashDistributedTable(relationId, distributionColumnName,
							   colocateWithTableName, ShardCount,
							   ShardReplicationFactor);

	if (ShouldSyncTableMetadata(relationId))
	{
		CreateTableMetadataOnWorkers(relationId);
	}

	PG_RETURN_VOID();
}


/*
 * create_reference_table accepts a table and then it creates a distributed
 * table which has one shard and replication factor is set to
 * the worker count.
 */
Datum
create_reference_table(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);

	CreateReferenceTable(relationId);

	PG_RETURN_VOID();
}


/*
 * CreateReferenceTable creates a distributed table with the given relationId. The
 * created table has one shard and replication factor is set to the active worker
 * count. In fact, the above is the definition of a reference table in Citus.
 */
static void
CreateReferenceTable(Oid relationId)
{
	uint32 colocationId = INVALID_COLOCATION_ID;
	List *workerNodeList = WorkerNodeList();
	int replicationFactor = list_length(workerNodeList);
	char *distributionColumnName = NULL;
	bool requireEmpty = true;
	char relationKind = 0;

	EnsureCoordinator();

	/* if there are no workers, error out */
	if (replicationFactor == 0)
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("cannot create reference table \"%s\"", relationName),
						errdetail("There are no active worker nodes.")));
	}

	/* relax empty table requirement for regular (non-foreign) tables */
	relationKind = get_rel_relkind(relationId);
	if (relationKind == RELKIND_RELATION)
	{
		requireEmpty = false;
	}

	colocationId = CreateReferenceTableColocationId();

	/* first, convert the relation into distributed relation */
	ConvertToDistributedTable(relationId, distributionColumnName,
							  DISTRIBUTE_BY_NONE, REPLICATION_MODEL_2PC, colocationId,
							  requireEmpty);

	/* now, create the single shard replicated to all nodes */
	CreateReferenceTableShard(relationId);

	CreateTableMetadataOnWorkers(relationId);

	/* copy over data for regular relations */
	if (relationKind == RELKIND_RELATION)
	{
		CopyLocalDataIntoShards(relationId);
	}
}


/*
 * ConvertToDistributedTable converts the given regular PostgreSQL table into a
 * distributed table. First, it checks if the given table can be distributed,
 * then it creates related tuple in pg_dist_partition. If requireEmpty is true,
 * this function errors out when presented with a relation containing rows.
 *
 * XXX: We should perform more checks here to see if this table is fit for
 * partitioning. At a minimum, we should validate the following: (i) this node
 * runs as the master node, (ii) table does not make use of the inheritance
 * mechanism, (iii) table does not own columns that are sequences, and (iv)
 * table does not have collated columns.
 */
static void
ConvertToDistributedTable(Oid relationId, char *distributionColumnName,
						  char distributionMethod, char replicationModel,
						  uint32 colocationId, bool requireEmpty)
{
	Relation relation = NULL;
	TupleDesc relationDesc = NULL;
	char *relationName = NULL;
	char relationKind = 0;
	Var *distributionColumn = NULL;

	/* check global replication settings before continuing */
	EnsureReplicationSettings(InvalidOid, replicationModel);

	/*
	 * Lock target relation with an exclusive lock - there's no way to make
	 * sense of this table until we've committed, and we don't want multiple
	 * backends manipulating this relation.
	 */
	relation = relation_open(relationId, ExclusiveLock);
	relationDesc = RelationGetDescr(relation);
	relationName = RelationGetRelationName(relation);

	EnsureTableOwner(relationId);

	/* check that the relation is not already distributed */
	if (IsDistributedTable(relationId))
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						errmsg("table \"%s\" is already distributed",
							   relationName)));
	}

	/* verify target relation does not use WITH (OIDS) PostgreSQL feature */
	if (relationDesc->tdhasoid)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot distribute relation: %s", relationName),
						errdetail("Distributed relations must not specify the WITH "
								  "(OIDS) option in their definitions.")));
	}

	/* verify target relation is either regular or foreign table */
	relationKind = relation->rd_rel->relkind;
	if (relationKind != RELKIND_RELATION && relationKind != RELKIND_FOREIGN_TABLE)
	{
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("cannot distribute relation: %s",
							   relationName),
						errdetail("Distributed relations must be regular or "
								  "foreign tables.")));
	}

	/* check that table is empty if that is required */
	if (requireEmpty && !LocalTableEmpty(relationId))
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						errmsg("cannot distribute relation \"%s\"",
							   relationName),
						errdetail("Relation \"%s\" contains data.",
								  relationName),
						errhint("Empty your table before distributing it.")));
	}

	/*
	 * Distribution column returns NULL for reference tables,
	 * but it is not used below for reference tables.
	 */
	distributionColumn = BuildDistributionKeyFromColumnName(relation,
															distributionColumnName);

	/* check for support function needed by specified partition method */
	if (distributionMethod == DISTRIBUTE_BY_HASH)
	{
		Oid hashSupportFunction = SupportFunctionForColumn(distributionColumn,
														   HASH_AM_OID, HASHPROC);
		if (hashSupportFunction == InvalidOid)
		{
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION),
							errmsg("could not identify a hash function for type %s",
								   format_type_be(distributionColumn->vartype)),
							errdatatype(distributionColumn->vartype),
							errdetail("Partition column types must have a hash function "
									  "defined to use hash partitioning.")));
		}
	}
	else if (distributionMethod == DISTRIBUTE_BY_RANGE)
	{
		Oid btreeSupportFunction = SupportFunctionForColumn(distributionColumn,
															BTREE_AM_OID, BTORDER_PROC);
		if (btreeSupportFunction == InvalidOid)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a comparison function for type %s",
							format_type_be(distributionColumn->vartype)),
					 errdatatype(distributionColumn->vartype),
					 errdetail("Partition column types must have a comparison function "
							   "defined to use range partitioning.")));
		}
	}

	ErrorIfNotSupportedConstraint(relation, distributionMethod, distributionColumn,
								  colocationId);

	InsertIntoPgDistPartition(relationId, distributionMethod, distributionColumn,
							  colocationId, replicationModel);

	relation_close(relation, NoLock);

	/*
	 * PostgreSQL supports truncate trigger for regular relations only.
	 * Truncate on foreign tables is not supported.
	 */
	if (relationKind == RELKIND_RELATION)
	{
		CreateTruncateTrigger(relationId);
	}
}


/*
 * ErrorIfNotSupportedConstraint run checks related to unique index / exclude
 * constraints.
 *
 * The function skips the uniqeness checks for reference tables (i.e., distribution
 * method is 'none').
 *
 * Forbid UNIQUE, PRIMARY KEY, or EXCLUDE constraints on append partitioned
 * tables, since currently there is no way of enforcing uniqueness for
 * overlapping shards.
 *
 * Similarly, do not allow such constraints if they do not include partition
 * column. This check is important for two reasons:
 * i. First, currently Citus does not enforce uniqueness constraint on multiple
 * shards.
 * ii. Second, INSERT INTO .. ON CONFLICT (i.e., UPSERT) queries can be executed
 * with no further check for constraints.
 */
static void
ErrorIfNotSupportedConstraint(Relation relation, char distributionMethod,
							  Var *distributionColumn, uint32 colocationId)
{
	char *relationName = NULL;
	List *indexOidList = NULL;
	ListCell *indexOidCell = NULL;

	/*
	 * We first perform check for foreign constraints. It is important to do this check
	 * before next check, because other types of constraints are allowed on reference
	 * tables and we return early for those constraints thanks to next check. Therefore,
	 * for reference tables, we first check for foreing constraints and if they are OK,
	 * we do not error out for other types of constraints.
	 */
	ErrorIfNotSupportedForeignConstraint(relation, distributionMethod, distributionColumn,
										 colocationId);

	/*
	 * Citus supports any kind of uniqueness constraints for reference tables
	 * given that they only consist of a single shard and we can simply rely on
	 * Postgres.
	 */
	if (distributionMethod == DISTRIBUTE_BY_NONE)
	{
		return;
	}

	relationName = RelationGetRelationName(relation);
	indexOidList = RelationGetIndexList(relation);

	foreach(indexOidCell, indexOidList)
	{
		Oid indexOid = lfirst_oid(indexOidCell);
		Relation indexDesc = index_open(indexOid, RowExclusiveLock);
		IndexInfo *indexInfo = NULL;
		AttrNumber *attributeNumberArray = NULL;
		bool hasDistributionColumn = false;
		int attributeCount = 0;
		int attributeIndex = 0;

		/* extract index key information from the index's pg_index info */
		indexInfo = BuildIndexInfo(indexDesc);

		/* only check unique indexes and exclusion constraints. */
		if (indexInfo->ii_Unique == false && indexInfo->ii_ExclusionOps == NULL)
		{
			index_close(indexDesc, NoLock);
			continue;
		}

		/*
		 * Citus cannot enforce uniqueness/exclusion constraints with overlapping shards.
		 * Thus, emit a warning for unique indexes and exclusion constraints on
		 * append partitioned tables.
		 */
		if (distributionMethod == DISTRIBUTE_BY_APPEND)
		{
			ereport(WARNING, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							  errmsg("table \"%s\" has a UNIQUE or EXCLUDE constraint",
									 relationName),
							  errdetail("UNIQUE constraints, EXCLUDE constraints, "
										"and PRIMARY KEYs on "
										"append-partitioned tables cannot be enforced."),
							  errhint("Consider using hash partitioning.")));
		}

		attributeCount = indexInfo->ii_NumIndexAttrs;
		attributeNumberArray = indexInfo->ii_KeyAttrNumbers;

		for (attributeIndex = 0; attributeIndex < attributeCount; attributeIndex++)
		{
			AttrNumber attributeNumber = attributeNumberArray[attributeIndex];
			bool uniqueConstraint = false;
			bool exclusionConstraintWithEquality = false;

			if (distributionColumn->varattno != attributeNumber)
			{
				continue;
			}

			uniqueConstraint = indexInfo->ii_Unique;
			exclusionConstraintWithEquality = (indexInfo->ii_ExclusionOps != NULL &&
											   OperatorImplementsEquality(
												   indexInfo->ii_ExclusionOps[
													   attributeIndex]));

			if (uniqueConstraint || exclusionConstraintWithEquality)
			{
				hasDistributionColumn = true;
				break;
			}
		}

		if (!hasDistributionColumn)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot distribute relation: \"%s\"",
								   relationName),
							errdetail("Distributed relations cannot have UNIQUE, "
									  "EXCLUDE, or PRIMARY KEY constraints that do not "
									  "include the partition column (with an equality "
									  "operator if EXCLUDE).")));
		}

		index_close(indexDesc, NoLock);
	}
}


/*
 * ErrorIfNotSupportedForeignConstraint runs checks related to foreign constraints and
 * errors out if it is not possible to create one of the foreign constraint in distributed
 * environment.
 *
 * To support foreign constraints, we require that;
 * - Referencing and referenced tables are hash distributed.
 * - Referencing and referenced tables are co-located.
 * - Foreign constraint is defined over distribution column.
 * - ON DELETE/UPDATE SET NULL, ON DELETE/UPDATE SET DEFAULT and ON UPDATE CASCADE options
 *   are not used.
 * - Replication factors of referencing and referenced table are 1.
 */
static void
ErrorIfNotSupportedForeignConstraint(Relation relation, char distributionMethod,
									 Var *distributionColumn, uint32 colocationId)
{
	Relation pgConstraint = NULL;
	SysScanDesc scanDescriptor = NULL;
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	HeapTuple heapTuple = NULL;

	Oid referencedTableId = InvalidOid;
	uint32 referencedTableColocationId = INVALID_COLOCATION_ID;
	Var *referencedTablePartitionColumn = NULL;

	Datum referencingColumnsDatum;
	Datum *referencingColumnArray;
	int referencingColumnCount = 0;
	Datum referencedColumnsDatum;
	Datum *referencedColumnArray;
	int referencedColumnCount = 0;
	bool isNull = false;
	int attrIdx = 0;
	bool foreignConstraintOnPartitionColumn = false;
	bool selfReferencingTable = false;

	pgConstraint = heap_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&scanKey[0], Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ,
				relation->rd_id);
	scanDescriptor = systable_beginscan(pgConstraint, ConstraintRelidIndexId, true, NULL,
										scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		Form_pg_constraint constraintForm = (Form_pg_constraint) GETSTRUCT(heapTuple);

		if (constraintForm->contype != CONSTRAINT_FOREIGN)
		{
			heapTuple = systable_getnext(scanDescriptor);
			continue;
		}

		referencedTableId = constraintForm->confrelid;
		selfReferencingTable = relation->rd_id == referencedTableId;

		/*
		 * We do not support foreign keys for reference tables. Here we skip the second
		 * part of check if the table is a self referencing table because;
		 * - PartitionMethod only works for distributed tables and this table is not
		 * distributed yet.
		 * - Since referencing and referenced tables are same, it is OK to not checking
		 * distribution method twice.
		 */
		if (distributionMethod == DISTRIBUTE_BY_NONE ||
			(!selfReferencingTable &&
			 PartitionMethod(referencedTableId) == DISTRIBUTE_BY_NONE))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create foreign key constraint"),
							errdetail("Foreign key constraints are not allowed from or "
									  "to reference tables.")));
		}

		/*
		 * ON DELETE SET NULL and ON DELETE SET DEFAULT is not supported. Because we do
		 * not want to set partition column to NULL or default value.
		 */
		if (constraintForm->confdeltype == FKCONSTR_ACTION_SETNULL ||
			constraintForm->confdeltype == FKCONSTR_ACTION_SETDEFAULT)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create foreign key constraint"),
							errdetail("SET NULL or SET DEFAULT is not supported"
									  " in ON DELETE operation.")));
		}

		/*
		 * ON UPDATE SET NULL, ON UPDATE SET DEFAULT and UPDATE CASCADE is not supported.
		 * Because we do not want to set partition column to NULL or default value. Also
		 * cascading update operation would require re-partitioning. Updating partition
		 * column value is not allowed anyway even outside of foreign key concept.
		 */
		if (constraintForm->confupdtype == FKCONSTR_ACTION_SETNULL ||
			constraintForm->confupdtype == FKCONSTR_ACTION_SETDEFAULT ||
			constraintForm->confupdtype == FKCONSTR_ACTION_CASCADE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create foreign key constraint"),
							errdetail("SET NULL, SET DEFAULT or CASCADE is not"
									  " supported in ON UPDATE operation.")));
		}

		/*
		 * Some checks are not meaningful if foreign key references the table itself.
		 * Therefore we will skip those checks.
		 */
		if (!selfReferencingTable)
		{
			if (!IsDistributedTable(referencedTableId))
			{
				ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								errmsg("cannot create foreign key constraint"),
								errdetail("Referenced table must be a distributed "
										  "table.")));
			}

			/* to enforce foreign constraints, tables must be co-located */
			referencedTableColocationId = TableColocationId(referencedTableId);
			if (relation->rd_id != referencedTableId &&
				(colocationId == INVALID_COLOCATION_ID ||
				 colocationId != referencedTableColocationId))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("cannot create foreign key constraint"),
								errdetail("Foreign key constraint can only be created"
										  " on co-located tables.")));
			}

			/*
			 * Partition column must exist in both referencing and referenced side of the
			 * foreign key constraint. They also must be in same ordinal.
			 */
			referencedTablePartitionColumn = PartitionKey(referencedTableId);
		}
		else
		{
			/*
			 * Partition column must exist in both referencing and referenced side of the
			 * foreign key constraint. They also must be in same ordinal.
			 */
			referencedTablePartitionColumn = distributionColumn;
		}

		/*
		 * Column attributes are not available in Form_pg_constraint, therefore we need
		 * to find them in the system catalog. After finding them, we iterate over column
		 * attributes together because partition column must be at the same place in both
		 * referencing and referenced side of the foreign key constraint
		 */
		referencingColumnsDatum = SysCacheGetAttr(CONSTROID, heapTuple,
												  Anum_pg_constraint_conkey, &isNull);
		referencedColumnsDatum = SysCacheGetAttr(CONSTROID, heapTuple,
												 Anum_pg_constraint_confkey, &isNull);

		deconstruct_array(DatumGetArrayTypeP(referencingColumnsDatum), INT2OID, 2, true,
						  's', &referencingColumnArray, NULL, &referencingColumnCount);
		deconstruct_array(DatumGetArrayTypeP(referencedColumnsDatum), INT2OID, 2, true,
						  's', &referencedColumnArray, NULL, &referencedColumnCount);

		Assert(referencingColumnCount == referencedColumnCount);

		for (attrIdx = 0; attrIdx < referencingColumnCount; ++attrIdx)
		{
			AttrNumber referencingAttrNo = DatumGetInt16(referencingColumnArray[attrIdx]);
			AttrNumber referencedAttrNo = DatumGetInt16(referencedColumnArray[attrIdx]);

			if (distributionColumn->varattno == referencingAttrNo &&
				referencedTablePartitionColumn->varattno == referencedAttrNo)
			{
				foreignConstraintOnPartitionColumn = true;
			}
		}

		if (!foreignConstraintOnPartitionColumn)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create foreign key constraint"),
							errdetail("Partition column must exist both "
									  "referencing and referenced side of the "
									  "foreign constraint statement and it must "
									  "be in the same ordinal in both sides.")));
		}

		/*
		 * We do not allow to create foreign constraints if shard replication factor is
		 * greater than 1. Because in our current design, multiple replicas may cause
		 * locking problems and inconsistent shard contents.
		 */
		if (ShardReplicationFactor > 1 || (referencedTableId != relation->rd_id &&
										   !SingleReplicatedTable(referencedTableId)))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create foreign key constraint"),
							errdetail("Citus Community Edition currently supports "
									  "foreign key constraints only for "
									  "\"citus.shard_replication_factor = 1\"."),
							errhint("Please change \"citus.shard_replication_factor to "
									"1\". To learn more about using foreign keys with "
									"other replication factors, please contact us at "
									"https://citusdata.com/about/contact_us.")));
		}

		heapTuple = systable_getnext(scanDescriptor);
	}

	/* clean up scan and close system catalog */
	systable_endscan(scanDescriptor);
	heap_close(pgConstraint, AccessShareLock);
}


/*
 * LookupDistributionMethod maps the oids of citus.distribution_type enum
 * values to pg_dist_partition.partmethod values.
 *
 * The passed in oid has to belong to a value of citus.distribution_type.
 */
static char
LookupDistributionMethod(Oid distributionMethodOid)
{
	HeapTuple enumTuple = NULL;
	Form_pg_enum enumForm = NULL;
	char distributionMethod = 0;
	const char *enumLabel = NULL;

	enumTuple = SearchSysCache1(ENUMOID, ObjectIdGetDatum(distributionMethodOid));
	if (!HeapTupleIsValid(enumTuple))
	{
		ereport(ERROR, (errmsg("invalid internal value for enum: %u",
							   distributionMethodOid)));
	}

	enumForm = (Form_pg_enum) GETSTRUCT(enumTuple);
	enumLabel = NameStr(enumForm->enumlabel);

	if (strncmp(enumLabel, "append", NAMEDATALEN) == 0)
	{
		distributionMethod = DISTRIBUTE_BY_APPEND;
	}
	else if (strncmp(enumLabel, "hash", NAMEDATALEN) == 0)
	{
		distributionMethod = DISTRIBUTE_BY_HASH;
	}
	else if (strncmp(enumLabel, "range", NAMEDATALEN) == 0)
	{
		distributionMethod = DISTRIBUTE_BY_RANGE;
	}
	else
	{
		ereport(ERROR, (errmsg("invalid label for enum: %s", enumLabel)));
	}

	ReleaseSysCache(enumTuple);

	return distributionMethod;
}


/*
 *	SupportFunctionForColumn locates a support function given a column, an access method,
 *	and and id of a support function. This function returns InvalidOid if there is no
 *	support function for the operator class family of the column, but if the data type
 *	of the column has no default operator class whatsoever, this function errors out.
 */
static Oid
SupportFunctionForColumn(Var *partitionColumn, Oid accessMethodId,
						 int16 supportFunctionNumber)
{
	Oid operatorFamilyId = InvalidOid;
	Oid supportFunctionOid = InvalidOid;
	Oid operatorClassInputType = InvalidOid;
	Oid columnOid = partitionColumn->vartype;
	Oid operatorClassId = GetDefaultOpClass(columnOid, accessMethodId);

	/* currently only support using the default operator class */
	if (operatorClassId == InvalidOid)
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("data type %s has no default operator class for specified"
							   " partition method", format_type_be(columnOid)),
						errdatatype(columnOid),
						errdetail("Partition column types must have a default operator"
								  " class defined.")));
	}

	operatorFamilyId = get_opclass_family(operatorClassId);
	operatorClassInputType = get_opclass_input_type(operatorClassId);
	supportFunctionOid = get_opfamily_proc(operatorFamilyId, operatorClassInputType,
										   operatorClassInputType,
										   supportFunctionNumber);

	return supportFunctionOid;
}


/*
 * LocalTableEmpty function checks whether given local table contains any row and
 * returns false if there is any data. This function is only for local tables and
 * should not be called for distributed tables.
 */
static bool
LocalTableEmpty(Oid tableId)
{
	Oid schemaId = get_rel_namespace(tableId);
	char *schemaName = get_namespace_name(schemaId);
	char *tableName = get_rel_name(tableId);
	char *tableQualifiedName = quote_qualified_identifier(schemaName, tableName);

	int spiConnectionResult = 0;
	int spiQueryResult = 0;
	StringInfo selectExistQueryString = makeStringInfo();

	HeapTuple tuple = NULL;
	Datum hasDataDatum = 0;
	bool localTableEmpty = false;
	bool columnNull = false;
	bool readOnly = true;

	int rowId = 0;
	int attributeId = 1;

	AssertArg(!IsDistributedTable(tableId));

	spiConnectionResult = SPI_connect();
	if (spiConnectionResult != SPI_OK_CONNECT)
	{
		ereport(ERROR, (errmsg("could not connect to SPI manager")));
	}

	appendStringInfo(selectExistQueryString, SELECT_EXIST_QUERY, tableQualifiedName);

	spiQueryResult = SPI_execute(selectExistQueryString->data, readOnly, 0);
	if (spiQueryResult != SPI_OK_SELECT)
	{
		ereport(ERROR, (errmsg("execution was not successful \"%s\"",
							   selectExistQueryString->data)));
	}

	/* we expect that SELECT EXISTS query will return single value in a single row */
	Assert(SPI_processed == 1);

	tuple = SPI_tuptable->vals[rowId];
	hasDataDatum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, attributeId, &columnNull);
	localTableEmpty = !DatumGetBool(hasDataDatum);

	SPI_finish();

	return localTableEmpty;
}


/*
 * CreateTruncateTrigger creates a truncate trigger on table identified by relationId
 * and assigns citus_truncate_trigger() as handler.
 */
void
CreateTruncateTrigger(Oid relationId)
{
	CreateTrigStmt *trigger = NULL;
	StringInfo triggerName = makeStringInfo();
	bool internal = true;

	appendStringInfo(triggerName, "truncate_trigger");

	trigger = makeNode(CreateTrigStmt);
	trigger->trigname = triggerName->data;
	trigger->relation = NULL;
	trigger->funcname = SystemFuncName("citus_truncate_trigger");
	trigger->args = NIL;
	trigger->row = false;
	trigger->timing = TRIGGER_TYPE_BEFORE;
	trigger->events = TRIGGER_TYPE_TRUNCATE;
	trigger->columns = NIL;
	trigger->whenClause = NULL;
	trigger->isconstraint = false;

	CreateTrigger(trigger, NULL, relationId, InvalidOid, InvalidOid, InvalidOid,
				  internal);
}


/*
 * CreateHashDistributedTable creates a hash distributed table.
 */
static void
CreateHashDistributedTable(Oid relationId, char *distributionColumnName,
						   char *colocateWithTableName, int shardCount,
						   int replicationFactor)
{
	Relation distributedRelation = NULL;
	Relation pgDistColocation = NULL;
	uint32 colocationId = INVALID_COLOCATION_ID;
	Oid sourceRelationId = InvalidOid;
	Oid distributionColumnType = InvalidOid;
	bool requireEmpty = true;
	char relationKind = 0;

	/* get an access lock on the relation to prevent DROP TABLE and ALTER TABLE */
	distributedRelation = relation_open(relationId, AccessShareLock);

	/*
	 * Get an exclusive lock on the colocation system catalog. Therefore, we
	 * can be sure that there will no modifications on the colocation table
	 * until this transaction is committed.
	 */
	pgDistColocation = heap_open(DistColocationRelationId(), ExclusiveLock);

	/* get distribution column data type */
	distributionColumnType = ColumnType(relationId, distributionColumnName);

	if (pg_strncasecmp(colocateWithTableName, "default", NAMEDATALEN) == 0)
	{
		/* check for default colocation group */
		colocationId = ColocationId(shardCount, replicationFactor,
									distributionColumnType);
		if (colocationId == INVALID_COLOCATION_ID)
		{
			colocationId = CreateColocationGroup(shardCount, replicationFactor,
												 distributionColumnType);
		}
		else
		{
			sourceRelationId = ColocatedTableId(colocationId);
		}
	}
	else if (pg_strncasecmp(colocateWithTableName, "none", NAMEDATALEN) == 0)
	{
		colocationId = GetNextColocationId();
	}
	else
	{
		/* get colocation group of the target table */
		text *colocateWithTableNameText = cstring_to_text(colocateWithTableName);
		sourceRelationId = ResolveRelationId(colocateWithTableNameText);

		colocationId = TableColocationId(sourceRelationId);
	}

	/* relax empty table requirement for regular (non-foreign) tables */
	relationKind = get_rel_relkind(relationId);
	if (relationKind == RELKIND_RELATION)
	{
		requireEmpty = false;
	}

	/* create distributed table metadata */
	ConvertToDistributedTable(relationId, distributionColumnName, DISTRIBUTE_BY_HASH,
							  ReplicationModel, colocationId, requireEmpty);

	/* create shards */
	if (sourceRelationId != InvalidOid)
	{
		/* first run checks */
		CheckReplicationModel(sourceRelationId, relationId);
		CheckDistributionColumnType(sourceRelationId, relationId);


		CreateColocatedShards(relationId, sourceRelationId);
	}
	else
	{
		CreateShardsWithRoundRobinPolicy(relationId, shardCount, replicationFactor);
	}

	/* copy over data for regular relations */
	if (relationKind == RELKIND_RELATION)
	{
		CopyLocalDataIntoShards(relationId);
	}

	heap_close(pgDistColocation, NoLock);
	relation_close(distributedRelation, NoLock);
}


/*
 * ColumnType returns the column type of the given column.
 */
static Oid
ColumnType(Oid relationId, char *columnName)
{
	AttrNumber columnIndex = get_attnum(relationId, columnName);
	Oid columnType = get_atttype(relationId, columnIndex);

	return columnType;
}


/*
 * Check that the current replication factor setting is compatible with the
 * replication model of relationId, if valid. If InvalidOid, check that the
 * global replication model setting instead. Errors out if an invalid state
 * is detected.
 */
void
EnsureReplicationSettings(Oid relationId, char replicationModel)
{
	char *msgSuffix = "the streaming replication model";
	char *extraHint = " or setting \"citus.replication_model\" to \"statement\"";

	if (relationId != InvalidOid)
	{
		msgSuffix = "tables which use the streaming replication model";
		extraHint = "";
	}

	if (replicationModel == REPLICATION_MODEL_STREAMING && ShardReplicationFactor != 1)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("replication factors above one are incompatible with %s",
							   msgSuffix),
						errhint("Try again after reducing \"citus.shard_replication_"
								"factor\" to one%s.", extraHint)));
	}
}


/*
 * CopyLocalDataIntoShards copies data from the local table, which is hidden
 * after converting it to a distributed table, into the shards of the distributed
 * table.
 *
 * This function uses CitusCopyDestReceiver to invoke the distributed COPY logic.
 * We cannot use a regular COPY here since that cannot read from a table. Instead
 * we read from the table and pass each tuple to the CitusCopyDestReceiver which
 * opens a connection and starts a COPY for each shard placement that will have
 * data.
 *
 * We could call the planner and executor here and send the output to the
 * DestReceiver, but we are in a tricky spot here since Citus is already
 * intercepting queries on this table in the planner and executor hooks and we
 * want to read from the local table. To keep it simple, we perform a heap scan
 * directly on the table.
 *
 * Any writes on the table that are started during this operation will be handled
 * as distributed queries once the current transaction commits. SELECTs will
 * continue to read from the local table until the current transaction commits,
 * after which new SELECTs will be handled as distributed queries.
 *
 * After copying local data into the distributed table, the local data remains
 * in place and should be truncated at a later time.
 */
static void
CopyLocalDataIntoShards(Oid distributedRelationId)
{
	DestReceiver *copyDest = NULL;
	List *columnNameList = NIL;
	Relation distributedRelation = NULL;
	TupleDesc tupleDescriptor = NULL;
	bool stopOnFailure = true;

	EState *estate = NULL;
	HeapScanDesc scan = NULL;
	HeapTuple tuple = NULL;
	ExprContext *econtext = NULL;
	MemoryContext oldContext = NULL;
	TupleTableSlot *slot = NULL;
	uint64 rowsCopied = 0;

	/* take an ExclusiveLock to block all operations except SELECT */
	distributedRelation = heap_open(distributedRelationId, ExclusiveLock);

	/* get the table columns */
	tupleDescriptor = RelationGetDescr(distributedRelation);
	slot = MakeSingleTupleTableSlot(tupleDescriptor);
	columnNameList = TupleDescColumnNameList(tupleDescriptor);

	/* initialise per-tuple memory context */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = slot;

	copyDest =
		(DestReceiver *) CreateCitusCopyDestReceiver(distributedRelationId,
													 columnNameList, estate,
													 stopOnFailure);

	/* initialise state for writing to shards, we'll open connections on demand */
	copyDest->rStartup(copyDest, 0, tupleDescriptor);

	/* begin reading from local table */
	scan = heap_beginscan(distributedRelation, GetActiveSnapshot(), 0, NULL);

	oldContext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		/* materialize tuple and send it to a shard */
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
		copyDest->receiveSlot(slot, copyDest);

		/* clear tuple memory */
		ResetPerTupleExprContext(estate);

		/* make sure we roll back on cancellation */
		CHECK_FOR_INTERRUPTS();

		if (rowsCopied == 0)
		{
			ereport(NOTICE, (errmsg("Copying data from local table...")));
		}

		rowsCopied++;

		if (rowsCopied % 1000000 == 0)
		{
			ereport(DEBUG1, (errmsg("Copied %ld rows", rowsCopied)));
		}
	}

	if (rowsCopied % 1000000 != 0)
	{
		ereport(DEBUG1, (errmsg("Copied %ld rows", rowsCopied)));
	}

	MemoryContextSwitchTo(oldContext);

	/* finish reading from the local table */
	heap_endscan(scan);

	/* finish writing into the shards */
	copyDest->rShutdown(copyDest);

	/* free memory and close the relation */
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	heap_close(distributedRelation, NoLock);
}


/*
 * TupleDescColumnNameList returns a list of column names for the given tuple
 * descriptor as plain strings.
 */
static List *
TupleDescColumnNameList(TupleDesc tupleDescriptor)
{
	List *columnNameList = NIL;
	int columnIndex = 0;

	for (columnIndex = 0; columnIndex < tupleDescriptor->natts; columnIndex++)
	{
		Form_pg_attribute currentColumn = tupleDescriptor->attrs[columnIndex];
		char *columnName = NameStr(currentColumn->attname);

		if (currentColumn->attisdropped)
		{
			continue;
		}

		columnNameList = lappend(columnNameList, columnName);
	}

	return columnNameList;
}
