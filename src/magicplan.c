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
#include "nodes/nodeFuncs.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

static planner_hook_type prev_planner = NULL;

void _PG_init(void);
void _PG_fini(void);


/*
 * Context for keeping the state across the whole mutator
 * tree walking.
 */
typedef struct
{
	// Actual state
	Query *base_query;         // Original query
	Query *best_query;         // Current best rewritten query
	PlannedStmt *base_plan;    // Original plan
	PlannedStmt *best_plan;    // Best rewritten plan
	// Arguments to easily call the planner from the tree
	// traversal
	Query *current_query;      // Query to plan when calling real_plan
	const char *queryString;   // For PG >= 13, query string is needed
	int cursorOptions;         // Cursor options for planification
	ParamListInfo boundParams; // Bound params

} magicplan_mutator_context;


/*
 * An additional argument (queryString) has been added in PG13, so abstract that
 * away using macros taking the arguments from the context state
 */
#if PG_VERSION_NUM >= 130000
#define HOOK_ARGS Query *parse, const char* queryString, int cursorOptions, ParamListInfo boundParams
#define HOOK_PARAMS(prefix) prefix->current_query, prefix->queryString, prefix->cursorOptions, prefix->boundParams
#else
#define HOOK_ARGS Query *parse, int cursorOptions, ParamListInfo boundParams
#define HOOK_PARAMS(prefix) prefix->current_query, prefix->cursorOptions, prefix->boundParams
#endif

/* Hook function adresses */
static PlannedStmt *magicplan_planner(HOOK_ARGS);
static PlannedStmt *real_plan(magicplan_mutator_context *context);


/*
 * GUC variables
 * See their description in the initialization code below.
 * */
bool magicplan_enabled;
double magicplan_threshold;


Node* magicplan_mutator (Node *node, magicplan_mutator_context *context);
bool find_best_query(magicplan_mutator_context * context, Query* a);

void
_PG_init(void)
{
	/* Install hooks. */
	prev_planner = planner_hook;
	planner_hook = magicplan_planner;

	/* Setup guc */
	DefineCustomBoolVariable("magicplan.enabled",
		"Sets whether magicplan should try to optimize the plans.", NULL /* long desc */,
		&magicplan_enabled, true /* default */,
		PGC_USERSET, 0 /* flags */, NULL /* check_hook */, NULL /* assign_hook */, NULL /* show_hook */);

	DefineCustomRealVariable("magicplan.threshold",
		"Threshold required to inject the OFFSET 0 in the query.", "The total_cost of old_plan / new_plan must be over this threshold for the new plan to be used.",
		&magicplan_threshold, 1.0 /* default */, 0.0 /* min */, 10000.0 /* max, to be confirmed */,
		PGC_USERSET, 0 /* flags */, NULL /* check_hook */, NULL /* assign_hook */, NULL /* show_hook */);
}

void
_PG_fini(void)
{
	/* Uninstall hooks. */
	planner_hook = prev_planner;
}

static PlannedStmt *
real_plan(magicplan_mutator_context *context)
{
	if (prev_planner)
		return prev_planner(HOOK_PARAMS(context));
	else
		return standard_planner(HOOK_PARAMS(context));
}

/*
 * Compare the plan for a given query with the current best_plan,
 * and store the result in the mutator_context
 */
bool
find_best_query(magicplan_mutator_context * context, Query* candidate)
{
	Query * previous_context_query = context->current_query;
	PlannedStmt *candidate_plan;
	context->current_query = copyObject(candidate);
	candidate_plan = real_plan(context);
	context->current_query = previous_context_query;
	if (context->best_plan == NULL ||
	   (candidate_plan->planTree->total_cost <= context->best_plan->planTree->total_cost))
	{
		context->best_plan = candidate_plan;
		context->best_query = candidate;
		return true;
	}
	return false;
}

