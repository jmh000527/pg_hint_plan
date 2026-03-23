/*-------------------------------------------------------------------------
 *
 * normalize_query.h
 *		Normalize query string.
 *
 * This header file is created from pg_stat_statements.c to implement
 * normalization of query string.
 *
 * Portions Copyright (c) 2008-2020, PostgreSQL Global Development Group
 */
#ifndef NORMALIZE_QUERY_H
#define NORMALIZE_QUERY_H

 /*
  * 记录语法树中单个常量（Const 节点）在原始 SQL 文本中坐标的结构体。
  * 在归一化（Normalization）过程中，所有这类坐标标记的文本段都会被抹去。
  */
typedef struct pgssLocationLen
{
	/* 该常量在原始 SQL 查询字符串中的字节起始偏移量（从 0 开始） */
	int			location;

	/*
	 * 该常量在文本中占据的字节长度。
	 * 特殊值：如果是 -1，在底层表示该位置可能不明确或是一个特殊的语法构造，往往会被忽略替换。
	 */
	int			length;
} pgssLocationLen;

/*
 * 用于计算查询指纹（Jumble）以及生成归一化查询字符串（Normalized Query）的工作状态结构体。
 *
 * 【工作原理（逻辑完全借用自原生插件 pg_stat_statements）】
 * 当我们需要将不同参数但结构相同的 SQL （例如 `SELECT * FROM t WHERE id = 1` 与
 * `SELECT * FROM t WHERE id = 2`）归并为同一个抽象模板（用于给 Hint Table 做匹配）时：
 *
 * 1. 遍历查询的语法解析树（Query Tree），将代表骨架结构的节点特征序列化拼接到 `jumble` 数组中。
 * 2. 遍历期间，凡是遇到代表具体硬编码数值的常量（Const 节点），不仅要把它们跳过去（不混入特征），
 *    还要极其精准地记录下它们在原始 SQL 文本中的起始位置和长度，将其存入 `clocations` 数组。
 * 3. 最终在字符串归一化环节（`generate_normalized_query`），按照 `clocations` 提供的位置坐标，
 *    把原始 SQL 文本中对应的数值片段全部挖掉，替换成占位符 `?`。
 */
typedef struct pgssJumbleState
{
	/* 存储当前查询树的哈希指纹基础序列。也就是上文说到的“被抽出来的骨架结构”。 */
	unsigned char* jumble;

	/* jumble 数组当前已被使用的字节数 */
	Size		jumble_len;

	/*
	 * 常量坐标记录数组：记录了所有由于属于“动态取值”而应当在稍后文本处理中被挖去、
	 * 替换为 `?` 的常数的绝对文本偏移位置与长度。
	 */
	pgssLocationLen* clocations;

	/* clocations 数组当前在内存中分配的总容量大小 */
	int			clocations_buf_size;

	/* clocations 数组中实际包含的处于有效状态的常量记录条数 */
	int			clocations_count;

	/*
	 * 遍历解析树时见过的最大的外部参数 ID (比如查询里存在的 $1, $2, 此处最高就是 2)。
	 * 归一化步骤在决定替换常量时，可能需要依照此信息判定哪些参数坑位已被占用，从而保证一致的映射。
	 */
	int			highest_extern_param_id;
} pgssJumbleState;

static char*
generate_normalized_query(pgssJumbleState* jstate, const char* query,
	int query_loc, int* query_len_p, int encoding);
static void JumbleQuery(pgssJumbleState* jstate, Query* query);

#define JUMBLE_SIZE		1024

#endif	/* NORMALIZE_QUERY_H */
