/*-------------------------------------------------------------------------
 *
 * pg_stat_statements.c
 *
 * PostgreSQL 10 中 pg_stat_statements.c 的一部分。
 *
 * Copyright (c) 2008-2020, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/hash.h"
#include "parser/scanner.h"

static void AppendJumble(pgssJumbleState* jstate,
	const unsigned char* item, Size size);
static void JumbleQuery(pgssJumbleState* jstate, Query* query);
static void JumbleRangeTable(pgssJumbleState* jstate, List* rtable);
static void JumbleExpr(pgssJumbleState* jstate, Node* node);
static void RecordConstLocation(pgssJumbleState* jstate, int location);
static char* generate_normalized_query(pgssJumbleState* jstate, const char* query,
	int query_loc, int* query_len_p, int encoding);
static void fill_in_constant_lengths(pgssJumbleState* jstate, const char* query,
	int query_loc);
static int	comp_location(const void* a, const void* b);

/*
 * AppendJumble: 将给定查询中有实质意义的值追加到当前的杂凑（jumble）中。
 */
static void
AppendJumble(pgssJumbleState* jstate, const unsigned char* item, Size size)
{
	unsigned char* jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * 每当杂凑（jumble）缓冲区满时，我们对当前内容进行哈希处理，
	 * 并重置缓冲区使其仅包含该哈希值，从而依赖哈希值来总结迄今为止的全部内容。
	 */
	while (size > 0) {
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE) {
			uint64		start_hash;

			start_hash = DatumGetUInt64(hash_any_extended(jumble,
				JUMBLE_SIZE, 0));
			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;
	}
	jstate->jumble_len = jumble_len;
}

/*
 * AppendJumble 的包装器，用于封装单个局部变量元素的序列化细节。
 */
#define APP_JUMB(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define APP_JUMB_STRING(str) \
	AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1)

 /*
 * 核心查询杂凑 (Jumble) 序列化入口：为 Query 树提取“骨架指纹”。
 *
 * 【工作原理与使用场景】
 * 顾名思义，“Jumble”的作用是去掉 SQL 中表面的血肉（如常量值、大小写、表别名、多余空格），
 * 仅仅提取其不可变的“结构骨架”。提取到的核心内容会被依次追加到 jstate->jumble 的字节序列中，
 * 最终用这个序列可以算出一个哈希值，作为该 SQL 结构的唯一指纹。
 * 在 pg_hint_plan 中，这条流水线被用来匹配 Hint Table 里的归一化 SQL（即把所有带有硬编码
 * 参数的 SQL 映射到一张统一的计划模版里）。
 *
 * 提取法则：
 * 1. 抓取本质：只进入和序列化真正影响执行计划与逻辑语义的核心块——例如 commandType (命令类型),
 *    jointree (FROM 与 WHERE 连接树), targetList (SELECT 目标列), sortClause (ORDER BY) 等。
 * 2. 丢弃表象：强制忽略任何语义上无关紧要的部分（如 AS 别名），以及可以由子节点自行推导出的
 *    顶层包装信息（防止父节点和子节点做双重哈希污染字节流）。
 * 3. 剥离常量：在后续层层递归的时候，一旦探底遇到 Const (常量) 节点，决不能记录它的实际 Value，
 *    只保留它的类型（consttype），并将其在原始 SQL 文本中的偏移量（location）记录进 jstate->clocations
 *    数组中。之后，归一化函数就会根据这些偏移量，把原文本中的数值全部抠掉并换上 '?'。
 *
 * 示例 A：参数不同的同构查询（殊途同归）
 *   场景：客户端 A 下发 `SELECT * FROM users WHERE age > 18;`
 *        客户端 B 下发 `SELECT * FROM users WHERE age > 65;`
 *   推演：它们会被 Parse 阶段解析出结构相同的 Query 树。当经过本函数时，由于 jointree 结构一致，
 *        且底层的 18 和 65 被当作透明的节点只记录偏移而忽略值，所以最终吐出的 jstate->jumble
 *        字节序列完全如出一辙。它们会命中 Hint Table 里同一条针对 `SELECT * FROM users WHERE age > ?` 的规则。
 *
 * 示例 B：含细微结构性差别的查询（严格区分）
 *   场景：客户端 C 下发 `SELECT * FROM users WHERE age > 18 ORDER BY age;`
 *   推演：依然与上面的表和条件相同，但由于多了一句排序，Query 树中的 query->sortClause 不再为空。
 *        代码进行到 `JumbleExpr(..., query->sortClause)` 时会产生显著不同的串行化动作。最终
 *        其指纹与前两者彻底隔离，保证不会乱套（错套 Hint）。
 */