/*
 * Callback  for the expression_tree_mutator, query_tree_mutator functions.
 * We don't care about most nodes except for queries, which we recurse into,
 * and EXISTS(SELECT ..) nodes (Sublink of type EXISTS_SUBLINK).
 * In the latter case, we try to inject an OFFSET 0 to the query,
 * and replan the whole query to see if we have an improvement.
 * If it improves the query, store it in the context.
 */
Node*
magicplan_mutator (Node *node, magicplan_mutator_context *context)
{
	if (node == NULL)
		return NULL;

	/*
	 * expression_tree_mutator doesn't recurse into queries, we have to do it
	 * ourselves through a call to query_tree_mutator
	 */
	if (IsA(node, Query))
	{
		return (Node*) query_tree_mutator((Query*) node, magicplan_mutator, context, 0);
	}
	/*
	 * The interesting case: EXISTS (SELECT ...)
	 */
	if (IsA(node, SubLink))
	{
		SubLink * sublink = (SubLink*) node;
		if ((sublink->subLinkType == EXISTS_SUBLINK) && (sublink->subselect->type == T_Query))
		{
			Query *newquery;
			/* First, recurse into the subquery like we would normally do. This
			 * is for the case of nested EXISTS() for example.*/
			newquery = query_tree_mutator((Query*) sublink->subselect, magicplan_mutator, context, QTW_DONT_COPY_QUERY);
			/* If the query do not have a limit or offset, add an OFFSET 0
			 * clause */
			if (newquery->limitOffset == NULL)
			{
				newquery->limitOffset = (Node *) makeConst(INT8OID,
													  -1,
													  InvalidOid,
													  sizeof(int64),
													  Int64GetDatum(0),
													  false,
													  true);
				/* Plan the new query, and store the result */
				sublink->subselect = (Node *) newquery;
				find_best_query(context, context->best_query);
			}
		}
		return (Node *) sublink;
	}
	/* Default case: let expression_tree_mutator recurse into the tree */
	return expression_tree_mutator(node, magicplan_mutator, context);
}

static PlannedStmt *
magicplan_planner(HOOK_ARGS)
{
	magicplan_mutator_context mutator_context;
	Cost best_cost,
		 base_cost;
	Query * backup = copyObject(parse);
	/* Initialize the mutator_context */
	mutator_context.current_query = parse;
	#if PG_VERSION_NUM >= 130000
	mutator_context.queryString = queryString;
	#endif
	mutator_context.cursorOptions = cursorOptions;
	mutator_context.boundParams = boundParams;
	mutator_context.current_query = parse;
	mutator_context.base_query = backup;
	mutator_context.best_query = parse;
	mutator_context.best_plan = NULL;

	/* Plan the original query for future reference */
	mutator_context.current_query = backup;
	mutator_context.base_plan = real_plan(&mutator_context);

	if (!magicplan_enabled)
		return mutator_context.base_plan;

	/* Walk the query tree, and replace the EXISTS() with an EXISTS(... OFFSET
	 * 0) */
	query_tree_mutator(parse, magicplan_mutator, &mutator_context, QTW_DONT_COPY_QUERY);
	mutator_context.current_query = backup;

	/* If we found a better plan with OFFSET 0 sprinkled here and there
	 * use that if the improvement in cost crosses the magicplan_threshold
	 */
	if (mutator_context.best_plan == NULL)
	{
		return mutator_context.base_plan;
	}
	base_cost = mutator_context.base_plan->planTree->total_cost;
	best_cost = mutator_context.best_plan->planTree->total_cost;

	if ((base_cost / best_cost) <= magicplan_threshold)
	{
		elog(DEBUG1, "magicplan - kept the pristine plan, pristine=%f vs 'optimized'=%f", base_cost, best_cost);
		return mutator_context.base_plan;
	}
	else
	{
		elog(DEBUG1, "magicplan - injected an OFFSET 0, pristine=%f vs 'optimized'=%f", base_cost, best_cost);
		return mutator_context.best_plan;
	}
}
