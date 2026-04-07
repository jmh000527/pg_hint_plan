/*-------------------------------------------------------------------------
 * hint_table_replace.c
 *
 * 将 pg_hint_plan 风格的 hint 字符串中的表名，
 * 按照内置的 map (原始表名 -> 别名) 进行替换。
 *
 * 支持所有 hint 类型：
 *   - 普通 hint:  SeqScan(employees)  HashJoin(employees departments)
 *   - Leading 简单形式: Leading(employees departments salaries)
 *   - Leading 嵌套括号:  Leading((employees (departments salaries)))
 *
 * 编译:
 *   gcc -o hint_replace hint_table_replace.c
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif

/* ======================================================================
 * 1. 表名映射 (map)
 * ====================================================================== */
typedef struct TableAliasEntry
{
    const char *original;
    const char *alias;
} TableAliasEntry;

static const TableAliasEntry table_alias_map[] = {
    {"employees",   "e"},
    {"departments", "d"},
    {"salaries",    "s"},
    {"titles",      "t"},
    {"projects",    "p"},
    {"customers",   "c"},
    {"orders",      "o"},
    {NULL,          NULL}
};

static const char *
lookup_alias(const char *name)
{
    const TableAliasEntry *entry;
    for (entry = table_alias_map; entry->original != NULL; entry++)
    {
        if (strcmp(entry->original, name) == 0)
            return entry->alias;
    }
    return NULL;
}

/* ======================================================================
 * 2. 动态字符串缓冲区
 * ====================================================================== */
typedef struct StringBuf
{
    char   *data;
    int     len;
    int     cap;
} StringBuf;

static void buf_init(StringBuf *buf)
{
    buf->cap = 256;
    buf->data = (char *) malloc(buf->cap);
    if (buf->data == NULL)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    buf->data[0] = '\0';
    buf->len = 0;
}