static void
JumbleQuery(pgssJumbleState* jstate, Query* query)
{
	Assert(IsA(query, Query));
	Assert(query->utilityStmt == NULL);

	APP_JUMB(query->commandType);
	/* resultRelation 通常可以根据 commandType 预测 */
	JumbleExpr(jstate, (Node*)query->cteList);
	JumbleRangeTable(jstate, query->rtable);
	JumbleExpr(jstate, (Node*)query->jointree);
	JumbleExpr(jstate, (Node*)query->targetList);
	JumbleExpr(jstate, (Node*)query->onConflict);
	JumbleExpr(jstate, (Node*)query->returningList);
	JumbleExpr(jstate, (Node*)query->groupClause);
	JumbleExpr(jstate, (Node*)query->groupingSets);
	JumbleExpr(jstate, query->havingQual);
	JumbleExpr(jstate, (Node*)query->windowClause);
	JumbleExpr(jstate, (Node*)query->distinctClause);
	JumbleExpr(jstate, (Node*)query->sortClause);
	JumbleExpr(jstate, query->limitOffset);
	JumbleExpr(jstate, query->limitCount);
	/* 我们忽略 rowMarks */
	JumbleExpr(jstate, query->setOperations);
}

/*
 * 杂凑范围表
 */
static void
JumbleRangeTable(pgssJumbleState* jstate, List* rtable)
{
	ListCell* lc;

	foreach(lc, rtable)
	{
		RangeTblEntry* rte = lfirst_node(RangeTblEntry, lc);

		APP_JUMB(rte->rtekind);
		switch (rte->rtekind) {
		case RTE_RELATION:
			APP_JUMB(rte->relid);
			JumbleExpr(jstate, (Node*)rte->tablesample);
			break;
		case RTE_SUBQUERY:
			JumbleQuery(jstate, rte->subquery);
			break;
		case RTE_JOIN:
			APP_JUMB(rte->jointype);
			break;
		case RTE_FUNCTION:
			JumbleExpr(jstate, (Node*)rte->functions);
			break;
		case RTE_TABLEFUNC:
			JumbleExpr(jstate, (Node*)rte->tablefunc);
			break;
		case RTE_VALUES:
			JumbleExpr(jstate, (Node*)rte->values_lists);
			break;
		case RTE_CTE:

			/*
			 * 在这里依赖 CTE 名称并不理想，但这是我们识别引用的 WITH 项的唯一信息。
			 */
			APP_JUMB_STRING(rte->ctename);
			APP_JUMB(rte->ctelevelsup);
			break;
		case RTE_NAMEDTUPLESTORE:
			APP_JUMB_STRING(rte->enrname);
			break;
		case RTE_RESULT:
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int)rte->rtekind);
			break;
		}
	}
}

/*
 * 杂凑表达式树
 *
 * 通常，此函数应处理与 expression_tree_walker() 相同的所有节点类型，
 * 因此其代码尽可能与该函数保持并行。但是，由于我们仅在解析分析后立即
 * 对查询调用，因此不需要处理仅出现在规划阶段的节点类型。
 *
 * 注意：我们不直接使用 expression_tree_walker() 的原因是，该函数的
 * 目的是支持不关心大多数树节点类型的树遍历器，但在这里我们关心所有类型。
 * 我们应该对任何无法识别的节点类型发出警告。
 */
