# pg-magicplan

Improve planning of EXISTS in PostgreSQL subquery.
The optimizer, even in PostgreSQL 12 as of writing, does a very straight
optimization decision when executing queries that have an EXISTS sublink: it
considers that it will always be cheaper to transform the subquery into a join.
But such a decision is not always the best choice, and there is a way to tell
the optimizer not to do that, as documented in the optimizer source code (in 
function simplify_EXISTS_query), by adding an OFFSET 0 clause in the subquery.
During the development and optimization of queries in a PeopleDoc project, we
encountered a situation where such an OFFSET 0 would drastically improve 
performances for some customers, while destroying it for others, with no way
for the application to know when to add that clause.
We also realized that the PostgreSQL planner knew, in most situations, what was
indeed the best plan: when planning with an added OFFSET 0, the cost would be
lower than the pristine plan, and thus plan would indeed most of the time be a
much better plan.

The only solution we found to fix that without adding massive hacks in the
application was to add a planner hook in our database. Before executing the
query, if it matches the expected 'layout', we try to inject OFFSET 0 clauses
in every EXISTS subquery, and we execute only the best plan we found.

This solution is only a workaround. We are documenting the situation before
doing a proper bugreport along with, if possible, a way to reproduce the issue.
