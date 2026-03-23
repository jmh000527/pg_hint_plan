/*-------------------------------------------------------------------------
 *
 * pg_hint_plan.c
 *		  hinting on how to execute a query for PostgreSQL
 *
 * Copyright (c) 2012-2020, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_index.h"
#include "commands/prepare.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/scansup.h"
#include "partitioning/partbounds.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/resowner.h"

#include "catalog/pg_class.h"

#include "executor/spi.h"
#include "catalog/pg_type.h"

#include "plpgsql.h"

 /* partially copied from pg_stat_statements */
#include "normalize_query.h"

/* PostgreSQL */
#include "access/htup_details.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define BLOCK_COMMENT_START		"/*"
#define BLOCK_COMMENT_END		"*/"
#define HINT_COMMENT_KEYWORD	"+"
#define HINT_START				BLOCK_COMMENT_START HINT_COMMENT_KEYWORD
#define HINT_END				BLOCK_COMMENT_END

/* hint keywords */
#define HINT_SEQSCAN			"SeqScan"
#define HINT_INDEXSCAN			"IndexScan"
#define HINT_INDEXSCANREGEXP	"IndexScanRegexp"
#define HINT_BITMAPSCAN			"BitmapScan"
#define HINT_BITMAPSCANREGEXP	"BitmapScanRegexp"
#define HINT_TIDSCAN			"TidScan"
#define HINT_NOSEQSCAN			"NoSeqScan"
#define HINT_NOINDEXSCAN		"NoIndexScan"
#define HINT_NOBITMAPSCAN		"NoBitmapScan"
#define HINT_NOTIDSCAN			"NoTidScan"
#define HINT_INDEXONLYSCAN		"IndexOnlyScan"
#define HINT_INDEXONLYSCANREGEXP	"IndexOnlyScanRegexp"
#define HINT_NOINDEXONLYSCAN	"NoIndexOnlyScan"
#define HINT_PARALLEL			"Parallel"

#define HINT_NESTLOOP			"NestLoop"
#define HINT_MERGEJOIN			"MergeJoin"
#define HINT_HASHJOIN			"HashJoin"
#define HINT_NONESTLOOP			"NoNestLoop"
#define HINT_NOMERGEJOIN		"NoMergeJoin"
#define HINT_NOHASHJOIN			"NoHashJoin"
#define HINT_LEADING			"Leading"
#define HINT_SET				"Set"
#define HINT_ROWS				"Rows"

#define HINT_ARRAY_DEFAULT_INITSIZE 8

#define hint_ereport(str, detail) hint_parse_ereport(str, detail)
#define hint_parse_ereport(str, detail) \
	do { \
		ereport(pg_hint_plan_parse_message_level,		\
			(errmsg("pg_hint_plan: hint syntax error at or near \"%s\"", (str)), \
			 errdetail detail)); \
	} while(0)

#define skip_space(str) \
	while (isspace(*str)) \
		str++;

enum
{
	ENABLE_SEQSCAN = 0x01,
	ENABLE_INDEXSCAN = 0x02,
	ENABLE_BITMAPSCAN = 0x04,
	ENABLE_TIDSCAN = 0x08,
	ENABLE_INDEXONLYSCAN = 0x10
} SCAN_TYPE_BITS;

enum
{
	ENABLE_NESTLOOP = 0x01,
	ENABLE_MERGEJOIN = 0x02,
	ENABLE_HASHJOIN = 0x04
} JOIN_TYPE_BITS;

#define ENABLE_ALL_SCAN (ENABLE_SEQSCAN | ENABLE_INDEXSCAN | \
						 ENABLE_BITMAPSCAN | ENABLE_TIDSCAN | \
						 ENABLE_INDEXONLYSCAN)
#define ENABLE_ALL_JOIN (ENABLE_NESTLOOP | ENABLE_MERGEJOIN | ENABLE_HASHJOIN)
#define DISABLE_ALL_SCAN 0
#define DISABLE_ALL_JOIN 0

/* hint keyword of enum type*/
typedef enum HintKeyword
{
	HINT_KEYWORD_SEQSCAN,
	HINT_KEYWORD_INDEXSCAN,
	HINT_KEYWORD_INDEXSCANREGEXP,
	HINT_KEYWORD_BITMAPSCAN,
	HINT_KEYWORD_BITMAPSCANREGEXP,
	HINT_KEYWORD_TIDSCAN,
	HINT_KEYWORD_NOSEQSCAN,
	HINT_KEYWORD_NOINDEXSCAN,
	HINT_KEYWORD_NOBITMAPSCAN,
	HINT_KEYWORD_NOTIDSCAN,
	HINT_KEYWORD_INDEXONLYSCAN,
	HINT_KEYWORD_INDEXONLYSCANREGEXP,
	HINT_KEYWORD_NOINDEXONLYSCAN,

	HINT_KEYWORD_NESTLOOP,
	HINT_KEYWORD_MERGEJOIN,
	HINT_KEYWORD_HASHJOIN,
	HINT_KEYWORD_NONESTLOOP,
	HINT_KEYWORD_NOMERGEJOIN,
	HINT_KEYWORD_NOHASHJOIN,

	HINT_KEYWORD_LEADING,
	HINT_KEYWORD_SET,
	HINT_KEYWORD_ROWS,
	HINT_KEYWORD_PARALLEL,

	HINT_KEYWORD_UNRECOGNIZED
} HintKeyword;

#define SCAN_HINT_ACCEPTS_INDEX_NAMES(kw) \
	(kw == HINT_KEYWORD_INDEXSCAN ||			\
	 kw == HINT_KEYWORD_INDEXSCANREGEXP ||		\
	 kw == HINT_KEYWORD_INDEXONLYSCAN ||		\
	 kw == HINT_KEYWORD_INDEXONLYSCANREGEXP ||	\
	 kw == HINT_KEYWORD_BITMAPSCAN ||				\
	 kw == HINT_KEYWORD_BITMAPSCANREGEXP)


typedef struct Hint Hint;
typedef struct HintState HintState;

typedef Hint* (*HintCreateFunction) (const char* hint_str,
	const char* keyword,
	HintKeyword hint_keyword);
typedef void (*HintDeleteFunction) (Hint* hint);
typedef void (*HintDescFunction) (Hint* hint, StringInfo buf, bool nolf);
typedef int (*HintCmpFunction) (const Hint* a, const Hint* b);
typedef const char* (*HintParseFunction) (Hint* hint, HintState* hstate,
	Query* parse, const char* str);

/* hint types */
#define NUM_HINT_TYPE	6
typedef enum HintType
{
	HINT_TYPE_SCAN_METHOD,
	HINT_TYPE_JOIN_METHOD,
	HINT_TYPE_LEADING,
	HINT_TYPE_SET,
	HINT_TYPE_ROWS,
	HINT_TYPE_PARALLEL
} HintType;

typedef enum HintTypeBitmap
{
	HINT_BM_SCAN_METHOD = 1,
	HINT_BM_PARALLEL = 2
} HintTypeBitmap;

static const char* HintTypeName[] = {
	"scan method",
	"join method",
	"leading",
	"set",
	"rows",
	"parallel"
};

/* hint status */
typedef enum HintStatus
{
	HINT_STATE_NOTUSED = 0,		/* specified relation not used in query */
	HINT_STATE_USED,			/* hint is used */
	HINT_STATE_DUPLICATION,		/* specified hint duplication */
	HINT_STATE_ERROR			/* execute error (parse error does not include
								 * it) */
} HintStatus;

#define hint_state_enabled(hint) ((hint)->base.state == HINT_STATE_NOTUSED || \
								  (hint)->base.state == HINT_STATE_USED)

/*
 * ============================================================================
 * 全局状态与生命周期追踪变量 (Global State & Lifecycle Tracking)
 *
 * 这组静态变量共同维护了 `pg_hint_plan` 插件在处理单个后端进程 (Backend)
 * 中连续不断到来的 SQL 请求时的上下文连贯性与调试追踪能力。
 * ============================================================================
 */

 /*
  * 查询流水号 (Query Number):
  * 每当一条新的 SQL 尝试被解析（触发 post_parse_analyze_hook），`qno` 都会原子递增。
  * 它在整个后端会话中唯一标识一次特定的查询请求，是串联零散日志的“追踪探针”(Trace ID)。
  */
static unsigned int qno = 0;

/*
 * 消息防刷屏探针 (Message Query Number):
 * 用于记录上一次输出日志的是哪一个查询（qno）。配合内核报错抑制机制
 * `errhidestmt(msgqno != qno)` 使用。只有当一个查询第一次打日志时才会完整输出
 * Statement 和 Context 堆栈，后续的关联日志会被判定为同一个 `msgqno` 从而保持精简。
 */
static unsigned int msgqno = 0;

/*
 * 预格式化的查询流水追踪串:
 * 缓存类似 "[qno=0x1a]" 的字符串形式。旨在避免在海量高频日志打印时反复
 * 调用 snprintf()，是一种典型的 C 层级字符串格式化优化。
 */
static char qnostr[32];

/*
 * 当前存活的 Hint 字符串载体:
 * 这个指针是跨越了 PostgreSQL 不同执行阶段（从 Parse 飞跃到 Planner）的核心纽带。
 * 被提取出的提示文本必须分配在常驻的 `TopMemoryContext` 中并托管于此，
 * 直到被解析引擎（HintState 配置）彻底消费或随着查询结束被清理。
 */
static const char* current_hint_str = NULL;


/*
 * 标记当前查询生命周期内是否已经成功提取过 Hint 字符串。
 *
 * 【工作原理与示例】
 * pg_hint_plan 通常会在早期的 `post_parse_analyze_hook` （分析阶段）就去把 Hint 刮取出来。
 * 但是对于“扩展查询协议”或“预编译语句”（如 Java JDBC 发送的 Bind 消息），
 * 查询跳过了 Analyze 阶段，直接进入了 `planner_hook`（规划阶段）。
 * 因此，我们在规划阶段也做了兜底去捞一次 Hint。
 * 此标志位用于防止在同一个查询生命周期中，既在 Analyze 阶段刮了一次，又在 Planner 阶段重复刮取。
 * 重复刮取不仅仅是性能问题，还会导致内存上下文紊乱和不必要的错误。
 *
 * 示例：预编译语句避免重复提取
 *   场景 1: 普通查询 (psql: SELECT / *+ SeqScan(t1) * / * FROM t1;)
 *     - 走到 `post_parse_analyze_hook`: 提取 Hint，设置 `current_hint_retrieved = true`。
 *     - 走到 `planner_hook`: 看到标志为 true，直接跳过兜底提取策略，保护了现场。
 *   场景 2: 预编译执行 (JDBC: EXECUTE my_stmt;)
 *     - 绕过了 Analyze 阶段。进入 Planner。
 *     - 走到 `planner_hook`: 看到标志位仍为 false，触发兜底策略，强行从 Query 树里提取 Hint，
 *       随后将其设置为 true，保证后续即使还有子查询规划也不会死循环重复捞取。
 */
static bool current_hint_retrieved = false;

/* common data for all hints. */
struct Hint
{
	const char* hint_str;		/* must not do pfree */
	const char* keyword;		/* must not do pfree */
	HintKeyword			hint_keyword;
	HintType			type;
	HintStatus			state;
	HintDeleteFunction	delete_func;
	HintDescFunction	desc_func;
	HintCmpFunction		cmp_func;
	HintParseFunction	parse_func;
};

/* scan method hints */
typedef struct ScanMethodHint
{
	Hint			base;
	char* relname;
	List* indexnames;
	bool			regexp;
	unsigned char	enforce_mask;
} ScanMethodHint;

typedef struct ParentIndexInfo
{
	bool		indisunique;
	Oid			method;
	List* column_names;
	char* expression_str;
	Oid* indcollation;
	Oid* opclass;
	int16* indoption;
	char* indpred_str;
} ParentIndexInfo;

/* join method hints */
typedef struct JoinMethodHint
{
	Hint			base;
	int				nrels;
	int				inner_nrels;
	char** relnames;
	unsigned char	enforce_mask;
	Relids			joinrelids;
	Relids			inner_joinrelids;
} JoinMethodHint;

/* join order hints */
typedef struct OuterInnerRels
{
	char* relation;
	List* outer_inner_pair;
} OuterInnerRels;

typedef struct LeadingHint
{
	Hint			base;
	List* relations;	/* relation names specified in Leading hint */
	OuterInnerRels* outer_inner;
} LeadingHint;

/* change a run-time parameter hints */
typedef struct SetHint
{
	Hint	base;
	char* name;				/* name of variable */
	char* value;
	List* words;
} SetHint;

/* rows hints */
typedef enum RowsValueType {
	RVT_ABSOLUTE,		/* Rows(... #1000) */
	RVT_ADD,			/* Rows(... +1000) */
	RVT_SUB,			/* Rows(... -1000) */
	RVT_MULTI,			/* Rows(... *1.2) */
} RowsValueType;
typedef struct RowsHint
{
	Hint			base;
	int				nrels;
	int				inner_nrels;
	char** relnames;
	Relids			joinrelids;
	Relids			inner_joinrelids;
	char* rows_str;
	RowsValueType	value_type;
	double			rows;
} RowsHint;

/* parallel hints */
typedef struct ParallelHint
{
	Hint			base;
	char* relname;
	char* nworkers_str;	/* original string of nworkers */
	int				nworkers;		/* num of workers specified by Worker */
	bool			force_parallel;	/* force parallel scan */
} ParallelHint;

/*
 * Describes a context of hint processing.
 */
struct HintState
{
	char* hint_str;			/* original hint string */

	/* all hint */
	int				nall_hints;			/* # of valid all hints */
	int				max_all_hints;		/* # of slots for all hints */
	Hint** all_hints;			/* parsed all hints */

	/* # of each hints */
	int				num_hints[NUM_HINT_TYPE];

	/* for scan method hints */
	ScanMethodHint** scan_hints;		/* parsed scan hints */

	/* Initial values of parameters  */
	int				init_scan_mask;		/* enable_* mask */
	int				init_nworkers;		/* max_parallel_workers_per_gather */
	/* min_parallel_table_scan_size*/
	int				init_min_para_tablescan_size;
	/* min_parallel_index_scan_size*/
	int				init_min_para_indexscan_size;
	double			init_paratup_cost;	/* parallel_tuple_cost */
	double			init_parasetup_cost;/* parallel_setup_cost */

	PlannerInfo* current_root;		/* PlannerInfo for the followings */
	Index			parent_relid;		/* inherit parent of table relid */
	ScanMethodHint* parent_scan_hint;	/* scan hint for the parent */
	ParallelHint* parent_parallel_hint; /* parallel hint for the parent */
	List* parent_index_infos; /* list of parent table's index */

	JoinMethodHint** join_hints;		/* parsed join hints */
	int				init_join_mask;		/* initial value join parameter */
	List** join_hint_level;
	LeadingHint** leading_hint;		/* parsed Leading hints */
	SetHint** set_hints;			/* parsed Set hints */
	GucContext		context;			/* which GUC parameters can we set? */
	RowsHint** rows_hints;			/* parsed Rows hints */
	ParallelHint** parallel_hints;		/* parsed Parallel hints */
};

/*
 * Describes a hint parser module which is bound with particular hint keyword.
 */
typedef struct HintParser
{
	char* keyword;
	HintCreateFunction	create_func;
	HintKeyword			hint_keyword;
} HintParser;

/* Module callbacks */
void		_PG_init(void);
void		_PG_fini(void);

static void push_hint(HintState* hstate);
static void pop_hint(void);

static void pg_hint_plan_post_parse_analyze(ParseState* pstate, Query* query);
static void pg_hint_plan_ProcessUtility(PlannedStmt* pstmt,
	const char* queryString,
	ProcessUtilityContext context,
	ParamListInfo params, QueryEnvironment* queryEnv,
	DestReceiver* dest, char* completionTag);
static PlannedStmt* pg_hint_plan_planner(Query* parse, int cursorOptions,
	ParamListInfo boundParams);
static RelOptInfo* pg_hint_plan_join_search(PlannerInfo* root,
	int levels_needed,
	List* initial_rels);

/* Scan method hint callbacks */
static Hint* ScanMethodHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void ScanMethodHintDelete(ScanMethodHint* hint);
static void ScanMethodHintDesc(ScanMethodHint* hint, StringInfo buf, bool nolf);
static int ScanMethodHintCmp(const ScanMethodHint* a, const ScanMethodHint* b);
static const char* ScanMethodHintParse(ScanMethodHint* hint, HintState* hstate,
	Query* parse, const char* str);

/* Join method hint callbacks */
static Hint* JoinMethodHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void JoinMethodHintDelete(JoinMethodHint* hint);
static void JoinMethodHintDesc(JoinMethodHint* hint, StringInfo buf, bool nolf);
static int JoinMethodHintCmp(const JoinMethodHint* a, const JoinMethodHint* b);
static const char* JoinMethodHintParse(JoinMethodHint* hint, HintState* hstate,
	Query* parse, const char* str);

/* Leading hint callbacks */
static Hint* LeadingHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void LeadingHintDelete(LeadingHint* hint);
static void LeadingHintDesc(LeadingHint* hint, StringInfo buf, bool nolf);
static int LeadingHintCmp(const LeadingHint* a, const LeadingHint* b);
static const char* LeadingHintParse(LeadingHint* hint, HintState* hstate,
	Query* parse, const char* str);

/* Set hint callbacks */
static Hint* SetHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void SetHintDelete(SetHint* hint);
static void SetHintDesc(SetHint* hint, StringInfo buf, bool nolf);
static int SetHintCmp(const SetHint* a, const SetHint* b);
static const char* SetHintParse(SetHint* hint, HintState* hstate, Query* parse,
	const char* str);

/* Rows hint callbacks */
static Hint* RowsHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void RowsHintDelete(RowsHint* hint);
static void RowsHintDesc(RowsHint* hint, StringInfo buf, bool nolf);
static int RowsHintCmp(const RowsHint* a, const RowsHint* b);
static const char* RowsHintParse(RowsHint* hint, HintState* hstate,
	Query* parse, const char* str);

/* Parallel hint callbacks */
static Hint* ParallelHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword);
static void ParallelHintDelete(ParallelHint* hint);
static void ParallelHintDesc(ParallelHint* hint, StringInfo buf, bool nolf);
static int ParallelHintCmp(const ParallelHint* a, const ParallelHint* b);
static const char* ParallelHintParse(ParallelHint* hint, HintState* hstate,
	Query* parse, const char* str);

static void quote_value(StringInfo buf, const char* value);

static const char* parse_quoted_value(const char* str, char** word,
	bool truncate);

RelOptInfo* pg_hint_plan_standard_join_search(PlannerInfo* root,
	int levels_needed,
	List* initial_rels);
void pg_hint_plan_join_search_one_level(PlannerInfo* root, int level);
void pg_hint_plan_set_rel_pathlist(PlannerInfo* root, RelOptInfo* rel,
	Index rti, RangeTblEntry* rte);
static void create_plain_partial_paths(PlannerInfo* root,
	RelOptInfo* rel);
static void make_rels_by_clause_joins(PlannerInfo* root, RelOptInfo* old_rel,
	ListCell* other_rels);
static void make_rels_by_clauseless_joins(PlannerInfo* root,
	RelOptInfo* old_rel,
	ListCell* other_rels);
static bool has_join_restriction(PlannerInfo* root, RelOptInfo* rel);
static void set_plain_rel_pathlist(PlannerInfo* root, RelOptInfo* rel,
	RangeTblEntry* rte);
static void set_append_rel_pathlist(PlannerInfo* root, RelOptInfo* rel,
	Index rti, RangeTblEntry* rte);
RelOptInfo* pg_hint_plan_make_join_rel(PlannerInfo* root, RelOptInfo* rel1,
	RelOptInfo* rel2);

static void pg_hint_plan_plpgsql_stmt_beg(PLpgSQL_execstate* estate,
	PLpgSQL_stmt* stmt);
static void pg_hint_plan_plpgsql_stmt_end(PLpgSQL_execstate* estate,
	PLpgSQL_stmt* stmt);
static void plpgsql_query_erase_callback(ResourceReleasePhase phase,
	bool isCommit,
	bool isTopLevel,
	void* arg);
static int set_config_option_noerror(const char* name, const char* value,
	GucContext context, GucSource source,
	GucAction action, bool changeVal, int elevel);
static void setup_scan_method_enforcement(ScanMethodHint* scanhint,
	HintState* state);
static int set_config_int32_option(const char* name, int32 value,
	GucContext context);
static int set_config_double_option(const char* name, double value,
	GucContext context);

/* GUC variables */
static bool	pg_hint_plan_enable_hint = true;
static int debug_level = 0;
static int	pg_hint_plan_parse_message_level = INFO;
static int	pg_hint_plan_debug_message_level = LOG;
/* Default is off, to keep backward compatibility. */
static bool	pg_hint_plan_enable_hint_table = false;

static int plpgsql_recurse_level = 0;		/* PLpgSQL recursion level            */
static int recurse_level = 0;		/* recursion level incl. direct SPI calls */
static int hint_inhibit_level = 0;			/* Inhibit hinting if this is above 0 */
/* (This could not be above 1)        */
static int max_hint_nworkers = -1;		/* Maximum nworkers of Workers hints */

static const struct config_enum_entry parse_messages_level_options[] = {
	{"debug", DEBUG2, true},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"log", LOG, false},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	/*
	 * {"fatal", FATAL, true},
	 * {"panic", PANIC, true},
	 */
	{NULL, 0, false}
};

static const struct config_enum_entry parse_debug_level_options[] = {
	{"off", 0, false},
	{"on", 1, false},
	{"detailed", 2, false},
	{"verbose", 3, false},
	{"0", 0, true},
	{"1", 1, true},
	{"2", 2, true},
	{"3", 3, true},
	{"no", 0, true},
	{"yes", 1, true},
	{"false", 0, true},
	{"true", 1, true},
	{NULL, 0, false}
};

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner = NULL;
static join_search_hook_type prev_join_search = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Hold reference to currently active hint */
static HintState* current_hint_state = NULL;

/*
 * List of hint contexts.  We treat the head of the list as the Top of the
 * context stack, so current_hint_state always points the first element of this
 * list.
 */
static List* HintStateStack = NIL;

static const HintParser parsers[] = {
	{HINT_SEQSCAN, ScanMethodHintCreate, HINT_KEYWORD_SEQSCAN},
	{HINT_INDEXSCAN, ScanMethodHintCreate, HINT_KEYWORD_INDEXSCAN},
	{HINT_INDEXSCANREGEXP, ScanMethodHintCreate, HINT_KEYWORD_INDEXSCANREGEXP},
	{HINT_BITMAPSCAN, ScanMethodHintCreate, HINT_KEYWORD_BITMAPSCAN},
	{HINT_BITMAPSCANREGEXP, ScanMethodHintCreate,
	 HINT_KEYWORD_BITMAPSCANREGEXP},
	{HINT_TIDSCAN, ScanMethodHintCreate, HINT_KEYWORD_TIDSCAN},
	{HINT_NOSEQSCAN, ScanMethodHintCreate, HINT_KEYWORD_NOSEQSCAN},
	{HINT_NOINDEXSCAN, ScanMethodHintCreate, HINT_KEYWORD_NOINDEXSCAN},
	{HINT_NOBITMAPSCAN, ScanMethodHintCreate, HINT_KEYWORD_NOBITMAPSCAN},
	{HINT_NOTIDSCAN, ScanMethodHintCreate, HINT_KEYWORD_NOTIDSCAN},
	{HINT_INDEXONLYSCAN, ScanMethodHintCreate, HINT_KEYWORD_INDEXONLYSCAN},
	{HINT_INDEXONLYSCANREGEXP, ScanMethodHintCreate,
	 HINT_KEYWORD_INDEXONLYSCANREGEXP},
	{HINT_NOINDEXONLYSCAN, ScanMethodHintCreate, HINT_KEYWORD_NOINDEXONLYSCAN},

	{HINT_NESTLOOP, JoinMethodHintCreate, HINT_KEYWORD_NESTLOOP},
	{HINT_MERGEJOIN, JoinMethodHintCreate, HINT_KEYWORD_MERGEJOIN},
	{HINT_HASHJOIN, JoinMethodHintCreate, HINT_KEYWORD_HASHJOIN},
	{HINT_NONESTLOOP, JoinMethodHintCreate, HINT_KEYWORD_NONESTLOOP},
	{HINT_NOMERGEJOIN, JoinMethodHintCreate, HINT_KEYWORD_NOMERGEJOIN},
	{HINT_NOHASHJOIN, JoinMethodHintCreate, HINT_KEYWORD_NOHASHJOIN},

	{HINT_LEADING, LeadingHintCreate, HINT_KEYWORD_LEADING},
	{HINT_SET, SetHintCreate, HINT_KEYWORD_SET},
	{HINT_ROWS, RowsHintCreate, HINT_KEYWORD_ROWS},
	{HINT_PARALLEL, ParallelHintCreate, HINT_KEYWORD_PARALLEL},

	{NULL, NULL, HINT_KEYWORD_UNRECOGNIZED}
};