static void
JumbleExpr(pgssJumbleState* jstate, Node* node)
{
	ListCell* temp;

	if (node == NULL)
		return;

	/* 防止由于表达式过于复杂导致的栈溢出 */
	check_stack_depth();

	/*
	 * 我们总是先发出节点的 NodeTag，然后是任何被认为重要的附加字段，
	 * 最后递归处理任何子节点。
	 */
	APP_JUMB(node->type);

	switch (nodeTag(node)) {
	case T_Var:
	{
		Var* var = (Var*)node;

		APP_JUMB(var->varno);
		APP_JUMB(var->varattno);
		APP_JUMB(var->varlevelsup);
	}
	break;
	case T_Const:
	{
		Const* c = (Const*)node;

		/* 我们只对常量的类型进行杂凑，不处理其值 */
		APP_JUMB(c->consttype);
		/* 同时，记录其解析位置用于查询标准化 */
		RecordConstLocation(jstate, c->location);
	}
	break;
	case T_Param:
	{
		Param* p = (Param*)node;

		APP_JUMB(p->paramkind);
		APP_JUMB(p->paramid);
		APP_JUMB(p->paramtype);
		/* 同时，跟踪最高的外部参数 ID */
		if (p->paramkind == PARAM_EXTERN &&
			p->paramid > jstate->highest_extern_param_id)
			jstate->highest_extern_param_id = p->paramid;
	}
	break;
	case T_Aggref:
	{
		Aggref* expr = (Aggref*)node;

		APP_JUMB(expr->aggfnoid);
		JumbleExpr(jstate, (Node*)expr->aggdirectargs);
		JumbleExpr(jstate, (Node*)expr->args);
		JumbleExpr(jstate, (Node*)expr->aggorder);
		JumbleExpr(jstate, (Node*)expr->aggdistinct);
		JumbleExpr(jstate, (Node*)expr->aggfilter);
	}
	break;
	case T_GroupingFunc:
	{
		GroupingFunc* grpnode = (GroupingFunc*)node;

		JumbleExpr(jstate, (Node*)grpnode->refs);
	}
	break;
	case T_WindowFunc:
	{
		WindowFunc* expr = (WindowFunc*)node;

		APP_JUMB(expr->winfnoid);
		APP_JUMB(expr->winref);
		JumbleExpr(jstate, (Node*)expr->args);
		JumbleExpr(jstate, (Node*)expr->aggfilter);
	}
	break;
	case T_SubscriptingRef:
	{
		SubscriptingRef* sbsref = (SubscriptingRef*)node;

		JumbleExpr(jstate, (Node*)sbsref->refupperindexpr);
		JumbleExpr(jstate, (Node*)sbsref->reflowerindexpr);
		JumbleExpr(jstate, (Node*)sbsref->refexpr);
		JumbleExpr(jstate, (Node*)sbsref->refassgnexpr);
	}
	break;
	case T_FuncExpr:
	{
		FuncExpr* expr = (FuncExpr*)node;

		APP_JUMB(expr->funcid);
		JumbleExpr(jstate, (Node*)expr->args);
	}
	break;
	case T_NamedArgExpr:
	{
		NamedArgExpr* nae = (NamedArgExpr*)node;

		APP_JUMB(nae->argnumber);
		JumbleExpr(jstate, (Node*)nae->arg);
	}
	break;
	case T_OpExpr:
	case T_DistinctExpr:	/* 结构上等同于 OpExpr */
	case T_NullIfExpr:		/* 结构上等同于 OpExpr */
	{
		OpExpr* expr = (OpExpr*)node;

		APP_JUMB(expr->opno);
		JumbleExpr(jstate, (Node*)expr->args);
	}
	break;
	case T_ScalarArrayOpExpr:
	{
		ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

		APP_JUMB(expr->opno);
		APP_JUMB(expr->useOr);
		JumbleExpr(jstate, (Node*)expr->args);
	}
	break;
	case T_BoolExpr:
	{
		BoolExpr* expr = (BoolExpr*)node;

		APP_JUMB(expr->boolop);
		JumbleExpr(jstate, (Node*)expr->args);
	}
	break;
	case T_SubLink:
	{
		SubLink* sublink = (SubLink*)node;

		APP_JUMB(sublink->subLinkType);
		APP_JUMB(sublink->subLinkId);
		JumbleExpr(jstate, (Node*)sublink->testexpr);
		JumbleQuery(jstate, castNode(Query, sublink->subselect));
	}
	break;
	case T_FieldSelect:
	{
		FieldSelect* fs = (FieldSelect*)node;

		APP_JUMB(fs->fieldnum);
		JumbleExpr(jstate, (Node*)fs->arg);
	}
	break;
	case T_FieldStore:
	{
		FieldStore* fstore = (FieldStore*)node;

		JumbleExpr(jstate, (Node*)fstore->arg);
		JumbleExpr(jstate, (Node*)fstore->newvals);
	}
	break;
	case T_RelabelType:
	{
		RelabelType* rt = (RelabelType*)node;

		APP_JUMB(rt->resulttype);
		JumbleExpr(jstate, (Node*)rt->arg);
	}
	break;
	case T_CoerceViaIO:
	{
		CoerceViaIO* cio = (CoerceViaIO*)node;

		APP_JUMB(cio->resulttype);
		JumbleExpr(jstate, (Node*)cio->arg);
	}
	break;
	case T_ArrayCoerceExpr:
	{
		ArrayCoerceExpr* acexpr = (ArrayCoerceExpr*)node;

		APP_JUMB(acexpr->resulttype);
		JumbleExpr(jstate, (Node*)acexpr->arg);
		JumbleExpr(jstate, (Node*)acexpr->elemexpr);
	}
	break;
	case T_ConvertRowtypeExpr:
	{
		ConvertRowtypeExpr* crexpr = (ConvertRowtypeExpr*)node;

		APP_JUMB(crexpr->resulttype);
		JumbleExpr(jstate, (Node*)crexpr->arg);
	}
	break;
	case T_CollateExpr:
	{
		CollateExpr* ce = (CollateExpr*)node;

		APP_JUMB(ce->collOid);
		JumbleExpr(jstate, (Node*)ce->arg);
	}
	break;
	case T_CaseExpr:
	{
		CaseExpr* caseexpr = (CaseExpr*)node;

		JumbleExpr(jstate, (Node*)caseexpr->arg);
		foreach(temp, caseexpr->args)
		{
			CaseWhen* when = lfirst_node(CaseWhen, temp);

			JumbleExpr(jstate, (Node*)when->expr);
			JumbleExpr(jstate, (Node*)when->result);
		}
		JumbleExpr(jstate, (Node*)caseexpr->defresult);
	}
	break;
	case T_CaseTestExpr:
	{
		CaseTestExpr* ct = (CaseTestExpr*)node;

		APP_JUMB(ct->typeId);
	}
	break;
	case T_ArrayExpr:
		JumbleExpr(jstate, (Node*)((ArrayExpr*)node)->elements);
		break;
	case T_RowExpr:
		JumbleExpr(jstate, (Node*)((RowExpr*)node)->args);
		break;
	case T_RowCompareExpr:
	{
		RowCompareExpr* rcexpr = (RowCompareExpr*)node;

		APP_JUMB(rcexpr->rctype);
		JumbleExpr(jstate, (Node*)rcexpr->largs);
		JumbleExpr(jstate, (Node*)rcexpr->rargs);
	}
	break;
	case T_CoalesceExpr:
		JumbleExpr(jstate, (Node*)((CoalesceExpr*)node)->args);
		break;
	case T_MinMaxExpr:
	{
		MinMaxExpr* mmexpr = (MinMaxExpr*)node;

		APP_JUMB(mmexpr->op);
		JumbleExpr(jstate, (Node*)mmexpr->args);
	}
	break;
	case T_SQLValueFunction:
	{
		SQLValueFunction* svf = (SQLValueFunction*)node;

		APP_JUMB(svf->op);
		/* 类型完全由操作符决定 */
		APP_JUMB(svf->typmod);
	}
	break;
	case T_XmlExpr:
	{
		XmlExpr* xexpr = (XmlExpr*)node;

		APP_JUMB(xexpr->op);
		JumbleExpr(jstate, (Node*)xexpr->named_args);
		JumbleExpr(jstate, (Node*)xexpr->args);
	}
	break;
	case T_NullTest:
	{
		NullTest* nt = (NullTest*)node;

		APP_JUMB(nt->nulltesttype);
		JumbleExpr(jstate, (Node*)nt->arg);
	}
	break;
	case T_BooleanTest:
	{
		BooleanTest* bt = (BooleanTest*)node;

		APP_JUMB(bt->booltesttype);
		JumbleExpr(jstate, (Node*)bt->arg);
	}
	break;
	case T_CoerceToDomain:
	{
		CoerceToDomain* cd = (CoerceToDomain*)node;

		APP_JUMB(cd->resulttype);
		JumbleExpr(jstate, (Node*)cd->arg);
	}
	break;
	case T_CoerceToDomainValue:
	{
		CoerceToDomainValue* cdv = (CoerceToDomainValue*)node;

		APP_JUMB(cdv->typeId);
	}
	break;
	case T_SetToDefault:
	{
		SetToDefault* sd = (SetToDefault*)node;

		APP_JUMB(sd->typeId);
	}
	break;
	case T_CurrentOfExpr:
	{
		CurrentOfExpr* ce = (CurrentOfExpr*)node;

		APP_JUMB(ce->cvarno);
		if (ce->cursor_name)
			APP_JUMB_STRING(ce->cursor_name);
		APP_JUMB(ce->cursor_param);
	}
	break;
	case T_NextValueExpr:
	{
		NextValueExpr* nve = (NextValueExpr*)node;

		APP_JUMB(nve->seqid);
		APP_JUMB(nve->typeId);
	}
	break;
	case T_InferenceElem:
	{
		InferenceElem* ie = (InferenceElem*)node;

		APP_JUMB(ie->infercollid);
		APP_JUMB(ie->inferopclass);
		JumbleExpr(jstate, ie->expr);
	}
	break;
	case T_TargetEntry:
	{
		TargetEntry* tle = (TargetEntry*)node;

		APP_JUMB(tle->resno);
		APP_JUMB(tle->ressortgroupref);
		JumbleExpr(jstate, (Node*)tle->expr);
	}
	break;
	case T_RangeTblRef:
	{
		RangeTblRef* rtr = (RangeTblRef*)node;

		APP_JUMB(rtr->rtindex);
	}
	break;
	case T_JoinExpr:
	{
		JoinExpr* join = (JoinExpr*)node;

		APP_JUMB(join->jointype);
		APP_JUMB(join->isNatural);
		APP_JUMB(join->rtindex);
		JumbleExpr(jstate, join->larg);
		JumbleExpr(jstate, join->rarg);
		JumbleExpr(jstate, join->quals);
	}
	break;
	case T_FromExpr:
	{
		FromExpr* from = (FromExpr*)node;

		JumbleExpr(jstate, (Node*)from->fromlist);
		JumbleExpr(jstate, from->quals);
	}
	break;
	case T_OnConflictExpr:
	{
		OnConflictExpr* conf = (OnConflictExpr*)node;

		APP_JUMB(conf->action);
		JumbleExpr(jstate, (Node*)conf->arbiterElems);
		JumbleExpr(jstate, conf->arbiterWhere);
		JumbleExpr(jstate, (Node*)conf->onConflictSet);
		JumbleExpr(jstate, conf->onConflictWhere);
		APP_JUMB(conf->constraint);
		APP_JUMB(conf->exclRelIndex);
		JumbleExpr(jstate, (Node*)conf->exclRelTlist);
	}
	break;
	case T_List:
		foreach(temp, (List*)node)
		{
			JumbleExpr(jstate, (Node*)lfirst(temp));
		}
		break;
	case T_IntList:
		foreach(temp, (List*)node)
		{
			APP_JUMB(lfirst_int(temp));
		}
		break;
	case T_SortGroupClause:
	{
		SortGroupClause* sgc = (SortGroupClause*)node;

		APP_JUMB(sgc->tleSortGroupRef);
		APP_JUMB(sgc->eqop);
		APP_JUMB(sgc->sortop);
		APP_JUMB(sgc->nulls_first);
	}
	break;
	case T_GroupingSet:
	{
		GroupingSet* gsnode = (GroupingSet*)node;

		JumbleExpr(jstate, (Node*)gsnode->content);
	}
	break;
	case T_WindowClause:
	{
		WindowClause* wc = (WindowClause*)node;

		APP_JUMB(wc->winref);
		APP_JUMB(wc->frameOptions);
		JumbleExpr(jstate, (Node*)wc->partitionClause);
		JumbleExpr(jstate, (Node*)wc->orderClause);
		JumbleExpr(jstate, wc->startOffset);
		JumbleExpr(jstate, wc->endOffset);
	}
	break;
	case T_CommonTableExpr:
	{
		CommonTableExpr* cte = (CommonTableExpr*)node;

		/* 我们存储字符串名称，因为 RTE_CTE 类型的 RTE 需要它 */
		APP_JUMB_STRING(cte->ctename);
		APP_JUMB(cte->ctematerialized);
		JumbleQuery(jstate, castNode(Query, cte->ctequery));
	}
	break;
	case T_SetOperationStmt:
	{
		SetOperationStmt* setop = (SetOperationStmt*)node;

		APP_JUMB(setop->op);
		APP_JUMB(setop->all);
		JumbleExpr(jstate, setop->larg);
		JumbleExpr(jstate, setop->rarg);
	}
	break;
	case T_RangeTblFunction:
	{
		RangeTblFunction* rtfunc = (RangeTblFunction*)node;

		JumbleExpr(jstate, rtfunc->funcexpr);
	}
	break;
	case T_TableFunc:
	{
		TableFunc* tablefunc = (TableFunc*)node;

		JumbleExpr(jstate, tablefunc->docexpr);
		JumbleExpr(jstate, tablefunc->rowexpr);
		JumbleExpr(jstate, (Node*)tablefunc->colexprs);
	}
	break;
	case T_TableSampleClause:
	{
		TableSampleClause* tsc = (TableSampleClause*)node;

		APP_JUMB(tsc->tsmhandler);
		JumbleExpr(jstate, (Node*)tsc->args);
		JumbleExpr(jstate, (Node*)tsc->repeatable);
	}
	break;
	default:
		/* 只是一个警告，因为我们无论如何都可以继续处理 */
		elog(WARNING, "unrecognized node type: %d",
			(int)nodeTag(node));
		break;
	}
}

