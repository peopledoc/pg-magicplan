/*
 * Magic plan for PostgreSQL
 *
 * 2019, Pierre Ducroquet
 */
#include "postgres.h"
#include "fmgr.h"

#include "commands/explain.h"
#include "optimizer/planner.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"

PG_MODULE_MAGIC;

static planner_hook_type prev_planner = NULL;

void _PG_init(void);
void _PG_fini(void);

#if PG_VERSION_NUM >= 130000
#define HOOK_ARGS Query *parse, const char* queryString, int cursorOptions, ParamListInfo boundParams
#define HOOK_PARAMS parse, queryString, cursorOptions, boundParams
#else
#define HOOK_ARGS Query *parse, int cursorOptions, ParamListInfo boundParams
#define HOOK_PARAMS parse, cursorOptions, boundParams
#endif

static PlannedStmt *magicplan_planner(HOOK_ARGS);

static PlannedStmt *real_plan(HOOK_ARGS);

void
_PG_init(void)
{
	/* Install hooks. */
	prev_planner = planner_hook;
	planner_hook = magicplan_planner;
}

void
_PG_fini(void)
{
	/* Uninstall hooks. */
	planner_hook = prev_planner;
}

static PlannedStmt *
real_plan(HOOK_ARGS)
{
	if (prev_planner)
		return prev_planner(HOOK_PARAMS);
	else
		return standard_planner(HOOK_PARAMS);
}

static PlannedStmt *
magicplan_planner(HOOK_ARGS)
{
	Query *parse_backup;
	PlannedStmt *best_plan = NULL;
	BoolExpr *and_clause;
	ListCell   *lc;
	Expr *and_node;
	SubLink *sublink;
	PlannedStmt *new_plan;
	Node *zero_const = NULL;

	if (!parse->jointree || !parse->jointree->quals || parse->jointree->quals->type != T_BoolExpr)
		return real_plan(HOOK_PARAMS);

	// We will handle only an AND in the quals
	and_clause = (BoolExpr*) parse->jointree->quals;
	if (and_clause->boolop != AND_EXPR)
		return real_plan(HOOK_PARAMS);

	// We must check if our AND contains an EXISTS
	foreach(lc, and_clause->args)
	{
		and_node = (Expr *) lfirst(lc);
		if (and_node->type == T_SubLink)
		{
			// It's a sublink, is it an exists of a query with no offset ?
			sublink = (SubLink *) and_node;
			if ((sublink->subLinkType == EXISTS_SUBLINK) && (sublink->subselect->type == T_Query))
			{
				Query *subquery = (Query*) (sublink->subselect);
				if (subquery->limitOffset != NULL)
					continue;

				// We got one !
				parse_backup = copyObject(parse);

				if (!zero_const)
					zero_const = (Node *) makeConst(INT8OID,
					                              -1,
					                              InvalidOid,
					                              sizeof(int64),
					                              Int64GetDatum(0),
					                              false,
					                              true);

				subquery->limitOffset = zero_const;

				new_plan = real_plan(HOOK_PARAMS);
				if (best_plan != NULL)
				{
					if (new_plan->planTree->total_cost < best_plan->planTree->total_cost)
						best_plan = new_plan;
				}
				else
				{
					best_plan = new_plan;
				}
				// Reset for the next attempts
				parse = parse_backup;
			}
		}
	}

	if (best_plan != NULL)
	{
		new_plan = real_plan(HOOK_PARAMS);
		if (new_plan->planTree->total_cost < best_plan->planTree->total_cost)
		{
			elog(DEBUG1, "magicplan - kept the pristine plan");
			return new_plan;
		}
		else
		{
			elog(DEBUG1, "magicplan - injected an OFFSET 0");
			return best_plan;
		}
	}
	return real_plan(HOOK_PARAMS);
}