PLpgSQL_plugin  plugin_funcs = {
	NULL,
	NULL,
	NULL,
	pg_hint_plan_plpgsql_stmt_beg,
	pg_hint_plan_plpgsql_stmt_end,
	NULL,
	NULL,
};

/*
 * pg_hint_ExecutorEnd
 *
 * Force a hint to be retrieved when we are at the top of a PL recursion
 * level.  This can become necessary to handle hints in queries executed
 * in the extended protocol, where the executor can be executed multiple
 * times in a portal, but it could be possible to fail the hint retrieval.
 */
static void
pg_hint_ExecutorEnd(QueryDesc* queryDesc)
{
	if (plpgsql_recurse_level <= 0)
		current_hint_retrieved = false;

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Module load callbacks
 */
void
_PG_init(void)
{
	PLpgSQL_plugin** var_ptr;

	/* Define custom GUC variables. */
	DefineCustomBoolVariable("pg_hint_plan.enable_hint",
		"Force planner to use plans specified in the hint comment preceding to the query.",
		NULL,
		&pg_hint_plan_enable_hint,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomEnumVariable("pg_hint_plan.debug_print",
		"Logs results of hint parsing.",
		NULL,
		&debug_level,
		false,
		parse_debug_level_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomEnumVariable("pg_hint_plan.parse_messages",
		"Message level of parse errors.",
		NULL,
		&pg_hint_plan_parse_message_level,
		INFO,
		parse_messages_level_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomEnumVariable("pg_hint_plan.message_level",
		"Message level of debug messages.",
		NULL,
		&pg_hint_plan_debug_message_level,
		LOG,
		parse_messages_level_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable("pg_hint_plan.enable_hint_table",
		"Let pg_hint_plan look up the hint table.",
		NULL,
		&pg_hint_plan_enable_hint_table,
		false,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	EmitWarningsOnPlaceholders("pg_hint_plan");

	/* Install hooks. */
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pg_hint_plan_post_parse_analyze;
	prev_planner = planner_hook;
	planner_hook = pg_hint_plan_planner;
	prev_join_search = join_search_hook;
	join_search_hook = pg_hint_plan_join_search;
	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = pg_hint_plan_set_rel_pathlist;
	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = pg_hint_plan_ProcessUtility;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pg_hint_ExecutorEnd;

	/* setup PL/pgSQL plugin hook */
	var_ptr = (PLpgSQL_plugin**)find_rendezvous_variable("PLpgSQL_plugin");
	*var_ptr = &plugin_funcs;

	RegisterResourceReleaseCallback(plpgsql_query_erase_callback, NULL);
}

/*
 * Module unload callback
 * XXX never called
 */
void
_PG_fini(void)
{
	PLpgSQL_plugin** var_ptr;

	/* Uninstall hooks. */
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	planner_hook = prev_planner;
	join_search_hook = prev_join_search;
	set_rel_pathlist_hook = prev_set_rel_pathlist;
	ProcessUtility_hook = prev_ProcessUtility_hook;
	ExecutorEnd_hook = prev_ExecutorEnd;

	/* uninstall PL/pgSQL plugin hook */
	var_ptr = (PLpgSQL_plugin**)find_rendezvous_variable("PLpgSQL_plugin");
	*var_ptr = NULL;
}

/*
 * create and delete functions the hint object
 */

static Hint*
ScanMethodHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	ScanMethodHint* hint;

	hint = palloc(sizeof(ScanMethodHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_SCAN_METHOD;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)ScanMethodHintDelete;
	hint->base.desc_func = (HintDescFunction)ScanMethodHintDesc;
	hint->base.cmp_func = (HintCmpFunction)ScanMethodHintCmp;
	hint->base.parse_func = (HintParseFunction)ScanMethodHintParse;
	hint->relname = NULL;
	hint->indexnames = NIL;
	hint->regexp = false;
	hint->enforce_mask = 0;

	return (Hint*)hint;
}

static void
ScanMethodHintDelete(ScanMethodHint* hint)
{
	if (!hint)
		return;

	if (hint->relname)
		pfree(hint->relname);
	list_free_deep(hint->indexnames);
	pfree(hint);
}

static Hint*
JoinMethodHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	JoinMethodHint* hint;

	hint = palloc(sizeof(JoinMethodHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_JOIN_METHOD;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)JoinMethodHintDelete;
	hint->base.desc_func = (HintDescFunction)JoinMethodHintDesc;
	hint->base.cmp_func = (HintCmpFunction)JoinMethodHintCmp;
	hint->base.parse_func = (HintParseFunction)JoinMethodHintParse;
	hint->nrels = 0;
	hint->inner_nrels = 0;
	hint->relnames = NULL;
	hint->enforce_mask = 0;
	hint->joinrelids = NULL;
	hint->inner_joinrelids = NULL;

	return (Hint*)hint;
}

static void
JoinMethodHintDelete(JoinMethodHint* hint)
{
	if (!hint)
		return;

	if (hint->relnames) {
		int	i;

		for (i = 0; i < hint->nrels; i++)
			pfree(hint->relnames[i]);
		pfree(hint->relnames);
	}

	bms_free(hint->joinrelids);
	bms_free(hint->inner_joinrelids);
	pfree(hint);
}

static Hint*
LeadingHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	LeadingHint* hint;

	hint = palloc(sizeof(LeadingHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_LEADING;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)LeadingHintDelete;
	hint->base.desc_func = (HintDescFunction)LeadingHintDesc;
	hint->base.cmp_func = (HintCmpFunction)LeadingHintCmp;
	hint->base.parse_func = (HintParseFunction)LeadingHintParse;
	hint->relations = NIL;
	hint->outer_inner = NULL;

	return (Hint*)hint;
}

static void
LeadingHintDelete(LeadingHint* hint)
{
	if (!hint)
		return;

	list_free_deep(hint->relations);
	if (hint->outer_inner)
		pfree(hint->outer_inner);
	pfree(hint);
}

static Hint*
SetHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	SetHint* hint;

	hint = palloc(sizeof(SetHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_SET;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)SetHintDelete;
	hint->base.desc_func = (HintDescFunction)SetHintDesc;
	hint->base.cmp_func = (HintCmpFunction)SetHintCmp;
	hint->base.parse_func = (HintParseFunction)SetHintParse;
	hint->name = NULL;
	hint->value = NULL;
	hint->words = NIL;

	return (Hint*)hint;
}

static void
SetHintDelete(SetHint* hint)
{
	if (!hint)
		return;

	if (hint->name)
		pfree(hint->name);
	if (hint->value)
		pfree(hint->value);
	if (hint->words)
		list_free(hint->words);
	pfree(hint);
}

static Hint*
RowsHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	RowsHint* hint;

	hint = palloc(sizeof(RowsHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_ROWS;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)RowsHintDelete;
	hint->base.desc_func = (HintDescFunction)RowsHintDesc;
	hint->base.cmp_func = (HintCmpFunction)RowsHintCmp;
	hint->base.parse_func = (HintParseFunction)RowsHintParse;
	hint->nrels = 0;
	hint->inner_nrels = 0;
	hint->relnames = NULL;
	hint->joinrelids = NULL;
	hint->inner_joinrelids = NULL;
	hint->rows_str = NULL;
	hint->value_type = RVT_ABSOLUTE;
	hint->rows = 0;

	return (Hint*)hint;
}

static void
RowsHintDelete(RowsHint* hint)
{
	if (!hint)
		return;

	if (hint->relnames) {
		int	i;

		for (i = 0; i < hint->nrels; i++)
			pfree(hint->relnames[i]);
		pfree(hint->relnames);
	}

	bms_free(hint->joinrelids);
	bms_free(hint->inner_joinrelids);
	pfree(hint);
}

static Hint*
ParallelHintCreate(const char* hint_str, const char* keyword,
	HintKeyword hint_keyword)
{
	ParallelHint* hint;

	hint = palloc(sizeof(ParallelHint));
	hint->base.hint_str = hint_str;
	hint->base.keyword = keyword;
	hint->base.hint_keyword = hint_keyword;
	hint->base.type = HINT_TYPE_PARALLEL;
	hint->base.state = HINT_STATE_NOTUSED;
	hint->base.delete_func = (HintDeleteFunction)ParallelHintDelete;
	hint->base.desc_func = (HintDescFunction)ParallelHintDesc;
	hint->base.cmp_func = (HintCmpFunction)ParallelHintCmp;
	hint->base.parse_func = (HintParseFunction)ParallelHintParse;
	hint->relname = NULL;
	hint->nworkers = 0;
	hint->nworkers_str = "0";

	return (Hint*)hint;
}

static void
ParallelHintDelete(ParallelHint* hint)
{
	if (!hint)
		return;

	if (hint->relname)
		pfree(hint->relname);
	pfree(hint);
}


static HintState*
HintStateCreate(void)
{
	HintState* hstate;

	hstate = palloc(sizeof(HintState));
	hstate->hint_str = NULL;
	hstate->nall_hints = 0;
	hstate->max_all_hints = 0;
	hstate->all_hints = NULL;
	memset(hstate->num_hints, 0, sizeof(hstate->num_hints));
	hstate->scan_hints = NULL;
	hstate->init_scan_mask = 0;
	hstate->init_nworkers = 0;
	hstate->init_min_para_tablescan_size = 0;
	hstate->init_min_para_indexscan_size = 0;
	hstate->init_paratup_cost = 0;
	hstate->init_parasetup_cost = 0;
	hstate->current_root = NULL;
	hstate->parent_relid = 0;
	hstate->parent_scan_hint = NULL;
	hstate->parent_parallel_hint = NULL;
	hstate->parent_index_infos = NIL;
	hstate->join_hints = NULL;
	hstate->init_join_mask = 0;
	hstate->join_hint_level = NULL;
	hstate->leading_hint = NULL;
	hstate->context = superuser() ? PGC_SUSET : PGC_USERSET;
	hstate->set_hints = NULL;
	hstate->rows_hints = NULL;
	hstate->parallel_hints = NULL;

	return hstate;
}

static void
HintStateDelete(HintState* hstate)
{
	int			i;

	if (!hstate)
		return;

	if (hstate->hint_str)
		pfree(hstate->hint_str);

	for (i = 0; i < hstate->nall_hints; i++)
		hstate->all_hints[i]->delete_func(hstate->all_hints[i]);
	if (hstate->all_hints)
		pfree(hstate->all_hints);
	if (hstate->parent_index_infos)
		list_free(hstate->parent_index_infos);

	/*
	 * We have another few or dozen of palloced block in the struct, but don't
	 * bother completely clean up all of them since they will be cleaned-up at
	 * the end of this query.
	 */
}

/*
 * Copy given value into buf, with quoting with '"' if necessary.
 */
static void
quote_value(StringInfo buf, const char* value)
{
	bool		need_quote = false;
	const char* str;

	for (str = value; *str != '\0'; str++) {
		if (isspace(*str) || *str == '(' || *str == ')' || *str == '"') {
			need_quote = true;
			appendStringInfoCharMacro(buf, '"');
			break;
		}
	}

	for (str = value; *str != '\0'; str++) {
		if (*str == '"')
			appendStringInfoCharMacro(buf, '"');

		appendStringInfoCharMacro(buf, *str);
	}

	if (need_quote)
		appendStringInfoCharMacro(buf, '"');
}

static void
ScanMethodHintDesc(ScanMethodHint* hint, StringInfo buf, bool nolf)
{
	ListCell* l;

	appendStringInfo(buf, "%s(", hint->base.keyword);
	if (hint->relname != NULL) {
		quote_value(buf, hint->relname);
		foreach(l, hint->indexnames)
		{
			appendStringInfoCharMacro(buf, ' ');
			quote_value(buf, (char*)lfirst(l));
		}
	}
	appendStringInfoString(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

static void
JoinMethodHintDesc(JoinMethodHint* hint, StringInfo buf, bool nolf)
{
	int	i;

	appendStringInfo(buf, "%s(", hint->base.keyword);
	if (hint->relnames != NULL) {
		quote_value(buf, hint->relnames[0]);
		for (i = 1; i < hint->nrels; i++) {
			appendStringInfoCharMacro(buf, ' ');
			quote_value(buf, hint->relnames[i]);
		}
	}
	appendStringInfoString(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

static void
OuterInnerDesc(OuterInnerRels* outer_inner, StringInfo buf)
{
	if (outer_inner->relation == NULL) {
		bool		is_first;
		ListCell* l;

		is_first = true;

		appendStringInfoCharMacro(buf, '(');
		foreach(l, outer_inner->outer_inner_pair)
		{
			if (is_first)
				is_first = false;
			else
				appendStringInfoCharMacro(buf, ' ');

			OuterInnerDesc(lfirst(l), buf);
		}

		appendStringInfoCharMacro(buf, ')');
	}
	else
		quote_value(buf, outer_inner->relation);
}

static void
LeadingHintDesc(LeadingHint* hint, StringInfo buf, bool nolf)
{
	appendStringInfo(buf, "%s(", HINT_LEADING);
	if (hint->outer_inner == NULL) {
		ListCell* l;
		bool		is_first;

		is_first = true;

		foreach(l, hint->relations)
		{
			if (is_first)
				is_first = false;
			else
				appendStringInfoCharMacro(buf, ' ');

			quote_value(buf, (char*)lfirst(l));
		}
	}
	else
		OuterInnerDesc(hint->outer_inner, buf);

	appendStringInfoString(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

static void
SetHintDesc(SetHint* hint, StringInfo buf, bool nolf)
{
	bool		is_first = true;
	ListCell* l;

	appendStringInfo(buf, "%s(", HINT_SET);
	foreach(l, hint->words)
	{
		if (is_first)
			is_first = false;
		else
			appendStringInfoCharMacro(buf, ' ');

		quote_value(buf, (char*)lfirst(l));
	}
	appendStringInfo(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

static void
RowsHintDesc(RowsHint* hint, StringInfo buf, bool nolf)
{
	int	i;

	appendStringInfo(buf, "%s(", hint->base.keyword);
	if (hint->relnames != NULL) {
		quote_value(buf, hint->relnames[0]);
		for (i = 1; i < hint->nrels; i++) {
			appendStringInfoCharMacro(buf, ' ');
			quote_value(buf, hint->relnames[i]);
		}
	}
	if (hint->rows_str != NULL)
		appendStringInfo(buf, " %s", hint->rows_str);
	appendStringInfoString(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

static void
ParallelHintDesc(ParallelHint* hint, StringInfo buf, bool nolf)
{
	appendStringInfo(buf, "%s(", hint->base.keyword);
	if (hint->relname != NULL) {
		quote_value(buf, hint->relname);

		/* number of workers  */
		appendStringInfoCharMacro(buf, ' ');
		quote_value(buf, hint->nworkers_str);
		/* application mode of num of workers */
		appendStringInfoCharMacro(buf, ' ');
		appendStringInfoString(buf,
			(hint->force_parallel ? "hard" : "soft"));
	}
	appendStringInfoString(buf, ")");
	if (!nolf)
		appendStringInfoChar(buf, '\n');
}

/*
 * Append string which represents all hints in a given state to buf, with
 * preceding title with them.
 */
static void
desc_hint_in_state(HintState* hstate, StringInfo buf, const char* title,
	HintStatus state, bool nolf)
{
	int	i, nshown;

	appendStringInfo(buf, "%s:", title);
	if (!nolf)
		appendStringInfoChar(buf, '\n');

	nshown = 0;
	for (i = 0; i < hstate->nall_hints; i++) {
		if (hstate->all_hints[i]->state != state)
			continue;

		hstate->all_hints[i]->desc_func(hstate->all_hints[i], buf, nolf);
		nshown++;
	}

	if (nolf && nshown == 0)
		appendStringInfoString(buf, "(none)");
}

/*
 * 打印当前 Hint 状态的诊断信息到 PostgreSQL 服务器日志中。
 *
 * 【工作原理与示例】
 * 这两个函数负责在查询规划结束后，将该查询中所有 Hint 的最终命运（使用了、未使用、
 * 重复、解析错误）分门别类地汇总并写进服务器日志，供 DBA 排查"我的 Hint 为什么没生效"。
 *
 * - HintStateDump 对应参数 `pg_hint_plan.debug_print = on`（采用适合人阅读的纵向多行排版）。
 * - HintStateDump2 对应 `pg_hint_plan.debug_print = detailed`（采用紧凑的单行排版，
 *   附加底层查询跟踪编号 [qno]，并通过 errhidestmt/errhidecontext 屏蔽过多环境杂音）。
 *
 * 示例：一条包含了所有 4 种常见状态的 Hint 查询
 *   场景: 客户端执行带有可能存在问题的测试 SQL:
 *        / *+ SeqScan(t1) SeqScan(t1) IndexScan(t2) BadHint(t3) * /
 *        SELECT * FROM t1 JOIN t2 ON t1.id = t2.id;
 *
 *   状态分类推演:
 *     - 1. 第一个 SeqScan(t1): 被成功解析并在规划期派上用场（归类为 USED）。
 *     - 2. 第二个 SeqScan(t1): 发现是冗余冲突的定义（归类为 DUPLICATION）。
 *     - 3. IndexScan(t2): 解析正确合法，但可能因为条件或者其它限制，
 *          底层优化器依然拒绝采用该路径（归类为 NOT USED）。
 *     - 4. BadHint(t3): 词法本身不合法（无此关键字），直接解析出错（归类为 ERROR）。
 *
 *   日志落盘效果 A (调用 HintStateDump):
 *     LOG: pg_hint_plan:
 *          used hint:
 *          SeqScan(t1)
 *          not used hint:
 *          IndexScan(t2)
 *          duplication hint:
 *          SeqScan(t1)
 *          error hint:
 *          BadHint(t3)
 *
 *   日志落盘效果 B (调用 HintStateDump2):
 *     LOG: pg_hint_plan[qno=0x1]: HintStateDump: {used hints: SeqScan(t1)}, {not used hints: IndexScan(t2)}, {duplicate hints: SeqScan(t1)}, {error hints: BadHint(t3)}
 */
static void
HintStateDump(HintState* hstate)
{
	StringInfoData	buf;

	if (!hstate) {
		elog(pg_hint_plan_debug_message_level, "pg_hint_plan:\nno hint");
		return;
	}

	initStringInfo(&buf);

	appendStringInfoString(&buf, "pg_hint_plan:\n");
	desc_hint_in_state(hstate, &buf, "used hint", HINT_STATE_USED, false);
	desc_hint_in_state(hstate, &buf, "not used hint", HINT_STATE_NOTUSED, false);
	desc_hint_in_state(hstate, &buf, "duplication hint", HINT_STATE_DUPLICATION, false);
	desc_hint_in_state(hstate, &buf, "error hint", HINT_STATE_ERROR, false);

	ereport(pg_hint_plan_debug_message_level,
		(errmsg("%s", buf.data)));

	pfree(buf.data);
}

static void
HintStateDump2(HintState* hstate)
{
	StringInfoData	buf;

	if (!hstate) {
		elog(pg_hint_plan_debug_message_level,
			"pg_hint_plan%s: HintStateDump: no hint", qnostr);
		return;
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "pg_hint_plan%s: HintStateDump: ", qnostr);
	desc_hint_in_state(hstate, &buf, "{used hints", HINT_STATE_USED, true);
	desc_hint_in_state(hstate, &buf, "}, {not used hints", HINT_STATE_NOTUSED, true);
	desc_hint_in_state(hstate, &buf, "}, {duplicate hints", HINT_STATE_DUPLICATION, true);
	desc_hint_in_state(hstate, &buf, "}, {error hints", HINT_STATE_ERROR, true);
	appendStringInfoChar(&buf, '}');

	ereport(pg_hint_plan_debug_message_level,
		(errmsg("%s", buf.data),
			errhidestmt(true),
			errhidecontext(true)));

	pfree(buf.data);
}

/*
 * compare functions
 */

static int
RelnameCmp(const void* a, const void* b)
{
	const char* relnamea = *((const char**)a);
	const char* relnameb = *((const char**)b);

	return strcmp(relnamea, relnameb);
}

static int
ScanMethodHintCmp(const ScanMethodHint* a, const ScanMethodHint* b)
{
	return RelnameCmp(&a->relname, &b->relname);
}

static int
JoinMethodHintCmp(const JoinMethodHint* a, const JoinMethodHint* b)
{
	int	i;

	if (a->nrels != b->nrels)
		return a->nrels - b->nrels;

	for (i = 0; i < a->nrels; i++) {
		int	result;
		if ((result = RelnameCmp(&a->relnames[i], &b->relnames[i])) != 0)
			return result;
	}

	return 0;
}

static int
LeadingHintCmp(const LeadingHint* a, const LeadingHint* b)
{
	return 0;
}

static int
SetHintCmp(const SetHint* a, const SetHint* b)
{
	return strcmp(a->name, b->name);
}

static int
RowsHintCmp(const RowsHint* a, const RowsHint* b)
{
	int	i;

	if (a->nrels != b->nrels)
		return a->nrels - b->nrels;

	for (i = 0; i < a->nrels; i++) {
		int	result;
		if ((result = RelnameCmp(&a->relnames[i], &b->relnames[i])) != 0)
			return result;
	}

	return 0;
}

static int
ParallelHintCmp(const ParallelHint* a, const ParallelHint* b)
{
	return RelnameCmp(&a->relname, &b->relname);
}

static int
HintCmp(const void* a, const void* b)
{
	const Hint* hinta = *((const Hint**)a);
	const Hint* hintb = *((const Hint**)b);

	if (hinta->type != hintb->type)
		return hinta->type - hintb->type;
	if (hinta->state == HINT_STATE_ERROR)
		return -1;
	if (hintb->state == HINT_STATE_ERROR)
		return 1;
	return hinta->cmp_func(hinta, hintb);
}

/*
 * Returns byte offset of hint b from hint a.  If hint a was specified before
 * b, positive value is returned.
 */
static int
HintCmpWithPos(const void* a, const void* b)
{
	const Hint* hinta = *((const Hint**)a);
	const Hint* hintb = *((const Hint**)b);
	int		result;

	result = HintCmp(a, b);
	if (result == 0)
		result = hinta->hint_str - hintb->hint_str;

	return result;
}

/*
 * parse functions
 */
static const char*
parse_keyword(const char* str, StringInfo buf)
{
	skip_space(str);

	while (!isspace(*str) && *str != '(' && *str != '\0')
		appendStringInfoCharMacro(buf, *str++);

	return str;
}

static const char*
skip_parenthesis(const char* str, char parenthesis)
{
	skip_space(str);

	if (*str != parenthesis) {
		if (parenthesis == '(')
			hint_ereport(str, ("Opening parenthesis is necessary."));
		else if (parenthesis == ')')
			hint_ereport(str, ("Closing parenthesis is necessary."));

		return NULL;
	}

	str++;

	return str;
}

/*
 * Parse a token from str, and store malloc'd copy into word.  A token can be
 * quoted with '"'.  Return value is pointer to unparsed portion of original
 * string, or NULL if an error occurred.
 *
 * Parsed token is truncated within NAMEDATALEN-1 bytes, when truncate is true.
 */
static const char*
parse_quoted_value(const char* str, char** word, bool truncate)
{
	StringInfoData	buf;
	bool			in_quote;

	/* Skip leading spaces. */
	skip_space(str);

	initStringInfo(&buf);
	if (*str == '"') {
		str++;
		in_quote = true;
	}
	else
		in_quote = false;

	while (true) {
		if (in_quote) {
			/* Double quotation must be closed. */
			if (*str == '\0') {
				pfree(buf.data);
				hint_ereport(str, ("Unterminated quoted string."));
				return NULL;
			}

			/*
			 * Skip escaped double quotation.
			 *
			 * We don't allow slash-asterisk and asterisk-slash (delimiters of
			 * block comments) to be an object name, so users must specify
			 * alias for such object names.
			 *
			 * Those special names can be allowed if we care escaped slashes
			 * and asterisks, but we don't.
			 */
			if (*str == '"') {
				str++;
				if (*str != '"')
					break;
			}
		}
		else if (isspace(*str) || *str == '(' || *str == ')' || *str == '"' ||
			*str == '\0')
			break;

		appendStringInfoCharMacro(&buf, *str++);
	}

	if (buf.len == 0) {
		hint_ereport(str, ("Zero-length delimited string."));

		pfree(buf.data);

		return NULL;
	}

	/* Truncate name if it's too long */
	if (truncate)
		truncate_identifier(buf.data, strlen(buf.data), true);

	*word = buf.data;

	return str;
}

static OuterInnerRels*
OuterInnerRelsCreate(char* name, List* outer_inner_list)
{
	OuterInnerRels* outer_inner;

	outer_inner = palloc(sizeof(OuterInnerRels));
	outer_inner->relation = name;
	outer_inner->outer_inner_pair = outer_inner_list;

	return outer_inner;
}

static const char*
parse_parentheses_Leading_in(const char* str, OuterInnerRels** outer_inner)
{
	List* outer_inner_pair = NIL;

	if ((str = skip_parenthesis(str, '(')) == NULL)
		return NULL;

	skip_space(str);

	/* Store words in parentheses into outer_inner_list. */
	while (*str != ')' && *str != '\0') {
		OuterInnerRels* outer_inner_rels;

		if (*str == '(') {
			str = parse_parentheses_Leading_in(str, &outer_inner_rels);
			if (str == NULL)
				break;
		}
		else {
			char* name;

			if ((str = parse_quoted_value(str, &name, true)) == NULL)
				break;
			else
				outer_inner_rels = OuterInnerRelsCreate(name, NIL);
		}

		outer_inner_pair = lappend(outer_inner_pair, outer_inner_rels);
		skip_space(str);
	}

	if (str == NULL ||
		(str = skip_parenthesis(str, ')')) == NULL) {
		list_free(outer_inner_pair);
		return NULL;
	}

	*outer_inner = OuterInnerRelsCreate(NULL, outer_inner_pair);

	return str;
}

static const char*
parse_parentheses_Leading(const char* str, List** name_list,
	OuterInnerRels** outer_inner)
{
	char* name;
	bool	truncate = true;

	if ((str = skip_parenthesis(str, '(')) == NULL)
		return NULL;

	skip_space(str);
	if (*str == '(') {
		if ((str = parse_parentheses_Leading_in(str, outer_inner)) == NULL)
			return NULL;
	}
	else {
		/* Store words in parentheses into name_list. */
		while (*str != ')' && *str != '\0') {
			if ((str = parse_quoted_value(str, &name, truncate)) == NULL) {
				list_free(*name_list);
				return NULL;
			}

			*name_list = lappend(*name_list, name);
			skip_space(str);
		}
	}

	if ((str = skip_parenthesis(str, ')')) == NULL)
		return NULL;
	return str;
}

static const char*
parse_parentheses(const char* str, List** name_list, HintKeyword keyword)
{
	char* name;
	bool	truncate = true;

	if ((str = skip_parenthesis(str, '(')) == NULL)
		return NULL;

	skip_space(str);

	/* Store words in parentheses into name_list. */
	while (*str != ')' && *str != '\0') {
		if ((str = parse_quoted_value(str, &name, truncate)) == NULL) {
			list_free(*name_list);
			return NULL;
		}

		*name_list = lappend(*name_list, name);
		skip_space(str);

		if (keyword == HINT_KEYWORD_INDEXSCANREGEXP ||
			keyword == HINT_KEYWORD_INDEXONLYSCANREGEXP ||
			keyword == HINT_KEYWORD_BITMAPSCANREGEXP ||
			keyword == HINT_KEYWORD_SET) {
			truncate = false;
		}
	}

	if ((str = skip_parenthesis(str, ')')) == NULL)
		return NULL;
	return str;
}

static void
parse_hints(HintState* hstate, Query* parse, const char* str)
{
	StringInfoData	buf;
	char* head;

	initStringInfo(&buf);
	while (*str != '\0') {
		const HintParser* parser;

		/* in error message, we output the comment including the keyword. */
		head = (char*)str;

		/* parse only the keyword of the hint. */
		resetStringInfo(&buf);
		str = parse_keyword(str, &buf);

		for (parser = parsers; parser->keyword != NULL; parser++) {
			char* keyword = parser->keyword;
			Hint* hint;

			if (pg_strcasecmp(buf.data, keyword) != 0)
				continue;

			hint = parser->create_func(head, keyword, parser->hint_keyword);

			/* parser of each hint does parse in a parenthesis. */
			if ((str = hint->parse_func(hint, hstate, parse, str)) == NULL) {
				hint->delete_func(hint);
				pfree(buf.data);
				return;
			}

			/*
			 * Add hint information into all_hints array.  If we don't have
			 * enough space, double the array.
			 */
			if (hstate->nall_hints == 0) {
				hstate->max_all_hints = HINT_ARRAY_DEFAULT_INITSIZE;
				hstate->all_hints = (Hint**)
					palloc(sizeof(Hint*) * hstate->max_all_hints);
			}
			else if (hstate->nall_hints == hstate->max_all_hints) {
				hstate->max_all_hints *= 2;
				hstate->all_hints = (Hint**)
					repalloc(hstate->all_hints,
						sizeof(Hint*) * hstate->max_all_hints);
			}

			hstate->all_hints[hstate->nall_hints] = hint;
			hstate->nall_hints++;

			skip_space(str);

			break;
		}

		if (parser->keyword == NULL) {
			hint_ereport(head,
				("Unrecognized hint keyword \"%s\".", buf.data));
			pfree(buf.data);
			return;
		}
	}

	pfree(buf.data);
}


/*
 * 依据客户端归一化后的 SQL 文本及应用程序名称，从内部配置表中检索匹配的 Hint。
 *
 * 【工作原理与执行流程】
 * 该函数通过 PostgreSQL 的 Server Programming Interface (SPI) 发起内部查询，
 * 直接访问用户预先在此数据库中创建的 `hint_plan.hints` 视图或表。
 *
 * 匹配逻辑：
 * - 必须完全匹配 `norm_query_string`（归一化文本）。
 * - 客户端名称 `application_name` 必须精准匹配，或者表内的该字段为空（意味着全局通配）。
 * - 采用 `ORDER BY application_name DESC` 的原因是：如果有两条匹配规则（一条特定匹配，一条通配），
 *   具有明确名字的规则（比如 "JDBC"）的字典序排在空字符串前，优先级更高，会在结果集的第一行优先被抓取。
 *
 * 【防御性架构与上下文管理】
 * 1. 递归死循环防御 (`hint_inhibit_level`)：
 *    本函数内部执行提取 Hint 的 SELECT 语句时，必然会再次经过此插件的拦截 Hook。
 *    为了防止“为查 Hint 而查 Hint 导致的无限套娃”，我们在此通过增加禁止层级，
 *    临时让 Hook 失效，允许这条内部查询畅行无阻。
 * 2. 跨层内存逃逸 (`SPI_palloc`)：
 *    SPI 执行完毕退出 `SPI_finish()` 后，其附属的内部分配器会被无情销毁。
 *    如果我们试图将抓取结果原样返回，它立刻就会变成野指针。
 *    所以必须依靠 `SPI_palloc` 并配合 `strcpy`，在更高维度的内存上下文中为 Hint
 *    文本重新安家，确保其能平稳存活至外围的调用栈。
 */
static const char*
get_hints_from_table(const char* client_query, const char* client_application)
{
	const char* search_query =
		"SELECT hints "
		"  FROM hint_plan.hints "
		" WHERE norm_query_string = $1 "
		"   AND ( application_name = $2 "
		"    OR application_name = '' ) "
		" ORDER BY application_name DESC";

	/*
	 * 静态局部变量：缓存内部 SELECT 语句的执行计划树。
	 * 在此连接会话 (session) 生命周期内，只有首次调用本函数会去 Parse
	 * 和 Plan 这句固定的 SQL。之后成千上万次的 Hint Table 检索将直接利用该
	 * `plan` 重复 `execute`，避免无谓的引擎解析开销。
	 */
	static SPIPlanPtr plan = NULL;
	char* hints = NULL;

	/* $1 和 $2 参数对应的 PostgreSQL 内部数据类型，这里双双声明为 TEXTOID */
	Oid		argtypes[2] = { TEXTOID, TEXTOID };
	Datum	values[2];
	/* nulls 数组，用 ' ' (空格) 表示对应位置的实参不为空。若为 'n' 则代表实参传了 NULL */
	char 	nulls[2] = { ' ', ' ' };

	/* 用于挂载由 C 字符串转换而来的 PG 内部变长文本的数据结构指针 */
	text* qry;
	text* app;
	Oid		namespaceId;
	bool	hints_table_found = false;

	/*
	 * 防御性架构：系统目录探测与扩展状态感知
	 *
	 * 在盲目向内核发起 SPI 查询之前，必须向 PostgreSQL 的系统元数据字典 (System Catalogs)
	 * 确认 `hint_plan.hints` 这张表 (或视图) 是否真实存在。
	 *
	 * 【设计动机】
	 * 作为一个外部插件，`pg_hint_plan` 可能被记载在 `shared_preload_libraries` 中
	 * 随库启动，但用户可能并未在当前数据库执行 `CREATE EXTENSION pg_hint_plan`。
	 * 如果在这种“半激活”状态下直接去 SELECT `hint_plan.hints`，SPI 引擎会抛出一个
	 * 致命错误 (ERROR: relation does not exist)，导致当前应用的真实查询被无辜强行中断。
	 *
	 * 【底层机制】
	 * 1. LookupExplicitNamespace: 检查 `hint_plan` 这个 Schema (Namespace) 是否存在。
	 *    参数 `true` (missing_ok) 表示如果找不到，不要抛出 ERROR，而是安静地返回 InvalidOid。
	 * 2. get_relname_relid: 在该 Namespace OID 下，寻找名为 `hints` 的 Relation (表/视图)。
	 * 3. OidIsValid: 宏判断，确认上述函数返回的 OID 不是 0 (InvalidOid)。
	 *
	 * 只有双重 OID 均合法有效，我们才确信 Hint Table 就绪，能够安全地让 SPI 去查询。
	 */
	namespaceId = LookupExplicitNamespace("hint_plan", true);
	if (OidIsValid(namespaceId) &&
		OidIsValid(get_relname_relid("hints", namespaceId)))
		hints_table_found = true;

	/*
	 * 优雅降级与错误反馈机制：处理 Hint 表未安装的场景
	 *
	 * 如果上述元数据探测发现 `hint_plan.hints` 并不存在，说明当前数据库尚未执行扩展初始化。
	 * 此时绝对不能抛出致命错误（绝不能使用 ERROR 级别！），否则会因为“试图寻找优化配置”这一
	 * 附加行为，导致业务侧原本可以正常执行的 SQL 事务被强制中断并回滚（Transaction Abort），
	 * 引发极其严重的线上可用性事故。
	 *
	 * 处理策略：
	 * 1. 采用 WARNING 级别日志 (ereport): 仅向客户端或服务器日志抛出一个警告，业务语句继续执行。
	 * 2. 交互式提示 (errhint): 利用 PG 报错体系提供明确的 DBA 修复建议（运行 CREATE EXTENSION）。
	 * 3. 安全退出 (return NULL): 放弃本次“表驱动”的 Hint 查找动作，优雅回退到传统的行为模式
	 *    （即只看 SQL 文本中的注释 Hint，或者走 PostgreSQL 原生的查询优化计划）。
	 */
	if (!hints_table_found) {
		ereport(WARNING,
			(errmsg("cannot use the hint table"),
				errhint("Run \"CREATE EXTENSION pg_hint_plan\" to create the hint table.")));
		return NULL;
	}

	PG_TRY();
	{
		bool snapshot_set = false;

		/*
		 * 增加拦截屏蔽层级 (Inhibit Level)。
		 * 这是至关重要的防御设计：因为执行下方的 SPI_execute_plan() 本质上是在
		 * PostgreSQL 内部发起一次全新的 SELECT 查询。而由于 pg_hint_plan 注册了全局的
		 * post_parse_analyze_hook，这个内部 SELECT 理所当然也会被我们自己的 Hook 拦截。
		 * 如果不增加屏蔽层级，Hook 会再次尝试去获取 Hint，从而再次触发 SPI...
		 * 导致经典的“自旋引发的栈溢出 (Stack Overflow) ”。
		 */
		hint_inhibit_level++;

		/*
		 * 可见性快照管理：PushActiveSnapshot
		 *
		 * SPI 执行内部查询（查 `hint_plan.hints` 表）时，必须依赖一个有效的数据可见性快照 (Snapshot)，
		 * 以判断表里的哪些行是当前事务可见的。
		 *
		 * 【为什么需要手动推入快照？举例说明】
		 *
		 * 场景 A（正常情况 - 客户端直接发来 SELECT）：
		 * 1. 客户端发送: `SELECT * FROM my_table;`
		 * 2. PG 主引擎进入 `exec_simple_query`，此时早已在顶层调用过 `PushActiveSnapshot(...)`。
		 * 3. 此时经过我们的 hook，进入这个函数。`ActiveSnapshotSet()` 返回 TRUE。
		 * 4. 我们无需任何操作，SPI 直接复用外层的快照即可安全查询 hint 表。
		 *
		 * 场景 B（边界情况 - PL/pgSQL 函数内的隐式查询或某些 Utility 命令）：
		 * 1. 客户端发出: `DO $$ BEGIN PERFORM 1; END $$;`
		 * 2. 或是执行某个需要预先做分析的 DDL 命令，例如在建表或修改类型时。
		 * 3. 此时引擎可能还处于分析(Analyze)阶段的起步，或者是一个完全独立的内部 Worker 环境。
		 *    主流程尚未真正开始执行任何数据读取，因此引擎 **并没有** 建立或激活全局快照 (Active Snapshot 栈为空)。
		 * 4. 此时我们的 hook 被触发并开始执行 `get_hints_from_table`。
		 * 5. 如果不加以判断直接 `SPI_execute_plan`，SPI 引擎内部在试图扫描 `hint_plan.hints` 时，
		 *    会由于找不到 Snapshot 直接引发严重的内核崩溃 (Null Pointer Dereference 或 Snapshot failed ERROR)。
		 *
		 * 解决方案：
		 * 使用 `GetTransactionSnapshot()` 获取当前事务级别的默认快照，然后手动压入活动栈 (`PushActiveSnapshot`)。
		 * 执行完 SPI 后，再利用 `PopActiveSnapshot()` 将我们塞进去的快照弹出，恢复环境原样，做到“无痕访问”。
		 */
		if (!ActiveSnapshotSet()) {
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * 初始化服务器编程接口 (SPI) 运行环境。
		 *
		 * 任何想要从 C 代码中通过网络协议之外的方式内部执行 SQL 的扩展，
		 * 都必须首先调用推倒重来。
		 * `SPI_connect()` 会在当前的内存上下文中推入一个专门用于 SPI 执行的子上下文 (SPI ProcContext)。
		 * 这个上下文是一个"阅后即焚"的沙盒：后续 SPI_prepare, SPI_execute 分配的所有
		 * 临时对象都在这里面，只要调用 SPI_finish()，沙盒就会被一键清理，防止插件造成内存泄露。
		 */
		SPI_connect();

		/*
		 * 缓存执行计划树：提升高并发下的检索性能
		 * 我们只需要在当前 Session 的生命周期内第一次访问 Hint 表时，进行繁重的
		 * "Parse -> Analyze -> Rewrite -> Plan" 流程。
		 * `SPI_saveplan` 会把准备好的计划从易碎的 SPI 临时内存搬迁到能在 Session 内
		 * 长久留存的 CacheContext 中。之后的所有调用只需提供参数绑定复用即可。
		 */
		if (plan == NULL) {
			SPIPlanPtr	p;
			p = SPI_prepare(search_query, 2, argtypes);
			plan = SPI_saveplan(p);
			SPI_freeplan(p);
		}

		/*
		 * OID/Datum 内存形态装嵌：C 语言原生类型与 PG 内部引擎类型的桥接
		 *
		 * 在 PostgreSQL 内核中，函数传参和表列数据绝不会直接使用裸露的 C 语言指针（如 char*），
		 * 而是统一使用 `Datum` 类型（机器字长的通用槽位）包裹对象。
		 *
		 * 之前我们声明的参数 `argtypes` 数组设定了传给 SPI 的参数必须是 `TEXTOID` 类型。
		 * `TEXT` 在 PG 中是变长类型 (varlena)，其内存布局要求：头部必须带有长度信息 (vl_len_)，
		 * 并且不能依赖末尾的 '\0' 字符来判断结束。
		 *
		 * 转换过程：
		 * 1. `cstring_to_text`: 将普通的以 '\0' 结尾的 C 字符串 (client_query/client_application)，
		 *    重新 palloc 内存，在头部填入长度字节，并打包为合法的 PG text* 结构。
		 * 2. `PointerGetDatum`: 因为 text* 是个指针，我们将它强转存入通用的 Datum 槽位。
		 *    最终生成的 `values` 数组将直接用于填充下面 `SPI_execute_plan` 中的 $1 和 $2 位置。
		 */
		qry = cstring_to_text(client_query);
		app = cstring_to_text(client_application);
		values[0] = PointerGetDatum(qry);
		values[1] = PointerGetDatum(app);

		/*
		 * 核心执行驱动：触发现成执行计划与短路执行 (Short-Circuit)
		 *
		 * 调用 SPI_execute_plan() 真正把 SQL 推入 PostgreSQL 的 Executor (执行器) 运行。
		 *
		 * 【参数解析与防御性设计】
		 * - plan: 之前解析并缓存好的执行计划 (SPIPlanPtr)。
		 * - values, nulls: 传入我们刚刚用 Datum 封装好的 `$1` (SQL 指纹) 和 `$2` (App 名称)。
		 * - read_only (true): 强制只读沙盒模式。因为这是一个获取 Hint 的纯附加游离动作，绝对不允许
		 *   在此上下文中发生任何数据的修改（例如防止恶意/意外触发写入规则）。一旦触发写入，引擎会严厉报错拦截。
		 * - count (1): 执行器层面的硬性拉取上限，相当于底层的 "LIMIT 1"。
		 *
		 * 【性能优化：执行器短路】
		 * 我们的内部 SQL 定义了 `ORDER BY application_name DESC`，确保带有显式应用名（如 "JDBC"）
		 * 的精准匹配项必然排在纯通配符（''）之前，也就是永远在结果集的第一行。
		 * 当 `count=1` 传递给 SPI 后，执行器在向上传递 (Yield) 出第一个合格元组 (Tuple) 后，
		 * 就会立即触发“短路返回”，直接暴力切断底层的扫描 (Scan) 或排序 (Sort) 算子。
		 * 这保证了即使 hint_plan.hints 中存放了数万条规则，这句内部 SQL 消耗的 IO 和计算资源也微乎其微。
		 */
		SPI_execute_plan(plan, values, nulls, true, 1);

		if (SPI_processed > 0) {
			char* buf;

			/* 从 SPI 首行元组 (SPI_tuptable->vals[0]) 中抽取第 1 列的内容 (解析为 C String) */
			hints = SPI_getvalue(SPI_tuptable->vals[0],
				SPI_tuptable->tupdesc, 1);

			/*
			 * 内存逃逸机制与生命周期悬崖：
			 * ！！极大隐患点！！
			 * SPI_getvalue 返回的 `hints` 字符串存放于由 `SPI_connect()` 创建的专用内部 MemoryContext 中。
			 * 一旦我们执行下面的 `SPI_finish()`，整个环境会被瞬间释放回收（Garbage Collection）。
			 * 直接 return `hints` 会导致外层拿到一个立刻产生段错误的野指针 (Dangling Pointer)。
			 *
			 * `SPI_palloc` 的神奇之处在于，它会在调用 `SPI_connect` 之前的那个高阶上下文中分配内存。
			 * 我们通过 strcpy 将数据"桥接"偷渡过去，从而赋予了这段解析到的 Hint 真正的长久生命空间。
			 */
			buf = (char*)SPI_palloc(strlen(hints) + 1);
			strcpy(buf, hints);
			hints = buf;
		}

		/* 清理收尾：包括临时 TupleTable 与局部变量 */
		SPI_finish();

		/* 栈式清理我们刚刚可能 push 的事务快照 */
		if (snapshot_set)
			PopActiveSnapshot();

		/* 将屏蔽层级降回去，放行外层真正的业务查询的 Hook 拦截 */
		hint_inhibit_level--;
	}
	PG_CATCH();
	{
		/*
		 * PG 异常处理模式：
		 * PostgreSQL 使用 setjmp/longjmp 实现异常抛出。
		 * 当 SPI_execute_plan 内部发生某些抛出 ERROR 的极端故障时，代码流会瞬移到 PG_CATCH 块。
		 * 我们必须保证就算是跑路了，全局禁止位也要减回去，否则此 Session 之后所有的语句 Hint 都会莫名失效！
		 * 最后利用 PG_RE_THROW() 将原先的 ERROR 继续向上抛出交给主引擎兜底处理。
		 */
		hint_inhibit_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();

	return hints;
}

/*
 * 获取客户端提交的原始包含 Hint 的查询字符串，并在需要时（查 Hint Table）
 * 提取供指纹计算 (Jumble) 使用的深层查询语法树（剔除包裹层）。
 *
 * 【工作原理与特殊场景】
 * 这个函数的主要职责是：找到那句“真正干活、可以加 Hint”的 SQL。
 * 很多时候应用发来的语句并不是纯粹的 SELECT/UPDATE/DELETE，而是被很多
 * Utility 命令（如 EXPLAIN、DECLARE CURSOR、EXECUTE、CREATE TABLE AS）
 * 给包裹了一层甚至多层壳。如果直接把这层壳交给底下算指纹，是算不出来东西的。
 *
 * 因此，针对这些实用命令，这里必须像剥洋葱一样，把最里面的纯逻辑 Query 给"抠"出来。
 *
 * 示例推演：
 * 场景 A: 客户端执行 EXPLAIN / *+ SeqScan(t1) * / SELECT * FROM t1;
 * 动作:
 *   1. 进来的 query 是个 CMD_UTILITY。
 *   2. IsA(target_query, ExplainStmt) 命中。
 *   3. 剥掉 EXPLAIN 这层皮，强行将内部包含的纯 SELECT 查询提取为 target_query。
 *   4. 如果内层还嵌套了 Utility (例如 EXPLAIN EXECUTE)，则继续剥皮。
 *
 * 场景 B: 客户端执行 PREPARE stmt AS SELECT ... 然后 EXECUTE stmt;
 * 动作:
 *   1. 进来的 query 是个 ExecuteStmt。
 *   2. 缓存查询：利用 stmt->name 去 pg_prepared_statements 里面找。
 *   3. 如果缓存计划依然有效，则不仅把 Jumble 树替换为缓存里保存的心脏查询，
 *      还会把 p (要解析的 SQL 原文) 偷天换日指回当时 PREPARE 时的语句文本！
 */
static const char*
get_query_string(ParseState* pstate, Query* query, Query** jumblequery)
{
	/* `debug_query_string` 是 PG 引擎的一个全局变量，记录了客户端刚刚通过网络发来的最顶层 SQL 字符串。 */
	const char* p = debug_query_string;

	/*
	 * 某些内部调用（如处理 DESCRIBE 协议消息，或是执行 EXECUTE 命令时），
	 * 全局变量 debug_query_string 可能是 NULL 的。
	 * 此时我们通过 pstate->p_sourcetext 退而求其次去获取当前解析状态持有的文本记录。
	 */
	if (pstate && pstate->p_sourcetext)
		p = pstate->p_sourcetext;

	/* 如果所有的文本线索都找不到，说明不是个常规请求，直接弃疗返回 NULL（不影响引擎继续跑，只是没 Hint）。 */
	if (!p)
		return NULL;

	/* 暂时将待算指纹的 query 设为当前完整的 query 树 */
	if (jumblequery != NULL)
		*jumblequery = query;

	/* 开始“剥洋葱”：如果是一个封装类/工具类的命令 (CMD_UTILITY) */
	if (query->commandType == CMD_UTILITY) {
		Query* target_query = (Query*)query->utilityStmt;

		/*
		 * 处理 EXPLAIN 语句。
		 * 注意：EXPLAIN 后面还可以接别的 Utility 语句（比如 EXPLAIN CREATE TABLE AS），
		 * 以及 EXECUTE 也可以被其它语句包裹。这里的剥离顺序是精心安排好的。
		 */
		if (IsA(target_query, ExplainStmt)) {
			ExplainStmt* stmt = (ExplainStmt*)target_query;

			Assert(IsA(stmt->query, Query));
			target_query = (Query*)stmt->query;

			/* 如果 EXPLAIN 套着的还是个包裹语句 (比如 EXPLAIN EXECUTE stmt)，再剥一层。 */
			if (target_query->commandType == CMD_UTILITY &&
				target_query->utilityStmt != NULL)
				target_query = (Query*)target_query->utilityStmt;
		}

		/* 处理游标声明 DECLARE CURSOR */
		if (IsA(target_query, DeclareCursorStmt)) {
			DeclareCursorStmt* stmt = (DeclareCursorStmt*)target_query;
			Query* query = (Query*)stmt->query;

			/* 游标必须要包着一个 SELECT，才可能有意义。 */
			Assert(IsA(query, Query) && query->commandType == CMD_SELECT);
			target_query = query;
		}

		/* 处理 CREATE TABLE AS (CTAS) */
		if (IsA(target_query, CreateTableAsStmt)) {
			CreateTableAsStmt* stmt = (CreateTableAsStmt*)target_query;

			Assert(IsA(stmt->query, Query));
			target_query = (Query*)stmt->query;

			/* 同样处理深层嵌套 */
			if (target_query->commandType == CMD_UTILITY &&
				target_query->utilityStmt != NULL)
				target_query = (Query*)target_query->utilityStmt;
		}

		/* 处理准备语句执行 EXECUTE */
		if (IsA(target_query, ExecuteStmt)) {
			/*
			 * 遇到了 EXECUTE stmt1，本身没有实质性执行步骤，它的心脏全在预编译计划里。
			 */
			ExecuteStmt* stmt = (ExecuteStmt*)target_query;
			PreparedStatement* entry;

			/*
			 * 去内部拿这个预编译名字对应的缓存计划。
			 * 如果不存在静默忽略（比如在函数定义里包含的 EXECUTE）。反正是个死胎，等下自然会执行报错。
			 */
			entry = FetchPreparedStatement(stmt->name, false);

			if (entry && entry->plansource->is_valid) {
				/* 将文本替换为原版的 PREPARE 语句，并将心脏替换为缓存列表里的第一个结构 */
				p = entry->plansource->query_string;
				target_query = (Query*)linitial(entry->plansource->query_list);
			}
			else {
				/* 如果该计划已经被系统使其失效（例如表结构改了），那本次没法吃上旧 Hint 了 */
				p = NULL;
				target_query = NULL;
			}
		}

		/*
		 * 指纹模块 (JumbleQuery) 非常娇贵，只能吃纯血的 DML/DQL（非 Utility 语句）。
		 * 如果经过上面的层层剥皮，最终挖出来的对象依然不是一个纯 Query 甚至依然背着 utilityStmt，
		 * 那说明这个命令不可提炼 Hint，将它抛弃。
		 */
		if (target_query &&
			(!IsA(target_query, Query) ||
				target_query->utilityStmt != NULL))
			target_query = NULL;

		/* 把剥好皮的最深层查询交给外部的调用者去算指纹 */
		if (jumblequery)
			*jumblequery = target_query;
	}
	/*
	 * 防御机制：如果发现传入的解析状态 `pstate` 包含的文本不同于拿到的最外层宏观文本 `p`，
	 * 说明当前这个 `pstate` 并不是顶层查询发出的（可能是某个内部子流程衍生的）。
	 * 对于没有主动要求算指纹的调用（`!jumblequery`），直接无视这些非顶层杂音（返回 NULL）。
	 */
	else if (!jumblequery && pstate && pstate->p_sourcetext != p &&
		strcmp(pstate->p_sourcetext, p) != 0)
		p = NULL;

	return p;
}

/*
 * 从客户端提供的查询字符串的头部块注释中提取 hint。
 *
 * 【工作原理与示例】
 * 此函数会扫描 SQL 语句，寻找 hint 的起始标记（默认为 "/ *+"，注：实际代码中无空格）和结束标记（"* /"）。
 * 为了准确识别属于顶层的 hint，它对起始标记之前允许出现的字符进行了严格的白名单限制。
 *
 * 示例 A：完全合法的原生请求（成功提取）
 *   SQL: "SELECT / *+ SeqScan(t1) * / * FROM t1;"
 *   解析: 起始标记前面的字符串是 "SELECT "（大写字母加空格）。这些字符均在白名单内
 *        （白名单包括：数字、大小写字母、空格换行、下划线、逗号、左右括号）。
 *        因此能够成功定位并提取出 "SeqScan(t1)"。
 *
 * 示例 B：包含了白名单外字符的非顶层/复杂表达式（忽略 Hint）
 *   SQL: "SELECT 'test_string' / *+ SeqScan(t1) * / FROM t1;"
 *     或: "SELECT 1 + 1 / *+ SeqScan(t1) * / FROM t1;"
 *   解析: 起始标记前面只要含有类似单引号 "'" 或者加号 "+" 等不在白名单内的特殊字符，
 *        函数就会认为这可能不是一个顶层查询或正确的 hint 位置，直接返回 NULL 忽略它。
 *
 * 示例 C：嵌套块注释（防御性拦截并报错）
 *   SQL: "/ *+ SeqScan(t1) / * 我是一个内部注释 * / * /"
 *   解析: 函数在寻找结束符 "* /" 之前，如果遇到了普通块注释的开头 "/ *"，
 *        会判定为不支持的嵌套注释结构，抛出错误 "Nested block comments are not supported." 并返回 NULL。
 *
 * 注意：为了防止破坏 C 语言多行注释结构，上述示例里的注释符号都加了空格。
 */
static const char*
get_hints_from_comment(const char* p)
{
	const char* hint_head;
	char* head;
	char* tail;
	int			len;

	if (p == NULL)
		return NULL;

	/* 提取查询的头部注释：查找 hint 的起始标记（通常是 "/ *+"，连写） */
	hint_head = strstr(p, HINT_START);
	if (hint_head == NULL)
		return NULL;
	for (;p < hint_head; p++) {
		/*
		 * 允许 hint 注释前出现以下字符：
		 *   - 数字 (0-9)
		 *   - ASCII 范围内的字母 (a-z, A-Z)
		 *   - 空格、制表符和换行符
		 *   - 下划线（用于标识符）
		 *   - 逗号（用于 SELECT 子句、EXPLAIN 和 PREPARE）
		 *   - 括号（用于 EXPLAIN 和 PREPARE）
		 *
		 * 注意：这里不使用 ctype.h 中的 isalpha() 或 isalnum()，
		 * 以避免解析行为受到系统环境（locale）设置的影响。
		 */
		if (!(*p >= '0' && *p <= '9') &&
			!(*p >= 'A' && *p <= 'Z') &&
			!(*p >= 'a' && *p <= 'z') &&
			!isspace(*p) &&
			*p != '_' &&
			*p != ',' &&
			*p != '(' && *p != ')')
			return NULL;
	}

	len = strlen(HINT_START);
	head = (char*)p;
	p += len;
	skip_space(p);

	/* 查找 hint 的结束关键字。如果没找到，报错提示块注释未闭合。 */
	if ((tail = strstr(p, HINT_END)) == NULL) {
		hint_ereport(head, ("Unterminated block comment."));
		return NULL;
	}

	/* 目前不支持嵌套的块注释。如果在结束标记之前发现了新的块注释起始符，报错。 */
	if ((head = strstr(p, BLOCK_COMMENT_START)) != NULL && head < tail) {
		hint_ereport(head, ("Nested block comments are not supported."));
		return NULL;
	}

	/* 拷贝提取出的 hint 字符串，添加结束符 '\0' 后返回。 */
	len = tail - p;
	head = palloc(len + 1);
	memcpy(head, p, len);
	head[len] = '\0';
	p = head;

	return p;
}

/*
 * Parse hints that got, create hint struct from parse tree and parse hints.
 */
static HintState*
create_hintstate(Query* parse, const char* hints)
{
	const char* p;
	int			i;
	HintState* hstate;

	if (hints == NULL)
		return NULL;

	/* -1 means that no Parallel hint is specified. */
	max_hint_nworkers = -1;

	p = hints;
	hstate = HintStateCreate();
	hstate->hint_str = (char*)hints;

	/* parse each hint. */
	parse_hints(hstate, parse, p);

	/* When nothing specified a hint, we free HintState and returns NULL. */
	if (hstate->nall_hints == 0) {
		HintStateDelete(hstate);
		return NULL;
	}

	/* Sort hints in order of original position. */
	qsort(hstate->all_hints, hstate->nall_hints, sizeof(Hint*),
		HintCmpWithPos);

	/* Count number of hints per hint-type. */
	for (i = 0; i < hstate->nall_hints; i++) {
		Hint* cur_hint = hstate->all_hints[i];
		hstate->num_hints[cur_hint->type]++;
	}

	/*
	 * If an object (or a set of objects) has multiple hints of same hint-type,
	 * only the last hint is valid and others are ignored in planning.
	 * Hints except the last are marked as 'duplicated' to remember the order.
	 */
	for (i = 0; i < hstate->nall_hints - 1; i++) {
		Hint* cur_hint = hstate->all_hints[i];
		Hint* next_hint = hstate->all_hints[i + 1];

		/*
		 * Leading hint is marked as 'duplicated' in transform_join_hints.
		 */
		if (cur_hint->type == HINT_TYPE_LEADING &&
			next_hint->type == HINT_TYPE_LEADING)
			continue;

		/*
		 * Note that we need to pass addresses of hint pointers, because
		 * HintCmp is designed to sort array of Hint* by qsort.
		 */
		if (HintCmp(&cur_hint, &next_hint) == 0) {
			hint_ereport(cur_hint->hint_str,
				("Conflict %s hint.", HintTypeName[cur_hint->type]));
			cur_hint->state = HINT_STATE_DUPLICATION;
		}
	}

	/*
	 * Make sure that per-type array pointers point proper position in the
	 * array which consists of all hints.
	 */
	hstate->scan_hints = (ScanMethodHint**)hstate->all_hints;
	hstate->join_hints = (JoinMethodHint**)(hstate->scan_hints +
		hstate->num_hints[HINT_TYPE_SCAN_METHOD]);
	hstate->leading_hint = (LeadingHint**)(hstate->join_hints +
		hstate->num_hints[HINT_TYPE_JOIN_METHOD]);
	hstate->set_hints = (SetHint**)(hstate->leading_hint +
		hstate->num_hints[HINT_TYPE_LEADING]);
	hstate->rows_hints = (RowsHint**)(hstate->set_hints +
		hstate->num_hints[HINT_TYPE_SET]);
	hstate->parallel_hints = (ParallelHint**)(hstate->rows_hints +
		hstate->num_hints[HINT_TYPE_ROWS]);

	return hstate;
}

/*
 * Parse inside of parentheses of scan-method hints.
 */
static const char*
ScanMethodHintParse(ScanMethodHint* hint, HintState* hstate, Query* parse,
	const char* str)
{
	const char* keyword = hint->base.keyword;
	HintKeyword		hint_keyword = hint->base.hint_keyword;
	List* name_list = NIL;
	int				length;

	if ((str = parse_parentheses(str, &name_list, hint_keyword)) == NULL)
		return NULL;

	/* Parse relation name and index name(s) if given hint accepts. */
	length = list_length(name_list);

	/* at least twp parameters required */
	if (length < 1) {
		hint_ereport(str,
			("%s hint requires a relation.", hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	hint->relname = linitial(name_list);
	hint->indexnames = list_delete_first(name_list);

	/* check whether the hint accepts index name(s) */
	if (length > 1 && !SCAN_HINT_ACCEPTS_INDEX_NAMES(hint_keyword)) {
		hint_ereport(str,
			("%s hint accepts only one relation.",
				hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	/* Set a bit for specified hint. */
	switch (hint_keyword) {
	case HINT_KEYWORD_SEQSCAN:
		hint->enforce_mask = ENABLE_SEQSCAN;
		break;
	case HINT_KEYWORD_INDEXSCAN:
		hint->enforce_mask = ENABLE_INDEXSCAN;
		break;
	case HINT_KEYWORD_INDEXSCANREGEXP:
		hint->enforce_mask = ENABLE_INDEXSCAN;
		hint->regexp = true;
		break;
	case HINT_KEYWORD_BITMAPSCAN:
		hint->enforce_mask = ENABLE_BITMAPSCAN;
		break;
	case HINT_KEYWORD_BITMAPSCANREGEXP:
		hint->enforce_mask = ENABLE_BITMAPSCAN;
		hint->regexp = true;
		break;
	case HINT_KEYWORD_TIDSCAN:
		hint->enforce_mask = ENABLE_TIDSCAN;
		break;
	case HINT_KEYWORD_NOSEQSCAN:
		hint->enforce_mask = ENABLE_ALL_SCAN ^ ENABLE_SEQSCAN;
		break;
	case HINT_KEYWORD_NOINDEXSCAN:
		hint->enforce_mask = ENABLE_ALL_SCAN ^ ENABLE_INDEXSCAN;
		break;
	case HINT_KEYWORD_NOBITMAPSCAN:
		hint->enforce_mask = ENABLE_ALL_SCAN ^ ENABLE_BITMAPSCAN;
		break;
	case HINT_KEYWORD_NOTIDSCAN:
		hint->enforce_mask = ENABLE_ALL_SCAN ^ ENABLE_TIDSCAN;
		break;
	case HINT_KEYWORD_INDEXONLYSCAN:
		hint->enforce_mask = ENABLE_INDEXSCAN | ENABLE_INDEXONLYSCAN;
		break;
	case HINT_KEYWORD_INDEXONLYSCANREGEXP:
		hint->enforce_mask = ENABLE_INDEXSCAN | ENABLE_INDEXONLYSCAN;
		hint->regexp = true;
		break;
	case HINT_KEYWORD_NOINDEXONLYSCAN:
		hint->enforce_mask = ENABLE_ALL_SCAN ^ ENABLE_INDEXONLYSCAN;
		break;
	default:
		hint_ereport(str, ("Unrecognized hint keyword \"%s\".", keyword));
		return NULL;
		break;
	}

	return str;
}

static const char*
JoinMethodHintParse(JoinMethodHint* hint, HintState* hstate, Query* parse,
	const char* str)
{
	const char* keyword = hint->base.keyword;
	HintKeyword		hint_keyword = hint->base.hint_keyword;
	List* name_list = NIL;

	if ((str = parse_parentheses(str, &name_list, hint_keyword)) == NULL)
		return NULL;

	hint->nrels = list_length(name_list);

	if (hint->nrels > 0) {
		ListCell* l;
		int			i = 0;

		/*
		 * Transform relation names from list to array to sort them with qsort
		 * after.
		 */
		hint->relnames = palloc(sizeof(char*) * hint->nrels);
		foreach(l, name_list)
		{
			hint->relnames[i] = lfirst(l);
			i++;
		}
	}

	list_free(name_list);

	/* A join hint requires at least two relations */
	if (hint->nrels < 2) {
		hint_ereport(str,
			("%s hint requires at least two relations.",
				hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	/* Sort hints in alphabetical order of relation names. */
	qsort(hint->relnames, hint->nrels, sizeof(char*), RelnameCmp);

	switch (hint_keyword) {
	case HINT_KEYWORD_NESTLOOP:
		hint->enforce_mask = ENABLE_NESTLOOP;
		break;
	case HINT_KEYWORD_MERGEJOIN:
		hint->enforce_mask = ENABLE_MERGEJOIN;
		break;
	case HINT_KEYWORD_HASHJOIN:
		hint->enforce_mask = ENABLE_HASHJOIN;
		break;
	case HINT_KEYWORD_NONESTLOOP:
		hint->enforce_mask = ENABLE_ALL_JOIN ^ ENABLE_NESTLOOP;
		break;
	case HINT_KEYWORD_NOMERGEJOIN:
		hint->enforce_mask = ENABLE_ALL_JOIN ^ ENABLE_MERGEJOIN;
		break;
	case HINT_KEYWORD_NOHASHJOIN:
		hint->enforce_mask = ENABLE_ALL_JOIN ^ ENABLE_HASHJOIN;
		break;
	default:
		hint_ereport(str, ("Unrecognized hint keyword \"%s\".", keyword));
		return NULL;
		break;
	}

	return str;
}

static bool
OuterInnerPairCheck(OuterInnerRels* outer_inner)
{
	ListCell* l;
	if (outer_inner->outer_inner_pair == NIL) {
		if (outer_inner->relation)
			return true;
		else
			return false;
	}

	if (list_length(outer_inner->outer_inner_pair) == 2) {
		foreach(l, outer_inner->outer_inner_pair)
		{
			if (!OuterInnerPairCheck(lfirst(l)))
				return false;
		}
	}
	else
		return false;

	return true;
}

static List*
OuterInnerList(OuterInnerRels* outer_inner)
{
	List* outer_inner_list = NIL;
	ListCell* l;
	OuterInnerRels* outer_inner_rels;

	foreach(l, outer_inner->outer_inner_pair)
	{
		outer_inner_rels = (OuterInnerRels*)(lfirst(l));

		if (outer_inner_rels->relation != NULL)
			outer_inner_list = lappend(outer_inner_list,
				outer_inner_rels->relation);
		else
			outer_inner_list = list_concat(outer_inner_list,
				OuterInnerList(outer_inner_rels));
	}
	return outer_inner_list;
}

static const char*
LeadingHintParse(LeadingHint* hint, HintState* hstate, Query* parse,
	const char* str)
{
	List* name_list = NIL;
	OuterInnerRels* outer_inner = NULL;

	if ((str = parse_parentheses_Leading(str, &name_list, &outer_inner)) ==
		NULL)
		return NULL;

	if (outer_inner != NULL)
		name_list = OuterInnerList(outer_inner);

	hint->relations = name_list;
	hint->outer_inner = outer_inner;

	/* A Leading hint requires at least two relations */
	if (hint->outer_inner == NULL && list_length(hint->relations) < 2) {
		hint_ereport(hint->base.hint_str,
			("%s hint requires at least two relations.",
				HINT_LEADING));
		hint->base.state = HINT_STATE_ERROR;
	}
	else if (hint->outer_inner != NULL &&
		!OuterInnerPairCheck(hint->outer_inner)) {
		hint_ereport(hint->base.hint_str,
			("%s hint requires two sets of relations when parentheses nests.",
				HINT_LEADING));
		hint->base.state = HINT_STATE_ERROR;
	}

	return str;
}

static const char*
SetHintParse(SetHint* hint, HintState* hstate, Query* parse, const char* str)
{
	List* name_list = NIL;

	if ((str = parse_parentheses(str, &name_list, hint->base.hint_keyword))
		== NULL)
		return NULL;

	hint->words = name_list;

	/* We need both name and value to set GUC parameter. */
	if (list_length(name_list) == 2) {
		hint->name = linitial(name_list);
		hint->value = lsecond(name_list);
	}
	else {
		hint_ereport(hint->base.hint_str,
			("%s hint requires name and value of GUC parameter.",
				HINT_SET));
		hint->base.state = HINT_STATE_ERROR;
	}

	return str;
}

static const char*
RowsHintParse(RowsHint* hint, HintState* hstate, Query* parse,
	const char* str)
{
	HintKeyword		hint_keyword = hint->base.hint_keyword;
	List* name_list = NIL;
	char* rows_str;
	char* end_ptr;
	ListCell* l;
	int			i = 0;

	if ((str = parse_parentheses(str, &name_list, hint_keyword)) == NULL)
		return NULL;

	/* Last element must be rows specification */
	hint->nrels = list_length(name_list) - 1;

	if (hint->nrels < 1) {
		hint_ereport(str,
			("%s hint needs at least one relation followed by one correction term.",
				hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;

		return str;
	}


	/*
	 * Transform relation names from list to array to sort them with qsort
	 * after.
	 */
	hint->relnames = palloc(sizeof(char*) * hint->nrels);
	foreach(l, name_list)
	{
		if (hint->nrels <= i)
			break;
		hint->relnames[i] = lfirst(l);
		i++;
	}

	/* Retieve rows estimation */
	rows_str = list_nth(name_list, hint->nrels);
	hint->rows_str = rows_str;		/* store as-is for error logging */
	if (rows_str[0] == '#') {
		hint->value_type = RVT_ABSOLUTE;
		rows_str++;
	}
	else if (rows_str[0] == '+') {
		hint->value_type = RVT_ADD;
		rows_str++;
	}
	else if (rows_str[0] == '-') {
		hint->value_type = RVT_SUB;
		rows_str++;
	}
	else if (rows_str[0] == '*') {
		hint->value_type = RVT_MULTI;
		rows_str++;
	}
	else {
		hint_ereport(rows_str, ("Unrecognized rows value type notation."));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}
	hint->rows = strtod(rows_str, &end_ptr);
	if (*end_ptr) {
		hint_ereport(rows_str,
			("%s hint requires valid number as rows estimation.",
				hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	/* A join hint requires at least two relations */
	if (hint->nrels < 2) {
		hint_ereport(str,
			("%s hint requires at least two relations.",
				hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	list_free(name_list);

	/* Sort relnames in alphabetical order. */
	qsort(hint->relnames, hint->nrels, sizeof(char*), RelnameCmp);

	return str;
}

static const char*
ParallelHintParse(ParallelHint* hint, HintState* hstate, Query* parse,
	const char* str)
{
	HintKeyword		hint_keyword = hint->base.hint_keyword;
	List* name_list = NIL;
	int				length;
	char* end_ptr;
	int		nworkers;
	bool	force_parallel = false;

	if ((str = parse_parentheses(str, &name_list, hint_keyword)) == NULL)
		return NULL;

	/* Parse relation name and index name(s) if given hint accepts. */
	length = list_length(name_list);

	if (length < 2 || length > 3) {
		hint_ereport(")",
			("wrong number of arguments (%d): %s",
				length, hint->base.keyword));
		hint->base.state = HINT_STATE_ERROR;
		return str;
	}

	hint->relname = linitial(name_list);

	/* The second parameter is number of workers */
	hint->nworkers_str = list_nth(name_list, 1);
	nworkers = strtod(hint->nworkers_str, &end_ptr);
	if (*end_ptr || nworkers < 0 || nworkers > max_worker_processes) {
		if (*end_ptr)
			hint_ereport(hint->nworkers_str,
				("number of workers must be a number: %s",
					hint->base.keyword));
		else if (nworkers < 0)
			hint_ereport(hint->nworkers_str,
				("number of workers must be positive: %s",
					hint->base.keyword));
		else if (nworkers > max_worker_processes)
			hint_ereport(hint->nworkers_str,
				("number of workers = %d is larger than max_worker_processes(%d): %s",
					nworkers, max_worker_processes, hint->base.keyword));

		hint->base.state = HINT_STATE_ERROR;
	}

	hint->nworkers = nworkers;

	/* optional third parameter is specified */
	if (length == 3) {
		const char* modeparam = (const char*)list_nth(name_list, 2);
		if (pg_strcasecmp(modeparam, "hard") == 0)
			force_parallel = true;
		else if (pg_strcasecmp(modeparam, "soft") != 0) {
			hint_ereport(modeparam,
				("enforcement must be soft or hard: %s",
					hint->base.keyword));
			hint->base.state = HINT_STATE_ERROR;
		}
	}

	hint->force_parallel = force_parallel;

	if (hint->base.state != HINT_STATE_ERROR &&
		nworkers > max_hint_nworkers)
		max_hint_nworkers = nworkers;

	return str;
}

/*
 * set GUC parameter functions
 */

static int
get_current_scan_mask()
{
	int mask = 0;

	if (enable_seqscan)
		mask |= ENABLE_SEQSCAN;
	if (enable_indexscan)
		mask |= ENABLE_INDEXSCAN;
	if (enable_bitmapscan)
		mask |= ENABLE_BITMAPSCAN;
	if (enable_tidscan)
		mask |= ENABLE_TIDSCAN;
	if (enable_indexonlyscan)
		mask |= ENABLE_INDEXONLYSCAN;

	return mask;
}

static int
get_current_join_mask()
{
	int mask = 0;

	if (enable_nestloop)
		mask |= ENABLE_NESTLOOP;
	if (enable_mergejoin)
		mask |= ENABLE_MERGEJOIN;
	if (enable_hashjoin)
		mask |= ENABLE_HASHJOIN;

	return mask;
}

/*
 * Sets GUC parameters without throwing exception. Returns false if something
 * wrong.
 */
static int
set_config_option_noerror(const char* name, const char* value,
	GucContext context, GucSource source,
	GucAction action, bool changeVal, int elevel)
{
	int				result = 0;
	MemoryContext	ccxt = CurrentMemoryContext;

	PG_TRY();
	{
		result = set_config_option(name, value, context, source,
			action, changeVal, 0, false);
	}
	PG_CATCH();
	{
		ErrorData* errdata;

		/* Save error info */
		MemoryContextSwitchTo(ccxt);
		errdata = CopyErrorData();
		FlushErrorState();

		ereport(elevel,
			(errcode(errdata->sqlerrcode),
				errmsg("%s", errdata->message),
				errdata->detail ? errdetail("%s", errdata->detail) : 0,
				errdata->hint ? errhint("%s", errdata->hint) : 0));
		msgqno = qno;
		FreeErrorData(errdata);
	}
	PG_END_TRY();

	return result;
}

/*
 * Sets GUC parameter of int32 type without throwing exceptions. Returns false
 * if something wrong.
 */
static int
set_config_int32_option(const char* name, int32 value, GucContext context)
{
	char buf[16];	/* enough for int32 */

	if (snprintf(buf, 16, "%d", value) < 0) {
		ereport(pg_hint_plan_parse_message_level,
			(errmsg("Failed to convert integer to string: %d", value)));
		return false;
	}

	return
		set_config_option_noerror(name, buf, context,
			PGC_S_SESSION, GUC_ACTION_SAVE, true,
			pg_hint_plan_parse_message_level);
}

/*
 * Sets GUC parameter of double type without throwing exceptions. Returns false
 * if something wrong.
 */
static int
set_config_double_option(const char* name, double value, GucContext context)
{
	char* buf = float8out_internal(value);
	int	  result;

	result = set_config_option_noerror(name, buf, context,
		PGC_S_SESSION, GUC_ACTION_SAVE, true,
		pg_hint_plan_parse_message_level);
	pfree(buf);
	return result;
}

/* setup scan method enforcement according to given options */
static void
setup_guc_enforcement(SetHint** options, int noptions, GucContext context)
{
	int	i;

	for (i = 0; i < noptions; i++) {
		SetHint* hint = options[i];
		int			result;

		if (!hint_state_enabled(hint))
			continue;

		result = set_config_option_noerror(hint->name, hint->value, context,
			PGC_S_SESSION, GUC_ACTION_SAVE, true,
			pg_hint_plan_parse_message_level);
		if (result != 0)
			hint->base.state = HINT_STATE_USED;
		else
			hint->base.state = HINT_STATE_ERROR;
	}

	return;
}

/*
 * Setup parallel execution environment.
 *
 * If hint is not NULL, set up using it, elsewise reset to initial environment.
 */
static void
setup_parallel_plan_enforcement(ParallelHint* hint, HintState* state)
{
	if (hint) {
		hint->base.state = HINT_STATE_USED;
		set_config_int32_option("max_parallel_workers_per_gather",
			hint->nworkers, state->context);
	}
	else
		set_config_int32_option("max_parallel_workers_per_gather",
			state->init_nworkers, state->context);

	/* force means that enforce parallel as far as possible */
	if (hint && hint->force_parallel && hint->nworkers > 0) {
		set_config_double_option("parallel_tuple_cost", 0.0, state->context);
		set_config_double_option("parallel_setup_cost", 0.0, state->context);
		set_config_int32_option("min_parallel_table_scan_size", 0,
			state->context);
		set_config_int32_option("min_parallel_index_scan_size", 0,
			state->context);
	}
	else {
		set_config_double_option("parallel_tuple_cost",
			state->init_paratup_cost, state->context);
		set_config_double_option("parallel_setup_cost",
			state->init_parasetup_cost, state->context);
		set_config_int32_option("min_parallel_table_scan_size",
			state->init_min_para_tablescan_size,
			state->context);
		set_config_int32_option("min_parallel_index_scan_size",
			state->init_min_para_indexscan_size,
			state->context);
	}
}

#define SET_CONFIG_OPTION(name, type_bits) \
	set_config_option_noerror((name), \
		(mask & (type_bits)) ? "true" : "false", \
		context, PGC_S_SESSION, GUC_ACTION_SAVE, true, ERROR)


/*
 * Setup GUC environment to enforce scan methods. If scanhint is NULL, reset
 * GUCs to the saved state in state.
 */
static void
setup_scan_method_enforcement(ScanMethodHint* scanhint, HintState* state)
{
	unsigned char	enforce_mask = state->init_scan_mask;
	GucContext		context = state->context;
	unsigned char	mask;

	if (scanhint) {
		enforce_mask = scanhint->enforce_mask;
		scanhint->base.state = HINT_STATE_USED;
	}

	if (enforce_mask == ENABLE_SEQSCAN || enforce_mask == ENABLE_INDEXSCAN ||
		enforce_mask == ENABLE_BITMAPSCAN || enforce_mask == ENABLE_TIDSCAN
		|| enforce_mask == (ENABLE_INDEXSCAN | ENABLE_INDEXONLYSCAN)
		)
		mask = enforce_mask;
	else
		mask = enforce_mask & current_hint_state->init_scan_mask;

	SET_CONFIG_OPTION("enable_seqscan", ENABLE_SEQSCAN);
	SET_CONFIG_OPTION("enable_indexscan", ENABLE_INDEXSCAN);
	SET_CONFIG_OPTION("enable_bitmapscan", ENABLE_BITMAPSCAN);
	SET_CONFIG_OPTION("enable_tidscan", ENABLE_TIDSCAN);
	SET_CONFIG_OPTION("enable_indexonlyscan", ENABLE_INDEXONLYSCAN);
}

static void
set_join_config_options(unsigned char enforce_mask, GucContext context)
{
	unsigned char	mask;

	if (enforce_mask == ENABLE_NESTLOOP || enforce_mask == ENABLE_MERGEJOIN ||
		enforce_mask == ENABLE_HASHJOIN)
		mask = enforce_mask;
	else
		mask = enforce_mask & current_hint_state->init_join_mask;

	SET_CONFIG_OPTION("enable_nestloop", ENABLE_NESTLOOP);
	SET_CONFIG_OPTION("enable_mergejoin", ENABLE_MERGEJOIN);
	SET_CONFIG_OPTION("enable_hashjoin", ENABLE_HASHJOIN);

	/*
	 * Hash join may be rejected for the reason of estimated memory usage. Try
	 * getting rid of that limitation. This change on work_mem is reverted just
	 * after searching join path so no suginificant side-effects are expected.
	 */
	if (enforce_mask == ENABLE_HASHJOIN) {
		char			buf[32];

		/* See final_cost_hashjoin(). */
		if (work_mem < MAX_KILOBYTES) {
			snprintf(buf, sizeof(buf), UINT64_FORMAT, (uint64)MAX_KILOBYTES);
			set_config_option_noerror("work_mem", buf,
				context, PGC_S_SESSION, GUC_ACTION_SAVE,
				true, ERROR);
		}
	}
}

/*
 * 将解析出的 hint 状态结构压入 hint 栈中。
 *
 * 【工作原理与示例】
 * 在 PostgreSQL 中，常常会遇到“查询嵌套查询”的情况（例如：触发器、PL/pgSQL 函数内部又通过 SPI 执行了新的 SQL）。
 * 为了防止内层查询的 Hint 污染或覆盖外层查询的 Hint，代码使用了一个“栈 (Stack)”机制来管理上下文。
 * 该栈底层由 PostgreSQL 的普通链表（List）实现，链表的头部（Head）即当作栈顶。
 *
 * 示例 A：单层普通查询
 *   场景: 客户端执行简单的查询：/ *+ SeqScan(t1) * / SELECT * FROM t1;
 *   行为: 规划前调用 push_hint() 将包含 SeqScan(t1) 的状态压栈。HintStateStack 变为 [SeqScan(t1)]。
 *        全局指针 `current_hint_state` 随之指向该状态。查询规划全程使用此状态。
 *        规划结束后，调用 pop_hint() 清理，栈重新变空。
 *
 * 示例 B：触发器或函数导致的嵌套查询
 *   场景: 客户端执行插入：/ *+ IndexScan(t2) * / INSERT INTO t2 VALUES (1);
 *        该表上有一个行级触发器，触发器内部执行：/ *+ NestLoop(A B) * / UPDATE A SET val = 1 FROM B WHERE A.id = B.id;
 *   动作推演:
 *     1. 规划外层 INSERT:
 *        调用 push_hint(IndexScan状态)。栈状态变为 [IndexScan]，激活的 Hint 为 IndexScan。
 *     2. 规划内层 UPDATE 触发器:
 *        进入深一层 Planner，捕获到 NestLoop，调用 push_hint(NestLoop状态)。
 *        `lcons` 这个函数负责把新状态插到链表最前面。栈状态变为 [NestLoop, IndexScan]。
 *        此时全局指针 `current_hint_state` 被覆盖刷新为 NestLoop。
 *        内层的扫描和连接规划都会只看栈顶的 NestLoop，完全忽略外层的 IndexScan。
 *     3. 内层规划结束:
 *        触发 pop_hint()。弹出 NestLoop，栈退化回 [IndexScan]。
 *        `current_hint_state` 随之恢复为 IndexScan，从而保证外层逻辑不受任何干扰。
 */
static void
push_hint(HintState* hstate)
{
	/* lcons (List cons) 的作用是将新元素追加到链表头部，以此来模拟“压栈”。 */
	HintStateStack = lcons(hstate, HintStateStack);

	/* 刚刚压入栈顶的 hint，将成为接下来的代码执行时所“瞩目”的唯一活跃状态。 */
	current_hint_state = hstate;
}

/*
 * 从 hint 栈中弹出当前位于栈顶的 hint，并自动释放其占用的内存。
 *
 * 【工作原理与示例】
 * 它是 push_hint 的逆操作，通常在某一层级的查询规划结束，或者在出现异常（PG_CATCH）
 * 需要清理现场时予以调用。
 *
 * 示例：完成内层嵌套查询规划并“退栈”恢复外层状态
 *   场景: 紧接 push_hint 示例中的嵌套查询（触发器）。当前栈状态为:
 *        [内层NestLoop状态, 外层IndexScan状态]。
 *        全局指针 `current_hint_state` 指向内层的 NestLoop。
 *
 *   动作推演:
 *     1. 防御检查: 若栈为空（NIL），说明发生了不对称的超量 pop 调用，直接抛出 ERROR。
 *     2. 摘除栈顶: 调用 `list_delete_first` 将 NestLoop 状态从链表头部拿掉，内部栈结构退回 [IndexScan]。
 *     3. 释放内存: 调用 `HintStateDelete(NestLoop)`，以免每次规划函数结束都留下内存垃圾。
 *     4. 恢复关联: 判断如果此时链表是空的，说明所有查询全执行完了，全局状态置为 NULL；
 *        否则（当前示例中栈内还有 IndexScan），更新 `current_hint_state` 重新指向栈顶的 IndexScan。
 *        由此实现无缝闭环，外层的 INSERT 规划流程完全感知不到刚才发生了向内层 UPDATE 的切换。
 */
static void
pop_hint(void)
{
	/* 必须保证栈内有东西可弹出，否则是逻辑错误。 */
	if (HintStateStack == NIL)
		elog(ERROR, "hint stack is empty");

	/*
	 * 将处于链表头部（栈顶）的元素丢弃，并清理对应的内存结构。
	 * 随后将当前生效的 hint 状态指针指向新的栈顶元素（如果栈空了就指向 NULL）。
	 */
	HintStateStack = list_delete_first(HintStateStack);
	HintStateDelete(current_hint_state);
	if (HintStateStack == NIL)
		current_hint_state = NULL;
	else
		current_hint_state = (HintState*)lfirst(list_head(HintStateStack));
}

/*
 * 核心的 Hint 获取总枢纽，负责从 "SQL 注释" 或 "Hint 自动绑定表 (Hint Table)" 中提取 Hint。
 *
 * 【工作原理与示例】
 * 此函数是真正干活的地方。每次解析到一个新的查询时，它会依次做几件事：
 * 1. 拦截与去重：如果系统处于被禁用状态，或是该条查询在之前已被提取过了，直接短路返回。
 * 2. Hint Table 优先机制：当启用了 `pg_hint_plan.enable_hint_table = on` 时，系统会
 *    对用户发来的原始 SQL 进行"参数化归一化"（例如将 `WHERE id = 1` 变成 `WHERE id = ?`），
 *    然后去数据库中的配置表 `hint_plan.hints` 里查找有没有预先绑定的 Hint。如果有，
 *    直接使用表里的 Hint 并返回，跳过对 SQL 注释内 Hint 的解析（即表配置优先级高于代码硬注释）。
 * 3. 降级到 SQL 注释提取：如果表功能没开，或者开启了但在表里没有匹配到对应的语句，
 *    则会放行到下方调用 `get_hints_from_comment`，走传统的从 SQL 开头注释中提取的逻辑。
 *
 * 示例 A：从 Hint Table 提取（无感干涉应用）
 *   场景: 业务系统（如 ORM 框架）固定发出 `SELECT * FROM users WHERE age > 18;`，
 *        代码没法改，但我们通过向 `hint_plan.hints` 表中插入了一条规则，
 *        将预编译后的模板 `SELECT * FROM users WHERE age > ?;` 绑定了 `SeqScan(users)`。
 *
 *   动作推演:
 *     1. 截获 SQL 文本。
 *     2. 发现开启了 Hint Table 功能。
 *     3. 调用 JumbleQuery 剥离掉常量 18，并在 generate_normalized_query 里将常数位替换为问号，
 *        生成 normalized_query = "select * from users where age > ?;"（会转小写并去格式化）。
 *     4. 内存切换后，调用 `get_hints_from_table` 使用这段去敏的归一化字符串及外部的
 *        application_name (客户端名，例如 "JDBC") 去表中进行查找匹配。
 *     5. 成功找到匹配规则并返回 "SeqScan(users)" 存入 `current_hint_str`。随后触发 return 结案。
 *
 * 示例 B：常规的注释提取（或者查表降级）
 *   场景: 客户端主动发出 `/ *+ IndexScan(t1) * / SELECT * FROM t1;`
 *
 *   动作推演:
 *     1. 截获 SQL 文本。
 *     2. 没有开启 Hint Table，或者开启了但去表里查了一圈发现啥也没有（`current_hint_str` 为 NULL）。
 *     3. 程序执行流离开 if (pg_hint_plan_enable_hint_table) 大块，来到下半部分的判定。
 *     4. 由于 `query_str` 文本存在，将其喂给 `get_hints_from_comment` 函数。
 *     5. 成功抓出代码里人肉手写的 "IndexScan(t1)" 取代空指针，从而完成 Hint 的配置。
 */
static void
get_current_hint_string(ParseState* pstate, Query* query)
{
	const char* query_str;
	MemoryContext	oldcontext;

	/*
	 * 防御与短路逻辑 1：防止 Hint Table 递归死循环。
	 *
	 * 【工作原理】
	 * 当开启了 Hint Table 功能时，我们为了去系统表 `hint_plan.hints` 里查询绑定的 Hint，
	 * 实际上会通过 SPI（Server Programming Interface）在数据库内部执行一段普通的 SELECT 语句。
	 * 而这个内部 SELECT 语句在执行时，同样会触发当前的 `post_parse_analyze_hook`！
	 *
	 * 示例：如果不作拦截的灾难推演
	 *   1. 用户执行 `SELECT * FROM t1;`，触发 hook。
	 *   2. 进入本函数，准备去查表找 Hint。
	 *   3. 内部发起 `SELECT hints FROM hint_plan.hints WHERE ...;` 的系统查询。
	 *   4. 这个内部查询再次触发当前 hook。
	 *   5. hook 又为了这条内部查询，再去发起一次 `SELECT hints FROM hint_plan.hints...`
	 *   6. 瞬间陷入无限递归爆炸（Stack Overflow）。
	 *
	 * 解决方式：
	 * 在通过 SPI 发起内部查表操作前，系统会主动将 `hint_inhibit_level` 加 1。
	 * 这里只要看到 `hint_inhibit_level > 0`，就知道当前是在为查询 Hint Table 而执行的
	 * 附带底层查询，必须直接 return 予以放行。
	 */
	if (hint_inhibit_level > 0)
		return;

	/*
	 * 防御与短路逻辑 2：避免单次查询周期内重复解析。
	 *
	 * 【工作原理】
	 * 随着 PostgreSQL 查询的进行，一条 SQL 可能会在不同的入口点（例如外层普通 Parse、
	 * 内层 Prepare 绑定、扩展协议触发点等）被多次嗅探。如果 `current_hint_retrieved`
	 * 已经被置位为 true，说明我们已经为这条语句完成了 Hint 解析落盘，直接复用结果即可，绝不
	 * 做第二次重复徒劳的正则切分或查表操作，保障高并发下的解析性能。
	 */
	if (current_hint_retrieved)
		return;

	/*
	 * 防御与短路逻辑 3：标记解析完成与总开关检查
	 *
	 * 【工作原理】
	 * 1. current_hint_retrieved = true:
	 *    标记当前查询已经进入 Hint 解析流程。无论接下来是否能成功抓取到 Hint，
	 *    这都作为该 SQL 本次生命周期内的“已处理”凭证，防止后续扩展点（如执行器、其他 Hook）
	 *    重复触发冗余的解析寻址过程。
	 *
	 * 2. pg_hint_plan_enable_hint 检查:
	 *    这是整个 pg_hint_plan 插件的总闸（GUC 参数 pg_hint_plan.enable_hint = on/off）。
	 *    如果发现插件功能被全局关闭，则立即清空可能残留的 current_hint_str（避免内存
	 *    泄漏或脏数据），并直接停止解析退路到系统默认优化器流程。
	 */
	current_hint_retrieved = true;

	if (!pg_hint_plan_enable_hint) {
		if (current_hint_str) {
			pfree((void*)current_hint_str);
			current_hint_str = NULL;
		}
		return;
	}

	/*
	 * 维护查询的全局生命周期序列号 (Query Number)，用于高阶调试追踪。
	 *
	 * 【工作原理与诊断作用】
	 * 1. 唯一标识溯源：在高并发场景或是触发器/存储过程（SPI）嵌套引发的交织执行中，
	 *    会有大量的 SQL 前后脚经过这个 Hook。`qno` 是一个整数计数器，系统为每一个
	 *    进入并尝试解析 Hint 的查询动作颁发一个“追踪号”。
	 *
	 * 2. 串联日志上下文：如果开启了高等级调试（例如 pg_hint_plan.debug_print），
	 *    系统会将该序号格式化为如 "[qno=0x1a]" 的字符串缓存在 `qnostr` 数组中。
	 *    在随后去 Hint Table 查表、底层报错、或规划器真正应用 Hint 时，日志系统
	 *    都会打出这串标识。
	 *    这极大地帮助了 DBA 在海量 Postgres 日志中，把一条语句从 Parse 到 Plan 的
	 *    零散日志完美串联起来。
	 *
	 * 3. 递增查询号：每次进入本函数，都会将 `qno` 加 1，确保每个查询都有一个唯一的追踪号。
	 *    这样，当日志系统打印日志时，就可以根据 `qno` 来快速定位到具体的查询。可能重复自增了两次，
	 *    但这并不影响其作为唯一标识的作用。
	 */
	qnostr[0] = 0;
	if (debug_level > 1)
		snprintf(qnostr, sizeof(qnostr), "[qno=0x%x]", qno++);
	qno++;

	/*
	 * Hint Table (提示表) 匹配逻辑
	 *
	 * 【工作原理】
	 * 当 GUC 参数 pg_hint_plan.enable_hint_table 开启时，插件会尝试从数据库内部的
	 * hint_plan.hints 表中获取当前查询匹配的 Hint。这使得我们可以在不修改应用端
	 * 原始 SQL 代码的情况下，在外围强制干预执行计划。
	 *
	 * 核心步骤：
	 * 1. 结构剥离 (JumbleQuery)：借助 PostgreSQL 内置的查询指纹技术（核心逻辑借用自 pg_stat_statements），
	 *    将常量值（如 WHERE id = 123）从查询解析树中剥离，提取出语法骨架（JumbleState）。
	 * 2. 文本归一化 (generate_normalized_query)：将原始 SQL 文本中对应的常量位置替换为占位符 '?'
	 *    （例如变成 "select * from t where id = ?"）、转换为小写并消除多余空白。这使得结构相同
	 *    但参数不同的 SQL 能够匹配到同一条 Hint 规则。
	 * 3. 查表匹配 (get_hints_from_table)：使用【归一化后的 SQL 文本】和当前客户端的
	 *    【application_name】去 hint_plan.hints 表中查找对应配置，并将查到的 Hint
	 *    字符串保留在常驻内存中 (TopMemoryContext)。
	 */
	if (pg_hint_plan_enable_hint_table) {
		int				query_len;
		pgssJumbleState	jstate;
		/* 防御性编程：显式初始化为 NULL，消除未初始化的视觉错觉 */
		Query* jumblequery = NULL;
		char* normalized_query = NULL;

		/* 获取当前执行语句的原始文本，并拿到用于提取指纹的 jumblequery (核心纯查询树) */
		query_str = get_query_string(pstate, query, &jumblequery);

		/* 如果它不是一个可以获取/应用 hint 的常规查询（比如某些系统内部命令），直接退出 */
		if (!query_str)
			return;

		/*
		 * 清理残留：在重新获取 Hint 之前，务必清空可能存在的旧 Hint 字符串，
		 * 防止上下文穿透或内存泄漏。
		 */
		if (current_hint_str) {
			pfree((void*)current_hint_str);
			current_hint_str = NULL;
		}

		/*
		 * 防御拦截：get_query_string 有可能因为命令不可被 Hint 约束
		 * (例如不可解包的 Utility 语句) 而将传进去的 jumblequery 指针赋值为了 NULL。
		 * 如果是 NULL，必须跳过指纹计算，防止传给下方的 JumbleQuery 导致数据库宕机。
		 */
		if (jumblequery) {
			/*
			 * 第一步：初始化指纹计算状态空间
			 * XXX: 归一化代码直接复制自官方 pg_stat_statements.c，如果上游变更需同步更新。
			 */
			jstate.jumble = (unsigned char*)palloc(JUMBLE_SIZE);
			jstate.jumble_len = 0;
			jstate.clocations_buf_size = 32;
			jstate.clocations = (pgssLocationLen*)
				palloc(jstate.clocations_buf_size * sizeof(pgssLocationLen));
			jstate.clocations_count = 0;

			/* 计算查询的哈希指纹，并记录所有常量(参数)在原始 SQL 文本中的偏移位置 */
			JumbleQuery(&jstate, jumblequery);

			/*
			 * 第二步：生成归一化 SQL (脱敏并替换常量为 '?')
			 *
			 * 【工作原理与内在机制】
			 * 在上一步 (JumbleQuery) 中，`jstate.clocations` 数组已经记录了所有在 AST 中
			 * 被判定为常量的节点在原始 `query_str` 文本里的绝对字节偏移量 (location)。
			 *
			 * `generate_normalized_query` 会深入利用 Postgres 内部的词法扫描器 (Lexer)，
			 * 根据这些由 AST 逆向提供的偏移坐标，顺藤摸瓜找到原本的那些硬编码值（如 "123" 或
			 * "'apple'"），将它们精准“抠掉”，并替换成统一的参数占位符 '?'。同时，它还会清理
			 * 词法空白、统一缩进格式，最终得到一个高度收敛的规范化 SQL 字符串。
			 *
			 * 示例演变:
			 *   `SELECT * FROM t   WHERE id = 123 AND status='OK'`
			 *   会被“磨平”成：
			 *   `select * from t where id = ? and status=?`
			 *
			 * query_len 首先获取 `query_str` 长度并 +1，意在为底层的缓冲区预留 '\0' 的结尾空间。
			 * 此函数会返回一个通过 `palloc` 全新分配的字符串，作为去 Hint Table 匹配的最终主键。
			 */
			query_len = strlen(query_str) + 1;
			normalized_query =
				generate_normalized_query(&jstate, query_str, 0, &query_len,
					GetDatabaseEncoding());

			/*
			 * 第三步：根据归一化文本去 Hint Table 中匹配规则。
			 *
			 * 【内存上下文越级逃逸 (Context Escaping)】
			 * 这是一个非常关键的底层架构操作。PostgreSQL 的内存分配是基于树形 Context 的，
			 * 从高到低层级分明。当前我们正处于 Parse/Analyze (解析/分析) 阶段的临时上下文中，
			 * 随时面临着被销毁的风险；而应用 Hint 的核心主战场在后续的 Planner (规划器) 阶段。
			 *
			 * 潜在崩溃点：如果在这里用默认上下文分配器去接收抓取到的 Hint 字符串，
			 * 当退出当前 hook 返回引擎主循环时，该内存会被 Postgres 的垃圾回收机制瞬间抹除，
			 * 导致一会 Planner 去读 `current_hint_str` 时踩到野指针而引起数据库 Coredump。
			 *
			 * 解决方案：使用 MemoryContextSwitchTo() 临时将工作区强行上移至常驻的
			 * TopMemoryContext（这个上下文与会话生存期等同）。这样从系统表中查拉出来的
			 * current_hint_str 就能作为全局不朽资产，成功安全地“活”到接下来的规划器阶段。
			 * 取出后，再严谨地将上下文切回恢复现场。
			 */
			oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			current_hint_str =
				get_hints_from_table(normalized_query, application_name);
			MemoryContextSwitchTo(oldcontext);

			/* 调试日志：详细输出表匹配的命中/未命中状态及归一化结果 */
			if (debug_level > 1) {
				if (current_hint_str)
					ereport(pg_hint_plan_debug_message_level,
						(errmsg("pg_hint_plan[qno=0x%x]: "
							"post_parse_analyze_hook: "
							"hints from table: \"%s\": "
							"normalized_query=\"%s\", "
							"application name =\"%s\"",
							qno, current_hint_str,
							normalized_query, application_name),
							errhidestmt(msgqno != qno),
							errhidecontext(msgqno != qno)));
				else
					ereport(pg_hint_plan_debug_message_level,
						(errmsg("pg_hint_plan[qno=0x%x]: "
							"no match found in table:  "
							"application name = \"%s\", "
							"normalized_query=\"%s\"",
							qno, application_name,
							normalized_query),
							errhidestmt(msgqno != qno),
							errhidecontext(msgqno != qno)));

				msgqno = qno;
			}
		}

		/*
		 * 如果在 Hint Table 中成功找到了本条查询对应的 Hint（Hint Table 优先级最高），
		 * 那么大功告成，直接 return 结束本函数的截获逻辑；
		 * 否则，就会放行到下面的流程，退而去从 SQL 的普通开头注释中寻找 Hint。
		 */
		if (current_hint_str)
			return;
	}
	else
		/* 当开启了插件，但没有开启 Hint Table 功能时，仅获取文本用于随后的注释查取 */
		query_str = get_query_string(pstate, query, NULL);

	if (query_str) {
		/*
		 * 降级策略（Fallback）：从 SQL 语句的块注释中提取 Hint
		 *
		 * 执行到这里意味着要么没有开启 Hint Table，要么是在配置表里没找到匹配项。
		 * 系统现在退而求其次，采用最传统的从 SQL 文本前缀注释（/ *+ ... * /）中提取规则的方式。
		 * （注：实际语法没有空格）
		 *
		 * 【防御性清理】
		 * 在覆盖注入新 Hint 之前，如果 `current_hint_str` 中已经残留了数据，
		 * 必须先对其进行 `pfree` 释放以防止内存泄漏。
		 * 注：虽然理论上当前处理的 SQL 可能和上一次调用的完全相同，但执行一次字符串提取
		 * 的开销和进行深度字符串比对 (strcmp) 的开销差不多，所以这里干脆“无脑覆盖”。
		 */
		if (current_hint_str) {
			pfree((void*)current_hint_str);
			current_hint_str = NULL;
		}

		/*
		 * 内存上下文越级逃逸 (Context Escaping)
		 *
		 * 为了保障提取出来的 Hint 文本能够存活到后续的 Planner 阶段，必须强制将其
		 * 分配在全局持久化的 TopMemoryContext 当中，防止当前外围临时节点被瞬时回收导致野指针。
		 */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		current_hint_str = get_hints_from_comment(query_str);
		MemoryContextSwitchTo(oldcontext);
	}
	else {
		/*
		 * 缓存失效兜底 (Cache Invalidation Retreat)
		 *
		 * 如果 `query_str` 为 NULL，通常是因为 PostgreSQL 后台在尝试拉取或重建
		 * 已经失效的 Plan Cache（执行计划缓存）。在这种特殊瞬态下，SQL 文本往往不可用。
		 * 因此，我们需要“撤销”之前的完成标记（置 current_hint_retrieved 为 false），
		 * 允许引擎在下一次常规解析 (Parse) 环境真正就绪时，再次获得抓取 Hint 的机会。
		 */
		current_hint_retrieved = false;
	}

	/*
	 * 调试级日志转储：输出 Hint 提取结论及上下文比对
	 *
	 * 【工作原理】
	 * 当开启了深度调试 (`debug_level > 1`) 时，将本次从注释中抓取的结果落盘到 PostgreSQL 服务器日志。
	 *
	 * 这里有一个精妙的日志剪裁逻辑：
	 * - 如果只是 `debug_level == 1`（一般调试）并且剥离包装后的 `query_str` 与引擎视角的顶层原始 SQL
	 *   `debug_query_string` 存在差异（表明这是一条被包装在如 EXPLAIN 等 Utility 命令内部的语句），
	 *   系统仅精简地打出抓取到的 Hint 内容，省去大段冗长的冗余 SQL 文本输出。
	 * - 如果是最深度的调试模式或是遇到正常查询，则开启全量转储：将包含的提示、抽取出的核心查询文本、
	 *   以及原始发来的顶层文本全部打出。这对于排查诸如"客户端发送的 Prepare/Execute 结构究竟
	 *   把我的 Hint 藏到了哪个指针里"等高阶问题有奇效。
	 *
	 * 技巧：`errhidestmt` 与 `errhidecontext` 用于抑制默认 PG 报错时附带打印的
	 * Statement 和 Context 堆栈信息（只有首次进入时 `msgqno != qno` 才会打印上下文，后续静音防刷屏）。
	 */
	if (debug_level > 1) {
		if (debug_level == 1 && query_str && debug_query_string &&
			strcmp(query_str, debug_query_string))
			ereport(pg_hint_plan_debug_message_level,
				(errmsg("hints in comment=\"%s\"",
					current_hint_str ? current_hint_str : "(none)"),
					errhidestmt(msgqno != qno),
					errhidecontext(msgqno != qno)));
		else
			ereport(pg_hint_plan_debug_message_level,
				(errmsg("hints in comment=\"%s\", query=\"%s\", debug_query_string=\"%s\"",
					current_hint_str ? current_hint_str : "(none)",
					query_str ? query_str : "(none)",
					debug_query_string ? debug_query_string : "(none)"),
					errhidestmt(msgqno != qno),
					errhidecontext(msgqno != qno)));
		msgqno = qno;
	}
}

/*
 * 解析与分析阶段的 Hook 拦截入口。
 *
 * 【工作原理与示例】
 * 当 PostgreSQL 的 "Simple Query Protocol"（简单查询协议）执行普通 SQL 时，
 * 引擎会依序经过 Parse -> Analyze -> Plan。
 * 这个函数就挂载在 Analyze 阶段结束后的 `post_parse_analyze_hook` 上，
 * 其核心任务是先发制人地提取 Hint 字符串，并调用 `get_current_hint_string`（这里
 * 是 Hint Table 功能和普通查注释功能的共同大门）。
 *
 * 示例：顶层 SQL 与底层函数嵌套的解析管理
 *   场景: 执行 / *+ SeqScan(t1) * / SELECT * FROM t1 WHERE val = my_func();
 *        其中 `my_func()` 内部封装了 `SELECT * FROM t2;`
 *
 *   动作推演:
 *     1. 顶层解析: 第一次进入此 hook。此时 `plpgsql_recurse_level == 0`，
 *        代码强制置低标志位 `current_hint_retrieved = false;`，告诉后续流程这是
 *        一个全新大循环的开始，必须从头抓取 Hint（拿到 SeqScan(t1)）。
 *     2. 内层解析: 当分析到 `my_func()` 内部语句时，再次重入该 hook，由于
 *        `plpgsql_recurse_level > 0`，跳过强设标志位的动作，依赖内部状态管理，
 *        防止底层毫无必要地刷新或清除外层刚刚好不容易抓到的顶层 Hint 信息。
 */
static void
pg_hint_plan_post_parse_analyze(ParseState* pstate, Query* query)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query);

	if (plpgsql_recurse_level == 0)
		current_hint_retrieved = false;

	get_current_hint_string(pstate, query);
}

/*
 * We need to reset current_hint_retrieved flag always when a command execution
 * is finished. This is true even for a pure utility command that doesn't
 * involve planning phase.
 */
static void
pg_hint_plan_ProcessUtility(PlannedStmt* pstmt, const char* queryString,
	ProcessUtilityContext context,
	ParamListInfo params, QueryEnvironment* queryEnv,
	DestReceiver* dest, char* completionTag)
{
	if (prev_ProcessUtility_hook)
		prev_ProcessUtility_hook(pstmt, queryString, context, params, queryEnv,
			dest, completionTag);
	else
		standard_ProcessUtility(pstmt, queryString, context, params, queryEnv,
			dest, completionTag);

	if (plpgsql_recurse_level == 0)
		current_hint_retrieved = false;
}

/*
 * 读取并设置 hint 信息
 */
static PlannedStmt*
pg_hint_plan_planner(Query* parse, int cursorOptions, ParamListInfo boundParams)
{
	int				save_nestlevel;
	PlannedStmt* result;
	HintState* hstate;
	const char* prev_hint_str = NULL;

	/*
	 * 安全兜底分支：以下任一条件成立时，本次规划不使用 hint，直接走标准
	 * planner（standard_planner_proc）。
	 *
	 * 1) !pg_hint_plan_enable_hint
	 *    全局 hint 开关关闭。
	 *
	 * 2) hint_inhibit_level > 0
	 *    当前处于“禁止 hint 生效”的上下文（通常是内部流程，如 SPI 相关路径）
	 *    为避免递归干扰或副作用，临时禁用 hint。
	 *
	 * 注意：其他 hook 可能依赖 current_hint_state 改写计划，因此在该分支中
	 * 会进入标准流程并保持状态一致。
	 *
	 * 例子：
	 * - enable_hint=false, inhibit=0  => 走标准 planner。
	 * - enable_hint=true,  inhibit=1  => 走标准 planner。
	 * - enable_hint=true,  inhibit=0  => 继续后续 hint 解析与应用流程。
	 */
	if (!pg_hint_plan_enable_hint || hint_inhibit_level > 0) {
		if (debug_level > 1)
			ereport(pg_hint_plan_debug_message_level,
				(errmsg("pg_hint_plan%s: planner: enable_hint=%d,"
					" hint_inhibit_level=%d",
					qnostr, pg_hint_plan_enable_hint,
					hint_inhibit_level),
					errhidestmt(msgqno != qno)));
		msgqno = qno;

		goto standard_planner_proc;
	}

	/*
	 * 支持嵌套 PL/pgSQL 函数（“不太优雅”的特殊处理）。
	 *
	 * 【背景与原理】
	 * 在正常的 SQL 请求中，可以通过标准的 ParseState 或 Query 结构拿到原始带注释的 SQL 文本。
	 * 但在 PL/pgSQL 函数内部，SQL 语句会被预编译和缓存，触发规划器（Planner）时，
	 * 常规参数中已经丢失了原始的带有 Hint 的文本。
	 *
	 * 【曲线救国】
	 * 幸运的是，PostgreSQL 的错误处理机制（error_context_stack）会在执行内部 SQL 时，
	 * 将当前的 SQL 原文强转后存放在 error_context_stack->arg 中，以便报错时向用户报告。
	 * 于是这里“偷看”了错误上下文，强行从中提取 Hint 字符串。这正是作者觉得“不太优雅”的原因。
	 *
	 * 【内存上下文切换】
	 * 当前可能处于局部较短命的内存上下文中。为了保证提取出的 Hint 字符串能存活到整个查询规划结束，
	 * 这里临时切换到顶层内存上下文 (TopMemoryContext) 进行分配，解析完后再切回。
	 */
	if (plpgsql_recurse_level > 0 &&
		error_context_stack && error_context_stack->arg) {
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		current_hint_str =
			get_hints_from_comment((char*)error_context_stack->arg);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * 针对“扩展查询协议”(Extended Protocol) 的兜底补偿。
	 *
	 * 【工作原理与示例】
	 * PostgreSQL 有两种查询协议，
	 * 一个是直接给纯文本的 简单协议（比如 psql），
	 * 一个是分 Parse / Bind / Execute 三个步骤的扩展协议（比如 JDBC）。
	 *
	 * pg_hint_plan 默认是在前面的 Analyze 步骤挂个钩子（hook）去读 hint。但扩展协议在面对循环或预编译执行时，为了追求高性能，很容易直接就复用之前的解析树进入规划，跳过 Analyze 环节。这会导致此时的 hint 获取不到，所以这寥寥两行代码起到了关键的续命作用，
	 * 一旦发现是空的，立马强行从内部查询树 parse 里把文本刮出来获取 hint，让线上使用比如 JDBC 的系统也能正常生效！
	 *
	 * 正常情况下，PostgreSQL 的查询流程会经过 Parse -> Analyze -> Plan，
	 * 我们在该插件的 post_parse_analyze_hook (Analyze 阶段) 中会提前抓取并设置 current_hint_str。
	 * 但在 PostgreSQL 的“扩展查询协议”（预编译绑定）机制下，Analyze 阶段可能会被跳过，
	 * 直接进入 Planner 规划阶段，从而导致丢失 Hint。
	 *
	 * 示例 A：普通查询协议（Simple Protocol）
	 *   场景: 在终端 (psql) 直接执行 `SELECT / *+ SeqScan(t1) * / * FROM t1;`
	 *   行为: 核心执行流走标准的 Analyze 阶段，提前解析到 Hint。
	 *   结果: 运行到此处时，`current_hint_str` 已经有值（"SeqScan(t1)"），不需要补取。
	 *
	 * 示例 B：扩展协议或预编译语句（Extended Protocol / Prepared Statements）
	 *   场景: 客户端（如 Java JDBC / Npgsql）使用 `PreparedStatement` 机制，或者
	 *        用户手动调用 `PREPARE stmt ...` 然后 `EXECUTE stmt;`。
	 *   行为: 解析和分析阶段被缓存或跳过，执行阶段根据参数绑定直接拉起 Planner 进行重新规划。
	 *   结果: 此时未经 Analyze 钩子，`current_hint_str` 仍为 NULL。这个 if 判断就会生效，
	 *        通过 `get_current_hint_string(NULL, parse)` 强行在 Planner 起始阶段把 Hint 补抓回来，
	 *        保证了 JDBC 等应用使用预编译语句时 Hint 依然生效。
	 */
	if (!current_hint_str)
		get_current_hint_string(NULL, parse);

	/*
	 * Hint 解析与分支判断。
	 *
	 * 【工作原理与示例】
	 * 在确保已经尝试过所有方法获取 Hint 字符串后，这里进行最后两道关卡的拦截：
	 * 1. 字符串本身是否存在？如果不存在，说明此查询没有任何 Hint 意图，直接放行去标准 Planner。
	 * 2. 字符串存在时，其中的 Hint 语法是否被成功解析？如果解析结果为空（即 hstate == NULL），同样放行。
	 *
	 * 示例 A：根本没传 Hint
	 *   SQL: "SELECT * FROM t1;"
	 *   解析: `current_hint_str` 为 NULL。触发第一个 if，直接 goto standard_planner_proc，
	 *        跳过后续所有 pg_hint_plan 的独有逻辑，0 开销执行原生规划。
	 *
	 * 示例 B：传了错误的/无效的 Hint 语法
	 *   SQL: "SELECT / *+ WrongHintSyntax(t1) * / * FROM t1;"
	 *   解析: `current_hint_str` 不为空，顺利进入 `create_hintstate` 进行解析。
	 *        但由于语法错误或关键字不存在，`create_hintstate` 解析结束发现 "有效 hint 数量为 0"，
	 *        随之销毁结构体并返回 NULL。此时触发第二个 if，放弃干预，依然 goto standard_planner_proc。
	 *
	 * 示例 C：传了正确且有效的 Hint
	 *   SQL: "SELECT / *+ SeqScan(t1) * / * FROM t1;"
	 *   解析: 上述两道关卡均完美通过，程序继续往下走，把生成的 hstate 压栈（push_hint）
	 *        并开始真正的执行计划接管。
	 */
	if (!current_hint_str)
		goto standard_planner_proc;

	/* 拷贝并解析 hint 字符串，生成该语句的专属 hint state 结构 */
	hstate = create_hintstate(parse, pstrdup(current_hint_str));

	/* 即使有 hint 字符串，如果解析后发现并没有有效的规则，依然退回标准流程 */
	if (!hstate)
		goto standard_planner_proc;

	/*
	 * 将新 hint 压入栈中，以屏蔽之前的 hint 上下文。为保证栈状态始终一致，
	 * 在进入下面的 PG_TRY/PG_CATCH 之前不应出现 ERROR 级失败。
	 */
	push_hint(hstate);

	/* 在这里开始设置扫描相关的强制参数。 */
	save_nestlevel = NewGUCNestLevel();

	/*
	 * 清理并备份当前 Hint 字符串，防止被嵌套的 planner 调用污染。
	 *
	 * 【工作原理与示例】
	 * PostgreSQL 在规划过程中，常常会触发子查询、SQL 函数或视图的“嵌套规划”。
	 * 由于 `current_hint_str` 是一个全局静态变量，如果不在此刻“备份并清空”，内层
	 * 规划解析出的 Hint 字符串就会直接覆盖它，导致外层规划在后续流程中读到错误的数据，
	 * 甚至产生尝试释放同一块内存的严重错误（Double Free）。
	 *
	 * 示例：带有 SQL 函数的嵌套规划
	 *   场景: 创建了一个 SQL 函数 `my_func()`，其内部定义查询：
	 *         / *+ SeqScan(t2) * / SELECT val FROM t2 WHERE id = $1;
	 *        客户端执行外层查询：
	 *         / *+ IndexScan(t1) * / SELECT * FROM t1 WHERE val = my_func(t1.id);
	 *
	 *   动作推演:
	 *     1. 外层规划开始: `current_hint_str` 此时持有 "IndexScan(t1)"。
	 *     2. 现场保护(当前代码): 将 "IndexScan(t1)" 存入局部变量 `prev_hint_str`，
	 *        并将全局指针 `current_hint_str` 置为 NULL。
	 *     3. 深入底层 Planner: `standard_planner` 继续执行，处理到 `my_func` 时拉起内层规划。
	 *        内层获取到自身的 Hint，全局 `current_hint_str` 变为 "SeqScan(t2)"，完成内层规划。
	 *     4. 现场恢复: 下方的 PG_TRY/PG_CATCH 块执行完毕后，调用 `current_hint_str = prev_hint_str`，
	 *        把全局变量安全地调包回 "IndexScan(t1)"。外层逻辑完全不受 "SeqScan(t2)" 干扰。
	 */
	recurse_level++;
	prev_hint_str = current_hint_str;
	current_hint_str = NULL;

	/*
	 * 使用 PG_TRY 在 planner 规划期间提供安全沙箱，确保就算规划器报错，
	 * 被 Hint 篡改的系统参数 (GUC) 和 Hint 栈也能被正确清理和恢复。
	 *
	 * 【工作原理与示例】
	 * pg_hint_plan 会在规划前强行修改很多当前 PostgreSQL 会话的参数（例如 `enable_seqscan = off`，
	 * 或是用户指定的 / *+ Set(work_mem '1GB') * /）。如果底层的 `standard_planner` 执行中途抛出 ERROR
	 *（例如常量折叠除零、或者内存不足 OOM 等），代码执行流会直接中断。如果不捕获并恢复现场，
	 * 会话里的 GUC 就会被永久“毒化”（后续所有 SQL 都被迫不能走全表扫描），且由于 Hint 未弹栈（pop_hint），
	 * 就会导致严重的内存泄露甚至串扰问题。
	 *
	 * 示例 A：正常规划流程
	 *   场景: 执行 / *+ Set(work_mem '64MB') IndexScan(t1) * / SELECT * FROM t1 WHERE id = 1;
	 *   动作: PG_TRY 块内部首先应用 Set hint 修改 `work_mem` 为 64MB，并记录初始扫描掩码等。
	 *        成功调用 `standard_planner`，生成执行计划。接着退出 Try 块，下方逻辑会负责正常清场。
	 *
	 * 示例 B：规划阶段发生严重错误 (ERROR)
	 *   场景: 执行 / *+ Set(work_mem '64MB') SeqScan(t1) * / SELECT * FROM t1 WHERE id = 1 / 0;
	 *   动作推演:
	 *     1. 参数设置: Try 块内成功将 `work_mem` 临时设置为 64MB。
	 *     2. 报错中断: `standard_planner` 启动，在优化器的常量折叠阶段，由于试图计算 `1 / 0`，
	 *        立刻抛出 `ERROR: division by zero`，程序进入异常处理跳转。
	 *     3. 捕获拦管: 直接跳入 `PG_CATCH()` 分支内。
	 *     4. 现场急救 (核心职责):
	 *        - `AtEOXact_GUC(true, save_nestlevel)`: 强行将 `work_mem` 等所有修改退火恢复回默认初始值。
	 *        - `pop_hint()`: 把该次半途作废的 Hint 从全局栈中干净地丢弃。
	 *     5. 重新发车: 调用 `PG_RE_THROW()`，将“除零大意”这个错误原封不动地发报给客户端报错。
	 *        此时客户端断开或重试下一次查询，其所在会话环境依然干干净净，没有半分上一次错误 SQL 的残留。
	 */
	PG_TRY();
	{
		/*
		 * 应用并备份全局 GUC 参数，为强制改变执行计划做准备。
		 *
		 * 【工作原理与示例】
		 * 在把执行权交回给原生的 standard_planner 之前，需要根据解析出来的 Hint，
		 * 把对应的禁用（Disable）与开启（Enable）参数强行注入进 PostgreSQL 会话中。
		 * 如果不事先保存它们修改前的“初始状态”，规划结束后我们就无法把环境还原回去。
		 *
		 * 示例：混用 Set 参数与 Scan 强制掩码
		 *   场景: 执行 / *+ Set(work_mem '2GB') SeqScan(t1) * / SELECT * FROM t1;
		 *   动作推演:
		 *     1. setup_guc_enforcement: 把 `work_mem` 临时设置为 2GB。
		 *     2. 状态快照备份: 捕捉此刻系统的 `enable_seqscan` / `enable_indexscan` 以及
		 *        并行相关的诸多代价常量（如 parallel_tuple_cost 等）并存入 `current_hint_state->init_*` 变量中，
		 *        此时它们尚为原生默认值。
		 *     3. 后续在底层 hook （如 join_search_hook）触发时，才会用 "SeqScan(t1)" 生成的
		 *        掩码去覆盖 `get_current_scan_mask()` 的状态，通过改变 enable_* 参数从而达到控制扫描的唯一目的。
		 */
		setup_guc_enforcement(current_hint_state->set_hints,
			current_hint_state->num_hints[HINT_TYPE_SET],
			current_hint_state->context);

		current_hint_state->init_scan_mask = get_current_scan_mask();
		current_hint_state->init_join_mask = get_current_join_mask();
		current_hint_state->init_min_para_tablescan_size =
			min_parallel_table_scan_size;
		current_hint_state->init_min_para_indexscan_size =
			min_parallel_index_scan_size;
		current_hint_state->init_paratup_cost = parallel_tuple_cost;
		current_hint_state->init_parasetup_cost = parallel_setup_cost;

		/*
		 * 激活并在必要时拉起并行 Worker 参数。
		 *
		 * 示例: 如果传入了 / *+ Parallel(t1 4 hard) * /，这里 `max_hint_nworkers` 会是 4。
		 * 如果当前系统的 `max_parallel_workers_per_gather` 是 0（即默认关闭了并行），
		 * 为了让并行的 Hint 能真正打破系统限制，必须临时把该参数强制设成大于 0 的数（这里给了个底线 1）。
		 * 否则 PostgreSQL 规划器第一口判定发现 Worker 为 0，根本不会进入并行的寻路逻辑。
		 */
		if (max_hint_nworkers > 0 && max_parallel_workers_per_gather < 1)
			set_config_int32_option("max_parallel_workers_per_gather",
				1, current_hint_state->context);
		current_hint_state->init_nworkers = max_parallel_workers_per_gather;

		/*
		 * 将控制权交回 PostgreSQL 的原生/已存在 Planner，
		 * 原生 Planner 在内部构建查询树时，会触发 pg_hint_plan 注入的各个其它阶段的 hook
		 * （比如 get_relation_info_hook 去拿扫表 Hint、set_join_pathlist_hook 去拿连接 Hint 等），
		 * 靠那些具体的 hook 在微观粒度上再次拦截来最终影响生成的 `result`(PlannedStmt)。
		 */
		if (debug_level > 1) {
			ereport(pg_hint_plan_debug_message_level,
				(errhidestmt(msgqno != qno),
					errmsg("pg_hint_plan%s: planner", qnostr)));
			msgqno = qno;
		}

		if (prev_planner)
			result = (*prev_planner) (parse, cursorOptions, boundParams);
		else
			result = standard_planner(parse, cursorOptions, boundParams);

		/* 规划结束，安全退回当前全局的 Hint 上下文 */
		current_hint_str = prev_hint_str;
		recurse_level--;
	}
	PG_CATCH();
	{
		/*
		 * 回滚 GUC 参数变更，并将当前 hint 上下文从栈中弹出，恢复现场。
		 */
		current_hint_str = prev_hint_str;
		recurse_level--;
		AtEOXact_GUC(true, save_nestlevel);
		pop_hint();
		PG_RE_THROW();
	}
	PG_END_TRY();


	/*
	 * 彻底清理顶层 Hint 字符串内存，防止跨查询泄漏与状态残留。
	 *
	 * 【工作原理与示例】
	 * 当 `recurse_level < 1`，意味着当前已经退回到了整棵查询树（包括所有触发器、
	 * SQL 函数等嵌套逻辑）的最外层顶端。此时整个外层 SQL 的规划已经彻底结束（PlannedStmt 已生成），
	 * 之前通过 `get_hints_from_comment` 提取出并在 `TopMemoryContext` 中长期驻留的原始 Hint 字符串
	 * 已完成了其历史使命，必须立即释放。
	 *
	 * 示例：如果不释放导致的内存泄漏与串扰
	 *   场景 1: 单连接内连续执行两条查询
	 *     Query 1: / *+ SeqScan(t1) * / SELECT * FROM t1;
	 *     Query 2: SELECT * FROM t2; (无 Hint)
	 *   动作推演:
	 *     1. Query 1 规划结束: 此时 `current_hint_str` 在内存中仍指向 "SeqScan(t1)"。
	 *        如果缺少此段清理逻辑（或者 `current_hint_retrieved = false` 没重置），
	 *        这块由 `palloc` 从 `TopMemoryContext` 分配的内存将永远泄漏。
	 *     2. Query 2 开始规划: 由于上一轮的 `current_hint_retrieved` 标志可能未被正确拉低，
	 *        或者遗留的全局指针未被设为 NULL，Query 2 可能会把自身错误地当成 Query 1 来处理，
	 *        试图执行莫名其妙的强制 SeqScan。
	 *     3. 清理结果 (当前代码机制): 这个 IF 块会调用 `pfree` 严格回收 "SeqScan(t1)"，
	 *        并将指针和布尔标志全部复位归零，从而确保 Query 2 上来面对的是一张干净的白纸。
	 */
	if (recurse_level < 1 && current_hint_str) {
		pfree((void*)current_hint_str);
		current_hint_str = NULL;
		current_hint_retrieved = false;
	}

	/* 调试模式下打印 hint 信息。 */
	if (debug_level == 1)
		HintStateDump(current_hint_state);
	else if (debug_level > 1)
		HintStateDump2(current_hint_state);

	/*
	 * 回滚 GUC 参数变更，并弹出当前 hint 上下文，恢复状态。
	 */
	AtEOXact_GUC(true, save_nestlevel);
	pop_hint();

	return result;

standard_planner_proc:
	if (debug_level > 1) {
		ereport(pg_hint_plan_debug_message_level,
			(errhidestmt(msgqno != qno),
				errmsg("pg_hint_plan%s: planner: no valid hint",
					qnostr)));
		msgqno = qno;
	}

	/*
	 * 标准规划降级路径与状态安全继承。
	 *
	 * 【工作原理与示例】
	 * 凡是"没有解析出合法语法的 Hint"、"全局/局部禁用了 Hint" 等情况，都会通过直接 goto
	 * 跳转至本标签（standard_planner_proc），脱离干预直接生成计划。
	 *
	 * 示例：嵌套查询中，外层有 Hint、内层无 Hint
	 *   场景: 一个带 Hint 的外层查询内嵌了不带 Hint 的函数或子查询。
	 *         外层: / *+ SeqScan(t1) * / SELECT * FROM t1 WHERE val = my_func(t1.id);
	 *         内层(my_func): SELECT * FROM t2; (无 Hint)
	 *
	 *   动作推演:
	 *     1. 净身入场: 内层规划进入此分支，首先强行令 `current_hint_state = NULL`。
	 *        原因：此时全局栈中实际上还强压着外层的 `SeqScan(t1)` 状态。如果不屏蔽，
	 *        底层 hook 在给内层 t2 规划扫描路径时，会错误检索到外层的 Hint，引发跨层污染串扰。
	 *     2. 放行原生计划: 执行 `standard_planner` 生成内层函数纯净不受干扰的正常计划。
	 *     3. 恢复外层霸权: 内层规划结束即将返回时，执行 `if (HintStateStack != NIL)`。
	 *        发现栈里居然还有元素（即外层的 `SeqScan(t1)`），遂立刻将全局指针 `current_hint_state`
	 *        重新拔回栈顶的指针状态。这一步居功至伟，保证控制权交还外层时，外层能继续用之前被
	 *        暂时搁置的 Hint （比如对外层后续表的 Join）去干预并完成它尚未完结的规划任务。
	 */
	current_hint_state = NULL;
	if (prev_planner)
		result = (*prev_planner) (parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

	/* 上层 planner 仍然需要当前 hint 状态 */
	if (HintStateStack != NIL)
		current_hint_state = (HintState*)lfirst(list_head(HintStateStack));

	return result;
}

/*
 * Find scan method hint to be applied to the given relation
 *
 */
static ScanMethodHint*
find_scan_hint(PlannerInfo* root, Index relid)
{
	RelOptInfo* rel;
	RangeTblEntry* rte;
	ScanMethodHint* real_name_hint = NULL;
	ScanMethodHint* alias_hint = NULL;
	int				i;

	/* This should not be a join rel */
	Assert(relid > 0);
	rel = root->simple_rel_array[relid];

	/*
	 * This function is called for any RelOptInfo or its inheritance parent if
	 * any. If we are called from inheritance planner, the RelOptInfo for the
	 * parent of target child relation is not set in the planner info.
	 *
	 * Otherwise we should check that the reloptinfo is base relation or
	 * inheritance children.
	 */
	if (rel &&
		rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return NULL;

	/*
	 * This is baserel or appendrel children. We can refer to RangeTblEntry.
	 */
	rte = root->simple_rte_array[relid];
	Assert(rte);

	/* We don't hint on other than relation and foreign tables */
	if (rte->rtekind != RTE_RELATION ||
		rte->relkind == RELKIND_FOREIGN_TABLE)
		return NULL;

	/* Find scan method hint, which matches given names, from the list. */
	for (i = 0; i < current_hint_state->num_hints[HINT_TYPE_SCAN_METHOD]; i++) {
		ScanMethodHint* hint = current_hint_state->scan_hints[i];

		/* We ignore disabled hints. */
		if (!hint_state_enabled(hint))
			continue;

		if (!alias_hint &&
			RelnameCmp(&rte->eref->aliasname, &hint->relname) == 0)
			alias_hint = hint;

		/* check the real name for appendrel children */
		if (!real_name_hint &&
			rel && rel->reloptkind == RELOPT_OTHER_MEMBER_REL) {
			char* realname = get_rel_name(rte->relid);

			if (realname && RelnameCmp(&realname, &hint->relname) == 0)
				real_name_hint = hint;
		}

		/* No more match expected, break  */
		if (alias_hint && real_name_hint)
			break;
	}

	/* real name match precedes alias match */
	if (real_name_hint)
		return real_name_hint;

	return alias_hint;
}

static ParallelHint*
find_parallel_hint(PlannerInfo* root, Index relid)
{
	RelOptInfo* rel;
	RangeTblEntry* rte;
	ParallelHint* real_name_hint = NULL;
	ParallelHint* alias_hint = NULL;
	int				i;

	/* This should not be a join rel */
	Assert(relid > 0);
	rel = root->simple_rel_array[relid];

	/*
	 * Parallel planning is appliable only on base relation, which has
	 * RelOptInfo.
	 */
	if (!rel)
		return NULL;

	/*
	 * We have set root->glob->parallelModeOK if needed. What we should do here
	 * is just following the decision of planner.
	 */
	if (!rel->consider_parallel)
		return NULL;

	/*
	 * This is baserel or appendrel children. We can refer to RangeTblEntry.
	 */
	rte = root->simple_rte_array[relid];
	Assert(rte);

	/* Find parallel method hint, which matches given names, from the list. */
	for (i = 0; i < current_hint_state->num_hints[HINT_TYPE_PARALLEL]; i++) {
		ParallelHint* hint = current_hint_state->parallel_hints[i];

		/* We ignore disabled hints. */
		if (!hint_state_enabled(hint))
			continue;

		if (!alias_hint &&
			RelnameCmp(&rte->eref->aliasname, &hint->relname) == 0)
			alias_hint = hint;

		/* check the real name for appendrel children */
		if (!real_name_hint &&
			rel && rel->reloptkind == RELOPT_OTHER_MEMBER_REL) {
			char* realname = get_rel_name(rte->relid);

			if (realname && RelnameCmp(&realname, &hint->relname) == 0)
				real_name_hint = hint;
		}

		/* No more match expected, break  */
		if (alias_hint && real_name_hint)
			break;
	}

	/* real name match precedes alias match */
	if (real_name_hint)
		return real_name_hint;

	return alias_hint;
}

/*
 * regexeq
 *
 * Returns TRUE on match, FALSE on no match.
 *
 *   s1 --- the data to match against
 *   s2 --- the pattern
 *
 * Because we copy s1 to NameData, make the size of s1 less than NAMEDATALEN.
 */
static bool
regexpeq(const char* s1, const char* s2)
{
	NameData	name;
	text* regexp;
	Datum		result;

	strcpy(name.data, s1);
	regexp = cstring_to_text(s2);

	result = DirectFunctionCall2Coll(nameregexeq,
		DEFAULT_COLLATION_OID,
		NameGetDatum(&name),
		PointerGetDatum(regexp));
	return DatumGetBool(result);
}


/* Remove indexes instructed not to use by hint. */
static void
restrict_indexes(PlannerInfo* root, ScanMethodHint* hint, RelOptInfo* rel,
	bool using_parent_hint)
{
	ListCell* cell;
	ListCell* prev;
	ListCell* next;
	StringInfoData	buf;
	RangeTblEntry* rte = root->simple_rte_array[rel->relid];
	Oid				relationObjectId = rte->relid;

	/*
	 * We delete all the IndexOptInfo list and prevent you from being usable by
	 * a scan.
	 */
	if (hint->enforce_mask == ENABLE_SEQSCAN ||
		hint->enforce_mask == ENABLE_TIDSCAN) {
		list_free_deep(rel->indexlist);
		rel->indexlist = NIL;
		hint->base.state = HINT_STATE_USED;

		return;
	}

	/*
	 * When a list of indexes is not specified, we just use all indexes.
	 */
	if (hint->indexnames == NIL)
		return;

	/*
	 * Leaving only an specified index, we delete it from a IndexOptInfo list
	 * other than it.
	 */
	prev = NULL;
	if (debug_level > 0)
		initStringInfo(&buf);

	for (cell = list_head(rel->indexlist); cell; cell = next) {
		IndexOptInfo* info = (IndexOptInfo*)lfirst(cell);
		char* indexname = get_rel_name(info->indexoid);
		ListCell* l;
		bool			use_index = false;

		next = lnext(cell);

		foreach(l, hint->indexnames)
		{
			char* hintname = (char*)lfirst(l);
			bool	result;

			if (hint->regexp)
				result = regexpeq(indexname, hintname);
			else
				result = RelnameCmp(&indexname, &hintname) == 0;

			if (result) {
				use_index = true;
				if (debug_level > 0) {
					appendStringInfoCharMacro(&buf, ' ');
					quote_value(&buf, indexname);
				}

				break;
			}
		}

		/*
		 * Apply index restriction of parent hint to children. Since index
		 * inheritance is not explicitly described we should search for an
		 * children's index with the same definition to that of the parent.
		 */
		if (using_parent_hint && !use_index) {
			foreach(l, current_hint_state->parent_index_infos)
			{
				int					i;
				HeapTuple			ht_idx;
				ParentIndexInfo* p_info = (ParentIndexInfo*)lfirst(l);

				/*
				 * we check the 'same' index by comparing uniqueness, access
				 * method and index key columns.
				 */
				if (p_info->indisunique != info->unique ||
					p_info->method != info->relam ||
					list_length(p_info->column_names) != info->ncolumns)
					continue;

				/* Check if index key columns match */
				for (i = 0; i < info->ncolumns; i++) {
					char* c_attname = NULL;
					char* p_attname = NULL;

					p_attname = list_nth(p_info->column_names, i);

					/*
					 * if both of the key of the same position are expressions,
					 * ignore them for now and check later.
					 */
					if (info->indexkeys[i] == 0 && !p_attname)
						continue;

					/* deny if one is expression while another is not */
					if (info->indexkeys[i] == 0 || !p_attname)
						break;

					c_attname = get_attname(relationObjectId,
						info->indexkeys[i], false);

					/* deny if any of column attributes don't match */
					if (strcmp(p_attname, c_attname) != 0 ||
						p_info->indcollation[i] != info->indexcollations[i] ||
						p_info->opclass[i] != info->opcintype[i])
						break;

					/*
					 * Compare index ordering if this index is ordered.
					 *
					 * We already confirmed that this and the parent indexes
					 * share the same column set (actually only the length of
					 * the column set is compard, though.) and index access
					 * method. So if this index is unordered, the parent can be
					 * assumed to be be unodered. Thus no need to bother
					 * checking the parent's orderedness.
					 */
					if (info->sortopfamily != NULL &&
						(((p_info->indoption[i] & INDOPTION_DESC) != 0)
							!= info->reverse_sort[i] ||
							((p_info->indoption[i] & INDOPTION_NULLS_FIRST) != 0)
							!= info->nulls_first[i]))
						break;
				}

				/* deny this if any difference found */
				if (i != info->ncolumns)
					continue;

				/* check on key expressions  */
				if ((p_info->expression_str && (info->indexprs != NIL)) ||
					(p_info->indpred_str && (info->indpred != NIL))) {
					/* fetch the index of this child */
					ht_idx = SearchSysCache1(INDEXRELID,
						ObjectIdGetDatum(info->indexoid));

					/* check expressions if both expressions are available */
					if (p_info->expression_str &&
						!heap_attisnull(ht_idx, Anum_pg_index_indexprs, NULL)) {
						Datum       exprsDatum;
						bool        isnull;
						Datum       result;

						/*
						 * to change the expression's parameter of child's
						 * index to strings
						 */
						exprsDatum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indexprs,
							&isnull);

						result = DirectFunctionCall2(pg_get_expr,
							exprsDatum,
							ObjectIdGetDatum(
								relationObjectId));

						/* deny if expressions don't match */
						if (strcmp(p_info->expression_str,
							text_to_cstring(DatumGetTextP(result))) != 0) {
							/* Clean up */
							ReleaseSysCache(ht_idx);
							continue;
						}
					}

					/* compare index predicates  */
					if (p_info->indpred_str &&
						!heap_attisnull(ht_idx, Anum_pg_index_indpred, NULL)) {
						Datum       predDatum;
						bool        isnull;
						Datum       result;

						predDatum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indpred,
							&isnull);

						result = DirectFunctionCall2(pg_get_expr,
							predDatum,
							ObjectIdGetDatum(
								relationObjectId));

						if (strcmp(p_info->indpred_str,
							text_to_cstring(DatumGetTextP(result))) != 0) {
							/* Clean up */
							ReleaseSysCache(ht_idx);
							continue;
						}
					}

					/* Clean up */
					ReleaseSysCache(ht_idx);
				}
				else if (p_info->expression_str || (info->indexprs != NIL))
					continue;
				else if (p_info->indpred_str || (info->indpred != NIL))
					continue;

				use_index = true;

				/* to log the candidate of index */
				if (debug_level > 0) {
					appendStringInfoCharMacro(&buf, ' ');
					quote_value(&buf, indexname);
				}

				break;
			}
		}

		if (!use_index)
			rel->indexlist = list_delete_cell(rel->indexlist, cell, prev);
		else
			prev = cell;

		pfree(indexname);
	}

	if (debug_level == 1) {
		StringInfoData  rel_buf;
		char* disprelname = "";

		/*
		 * If this hint targetted the parent, use the real name of this
		 * child. Otherwise use hint specification.
		 */
		if (using_parent_hint)
			disprelname = get_rel_name(rte->relid);
		else
			disprelname = hint->relname;


		initStringInfo(&rel_buf);
		quote_value(&rel_buf, disprelname);

		ereport(pg_hint_plan_debug_message_level,
			(errmsg("available indexes for %s(%s):%s",
				hint->base.keyword,
				rel_buf.data,
				buf.data)));
		pfree(buf.data);
		pfree(rel_buf.data);
	}
}

/*
 * Return information of index definition.
 */
static ParentIndexInfo*
get_parent_index_info(Oid indexoid, Oid relid)
{
	ParentIndexInfo* p_info = palloc(sizeof(ParentIndexInfo));
	Relation	    indexRelation;
	Form_pg_index	index;
	char* attname;
	int				i;

	indexRelation = index_open(indexoid, RowExclusiveLock);

	index = indexRelation->rd_index;

	p_info->indisunique = index->indisunique;
	p_info->method = indexRelation->rd_rel->relam;

	p_info->column_names = NIL;
	p_info->indcollation = (Oid*)palloc(sizeof(Oid) * index->indnatts);
	p_info->opclass = (Oid*)palloc(sizeof(Oid) * index->indnatts);
	p_info->indoption = (int16*)palloc(sizeof(Oid) * index->indnatts);

	/*
	 * Collect relation attribute names of index columns for index
	 * identification, not index attribute names. NULL means expression index
	 * columns.
	 */
	for (i = 0; i < index->indnatts; i++) {
		attname = get_attname(relid, index->indkey.values[i], true);
		p_info->column_names = lappend(p_info->column_names, attname);

		p_info->indcollation[i] = indexRelation->rd_indcollation[i];
		p_info->opclass[i] = indexRelation->rd_opcintype[i];
		p_info->indoption[i] = indexRelation->rd_indoption[i];
	}

	/*
	 * to check to match the expression's parameter of index with child indexes
	 */
	p_info->expression_str = NULL;
	if (!heap_attisnull(indexRelation->rd_indextuple, Anum_pg_index_indexprs,
		NULL)) {
		Datum       exprsDatum;
		bool		isnull;
		Datum		result;

		exprsDatum = SysCacheGetAttr(INDEXRELID, indexRelation->rd_indextuple,
			Anum_pg_index_indexprs, &isnull);

		result = DirectFunctionCall2(pg_get_expr,
			exprsDatum,
			ObjectIdGetDatum(relid));

		p_info->expression_str = text_to_cstring(DatumGetTextP(result));
	}

	/*
	 * to check to match the predicate's parameter of index with child indexes
	 */
	p_info->indpred_str = NULL;
	if (!heap_attisnull(indexRelation->rd_indextuple, Anum_pg_index_indpred,
		NULL)) {
		Datum       predDatum;
		bool		isnull;
		Datum		result;

		predDatum = SysCacheGetAttr(INDEXRELID, indexRelation->rd_indextuple,
			Anum_pg_index_indpred, &isnull);

		result = DirectFunctionCall2(pg_get_expr,
			predDatum,
			ObjectIdGetDatum(relid));

		p_info->indpred_str = text_to_cstring(DatumGetTextP(result));
	}

	index_close(indexRelation, NoLock);

	return p_info;
}

/*
 * cancel hint enforcement
 */
static void
reset_hint_enforcement()
{
	setup_scan_method_enforcement(NULL, current_hint_state);
	setup_parallel_plan_enforcement(NULL, current_hint_state);
}

/*
 * Set planner guc parameters according to corresponding scan hints.  Returns
 * bitmap of HintTypeBitmap. If shint or phint is not NULL, set used hint
 * there respectively.
 */
static int
setup_hint_enforcement(PlannerInfo* root, RelOptInfo* rel,
	ScanMethodHint** rshint, ParallelHint** rphint)
{
	Index	new_parent_relid = 0;
	ListCell* l;
	ScanMethodHint* shint = NULL;
	ParallelHint* phint = NULL;
	bool			inhparent = root->simple_rte_array[rel->relid]->inh;
	Oid		relationObjectId = root->simple_rte_array[rel->relid]->relid;
	int				ret = 0;

	/* reset returns if requested  */
	if (rshint != NULL) *rshint = NULL;
	if (rphint != NULL) *rphint = NULL;

	/*
	 * We could register the parent relation of the following children here
	 * when inhparent == true but inheritnce planner doesn't call this function
	 * for parents. Since we cannot distinguish who called this function we
	 * cannot do other than always seeking the parent regardless of who called
	 * this function.
	 */
	if (inhparent) {
		/* set up only parallel hints for parent relation */
		phint = find_parallel_hint(root, rel->relid);
		if (phint) {
			setup_parallel_plan_enforcement(phint, current_hint_state);
			if (rphint) *rphint = phint;
			ret |= HINT_BM_PARALLEL;
			return ret;
		}

		if (debug_level > 1)
			ereport(pg_hint_plan_debug_message_level,
				(errhidestmt(true),
					errmsg("pg_hint_plan%s: setup_hint_enforcement"
						" skipping inh parent: relation=%u(%s), inhparent=%d,"
						" current_hint_state=%p, hint_inhibit_level=%d",
						qnostr, relationObjectId,
						get_rel_name(relationObjectId),
						inhparent, current_hint_state, hint_inhibit_level)));
		return 0;
	}

	/*
	 * Forget about the parent of another subquery, but don't forget if the
	 * inhTargetkind of the root is not INHKIND_NONE, which signals the root
	 * contains only appendrel members. See inheritance_planner for details.
	 *
	 * (PG12.0) 428b260f87 added one more planning cycle for updates on
	 * partitioned tables and hints set up in the cycle are overriden by the
	 * second cycle. Since I didn't find no apparent distinction between the
	 * PlannerRoot of the cycle and that of ordinary CMD_SELECT, pg_hint_plan
	 * accepts both cycles and the later one wins. In the second cycle root
	 * doesn't have inheritance information at all so use the parent_relid set
	 * in the first cycle.
	 */
	if (root->inhTargetKind == INHKIND_NONE) {
		if (root != current_hint_state->current_root)
			current_hint_state->parent_relid = 0;

		/* Find the parent for this relation other than the registered parent */
		foreach(l, root->append_rel_list)
		{
			AppendRelInfo* appinfo = (AppendRelInfo*)lfirst(l);

			if (appinfo->child_relid == rel->relid) {
				if (current_hint_state->parent_relid != appinfo->parent_relid) {
					new_parent_relid = appinfo->parent_relid;
					current_hint_state->current_root = root;
				}
				break;
			}
		}

		if (!l) {
			/*
			 * This relation doesn't have a parent. Cancel
			 * current_hint_state.
			 */
			current_hint_state->parent_relid = 0;
			current_hint_state->parent_scan_hint = NULL;
			current_hint_state->parent_parallel_hint = NULL;
		}
	}

	if (new_parent_relid > 0) {
		/*
		 * Here we found a new parent for the current relation. Scan continues
		 * hint to other childrens of this parent so remember it to avoid
		 * redundant setup cost.
		 */
		current_hint_state->parent_relid = new_parent_relid;

		/* Find hints for the parent */
		current_hint_state->parent_scan_hint =
			find_scan_hint(root, current_hint_state->parent_relid);

		current_hint_state->parent_parallel_hint =
			find_parallel_hint(root, current_hint_state->parent_relid);

		/*
		 * If hint is found for the parent, apply it for this child instead
		 * of its own.
		 */
		if (current_hint_state->parent_scan_hint) {
			ScanMethodHint* pshint = current_hint_state->parent_scan_hint;

			pshint->base.state = HINT_STATE_USED;

			/* Apply index mask in the same manner to the parent. */
			if (pshint->indexnames) {
				Oid			parentrel_oid;
				Relation	parent_rel;

				parentrel_oid =
					root->simple_rte_array[current_hint_state->parent_relid]->relid;
				parent_rel = heap_open(parentrel_oid, NoLock);

				/* Search the parent relation for indexes match the hint spec */
				foreach(l, RelationGetIndexList(parent_rel))
				{
					Oid         indexoid = lfirst_oid(l);
					char* indexname = get_rel_name(indexoid);
					ListCell* lc;
					ParentIndexInfo* parent_index_info;

					foreach(lc, pshint->indexnames)
					{
						if (RelnameCmp(&indexname, &lfirst(lc)) == 0)
							break;
					}
					if (!lc)
						continue;

					parent_index_info =
						get_parent_index_info(indexoid, parentrel_oid);
					current_hint_state->parent_index_infos =
						lappend(current_hint_state->parent_index_infos,
							parent_index_info);
				}
				heap_close(parent_rel, NoLock);
			}
		}
	}

	shint = find_scan_hint(root, rel->relid);
	if (!shint)
		shint = current_hint_state->parent_scan_hint;

	if (shint) {
		bool using_parent_hint =
			(shint == current_hint_state->parent_scan_hint);

		ret |= HINT_BM_SCAN_METHOD;

		/* Setup scan enforcement environment */
		setup_scan_method_enforcement(shint, current_hint_state);

		/* restrict unwanted inexes */
		restrict_indexes(root, shint, rel, using_parent_hint);

		if (debug_level > 1) {
			char* additional_message = "";

			if (shint == current_hint_state->parent_scan_hint)
				additional_message = " by parent hint";

			ereport(pg_hint_plan_debug_message_level,
				(errhidestmt(true),
					errmsg("pg_hint_plan%s: setup_hint_enforcement"
						" index deletion%s:"
						" relation=%u(%s), inhparent=%d, "
						"current_hint_state=%p,"
						" hint_inhibit_level=%d, scanmask=0x%x",
						qnostr, additional_message,
						relationObjectId,
						get_rel_name(relationObjectId),
						inhparent, current_hint_state,
						hint_inhibit_level,
						shint->enforce_mask)));
		}
	}

	/* Do the same for parallel plan enforcement */
	phint = find_parallel_hint(root, rel->relid);
	if (!phint)
		phint = current_hint_state->parent_parallel_hint;

	setup_parallel_plan_enforcement(phint, current_hint_state);

	if (phint)
		ret |= HINT_BM_PARALLEL;

	/* Nothing to apply. Reset the scan mask to intial state */
	if (!shint && !phint) {
		if (debug_level > 1)
			ereport(pg_hint_plan_debug_message_level,
				(errhidestmt(true),
					errmsg("pg_hint_plan%s: setup_hint_enforcement"
						" no hint applied:"
						" relation=%u(%s), inhparent=%d, current_hint=%p,"
						" hint_inhibit_level=%d, scanmask=0x%x",
						qnostr, relationObjectId,
						get_rel_name(relationObjectId),
						inhparent, current_hint_state, hint_inhibit_level,
						current_hint_state->init_scan_mask)));

		setup_scan_method_enforcement(NULL, current_hint_state);

		return ret;
	}

	if (rshint != NULL) *rshint = shint;
	if (rphint != NULL) *rphint = phint;

	return ret;
}

/*
 * Return index of relation which matches given aliasname, or 0 if not found.
 * If same aliasname was used multiple times in a query, return -1.
 */
static int
find_relid_aliasname(PlannerInfo* root, char* aliasname, List* initial_rels,
	const char* str)
{
	int		i;
	Index	found = 0;

	for (i = 1; i < root->simple_rel_array_size; i++) {
		ListCell* l;

		if (root->simple_rel_array[i] == NULL)
			continue;

		Assert(i == root->simple_rel_array[i]->relid);

		if (RelnameCmp(&aliasname,
			&root->simple_rte_array[i]->eref->aliasname) != 0)
			continue;

		foreach(l, initial_rels)
		{
			RelOptInfo* rel = (RelOptInfo*)lfirst(l);

			if (rel->reloptkind == RELOPT_BASEREL) {
				if (rel->relid != i)
					continue;
			}
			else {
				Assert(rel->reloptkind == RELOPT_JOINREL);

				if (!bms_is_member(i, rel->relids))
					continue;
			}

			if (found != 0) {
				hint_ereport(str,
					("Relation name \"%s\" is ambiguous.",
						aliasname));
				return -1;
			}

			found = i;
			break;
		}

	}

	return found;
}

/*
 * Return join hint which matches given joinrelids.
 */
static JoinMethodHint*
find_join_hint(Relids joinrelids)
{
	List* join_hint;
	ListCell* l;

	join_hint = current_hint_state->join_hint_level[bms_num_members(joinrelids)];

	foreach(l, join_hint)
	{
		JoinMethodHint* hint = (JoinMethodHint*)lfirst(l);

		if (bms_equal(joinrelids, hint->joinrelids))
			return hint;
	}

	return NULL;
}

static Relids
OuterInnerJoinCreate(OuterInnerRels* outer_inner, LeadingHint* leading_hint,
	PlannerInfo* root, List* initial_rels, HintState* hstate, int nbaserel)
{
	OuterInnerRels* outer_rels;
	OuterInnerRels* inner_rels;
	Relids			outer_relids;
	Relids			inner_relids;
	Relids			join_relids;
	JoinMethodHint* hint;

	if (outer_inner->relation != NULL) {
		return bms_make_singleton(
			find_relid_aliasname(root, outer_inner->relation,
				initial_rels,
				leading_hint->base.hint_str));
	}

	outer_rels = lfirst(outer_inner->outer_inner_pair->head);
	inner_rels = lfirst(outer_inner->outer_inner_pair->tail);

	outer_relids = OuterInnerJoinCreate(outer_rels,
		leading_hint,
		root,
		initial_rels,
		hstate,
		nbaserel);
	inner_relids = OuterInnerJoinCreate(inner_rels,
		leading_hint,
		root,
		initial_rels,
		hstate,
		nbaserel);

	join_relids = bms_add_members(outer_relids, inner_relids);

	if (bms_num_members(join_relids) > nbaserel)
		return join_relids;

	/*
	 * If we don't have join method hint, create new one for the
	 * join combination with all join methods are enabled.
	 */
	hint = find_join_hint(join_relids);
	if (hint == NULL) {
		/*
		 * Here relnames is not set, since Relids bitmap is sufficient to
		 * control paths of this query afterward.
		 */
		hint = (JoinMethodHint*)JoinMethodHintCreate(
			leading_hint->base.hint_str,
			HINT_LEADING,
			HINT_KEYWORD_LEADING);
		hint->base.state = HINT_STATE_USED;
		hint->nrels = bms_num_members(join_relids);
		hint->enforce_mask = ENABLE_ALL_JOIN;
		hint->joinrelids = bms_copy(join_relids);
		hint->inner_nrels = bms_num_members(inner_relids);
		hint->inner_joinrelids = bms_copy(inner_relids);

		hstate->join_hint_level[hint->nrels] =
			lappend(hstate->join_hint_level[hint->nrels], hint);
	}
	else {
		hint->inner_nrels = bms_num_members(inner_relids);
		hint->inner_joinrelids = bms_copy(inner_relids);
	}

	return join_relids;
}

static Relids
create_bms_of_relids(Hint* base, PlannerInfo* root, List* initial_rels,
	int nrels, char** relnames)
{
	int		relid;
	Relids	relids = NULL;
	int		j;
	char* relname;

	for (j = 0; j < nrels; j++) {
		relname = relnames[j];

		relid = find_relid_aliasname(root, relname, initial_rels,
			base->hint_str);

		if (relid == -1)
			base->state = HINT_STATE_ERROR;

		/*
		 * the aliasname is not found(relid == 0) or same aliasname was used
		 * multiple times in a query(relid == -1)
		 */
		if (relid <= 0) {
			relids = NULL;
			break;
		}
		if (bms_is_member(relid, relids)) {
			hint_ereport(base->hint_str,
				("Relation name \"%s\" is duplicated.", relname));
			base->state = HINT_STATE_ERROR;
			break;
		}

		relids = bms_add_member(relids, relid);
	}
	return relids;
}
/*
 * Transform join method hint into handy form.
 *
 *   - create bitmap of relids from alias names, to make it easier to check
 *     whether a join path matches a join method hint.
 *   - add join method hints which are necessary to enforce join order
 *     specified by Leading hint
 */
static bool
transform_join_hints(HintState* hstate, PlannerInfo* root, int nbaserel,
	List* initial_rels, JoinMethodHint** join_method_hints)
{
	int				i;
	int				relid;
	Relids			joinrelids;
	int				njoinrels;
	ListCell* l;
	char* relname;
	LeadingHint* lhint = NULL;

	/*
	 * Create bitmap of relids from alias names for each join method hint.
	 * Bitmaps are more handy than strings in join searching.
	 */
	for (i = 0; i < hstate->num_hints[HINT_TYPE_JOIN_METHOD]; i++) {
		JoinMethodHint* hint = hstate->join_hints[i];

		if (!hint_state_enabled(hint) || hint->nrels > nbaserel)
			continue;

		hint->joinrelids = create_bms_of_relids(&(hint->base), root,
			initial_rels, hint->nrels, hint->relnames);

		if (hint->joinrelids == NULL || hint->base.state == HINT_STATE_ERROR)
			continue;

		hstate->join_hint_level[hint->nrels] =
			lappend(hstate->join_hint_level[hint->nrels], hint);
	}

	/*
	 * Create bitmap of relids from alias names for each rows hint.
	 * Bitmaps are more handy than strings in join searching.
	 */
	for (i = 0; i < hstate->num_hints[HINT_TYPE_ROWS]; i++) {
		RowsHint* hint = hstate->rows_hints[i];

		if (!hint_state_enabled(hint) || hint->nrels > nbaserel)
			continue;

		hint->joinrelids = create_bms_of_relids(&(hint->base), root,
			initial_rels, hint->nrels, hint->relnames);
	}

	/* Do nothing if no Leading hint was supplied. */
	if (hstate->num_hints[HINT_TYPE_LEADING] == 0)
		return false;

	/*
	 * Decide whether to use Leading hint
	 */
	for (i = 0; i < hstate->num_hints[HINT_TYPE_LEADING]; i++) {
		LeadingHint* leading_hint = (LeadingHint*)hstate->leading_hint[i];
		Relids			relids;

		if (leading_hint->base.state == HINT_STATE_ERROR)
			continue;

		relid = 0;
		relids = NULL;

		foreach(l, leading_hint->relations)
		{
			relname = (char*)lfirst(l);;

			relid = find_relid_aliasname(root, relname, initial_rels,
				leading_hint->base.hint_str);
			if (relid == -1)
				leading_hint->base.state = HINT_STATE_ERROR;

			if (relid <= 0)
				break;

			if (bms_is_member(relid, relids)) {
				hint_ereport(leading_hint->base.hint_str,
					("Relation name \"%s\" is duplicated.", relname));
				leading_hint->base.state = HINT_STATE_ERROR;
				break;
			}

			relids = bms_add_member(relids, relid);
		}

		if (relid <= 0 || leading_hint->base.state == HINT_STATE_ERROR)
			continue;

		if (lhint != NULL) {
			hint_ereport(lhint->base.hint_str,
				("Conflict %s hint.", HintTypeName[lhint->base.type]));
			lhint->base.state = HINT_STATE_DUPLICATION;
		}
		leading_hint->base.state = HINT_STATE_USED;
		lhint = leading_hint;
	}

	/* check to exist Leading hint marked with 'used'. */
	if (lhint == NULL)
		return false;

	/*
	 * We need join method hints which fit specified join order in every join
	 * level.  For example, Leading(A B C) virtually requires following join
	 * method hints, if no join method hint supplied:
	 *   - level 1: none
	 *   - level 2: NestLoop(A B), MergeJoin(A B), HashJoin(A B)
	 *   - level 3: NestLoop(A B C), MergeJoin(A B C), HashJoin(A B C)
	 *
	 * If we already have join method hint which fits specified join order in
	 * that join level, we leave it as-is and don't add new hints.
	 */
	joinrelids = NULL;
	njoinrels = 0;
	if (lhint->outer_inner == NULL) {
		foreach(l, lhint->relations)
		{
			JoinMethodHint* hint;

			relname = (char*)lfirst(l);

			/*
			 * Find relid of the relation which has given name.  If we have the
			 * name given in Leading hint multiple times in the join, nothing to
			 * do.
			 */
			relid = find_relid_aliasname(root, relname, initial_rels,
				hstate->hint_str);

			/* Create bitmap of relids for current join level. */
			joinrelids = bms_add_member(joinrelids, relid);
			njoinrels++;

			/* We never have join method hint for single relation. */
			if (njoinrels < 2)
				continue;

			/*
			 * If we don't have join method hint, create new one for the
			 * join combination with all join methods are enabled.
			 */
			hint = find_join_hint(joinrelids);
			if (hint == NULL) {
				/*
				 * Here relnames is not set, since Relids bitmap is sufficient
				 * to control paths of this query afterward.
				 */
				hint = (JoinMethodHint*)JoinMethodHintCreate(
					lhint->base.hint_str,
					HINT_LEADING,
					HINT_KEYWORD_LEADING);
				hint->base.state = HINT_STATE_USED;
				hint->nrels = njoinrels;
				hint->enforce_mask = ENABLE_ALL_JOIN;
				hint->joinrelids = bms_copy(joinrelids);
			}

			join_method_hints[njoinrels] = hint;

			if (njoinrels >= nbaserel)
				break;
		}
		bms_free(joinrelids);

		if (njoinrels < 2)
			return false;

		/*
		 * Delete all join hints which have different combination from Leading
		 * hint.
		 */
		for (i = 2; i <= njoinrels; i++) {
			list_free(hstate->join_hint_level[i]);

			hstate->join_hint_level[i] = lappend(NIL, join_method_hints[i]);
		}
	}
	else {
		joinrelids = OuterInnerJoinCreate(lhint->outer_inner,
			lhint,
			root,
			initial_rels,
			hstate,
			nbaserel);

		njoinrels = bms_num_members(joinrelids);
		Assert(njoinrels >= 2);

		/*
		 * Delete all join hints which have different combination from Leading
		 * hint.
		 */
		for (i = 2;i <= njoinrels; i++) {
			if (hstate->join_hint_level[i] != NIL) {
				ListCell* prev = NULL;
				ListCell* next = NULL;
				for (l = list_head(hstate->join_hint_level[i]); l; l = next) {

					JoinMethodHint* hint = (JoinMethodHint*)lfirst(l);

					next = lnext(l);

					if (hint->inner_nrels == 0 &&
						!(bms_intersect(hint->joinrelids, joinrelids) == NULL ||
							bms_equal(bms_union(hint->joinrelids, joinrelids),
								hint->joinrelids))) {
						hstate->join_hint_level[i] =
							list_delete_cell(hstate->join_hint_level[i], l,
								prev);
					}
					else
						prev = l;
				}
			}
		}

		bms_free(joinrelids);
	}

	if (hint_state_enabled(lhint)) {
		set_join_config_options(DISABLE_ALL_JOIN, current_hint_state->context);
		return true;
	}
	return false;
}

/*
 * wrapper of make_join_rel()
 *
 * call make_join_rel() after changing enable_* parameters according to given
 * hints.
 */
static RelOptInfo*
make_join_rel_wrapper(PlannerInfo* root, RelOptInfo* rel1, RelOptInfo* rel2)
{
	Relids			joinrelids;
	JoinMethodHint* hint;
	RelOptInfo* rel;
	int				save_nestlevel;

	joinrelids = bms_union(rel1->relids, rel2->relids);
	hint = find_join_hint(joinrelids);
	bms_free(joinrelids);

	if (!hint)
		return pg_hint_plan_make_join_rel(root, rel1, rel2);

	if (hint->inner_nrels == 0) {
		save_nestlevel = NewGUCNestLevel();

		set_join_config_options(hint->enforce_mask,
			current_hint_state->context);

		rel = pg_hint_plan_make_join_rel(root, rel1, rel2);
		hint->base.state = HINT_STATE_USED;

		/*
		 * Restore the GUC variables we set above.
		 */
		AtEOXact_GUC(true, save_nestlevel);
	}
	else
		rel = pg_hint_plan_make_join_rel(root, rel1, rel2);

	return rel;
}

/*
 * TODO : comment
 */
static void
add_paths_to_joinrel_wrapper(PlannerInfo* root,
	RelOptInfo* joinrel,
	RelOptInfo* outerrel,
	RelOptInfo* innerrel,
	JoinType jointype,
	SpecialJoinInfo* sjinfo,
	List* restrictlist)
{
	Relids			joinrelids;
	JoinMethodHint* join_hint;
	int				save_nestlevel;

	joinrelids = bms_union(outerrel->relids, innerrel->relids);
	join_hint = find_join_hint(joinrelids);
	bms_free(joinrelids);

	if (join_hint && join_hint->inner_nrels != 0) {
		save_nestlevel = NewGUCNestLevel();

		if (bms_equal(join_hint->inner_joinrelids, innerrel->relids)) {

			set_join_config_options(join_hint->enforce_mask,
				current_hint_state->context);

			add_paths_to_joinrel(root, joinrel, outerrel, innerrel, jointype,
				sjinfo, restrictlist);
			join_hint->base.state = HINT_STATE_USED;
		}
		else {
			set_join_config_options(DISABLE_ALL_JOIN,
				current_hint_state->context);
			add_paths_to_joinrel(root, joinrel, outerrel, innerrel, jointype,
				sjinfo, restrictlist);
		}

		/*
		 * Restore the GUC variables we set above.
		 */
		AtEOXact_GUC(true, save_nestlevel);
	}
	else
		add_paths_to_joinrel(root, joinrel, outerrel, innerrel, jointype,
			sjinfo, restrictlist);
}

static int
get_num_baserels(List* initial_rels)
{
	int			nbaserel = 0;
	ListCell* l;

	foreach(l, initial_rels)
	{
		RelOptInfo* rel = (RelOptInfo*)lfirst(l);

		if (rel->reloptkind == RELOPT_BASEREL)
			nbaserel++;
		else if (rel->reloptkind == RELOPT_JOINREL)
			nbaserel += bms_num_members(rel->relids);
		else {
			/* other values not expected here */
			elog(ERROR, "unrecognized reloptkind type: %d", rel->reloptkind);
		}
	}

	return nbaserel;
}

static RelOptInfo*
pg_hint_plan_join_search(PlannerInfo* root, int levels_needed,
	List* initial_rels)
{
	JoinMethodHint** join_method_hints;
	int					nbaserel;
	RelOptInfo* rel;
	int					i;
	bool				leading_hint_enable;

	/*
	 * Use standard planner (or geqo planner) if pg_hint_plan is disabled or no
	 * valid hint is supplied or current nesting depth is nesting depth of SPI
	 * calls.
	 */
	if (!current_hint_state || hint_inhibit_level > 0) {
		if (prev_join_search)
			return (*prev_join_search) (root, levels_needed, initial_rels);
		else if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		else
			return standard_join_search(root, levels_needed, initial_rels);
	}

	/*
	 * In the case using GEQO, only scan method hints and Set hints have
	 * effect.  Join method and join order is not controllable by hints.
	 */
	if (enable_geqo && levels_needed >= geqo_threshold)
		return geqo(root, levels_needed, initial_rels);

	nbaserel = get_num_baserels(initial_rels);
	current_hint_state->join_hint_level =
		palloc0(sizeof(List*) * (nbaserel + 1));
	join_method_hints = palloc0(sizeof(JoinMethodHint*) * (nbaserel + 1));

	leading_hint_enable = transform_join_hints(current_hint_state,
		root, nbaserel,
		initial_rels, join_method_hints);

	rel = pg_hint_plan_standard_join_search(root, levels_needed, initial_rels);

	/*
	 * Adjust number of parallel workers of the result rel to the largest
	 * number of the component paths.
	 */
	if (current_hint_state->num_hints[HINT_TYPE_PARALLEL] > 0) {
		ListCell* lc;
		int 		nworkers = 0;

		foreach(lc, initial_rels)
		{
			ListCell* lcp;
			RelOptInfo* initrel = (RelOptInfo*)lfirst(lc);

			foreach(lcp, initrel->partial_pathlist)
			{
				Path* path = (Path*)lfirst(lcp);

				if (nworkers < path->parallel_workers)
					nworkers = path->parallel_workers;
			}
		}

		foreach(lc, rel->partial_pathlist)
		{
			Path* path = (Path*)lfirst(lc);

			if (path->parallel_safe && path->parallel_workers < nworkers)
				path->parallel_workers = nworkers;
		}
	}

	for (i = 2; i <= nbaserel; i++) {
		list_free(current_hint_state->join_hint_level[i]);

		/* free Leading hint only */
		if (join_method_hints[i] != NULL &&
			join_method_hints[i]->enforce_mask == ENABLE_ALL_JOIN)
			JoinMethodHintDelete(join_method_hints[i]);
	}
	pfree(current_hint_state->join_hint_level);
	pfree(join_method_hints);

	if (leading_hint_enable)
		set_join_config_options(current_hint_state->init_join_mask,
			current_hint_state->context);

	return rel;
}

/*
 * Force number of wokers if instructed by hint
 */
void
pg_hint_plan_set_rel_pathlist(PlannerInfo* root, RelOptInfo* rel,
	Index rti, RangeTblEntry* rte)
{
	ParallelHint* phint;
	ListCell* l;
	int				found_hints;

	/* call the previous hook */
	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	/* Nothing to do if no hint available */
	if (current_hint_state == NULL)
		return;

	/* Don't touch dummy rels. */
	if (IS_DUMMY_REL(rel))
		return;

	/*
	 * We can accept only plain relations, foreign tables and table saples are
	 * also unacceptable. See set_rel_pathlist.
	 */
	if ((rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY) ||
		rte->relkind == RELKIND_FOREIGN_TABLE ||
		rte->tablesample != NULL)
		return;

	/*
	 * Even though UNION ALL node doesn't have particular name so usually it is
	 * unhintable, turn on parallel when it contains parallel nodes.
	 */
	if (rel->rtekind == RTE_SUBQUERY) {
		ListCell* lc;
		bool	inhibit_nonparallel = false;

		if (rel->partial_pathlist == NIL)
			return;

		foreach(lc, rel->partial_pathlist)
		{
			ListCell* lcp;
			AppendPath* apath = (AppendPath*)lfirst(lc);
			int		parallel_workers = 0;

			if (!IsA(apath, AppendPath))
				continue;

			foreach(lcp, apath->subpaths)
			{
				Path* spath = (Path*)lfirst(lcp);

				if (spath->parallel_aware &&
					parallel_workers < spath->parallel_workers)
					parallel_workers = spath->parallel_workers;
			}

			apath->path.parallel_workers = parallel_workers;
			inhibit_nonparallel = true;
		}

		if (inhibit_nonparallel) {
			ListCell* lcr;

			foreach(lcr, rel->pathlist)
			{
				Path* path = (Path*)lfirst(lcr);

				if (path->startup_cost < disable_cost) {
					path->startup_cost += disable_cost;
					path->total_cost += disable_cost;
				}
			}
		}

		return;
	}

	/* We cannot handle if this requires an outer */
	if (rel->lateral_relids)
		return;

	/* Return if this relation gets no enfocement */
	if ((found_hints = setup_hint_enforcement(root, rel, NULL, &phint)) == 0)
		return;

	/* Here, we regenerate paths with the current hint restriction */
	if (found_hints & HINT_BM_SCAN_METHOD || found_hints & HINT_BM_PARALLEL) {
		/*
		 * When hint is specified on non-parent relations, discard existing
		 * paths and regenerate based on the hint considered. Otherwise we
		 * already have hinted childx paths then just adjust the number of
		 * planned number of workers.
		 */
		if (root->simple_rte_array[rel->relid]->inh) {
			/* enforce number of workers if requested */
			if (phint && phint->force_parallel) {
				if (phint->nworkers == 0) {
					list_free_deep(rel->partial_pathlist);
					rel->partial_pathlist = NIL;
				}
				else {
					/* prioritize partial paths */
					foreach(l, rel->partial_pathlist)
					{
						Path* ppath = (Path*)lfirst(l);

						if (ppath->parallel_safe) {
							ppath->parallel_workers = phint->nworkers;
							ppath->startup_cost = 0;
							ppath->total_cost = 0;
						}
					}

					/* disable non-partial paths */
					foreach(l, rel->pathlist)
					{
						Path* ppath = (Path*)lfirst(l);

						if (ppath->startup_cost < disable_cost) {
							ppath->startup_cost += disable_cost;
							ppath->total_cost += disable_cost;
						}
					}
				}
			}
		}
		else {
			/* Just discard all the paths considered so far */
			list_free_deep(rel->pathlist);
			rel->pathlist = NIL;
			list_free_deep(rel->partial_pathlist);
			rel->partial_pathlist = NIL;

			/* Regenerate paths with the current enforcement */
			set_plain_rel_pathlist(root, rel, rte);

			/* Additional work to enforce parallel query execution */
			if (phint && phint->nworkers > 0) {
				/*
				 * For Parallel Append to be planned properly, we shouldn't set
				 * the costs of non-partial paths to disable-value.  Lower the
				 * priority of non-parallel paths by setting partial path costs
				 * to 0 instead.
				 */
				foreach(l, rel->partial_pathlist)
				{
					Path* path = (Path*)lfirst(l);

					path->startup_cost = 0;
					path->total_cost = 0;
				}

				/* enforce number of workers if requested */
				if (phint->force_parallel) {
					foreach(l, rel->partial_pathlist)
					{
						Path* ppath = (Path*)lfirst(l);

						if (ppath->parallel_safe)
							ppath->parallel_workers = phint->nworkers;
					}
				}

				/* Generate gather paths */
				if (rel->reloptkind == RELOPT_BASEREL &&
					bms_membership(root->all_baserels) != BMS_SINGLETON)
					generate_gather_paths(root, rel, false);
			}
		}
	}

	reset_hint_enforcement();
}

/*
 * set_rel_pathlist
 *	  Build access paths for a base relation
 *
 * This function was copied and edited from set_rel_pathlist() in
 * src/backend/optimizer/path/allpaths.c in order not to copy other static
 * functions not required here.
 */
static void
set_rel_pathlist(PlannerInfo* root, RelOptInfo* rel,
	Index rti, RangeTblEntry* rte)
{
	if (IS_DUMMY_REL(rel)) {
		/* We already proved the relation empty, so nothing more to do */
	}
	else if (rte->inh) {
		/* It's an "append relation", process accordingly */
		set_append_rel_pathlist(root, rel, rti, rte);
	}
	else {
		if (rel->rtekind == RTE_RELATION) {
			if (rte->relkind == RELKIND_RELATION) {
				if (rte->tablesample != NULL)
					elog(ERROR, "sampled relation is not supported");

				/* Plain relation */
				set_plain_rel_pathlist(root, rel, rte);
			}
			else
				elog(ERROR, "unexpected relkind: %c", rte->relkind);
		}
		else
			elog(ERROR, "unexpected rtekind: %d", (int)rel->rtekind);
	}

	/*
	 * Allow a plugin to editorialize on the set of Paths for this base
	 * relation.  It could add new paths (such as CustomPaths) by calling
	 * add_path(), or delete or modify paths added by the core code.
	 */
	if (set_rel_pathlist_hook)
		(*set_rel_pathlist_hook) (root, rel, rti, rte);

	/* Now find the cheapest of the paths for this rel */
	set_cheapest(rel);
}

/*
 * stmt_beg callback is called when each query in PL/pgSQL function is about
 * to be executed.  At that timing, we save query string in the global variable
 * plpgsql_query_string to use it in planner hook.  It's safe to use one global
 * variable for the purpose, because its content is only necessary until
 * planner hook is called for the query, so recursive PL/pgSQL function calls
 * don't harm this mechanism.
 */
static void
pg_hint_plan_plpgsql_stmt_beg(PLpgSQL_execstate* estate, PLpgSQL_stmt* stmt)
{
	plpgsql_recurse_level++;
}

/*
 * stmt_end callback is called then each query in PL/pgSQL function has
 * finished.  At that timing, we clear plpgsql_query_string to tell planner
 * hook that next call is not for a query written in PL/pgSQL block.
 */
static void
pg_hint_plan_plpgsql_stmt_end(PLpgSQL_execstate* estate, PLpgSQL_stmt* stmt)
{

	/*
	 * If we come here, we should have gone through the statement begin
	 * callback at least once.
	 */
	if (plpgsql_recurse_level > 0)
		plpgsql_recurse_level--;
}

void plpgsql_query_erase_callback(ResourceReleasePhase phase,
	bool isCommit,
	bool isTopLevel,
	void* arg)
{
	/* Cleanup is just applied once all the locks are released */
	if (phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	if (isTopLevel) {
		/* Cancel recurse level */
		plpgsql_recurse_level = 0;
	}
	else if (plpgsql_recurse_level > 0) {
		/*
		 * This applies when a transaction is aborted for a PL/pgSQL query,
		 * like when a transaction triggers an exception, or for an internal
		 * commit.
		 */
		plpgsql_recurse_level--;
	}
}

#define standard_join_search pg_hint_plan_standard_join_search
#define join_search_one_level pg_hint_plan_join_search_one_level
#define make_join_rel make_join_rel_wrapper
#include "core.c"

#undef make_join_rel
#define make_join_rel pg_hint_plan_make_join_rel
#define add_paths_to_joinrel add_paths_to_joinrel_wrapper
#include "make_join_rel.c"

#include "pg_stat_statements.c"