/*
 * 记录当前正在遍历的查询树中常量在查询字符串中的位置。
 */
static void
RecordConstLocation(pgssJumbleState* jstate, int location)
{
	/* -1 表示未知或未定义的位置 */
	if (location >= 0) {
		/* 如果需要，扩大数组 */
		if (jstate->clocations_count >= jstate->clocations_buf_size) {
			jstate->clocations_buf_size *= 2;
			jstate->clocations = (pgssLocationLen*)
				repalloc(jstate->clocations,
					jstate->clocations_buf_size *
					sizeof(pgssLocationLen));
		}
		jstate->clocations[jstate->clocations_count].location = location;
		/* 将长度初始化为 -1 以简化 fill_in_constant_lengths 函数 */
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

/*
 * 生成用于代表所有相似查询的标准化查询字符串。
 *
 * 标准化（Normalization）即把查询中实际的常量字面量替换为占位符（例如 '?'）的过程。
 * 这样能够将结构上完全相同、只有具体参数值不同的 SQL 查询进行匹配和归类。
 *
 * 示例：
 *   - "SELECT * FROM users WHERE id = 123;"
 *     转换为 "SELECT * FROM users WHERE id = ?;"
 *   - "INSERT INTO tbl (col1, col2) VALUES ('abc', 456);"
 *     转换为 "INSERT INTO tbl (col1, col2) VALUES (?, ?);"
 *   - "SELECT id FROM accounts WHERE name = 'Alice' AND balance > 100.50;"
 *     转换为 "SELECT id FROM accounts WHERE name = ? AND balance > ?;"
 *
 * 请注意，根据使用哪种“等效”的查询来创建哈希表条目，标准化后的表示形式可能
 * 会有所不同。我们假设这种情况是可以接受的。
 *
 * 如果 query_loc > 0，则说明 "query" 相对于原始字符串的起点前移了这么长距离，
 * 所以我们需要对提供的位置信息进行转换和补偿。（这样做能够避免重新扫描所需查询
 * 之前的语句，从而提高效率）。
 *
 * *query_len_p 参数包含了输入字符串的长度，在函数退出时会更新为结果字符串的长度。
 * 结果字符串可能会因为常量的替换操作而变长或者变短。
 *
 * 函数返回通过 palloc 分配的字符串。
 */
static char*
generate_normalized_query(pgssJumbleState* jstate, const char* query,
	int query_loc, int* query_len_p, int encoding)
{
	char* norm_query;
	int			query_len = *query_len_p;
	int			i,
		norm_query_buflen,	/* norm_query 允许的空间 */
		len_to_wrt,		/* 要写入的长度（字节） */
		quer_loc = 0,	/* 源查询字节位置 */
		n_quer_loc = 0, /* 标准化查询字节位置 */
		last_off = 0,	/* 前一个标记相对于起始位置的偏移量 */
		last_tok_len = 0;	/* 该标记的长度（字节） */

	/*
	 * 获取常量的长度（核心系统只提供位置）。注意
	 * 这也确保了项目按位置排序。
	 */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * 允许 $n 符号比它们替换的常量更长。
	 * 常量在文本形式中至少占用一个字节，而 $n 符号
	 * 即使 n 达到 INT_MAX，也肯定不超过 11 个字节。我们
	 * 可以根据当前查询的 n 的最大值来调整该限制，
	 * 但似乎不值得为此付出额外的努力。
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* 分配结果缓冲区 */
	norm_query = palloc(norm_query_buflen + 1);

	for (i = 0; i < jstate->clocations_count; i++) {
		int		off,		/* 当前标记相对于起始位置的偏移量 */
			tok_len;	/* 该标记的长度（字节） */

		off = jstate->clocations[i].location;
		/* 如果处理的是部分字符串，调整记录的位置 */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* 忽略任何重复项 */

		/* 复制下一个块（下一个常量之前的内容） */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;

		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/*
		 * PG_HINT_PLAN: DON'T TAKE IN a6f22e8356 so that the designed behavior
		 * is kept stable.
		 */
		 /* 并在常量标记的位置插入 '?' */
		norm_query[n_quer_loc++] = '?';

		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * 我们已经复制到最后一个可忽略的常量。复制原始查询字符串的
	 * 剩余字节。
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * 给定一个有效的 SQL 字符串和常量位置记录数组，
 * 填充这些常量的文本长度。
 *
 * 常量可以使用任何允许的常量语法，例如浮点字面量、
 * 位字符串、单引号字符串和美元引号字符串。这是通过
 * 使用核心扫描器的公共 API 来实现的。
 *
 * 调用者有责任确保字符串是有效的 SQL 语句，
 * 并且在指定位置有常量。由于实际上字符串已经被解析，
 * 并且调用者提供的位置将来自权威解析器内部，
 * 这应该不是问题。
 *
 * 可能存在重复的常量指针，它们的长度将被标记为 '-1'，
 * 以便稍后被忽略。（实际上，我们假设长度最初被初始化为 -1，
 * 并且在此处不更改它们。）
 *
 * 如果 query_loc > 0，则 "query" 相对于原始字符串的起点前移了这么长距离，
 * 所以我们需要对提供的位置进行转换和补偿。（这样做可以避免重新扫描
 * 感兴趣的语句之前的语句，因此值得这样做。）
 *
 * 注意：这里假设 Const 位置的 '-' 字符开始一个负数值常量。
 * 这排除了常量以 '-' 开头的任何其他原因。
 */
static void
fill_in_constant_lengths(pgssJumbleState* jstate, const char* query,
	int query_loc)
{
	pgssLocationLen* locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			last_loc = -1;
	int			i;

	/*
	 * 按位置排序记录，以便我们可以在扫描查询文本时按顺序处理它们。
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			sizeof(pgssLocationLen), comp_location);
	locs = jstate->clocations;

	/* 初始化 flex 扫描器 --- 应该与 raw_parser() 匹配 */
	yyscanner = scanner_init(query,
		&yyextra,
		&ScanKeywords,
		ScanKeywordTokens);

	/* 我们不希望重新发出任何转义字符串警告 */
	yyextra.escape_string_warning = false;

	/* 按顺序搜索每个常量 */
	for (i = 0; i < jstate->clocations_count; i++) {
		int			loc = locs[i].location;
		int			tok;

		/* 如果处理的是部分字符串，调整记录的位置 */
		loc -= query_loc;

		Assert(loc >= 0);

		if (loc <= last_loc)
			continue;			/* 重复常量，忽略 */

		/* 词法分析标记，直到找到所需的常量 */
		for (;;) {
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* 我们不应该遇到字符串结束，但如果遇到了，要合理处理 */
			if (tok == 0)
				break;			/* 跳出内部 for 循环 */

			/*
			 * 我们应该准确找到标记位置，但如果我们以某种方式
			 * 越过了它，就使用那个位置。
			 */
			if (yylloc >= loc) {
				if (query[loc] == '-') {
					/*
				 * 这是一个负值 - 这是我们替换多个标记的唯一情况。
				 *
				 * 不要补偿核心系统在负常量情况下将位置调整到前导 '-'
				 * 运算符的特殊情况。从减号
				 * 符号开始对我们的目的也很有用。这样，像 "select * from foo
				 * where bar = 1" 和 "select * from foo where bar = -2"
				 * 这样的查询将具有相同的标准化查询字符串。
				 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* 跳出内部 for 循环 */
				}

				/*
				 * 我们现在依赖于这样一个假设：flex 已经在 scanbuf 中当前标记的文本后放置了一个零字节。
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* 跳出内部 for 循环 */
			}
		}

		/* 如果遇到字符串结束，放弃处理，将剩余长度保持为 -1 */
		if (tok == 0)
			break;

		last_loc = loc;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: 用于按位置对 pgssLocationLen 结构进行 qsort 排序的比较器
 */
static int
comp_location(const void* a, const void* b)
{
	int			l = ((const pgssLocationLen*)a)->location;
	int			r = ((const pgssLocationLen*)b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return +1;
	else
		return 0;
}