/*
 * Magic plan for PostgreSQL
 *
 * 2019, Pierre Ducroquet
 */
#include "postgres.h"
#include "fmgr.h"

#include "commands/explain.h"
#include "optimizer/planner.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"

PG_MODULE_MAGIC;

static planner_hook_type prev_planner = NULL;

void _PG_init(void);
void _PG_fini(void);

static PlannedStmt *magicplan_planner(Query *parse, int cursorOptions,
				ParamListInfo boundParams);

static PlannedStmt *real_plan(Query *parse, int cursorOptions,
				ParamListInfo boundParams);

void
_PG_init(void)
{
	/* Install hookds. */
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
real_plan(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	if (prev_planner)
		return prev_planner(parse, cursorOptions, boundParams);
	else
		return standard_planner(parse, cursorOptions, boundParams);
}

static PlannedStmt *
magicplan_planner(Query *parse, int cursorOptions,
				  ParamListInfo boundParams)
{
	Query *parse_backup;
	PlannedStmt *best_plan = NULL;
	BoolExpr *and_clause;
	ListCell   *lc;
	Expr *and_node;
	SubLink *sublink;
	PlannedStmt *new_plan;
	Node *zero_const = NULL;

	if (!parse->jointree)
		goto plan_and_run;
	if (!parse->jointree->quals)
		goto plan_and_run;
	// We will handle only an AND in the quals
	if (parse->jointree->quals->type != T_BoolExpr)
		goto plan_and_run;
	and_clause = (BoolExpr*) parse->jointree->quals;
	if (and_clause->boolop != AND_EXPR)
		goto plan_and_run;

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
				elog(WARNING, "We got an exists !");
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

				elog(WARNING, "Planning with an OFFSET 0");
				new_plan = real_plan(parse, cursorOptions, boundParams);
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


plan_and_run:
	if (best_plan != NULL)
	{
		elog(WARNING, "Planning the virg^W pristine (bravo) query");
		new_plan = real_plan(parse, cursorOptions, boundParams);
		if (new_plan->planTree->total_cost < best_plan->planTree->total_cost)
		{
			elog(WARNING, "I kept the pristine one");
			return new_plan;
		}
		else
		{
			elog(WARNING, "I beat him");
			return best_plan;
		}
	}
	return real_plan(parse, cursorOptions, boundParams);
}

