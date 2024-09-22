#include "postgres.h"

#include <float.h>

#include "access/amapi.h"
#include "access/reloptions.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "ivfflat.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#if PG_VERSION_NUM < 150000
#define MarkGUCPrefixReserved(x) EmitWarningsOnPlaceholders(x)
#endif

int			ivfflat_probes;
int			ivfflat_max_probes;
bool		ivfflat_streaming;
static relopt_kind ivfflat_relopt_kind;

/*
 * Initialize index options and variables
 */
void
IvfflatInit(void)
{
	ivfflat_relopt_kind = add_reloption_kind();
	add_int_reloption(ivfflat_relopt_kind, "lists", "Number of inverted lists",
					  IVFFLAT_DEFAULT_LISTS, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, AccessExclusiveLock);

	DefineCustomIntVariable("ivfflat.probes", "Sets the number of probes",
							"Valid range is 1..lists.", &ivfflat_probes,
							IVFFLAT_DEFAULT_PROBES, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("ivfflat.max_probes", "Sets the max number of probes for iterative scans",
							NULL, &ivfflat_max_probes,
							-1, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("ivfflat.streaming", "Use streaming mode",
							 NULL, &ivfflat_streaming,
							 IVFFLAT_DEFAULT_STREAMING, PGC_USERSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("ivfflat");
}

/*
 * Get the name of index build phase
 */
static char *
ivfflatbuildphasename(int64 phasenum)
{
	switch (phasenum)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PROGRESS_IVFFLAT_PHASE_KMEANS:
			return "performing k-means";
		case PROGRESS_IVFFLAT_PHASE_ASSIGN:
			return "assigning tuples";
		case PROGRESS_IVFFLAT_PHASE_LOAD:
			return "loading tuples";
		default:
			return NULL;
	}
}

/*
 * Estimate the number of probes for iterative scans
 */
static int
EstimateProbes(PlannerInfo *root, IndexPath *path, int lists)
{
	double		selectivity = 1;
	ListCell   *lc;
	double		tuplesPerList;

	/* Cannot estimate without limit */
	/* limit_tuples includes offset */
	if (root->limit_tuples < 0)
		return 0;

	/* Get the selectivity of non-index conditions */
	foreach(lc, path->indexinfo->indrestrictinfo)
	{
		RestrictInfo *rinfo = lfirst(lc);

		/* Skip DEFAULT_INEQ_SEL since it may be a distance filter */
		if (rinfo->norm_selec >= 0 && rinfo->norm_selec <= 1 && rinfo->norm_selec != (Selectivity) DEFAULT_INEQ_SEL)
			selectivity *= rinfo->norm_selec;
	}

	tuplesPerList = path->indexinfo->tuples * selectivity / (double) lists;
	if (tuplesPerList == 0)
		return lists;

	/* No need to cap at this point */
	return root->limit_tuples / tuplesPerList;
}

/*
 * Estimate the cost of an index scan
 */
static void
ivfflatcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	GenericCosts costs;
	int			lists;
	int			probes;
	double		ratio;
	double		spc_seq_page_cost;
	Relation	index;

	/* Never use index without order */
	if (path->indexorderbys == NULL)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost = get_float8_infinity();
		*indexSelectivity = 0;
		*indexCorrelation = 0;
		*indexPages = 0;
		return;
	}

	MemSet(&costs, 0, sizeof(costs));

	index = index_open(path->indexinfo->indexoid, NoLock);
	IvfflatGetMetaPageInfo(index, &lists, NULL);
	index_close(index, NoLock);

	probes = ivfflat_probes;
	if (ivfflat_streaming)
	{
		probes = Max(probes, EstimateProbes(root, path, lists));

		if (ivfflat_max_probes != -1)
			probes = Min(probes, ivfflat_max_probes);
	}

	/* Get the ratio of lists that we need to visit */
	ratio = ((double) probes) / lists;
	if (ratio > 1.0)
		ratio = 1.0;

	/*
	 * This gives us the subset of tuples to visit. This value is passed into
	 * the generic cost estimator to determine the number of pages to visit
	 * during the index scan.
	 */
	costs.numIndexTuples = path->indexinfo->tuples * ratio;

	genericcostestimate(root, path, loop_count, &costs);

	get_tablespace_page_costs(path->indexinfo->reltablespace, NULL, &spc_seq_page_cost);

	/* Adjust cost if needed since TOAST not included in seq scan cost */
	if (costs.numIndexPages > path->indexinfo->rel->pages && ratio < 0.5)
	{
		/* Change all page cost from random to sequential */
		costs.indexTotalCost -= costs.numIndexPages * (costs.spc_random_page_cost - spc_seq_page_cost);

		/* Remove cost of extra pages */
		costs.indexTotalCost -= (costs.numIndexPages - path->indexinfo->rel->pages) * spc_seq_page_cost;
	}
	else
	{
		/* Change some page cost from random to sequential */
		costs.indexTotalCost -= 0.5 * costs.numIndexPages * (costs.spc_random_page_cost - spc_seq_page_cost);
	}

	/*
	 * If the list selectivity is lower than what is returned from the generic
	 * cost estimator, use that.
	 */
	if (ratio < costs.indexSelectivity)
		costs.indexSelectivity = ratio;

	/* Use total cost since most work happens before first tuple is returned */
	*indexStartupCost = costs.indexTotalCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

/*
 * Parse and validate the reloptions
 */
static bytea *
ivfflatoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"lists", RELOPT_TYPE_INT, offsetof(IvfflatOptions, lists)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  ivfflat_relopt_kind,
									  sizeof(IvfflatOptions),
									  tab, lengthof(tab));
}

/*
 * Validate catalog entries for the specified operator class
 */
static bool
ivfflatvalidate(Oid opclassoid)
{
	return true;
}

/*
 * Define index handler
 *
 * See https://www.postgresql.org/docs/current/index-api.html
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(ivfflathandler);
Datum
ivfflathandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = 5;
	amroutine->amoptsprocnum = 0;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
	amroutine->amcanbackward = false;	/* can change direction mid-scan */
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
#if PG_VERSION_NUM >= 170000
	amroutine->amcanbuildparallel = true;
#endif
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false; /* not used during VACUUM */
#if PG_VERSION_NUM >= 160000
	amroutine->amsummarizing = false;
#endif
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype = InvalidOid;

	/* Interface functions */
	amroutine->ambuild = ivfflatbuild;
	amroutine->ambuildempty = ivfflatbuildempty;
	amroutine->aminsert = ivfflatinsert;
#if PG_VERSION_NUM >= 170000
	amroutine->aminsertcleanup = NULL;
#endif
	amroutine->ambulkdelete = ivfflatbulkdelete;
	amroutine->amvacuumcleanup = ivfflatvacuumcleanup;
	amroutine->amcanreturn = NULL;	/* tuple not included in heapsort */
	amroutine->amcostestimate = ivfflatcostestimate;
	amroutine->amoptions = ivfflatoptions;
	amroutine->amproperty = NULL;	/* TODO AMPROP_DISTANCE_ORDERABLE */
	amroutine->ambuildphasename = ivfflatbuildphasename;
	amroutine->amvalidate = ivfflatvalidate;
#if PG_VERSION_NUM >= 140000
	amroutine->amadjustmembers = NULL;
#endif
	amroutine->ambeginscan = ivfflatbeginscan;
	amroutine->amrescan = ivfflatrescan;
	amroutine->amgettuple = ivfflatgettuple;
	amroutine->amgetbitmap = NULL;
	amroutine->amendscan = ivfflatendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;

	/* Interface functions to support parallel index scans */
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}