static void buf_ensure(StringBuf *buf, int needed)
{
    while (buf->len + needed >= buf->cap)
    {
        char *newdata;
        buf->cap *= 2;
        newdata = (char *) realloc(buf->data, buf->cap);
        if (newdata == NULL)
        {
            free(buf->data);
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        buf->data = newdata;
    }
}

static void buf_append_char(StringBuf *buf, char c)
{
    buf_ensure(buf, 2);
    buf->data[buf->len++] = c;
    buf->data[buf->len] = '\0';
}

static void buf_append_str(StringBuf *buf, const char *s)
{
    int slen = (int) strlen(s);
    buf_ensure(buf, slen + 1);
    memcpy(buf->data + buf->len, s, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
}

/* ======================================================================
 * 3. 解析器状态
 * ====================================================================== */
typedef struct ParserState
{
    const char *input;
    int         pos;
    StringBuf  *output;
} ParserState;

static void skip_space_copy(ParserState *st)
{
    while (st->input[st->pos] != '\0' &&
           isspace((unsigned char) st->input[st->pos]))
    {
        buf_append_char(st->output, st->input[st->pos]);
        st->pos++;
    }
}

/* ======================================================================
 * 4. 标识符字符判断
 * ====================================================================== */
static bool is_ident_char(char c)
{
    return (isalnum((unsigned char) c) || c == '_' || c == '.');
}

/* ======================================================================
 * 5. 带引号标识符解析 (不替换)
 * ====================================================================== */
static void parse_quoted_identifier(ParserState *st)
{
    buf_append_char(st->output, '"');
    st->pos++;

    while (st->input[st->pos] != '\0')
    {
        if (st->input[st->pos] == '"')
        {
            st->pos++;
            if (st->input[st->pos] == '"')
            {
                buf_append_str(st->output, "\"\"");
                st->pos++;
            }
            else
            {
                buf_append_char(st->output, '"');
                return;
            }
        }
        else
        {
            buf_append_char(st->output, st->input[st->pos]);
            st->pos++;
        }
    }
    buf_append_char(st->output, '"');
}

/* ======================================================================
 * 6. 普通标识符解析 + map 替换
 * ====================================================================== */
static void parse_identifier_and_replace(ParserState *st)
{
    char        name[256];
    int         nlen = 0;
    const char *alias;

    while (st->input[st->pos] != '\0' && is_ident_char(st->input[st->pos]))
    {
        if (nlen < (int) sizeof(name) - 1)
            name[nlen++] = st->input[st->pos];
        st->pos++;
    }
    name[nlen] = '\0';

    alias = lookup_alias(name);
    if (alias != NULL)
        buf_append_str(st->output, alias);
    else
        buf_append_str(st->output, name);
}

/* ======================================================================
 * 7. Leading 嵌套括号递归解析
 * ====================================================================== */
static void parse_leading_join_elem(ParserState *st);

static void parse_leading_join_pair(ParserState *st)
{
    buf_append_char(st->output, '(');
    st->pos++;

    skip_space_copy(st);
    parse_leading_join_elem(st);
    skip_space_copy(st);
    parse_leading_join_elem(st);
    skip_space_copy(st);

    if (st->input[st->pos] == ')')
    {
        buf_append_char(st->output, ')');
        st->pos++;
    }
}

static void parse_leading_join_elem(ParserState *st)
{
    skip_space_copy(st);

    if (st->input[st->pos] == '(')
        parse_leading_join_pair(st);
    else if (st->input[st->pos] == '"')
        parse_quoted_identifier(st);
    else
        parse_identifier_and_replace(st);
}

/* ======================================================================
 * 8. Leading hint body 解析
 * ====================================================================== */
static void parse_leading_body(ParserState *st)
{
    skip_space_copy(st);

    if (st->input[st->pos] == '(')
    {
        parse_leading_join_pair(st);
    }
    else
    {
        while (st->input[st->pos] != ')' && st->input[st->pos] != '\0')
        {
            skip_space_copy(st);
            if (st->input[st->pos] == ')' || st->input[st->pos] == '\0')
                break;

            if (st->input[st->pos] == '"')
                parse_quoted_identifier(st);
            else
                parse_identifier_and_replace(st);
        }
    }
    skip_space_copy(st);
}

/* ======================================================================
 * 9. 普通 hint body 解析
 * ====================================================================== */
static bool is_leading_keyword(const char *kw)  { return (strcasecmp(kw, "Leading") == 0); }
static bool is_set_keyword(const char *kw)      { return (strcasecmp(kw, "Set") == 0); }
static bool is_parallel_keyword(const char *kw) { return (strcasecmp(kw, "Parallel") == 0); }
static bool is_rows_correction(const char *token)
{
    return (token[0] == '#' || token[0] == '+' || token[0] == '-' || token[0] == '*');
}

static void parse_generic_hint_body(ParserState *st, const char *keyword)
{
    bool is_set = is_set_keyword(keyword);
    bool is_parallel = is_parallel_keyword(keyword);
    int  param_idx = 0;

    skip_space_copy(st);

    while (st->input[st->pos] != ')' && st->input[st->pos] != '\0')
    {
        bool should_replace = true;

        skip_space_copy(st);
        if (st->input[st->pos] == ')' || st->input[st->pos] == '\0')
            break;

        if (st->input[st->pos] == '"')
        {
            parse_quoted_identifier(st);
            param_idx++;
            continue;
        }

        if (is_set)
            should_replace = false;
        if (is_parallel && param_idx > 0)
            should_replace = false;
        if (!is_set && !is_parallel && is_rows_correction(&st->input[st->pos]))
            should_replace = false;

        if (should_replace)
            parse_identifier_and_replace(st);
        else
        {
            while (st->input[st->pos] != '\0' &&
                   !isspace((unsigned char) st->input[st->pos]) &&
                   st->input[st->pos] != ')' &&
                   st->input[st->pos] != '(')
            {
                buf_append_char(st->output, st->input[st->pos]);
                st->pos++;
            }
        }

        param_idx++;
    }
}

/* ======================================================================
 * 10. 主入口
 * ====================================================================== */
char *
replace_hint_table_names(const char *hint_str)
{
    ParserState st;
    StringBuf   output;
    char        keyword[128];
    int         kwlen;

    if (hint_str == NULL || hint_str[0] == '\0')
    {
        char *empty = strdup("");
        if (empty == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        return empty;
    }

    buf_init(&output);
    st.input  = hint_str;
    st.pos    = 0;
    st.output = &output;

    while (st.input[st.pos] != '\0')
    {
        skip_space_copy(&st);
        if (st.input[st.pos] == '\0')
            break;

        if (!isalpha((unsigned char) st.input[st.pos]))
        {
            buf_append_char(&output, st.input[st.pos]);
            st.pos++;
            continue;
        }

        kwlen = 0;
        while (st.input[st.pos] != '\0' &&
               !isspace((unsigned char) st.input[st.pos]) &&
               st.input[st.pos] != '(' &&
               st.input[st.pos] != ')')
        {
            if (kwlen < (int) sizeof(keyword) - 1)
                keyword[kwlen++] = st.input[st.pos];
            buf_append_char(&output, st.input[st.pos]);
            st.pos++;
        }
        keyword[kwlen] = '\0';

        skip_space_copy(&st);

        if (st.input[st.pos] != '(')
            continue;

        buf_append_char(&output, '(');
        st.pos++;

        if (is_leading_keyword(keyword))
            parse_leading_body(&st);
        else
            parse_generic_hint_body(&st, keyword);

        skip_space_copy(&st);
        if (st.input[st.pos] == ')')
        {
            buf_append_char(&output, ')');
            st.pos++;
        }
    }

    return output.data;
}

/* ======================================================================
 * 11. 测试
 * ====================================================================== */

typedef struct TestCase
{
    const char *input;
    const char *expected;
} TestCase;

int main(void)
{
    TestCase tests[] = {
        /* 1. Leading 嵌套括号 */
        {
            "Leading((employees (departments salaries)))",
            "Leading((e (d s)))"
        },
        /* 2. Leading 更深层嵌套 */
        {
            "Leading(((employees departments) (salaries titles)))",
            "Leading(((e d) (s t)))"
        },
        /* 3. Leading 简单形式 */
        {
            "Leading(employees departments salaries)",
            "Leading(e d s)"
        },
        /* 4. SeqScan */
        {
            "SeqScan(employees)",
            "SeqScan(e)"
        },
        /* 5. HashJoin */
        {
            "HashJoin(employees departments)",
            "HashJoin(e d)"
        },
        /* 6. 混合多个 hint */
        {
            "SeqScan(employees) HashJoin(employees departments) Leading((employees (departments salaries)))",
            "SeqScan(e) HashJoin(e d) Leading((e (d s)))"
        },
        /* 7. Set hint (不替换参数) */
        {
            "Set(random_page_cost 1.0)",
            "Set(random_page_cost 1.0)"
        },
        /* 8. Rows hint (修正值不替换) */
        {
            "Rows(employees departments #1000)",
            "Rows(e d #1000)"
        },
        /* 9. Parallel hint (只替换第一个参数) */
        {
            "Parallel(employees 4 hard)",
            "Parallel(e 4 hard)"
        },
        /* 10. 不在 map 中的表名 */
        {
            "Leading((unknown_table (employees departments)))",
            "Leading((unknown_table (e d)))"
        },
        /* 11. 带引号标识符 (不替换) */
        {
            "SeqScan(\"employees\")",
            "SeqScan(\"employees\")"
        },
        /* 12. 完整 hint 注释格式 */
        {
            "/*+ SeqScan(employees) NestLoop(employees departments) Leading((employees (departments salaries))) */",
            "/*+ SeqScan(e) NestLoop(e d) Leading((e (d s))) */"
        },
        {NULL, NULL}
    };

    int i;
    int passed = 0, failed = 0;

    printf("=== hint_table_replace test suite ===\n\n");

    for (i = 0; tests[i].input != NULL; i++)
    {
        char *result = replace_hint_table_names(tests[i].input);
        bool ok = (strcmp(result, tests[i].expected) == 0);

        if (ok)
        {
            printf("[PASS] Test %d\n", i + 1);
            passed++;
        }
        else
        {
            printf("[FAIL] Test %d\n", i + 1);
            printf("  Input:    %s\n", tests[i].input);
            printf("  Expected: %s\n", tests[i].expected);
            printf("  Got:      %s\n", result);
            failed++;
        }
        free(result);
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);

    return (failed > 0) ? 1 : 0;
}
