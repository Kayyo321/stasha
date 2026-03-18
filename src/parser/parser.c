#include <string.h>
#include <stdlib.h>
#include "parser.h"

typedef struct {
    lexer_t lexer;
    token_t current;
    token_t previous;
    boolean_t had_error;
    storage_t group_storage; /* current storage-group qualifier, StorageDefault = none */
} parser_t;

/* ── save / restore for speculative parsing (casts) ── */

typedef struct {
    lexer_t lexer;
    token_t current;
    token_t previous;
} parser_state_t;

static parser_state_t save_state(parser_t *p) {
    return (parser_state_t){p->lexer, p->current, p->previous};
}

static void restore_state(parser_t *p, parser_state_t s) {
    p->lexer = s.lexer;
    p->current = s.current;
    p->previous = s.previous;
}

/* ── helpers ── */

static void advance_parser(parser_t *p) {
    p->previous = p->current;
    for (;;) {
        p->current = next_token(&p->lexer);
        if (p->current.kind != TokError) break;
        log_err("line %lu: %.*s", p->current.line,
                (int)p->current.length, p->current.start);
        p->had_error = True;
    }
}

static boolean_t check(parser_t *p, token_kind_t kind) {
    return p->current.kind == kind;
}

static boolean_t match_tok(parser_t *p, token_kind_t kind) {
    if (!check(p, kind)) return False;
    advance_parser(p);
    return True;
}

static token_t consume(parser_t *p, token_kind_t kind, const char *msg) {
    if (check(p, kind)) {
        advance_parser(p);
        return p->previous;
    }
    log_err("line %lu: expected %s, got '%.*s'",
            p->current.line, msg,
            (int)p->current.length, p->current.start);
    p->had_error = True;
    return p->current;
}

/* ── type helpers ── */

static boolean_t is_builtin_type_token(token_kind_t k) {
    return k == TokI8  || k == TokI16 || k == TokI32 || k == TokI64
        || k == TokU8  || k == TokU16 || k == TokU32 || k == TokU64
        || k == TokF32 || k == TokF64
        || k == TokBool || k == TokVoid;
}

static type_kind_t token_to_type(token_kind_t k) {
    switch (k) {
        case TokVoid: return TypeVoid;
        case TokBool: return TypeBool;
        case TokI8:   return TypeI8;
        case TokI16:  return TypeI16;
        case TokI32:  return TypeI32;
        case TokI64:  return TypeI64;
        case TokU8:   return TypeU8;
        case TokU16:  return TypeU16;
        case TokU32:  return TypeU32;
        case TokU64:  return TypeU64;
        case TokF32:  return TypeF32;
        case TokF64:  return TypeF64;
        default:      return TypeVoid;
    }
}

static type_info_t parse_type(parser_t *p) {
    type_info_t info = NO_TYPE;

    if (is_builtin_type_token(p->current.kind)) {
        info.base = token_to_type(p->current.kind);
        advance_parser(p);
    } else if (check(p, TokIdent)) {
        info.base = TypeUser;
        info.user_name = copy_token_text(p->current);
        advance_parser(p);
    } else {
        log_err("line %lu: expected type", p->current.line);
        p->had_error = True;
        return info;
    }

    /* pointer: * or *r or *w or *rw */
    if (check(p, TokStar)) {
        advance_parser(p);
        info.is_pointer = True;
        info.ptr_perm = PtrReadWrite; /* default */

        if (check(p, TokIdent)) {
            const char *s = p->current.start;
            usize_t len = p->current.length;
            if (len == 1 && s[0] == 'r') {
                info.ptr_perm = PtrRead;
                advance_parser(p);
            } else if (len == 1 && s[0] == 'w') {
                info.ptr_perm = PtrWrite;
                advance_parser(p);
            } else if (len == 2 && s[0] == 'r' && s[1] == 'w') {
                info.ptr_perm = PtrReadWrite;
                advance_parser(p);
            }
        }
    }

    return info;
}

/* ── forward declarations ── */

static node_t *parse_expr(parser_t *p);
static node_t *parse_block(parser_t *p);
static node_t *parse_statement(parser_t *p);
static node_t *parse_match_stmt(parser_t *p);
static node_t *parse_var_decl(parser_t *p, linkage_t linkage);
static node_t *parse_defer_stmt(parser_t *p);
static node_t *parse_storage_group(parser_t *p, storage_t storage);

static boolean_t can_start_type(parser_t *p) {
    return is_builtin_type_token(p->current.kind) || check(p, TokIdent);
}

/* In a parameter list, a bare identifier is only a type if followed by
   another identifier or '*' (i.e. "Type name" or "Type * name").
   If followed by ',' or ')', it must be a grouped parameter name. */
static boolean_t can_start_param_type(parser_t *p) {
    if (is_builtin_type_token(p->current.kind)) return True;
    if (!check(p, TokIdent)) return False;
    parser_state_t snap = save_state(p);
    advance_parser(p);
    boolean_t result = check(p, TokIdent) || check(p, TokStar);
    restore_state(p, snap);
    return result;
}

static boolean_t can_start_var_decl(parser_t *p) {
    return check(p, TokStack) || check(p, TokHeap) || check(p, TokAtomic)
        || check(p, TokConst)  || check(p, TokFinal)
        || is_builtin_type_token(p->current.kind);
}

/* ── primary ── */

static long parse_int_value(token_t t) {
    long val = 0;
    if (t.length > 2 && t.start[0] == '0' && (t.start[1] == 'x' || t.start[1] == 'X')) {
        for (usize_t i = 2; i < t.length; i++) {
            char c = t.start[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else d = 10 + c - 'A';
            val = val * 16 + d;
        }
        return val;
    }
    for (usize_t i = 0; i < t.length; i++)
        val = val * 10 + (t.start[i] - '0');
    return val;
}

static double parse_float_value(token_t t) {
    char buf[64];
    usize_t len = t.length < 63 ? t.length : 63;
    memcpy(buf, t.start, len);
    buf[len] = '\0';
    return strtod(buf, Null);
}

static char parse_char_value(token_t t) {
    /* token includes backticks: `x` or `\n` */
    const char *s = t.start + 1; /* skip opening backtick */
    if (s[0] == '\\') {
        switch (s[1]) {
            case 'n':  return '\n';
            case 't':  return '\t';
            case 'r':  return '\r';
            case '0':  return '\0';
            case '\\': return '\\';
            case '`':  return '`';
            case 'a':  return '\a';
            case 'b':  return '\b';
            default:   return s[1];
        }
    }
    return s[0];
}

static node_t *parse_primary(parser_t *p) {
    /* integer literal */
    if (check(p, TokIntLit)) {
        advance_parser(p);
        node_t *n = make_node(NodeIntLitExpr, p->previous.line);
        n->as.int_lit.value = parse_int_value(p->previous);
        return n;
    }

    /* float literal */
    if (check(p, TokFloatLit)) {
        advance_parser(p);
        node_t *n = make_node(NodeFloatLitExpr, p->previous.line);
        n->as.float_lit.value = parse_float_value(p->previous);
        return n;
    }

    /* boolean literal */
    if (check(p, TokTrue) || check(p, TokFalse)) {
        boolean_t val = check(p, TokTrue);
        advance_parser(p);
        node_t *n = make_node(NodeBoolLitExpr, p->previous.line);
        n->as.bool_lit.value = val;
        return n;
    }

    /* string literal */
    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        boolean_t is_heap = check(p, TokHeapStr);
        advance_parser(p);
        node_t *n = make_node(NodeStrLitExpr, p->previous.line);
        token_t t = p->previous;
        usize_t slen = t.length - 2;
        usize_t out_len = 0;
        n->as.str_lit.value = ast_strdup_escape(t.start + 1, slen, &out_len);
        n->as.str_lit.is_heap = is_heap;
        n->as.str_lit.len = out_len;
        return n;
    }

    /* char literal */
    if (check(p, TokCharLit)) {
        advance_parser(p);
        node_t *n = make_node(NodeCharLitExpr, p->previous.line);
        n->as.char_lit.value = parse_char_value(p->previous);
        return n;
    }

    /* new.(size) */
    if (check(p, TokNew)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *size = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeNewExpr, line);
        n->as.new_expr.size = size;
        return n;
    }

    /* sizeof.(type) */
    if (check(p, TokSizeof)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        type_info_t type = parse_type(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeSizeofExpr, line);
        n->as.sizeof_expr.type = type;
        return n;
    }

    /* rem.(ptr) — parsed as statement-level, but also works in expression position */
    if (check(p, TokRem)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *ptr = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeRemStmt, line);
        n->as.rem_stmt.ptr = ptr;
        return n;
    }

    /* nil — null pointer literal */
    if (check(p, TokNil)) {
        usize_t line = p->current.line;
        advance_parser(p);
        return make_node(NodeNilExpr, line);
    }

    /* mov.(ptr, new_size) — realloc */
    if (check(p, TokMov)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *ptr = parse_expr(p);
        consume(p, TokComma, "','");
        node_t *sz = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeMovExpr, line);
        n->as.mov_expr.ptr = ptr;
        n->as.mov_expr.size = sz;
        return n;
    }

    /* gpu.(fn)() / cpu.(fn)() */
    if (check(p, TokGpu) || check(p, TokCpu)) {
        boolean_t is_gpu = check(p, TokGpu);
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        token_t name_tok = consume(p, TokIdent, "function name");
        char *name = copy_token_text(name_tok);
        consume(p, TokRParen, "')'");
        consume(p, TokLParen, "'('");
        node_list_t args;
        node_list_init(&args);
        if (!check(p, TokRParen)) {
            do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
        }
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeParallelCall, line);
        n->as.parallel_call.is_gpu = is_gpu;
        n->as.parallel_call.callee = name;
        n->as.parallel_call.args = args;
        return n;
    }

    /* identifier */
    if (check(p, TokIdent)) {
        advance_parser(p);
        node_t *n = make_node(NodeIdentExpr, p->previous.line);
        n->as.ident.name = copy_token_text(p->previous);
        return n;
    }

    /* parenthesised expression or cast */
    if (check(p, TokLParen)) {
        /* try cast: (type)expr */
        if (is_builtin_type_token(p->current.kind) == False) {
            /* definitely not a cast if next isn't a type keyword — skip lookahead */
        } else {
            /* speculatively attempt: could still be (i32 + ...) etc. but that's unusual */
        }
        parser_state_t saved = save_state(p);
        boolean_t saved_err = p->had_error;
        advance_parser(p); /* consume '(' */

        if (is_builtin_type_token(p->current.kind)) {
            type_info_t cast_type = parse_type(p);
            if (check(p, TokRParen) && !p->had_error) {
                usize_t line = p->previous.line;
                advance_parser(p); /* consume ')' */
                node_t *expr = parse_primary(p); /* cast binds tightly */
                node_t *n = make_node(NodeCastExpr, line);
                n->as.cast_expr.target = cast_type;
                n->as.cast_expr.expr = expr;
                return n;
            }
        }
        /* not a cast — restore and parse as grouped expression */
        restore_state(p, saved);
        p->had_error = saved_err;
        advance_parser(p); /* consume '(' */
        node_t *expr = parse_expr(p);
        consume(p, TokRParen, "')'");
        return expr;
    }

    log_err("line %lu: expected expression, got '%.*s'",
            p->current.line, (int)p->current.length, p->current.start);
    p->had_error = True;
    advance_parser(p); /* skip the bad token so we don't loop forever */
    return make_node(NodeIntLitExpr, p->previous.line);
}

/* ── postfix: calls, indexing, member access, ++/-- ── */

static node_t *parse_postfix(parser_t *p) {
    node_t *expr = parse_primary(p);

    for (;;) {
        /* function call: ident(...) */
        if (check(p, TokLParen) && expr->kind == NodeIdentExpr) {
            char *name = expr->as.ident.name;
            usize_t line = expr->line;
            advance_parser(p);
            node_list_t args;
            node_list_init(&args);
            if (!check(p, TokRParen)) {
                do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
            }
            consume(p, TokRParen, "')'");
            node_t *n = make_node(NodeCallExpr, line);
            n->as.call.callee = name;
            n->as.call.args = args;
            expr = n;
            continue;
        }

        /* array index: expr[idx] */
        if (check(p, TokLBracket)) {
            usize_t line = p->current.line;
            advance_parser(p);
            node_t *index = parse_expr(p);
            consume(p, TokRBracket, "']'");
            node_t *n = make_node(NodeIndexExpr, line);
            n->as.index_expr.object = expr;
            n->as.index_expr.index = index;
            expr = n;
            continue;
        }

        /* member access / method call / self-member: expr.field, expr.method(), Type.(field) */
        if (check(p, TokDot)) {
            usize_t line = p->current.line;
            advance_parser(p);

            /* self-member: Type.(field) */
            if (check(p, TokLParen)) {
                advance_parser(p);
                token_t field = consume(p, TokIdent, "field name");
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeSelfMemberExpr, line);
                if (expr->kind == NodeIdentExpr)
                    n->as.self_member.type_name = expr->as.ident.name;
                else
                    n->as.self_member.type_name = ast_strdup("?", 1);
                n->as.self_member.field = copy_token_text(field);
                expr = n;
                continue;
            }

            /* consume method/field name — accept keywords new/rem as method names */
            char *field_name;
            if (check(p, TokIdent)) {
                field_name = copy_token_text(p->current);
                advance_parser(p);
            } else if (check(p, TokNew)) {
                field_name = ast_strdup("new", 3);
                advance_parser(p);
            } else if (check(p, TokRem)) {
                field_name = ast_strdup("rem", 3);
                advance_parser(p);
            } else {
                field_name = ast_strdup("?", 1);
                log_err("line %lu: expected field or method name", p->current.line);
                p->had_error = True;
            }

            /* method call: expr.method(args) */
            if (check(p, TokLParen)) {
                advance_parser(p);
                node_list_t args;
                node_list_init(&args);
                if (!check(p, TokRParen)) {
                    do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
                }
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeMethodCall, line);
                n->as.method_call.object = expr;
                n->as.method_call.method = field_name;
                n->as.method_call.args = args;
                expr = n;
                continue;
            }

            /* member access: expr.field */
            node_t *n = make_node(NodeMemberExpr, line);
            n->as.member_expr.object = expr;
            n->as.member_expr.field = field_name;
            expr = n;
            continue;
        }

        /* postfix ++ / -- */
        if (check(p, TokPlusPlus) || check(p, TokMinusMinus)) {
            token_kind_t op = p->current.kind;
            advance_parser(p);
            node_t *n = make_node(NodeUnaryPostfixExpr, expr->line);
            n->as.unary.op = op;
            n->as.unary.operand = expr;
            expr = n;
            continue;
        }

        break;
    }
    return expr;
}

/* ── unary prefix: ++ -- ! - ~ ── */

static node_t *parse_unary(parser_t *p) {
    /* address-of: &expr (unary prefix, distinct from binary &) */
    if (check(p, TokAmp)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *operand = parse_unary(p);
        node_t *n = make_node(NodeAddrOf, line);
        n->as.addr_of.operand = operand;
        return n;
    }
    if (check(p, TokPlusPlus) || check(p, TokMinusMinus)
        || check(p, TokBang) || check(p, TokMinus) || check(p, TokTilde)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *operand = parse_unary(p);
        node_t *n = make_node(NodeUnaryPrefixExpr, line);
        n->as.unary.op = op;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_postfix(p);
}

/* ── multiplicative: * / % ── */

static node_t *parse_multiplication(parser_t *p) {
    node_t *left = parse_unary(p);
    while (check(p, TokStar) || check(p, TokSlash) || check(p, TokPercent)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_unary(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── additive: + - ── */

static node_t *parse_addition(parser_t *p) {
    node_t *left = parse_multiplication(p);
    while (check(p, TokPlus) || check(p, TokMinus)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_multiplication(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── shift: << >> ── */

static node_t *parse_shift(parser_t *p) {
    node_t *left = parse_addition(p);
    while (check(p, TokLtLt) || check(p, TokGtGt)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_addition(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── relational: < > <= >= ── */

static node_t *parse_relational(parser_t *p) {
    node_t *left = parse_shift(p);
    while (check(p, TokLt) || check(p, TokGt) || check(p, TokLtEq) || check(p, TokGtEq)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_shift(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── equality: == != ── */

static node_t *parse_equality(parser_t *p) {
    node_t *left = parse_relational(p);
    while (check(p, TokEqEq) || check(p, TokBangEq)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_relational(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── bitwise AND: & ── */

static node_t *parse_bitwise_and(parser_t *p) {
    node_t *left = parse_equality(p);
    while (check(p, TokAmp)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_equality(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = TokAmp;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── bitwise XOR: ^ ── */

static node_t *parse_bitwise_xor(parser_t *p) {
    node_t *left = parse_bitwise_and(p);
    while (check(p, TokCaret)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_bitwise_and(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = TokCaret;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── bitwise OR: | ── */

static node_t *parse_bitwise_or(parser_t *p) {
    node_t *left = parse_bitwise_xor(p);
    while (check(p, TokPipe)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_bitwise_xor(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = TokPipe;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── logical AND: && ── */

static node_t *parse_logical_and(parser_t *p) {
    node_t *left = parse_bitwise_or(p);
    while (check(p, TokAmpAmp)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_bitwise_or(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = TokAmpAmp;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── logical OR: || ── */

static node_t *parse_logical_or(parser_t *p) {
    node_t *left = parse_logical_and(p);
    while (check(p, TokPipePipe)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_logical_and(p);
        node_t *n = make_node(NodeBinaryExpr, line);
        n->as.binary.op = TokPipePipe;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

/* ── ternary: cond ? then : else ── */

static node_t *parse_ternary(parser_t *p) {
    node_t *cond = parse_logical_or(p);
    if (check(p, TokQuestion)) {
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *then_expr = parse_expr(p);
        consume(p, TokColon, "':'");
        node_t *else_expr = parse_ternary(p);
        node_t *n = make_node(NodeTernaryExpr, line);
        n->as.ternary.cond = cond;
        n->as.ternary.then_expr = then_expr;
        n->as.ternary.else_expr = else_expr;
        return n;
    }
    return cond;
}

/* ── assignment: = += -= *= /= %= &= |= ^= <<= >>= ── */

static boolean_t is_compound_assign(token_kind_t k) {
    return k == TokPlusEq  || k == TokMinusEq || k == TokStarEq
        || k == TokSlashEq || k == TokPercentEq
        || k == TokAmpEq   || k == TokPipeEq  || k == TokCaretEq
        || k == TokLtLtEq  || k == TokGtGtEq;
}

static node_t *parse_assignment(parser_t *p) {
    node_t *expr = parse_ternary(p);

    if (expr->kind == NodeIdentExpr || expr->kind == NodeIndexExpr
        || expr->kind == NodeMemberExpr || expr->kind == NodeSelfMemberExpr) {
        if (check(p, TokEq)) {
            advance_parser(p);
            node_t *value = parse_assignment(p);
            node_t *n = make_node(NodeAssignExpr, expr->line);
            n->as.assign.target = expr;
            n->as.assign.value = value;
            return n;
        }
        if (is_compound_assign(p->current.kind)) {
            token_kind_t op = p->current.kind;
            advance_parser(p);
            node_t *value = parse_assignment(p);
            node_t *n = make_node(NodeCompoundAssign, expr->line);
            n->as.compound_assign.op = op;
            n->as.compound_assign.target = expr;
            n->as.compound_assign.value = value;
            return n;
        }
    }
    return expr;
}

static node_t *parse_expr(parser_t *p) {
    return parse_assignment(p);
}

/* ── statements ── */

static node_t *parse_block(parser_t *p) {
    usize_t line = p->current.line;
    consume(p, TokLBrace, "'{'");
    node_list_t stmts;
    node_list_init(&stmts);
    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_list_push(&stmts, parse_statement(p));
    }
    consume(p, TokRBrace, "'}'");
    node_t *n = make_node(NodeBlock, line);
    n->as.block.stmts = stmts;
    return n;
}

static node_t *parse_for_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    consume(p, TokLParen, "'('");

    /* init — could be a var decl or expression statement */
    node_t *init;
    if (can_start_var_decl(p))
        init = parse_var_decl(p, LinkageNone);
    else {
        init = parse_expr(p);
        consume(p, TokSemicolon, "';'");
    }

    node_t *cond = parse_expr(p);
    consume(p, TokSemicolon, "';'");
    node_t *update = parse_expr(p);
    consume(p, TokRParen, "')'");
    node_t *body = parse_block(p);

    node_t *n = make_node(NodeForStmt, line);
    n->as.for_stmt.init = init;
    n->as.for_stmt.cond = cond;
    n->as.for_stmt.update = update;
    n->as.for_stmt.body = body;
    return n;
}

static node_t *parse_while_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    consume(p, TokLParen, "'('");
    node_t *cond = parse_expr(p);
    consume(p, TokRParen, "')'");
    node_t *body = parse_block(p);

    node_t *n = make_node(NodeWhileStmt, line);
    n->as.while_stmt.cond = cond;
    n->as.while_stmt.body = body;
    return n;
}

static node_t *parse_do_while_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'do' */
    node_t *body = parse_block(p);
    consume(p, TokWhile, "'while'");
    consume(p, TokLParen, "'('");
    node_t *cond = parse_expr(p);
    consume(p, TokRParen, "')'");
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeDoWhileStmt, line);
    n->as.do_while_stmt.body = body;
    n->as.do_while_stmt.cond = cond;
    return n;
}

static node_t *parse_inf_loop(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_t *body = parse_block(p);
    node_t *n = make_node(NodeInfLoop, line);
    n->as.inf_loop.body = body;
    return n;
}

static node_t *parse_if_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    consume(p, TokLParen, "'('");
    node_t *cond = parse_expr(p);
    consume(p, TokRParen, "')'");
    node_t *then_block = parse_block(p);
    node_t *else_block = Null;
    if (match_tok(p, TokElse)) {
        if (check(p, TokIf))
            else_block = parse_if_stmt(p);
        else
            else_block = parse_block(p);
    }
    node_t *n = make_node(NodeIfStmt, line);
    n->as.if_stmt.cond = cond;
    n->as.if_stmt.then_block = then_block;
    n->as.if_stmt.else_block = else_block;
    return n;
}

static node_t *parse_ret_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_list_t values;
    node_list_init(&values);
    if (!check(p, TokSemicolon)) {
        do {
            node_list_push(&values, parse_expr(p));
        } while (match_tok(p, TokComma));
    }
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeRetStmt, line);
    n->as.ret_stmt.values = values;
    return n;
}

static node_t *parse_debug_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_t *value = parse_expr(p);
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeDebugStmt, line);
    n->as.debug_stmt.value = value;
    return n;
}

static node_t *parse_expr_stmt(parser_t *p) {
    usize_t line = p->current.line;
    node_t *expr = parse_expr(p);
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeExprStmt, line);
    n->as.expr_stmt.expr = expr;
    return n;
}

/* ── variable declaration ── */

static node_t *parse_var_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    storage_t storage = StorageDefault;
    boolean_t is_atomic = False;
    boolean_t is_const = False;
    boolean_t is_final = False;

    if (match_tok(p, TokStack))      storage = StorageStack;
    else if (match_tok(p, TokHeap))  storage = StorageHeap;
    else if (p->group_storage != StorageDefault) storage = p->group_storage;

    if (match_tok(p, TokAtomic)) is_atomic = True;
    if (match_tok(p, TokConst))  is_const = True;
    if (match_tok(p, TokFinal)) is_final = True;

    if (!can_start_type(p)) {
        log_err("line %lu: expected type", p->current.line);
        p->had_error = True;
        return make_node(NodeVarDecl, line);
    }
    type_info_t type = parse_type(p);

    /* multi-var declaration: [x, y] = expr */
    if (check(p, TokLBracket)) {
        advance_parser(p);
        node_list_t targets;
        node_list_init(&targets);
        node_list_t values;
        node_list_init(&values);
        do {
            token_t name_tok = consume(p, TokIdent, "variable name");
            node_t *var = make_node(NodeVarDecl, name_tok.line);
            var->as.var_decl.name = copy_token_text(name_tok);
            var->as.var_decl.type = type;
            var->as.var_decl.storage = storage;
            var->as.var_decl.linkage = linkage;
            var->as.var_decl.is_atomic = is_atomic;
            var->as.var_decl.is_const = is_const;
            var->as.var_decl.is_final = is_final;
            node_list_push(&targets, var);
        } while (match_tok(p, TokComma));
        consume(p, TokRBracket, "']'");
        consume(p, TokEq, "'='");

        /* parse initializer expressions (comma-separated) or single call */
        do {
            node_list_push(&values, parse_expr(p));
        } while (match_tok(p, TokComma));
        consume(p, TokSemicolon, "';'");

        node_t *n = make_node(NodeMultiAssign, line);
        n->as.multi_assign.targets = targets;
        n->as.multi_assign.values = values;
        return n;
    }

    token_t name_tok = consume(p, TokIdent, "variable name");
    char *name = copy_token_text(name_tok);

    /* array: type name[size] */
    boolean_t is_array = False;
    long array_size = 0;
    if (match_tok(p, TokLBracket)) {
        is_array = True;
        if (check(p, TokIntLit)) {
            array_size = parse_int_value(p->current);
            advance_parser(p);
        }
        consume(p, TokRBracket, "']'");
    }

    node_t *init_expr = Null;
    if (match_tok(p, TokEq)) init_expr = parse_expr(p);
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeVarDecl, line);
    n->as.var_decl.name = name;
    n->as.var_decl.type = type;
    n->as.var_decl.storage = storage;
    n->as.var_decl.linkage = linkage;
    n->as.var_decl.is_atomic = is_atomic;
    n->as.var_decl.is_const = is_const;
    n->as.var_decl.is_final = is_final;
    n->as.var_decl.is_array = is_array;
    n->as.var_decl.array_size = array_size;
    n->as.var_decl.init = init_expr;
    return n;
}

static node_t *parse_defer_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'defer' */
    node_t *body;
    if (check(p, TokLBrace))
        body = parse_block(p);
    else
        body = parse_statement(p);
    node_t *n = make_node(NodeDeferStmt, line);
    n->as.defer_stmt.body = body;
    return n;
}

/* parse the body of a storage-group block: stack ( ... ) / heap ( ... )
   Called after consuming the opening '('. Returns a NodeBlock. */
static node_t *parse_storage_group(parser_t *p, storage_t storage) {
    usize_t line = p->current.line;
    storage_t prev = p->group_storage;
    p->group_storage = storage;
    node_list_t stmts;
    node_list_init(&stmts);
    while (!check(p, TokRParen) && !check(p, TokEof)) {
        node_list_push(&stmts, parse_statement(p));
    }
    consume(p, TokRParen, "')'");
    p->group_storage = prev;
    node_t *n = make_node(NodeBlock, line);
    n->as.block.stmts = stmts;
    return n;
}

static node_t *parse_statement(parser_t *p) {
    if (check(p, TokFor))       return parse_for_stmt(p);
    if (check(p, TokWhile))     return parse_while_stmt(p);
    if (check(p, TokDo))        return parse_do_while_stmt(p);
    if (check(p, TokInf))       return parse_inf_loop(p);
    if (check(p, TokIf))        return parse_if_stmt(p);
    if (check(p, TokRet))       return parse_ret_stmt(p);
    if (check(p, TokDebug))     return parse_debug_stmt(p);
    if (check(p, TokMatch))     return parse_match_stmt(p);
    if (check(p, TokDefer))     return parse_defer_stmt(p);

    if (check(p, TokBreak)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokSemicolon, "';'");
        return make_node(NodeBreakStmt, line);
    }
    if (check(p, TokContinue)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokSemicolon, "';'");
        return make_node(NodeContinueStmt, line);
    }

    /* storage group block: stack ( ... ) or heap ( ... ) */
    if (check(p, TokStack) || check(p, TokHeap)) {
        storage_t grp = check(p, TokStack) ? StorageStack : StorageHeap;
        parser_state_t saved = save_state(p);
        advance_parser(p);
        if (check(p, TokLParen)) {
            advance_parser(p); /* consume '(' */
            return parse_storage_group(p, grp);
        }
        restore_state(p, saved);
    }

    if (can_start_var_decl(p)) return parse_var_decl(p, LinkageNone);
    return parse_expr_stmt(p);
}

/* ── struct body parsing ── */

static void parse_struct_body(parser_t *p, node_t *decl) {
    consume(p, TokLBrace, "'{'");
    storage_t current_section = StorageStack;

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        /* section markers: stack: / heap: */
        if (check(p, TokStack) || check(p, TokHeap)) {
            boolean_t is_heap = check(p, TokHeap);
            parser_state_t saved = save_state(p);
            advance_parser(p);
            if (match_tok(p, TokColon)) {
                current_section = is_heap ? StorageHeap : StorageStack;
                continue;
            }
            restore_state(p, saved);
        }

        linkage_t field_link = LinkageNone;
        if (check(p, TokInt) || check(p, TokExt)) {
            field_link = check(p, TokInt) ? LinkageInternal : LinkageExternal;
            advance_parser(p);
        }

        /* inline method */
        if (check(p, TokFn)) {
            advance_parser(p);
            token_t fn_name = consume(p, TokIdent, "method name");
            consume(p, TokLParen, "'('");
            node_list_t params;
            node_list_init(&params);
            if (!check(p, TokRParen) && !check(p, TokVoid)) {
                type_info_t ptype;
                storage_t last_ps = StorageDefault;
                do {
                    storage_t ps = StorageDefault;
                    if (check(p, TokStack))      { ps = StorageStack; advance_parser(p); }
                    else if (check(p, TokHeap)) { ps = StorageHeap;  advance_parser(p); }
                    if (ps != StorageDefault) last_ps = ps;
                    else ps = last_ps;
                    if (can_start_param_type(p))
                        ptype = parse_type(p);
                    token_t pname = consume(p, TokIdent, "parameter name");
                    node_t *param = make_node(NodeVarDecl, pname.line);
                    param->as.var_decl.name = copy_token_text(pname);
                    param->as.var_decl.type = ptype;
                    param->as.var_decl.storage = ps;
                    node_list_push(&params, param);
                } while (match_tok(p, TokComma));
            } else if (check(p, TokVoid)) {
                advance_parser(p);
            }
            consume(p, TokRParen, "')'");
            consume(p, TokColon, "':'");

            usize_t ret_count = 0;
            type_info_t *ret_types = Null;
            if (check(p, TokLBracket)) {
                advance_parser(p);
                node_list_t tmp;
                node_list_init(&tmp);
                do {
                    type_info_t rt = parse_type(p);
                    node_t *dummy = make_node(NodeVarDecl, p->previous.line);
                    dummy->as.var_decl.type = rt;
                    node_list_push(&tmp, dummy);
                } while (match_tok(p, TokComma));
                consume(p, TokRBracket, "']'");
                ret_count = tmp.count;
                ret_types = alloc_type_array(ret_count);
                for (usize_t i = 0; i < ret_count; i++)
                    ret_types[i] = tmp.items[i]->as.var_decl.type;
            } else {
                ret_count = 1;
                ret_types = alloc_type_array(1);
                ret_types[0] = parse_type(p);
            }

            node_t *body = parse_block(p);
            node_t *method = make_node(NodeFnDecl, fn_name.line);
            method->as.fn_decl.name = copy_token_text(fn_name);
            method->as.fn_decl.linkage = field_link;
            method->as.fn_decl.return_types = ret_types;
            method->as.fn_decl.return_count = ret_count;
            method->as.fn_decl.params = params;
            method->as.fn_decl.body = body;
            method->as.fn_decl.is_method = True;
            method->as.fn_decl.struct_name = decl->as.type_decl.name;
            node_list_push(&decl->as.type_decl.methods, method);
            continue;
        }

        /* field declaration: [int|ext] type name [, name, ...] ; */
        if (!can_start_type(p)) {
            log_err("line %lu: expected field type or method in struct", p->current.line);
            p->had_error = True;
            advance_parser(p);
            continue;
        }
        type_info_t ftype = parse_type(p);

        do {
            token_t fname = consume(p, TokIdent, "field name");
            node_t *field = make_node(NodeVarDecl, fname.line);
            field->as.var_decl.name = copy_token_text(fname);
            field->as.var_decl.type = ftype;
            field->as.var_decl.storage = current_section;
            field->as.var_decl.linkage = field_link;
            node_list_push(&decl->as.type_decl.fields, field);
        } while (match_tok(p, TokComma));
        consume(p, TokSemicolon, "';'");
    }
    consume(p, TokRBrace, "'}'");
}

/* ── match statement ── */

static node_t *parse_match_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'match' */

    node_t *subject = parse_expr(p);
    consume(p, TokLBrace, "'{'");

    node_t *n = make_node(NodeMatchStmt, line);
    n->as.match_stmt.expr = subject;
    node_list_init(&n->as.match_stmt.arms);

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_t *arm = make_node(NodeMatchArm, p->current.line);

        /* wildcard arm: _ => { ... } */
        if (check(p, TokIdent) &&
                p->current.length == 1 && p->current.start[0] == '_') {
            advance_parser(p);
            arm->as.match_arm.is_wildcard = True;
            arm->as.match_arm.enum_name    = Null;
            arm->as.match_arm.variant_name = Null;
            arm->as.match_arm.bind_name    = Null;
        } else {
            /* pattern: EnumName.Variant  or  EnumName.Variant(bind) */
            arm->as.match_arm.is_wildcard = False;
            token_t etok = consume(p, TokIdent, "enum name");
            arm->as.match_arm.enum_name = copy_token_text(etok);
            consume(p, TokDot, "'.'");
            token_t vtok = consume(p, TokIdent, "variant name");
            arm->as.match_arm.variant_name = copy_token_text(vtok);
            arm->as.match_arm.bind_name = Null;
            if (match_tok(p, TokLParen)) {
                token_t btok = consume(p, TokIdent, "binding name");
                arm->as.match_arm.bind_name = copy_token_text(btok);
                consume(p, TokRParen, "')'");
            }
        }

        consume(p, TokFatArrow, "'=>'");
        arm->as.match_arm.body = parse_block(p);
        node_list_push(&n->as.match_stmt.arms, arm);
        match_tok(p, TokComma); /* optional trailing comma */
    }

    consume(p, TokRBrace, "'}'");
    return n;
}

/* ── enum body parsing ── */

static void parse_enum_body(parser_t *p, node_t *decl) {
    consume(p, TokLBrace, "'{'");
    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        token_t vname = consume(p, TokIdent, "variant name");
        node_t *v = make_node(NodeEnumVariant, vname.line);
        v->as.enum_variant.name = copy_token_text(vname);
        v->as.enum_variant.has_payload = False;

        /* tagged variant: Variant(type) or Variant(stack type) / Variant(heap type) */
        if (match_tok(p, TokLParen)) {
            v->as.enum_variant.has_payload = True;
            storage_t ps = StorageDefault;
            if (check(p, TokStack))      { ps = StorageStack; advance_parser(p); }
            else if (check(p, TokHeap)) { ps = StorageHeap;  advance_parser(p); }
            v->as.enum_variant.payload_storage = ps;
            v->as.enum_variant.payload_type = parse_type(p);
            /* consume optional field name */
            if (check(p, TokIdent)) advance_parser(p);
            consume(p, TokRParen, "')'");
        }

        node_list_push(&decl->as.type_decl.variants, v);
        if (!match_tok(p, TokComma)) break;
    }
    consume(p, TokRBrace, "'}'");
}

/* ── type declaration: type Name: struct/enum/alias ── */

static node_t *parse_type_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'type' */
    token_t name_tok = consume(p, TokIdent, "type name");
    consume(p, TokColon, "':'");

    node_t *n = make_node(NodeTypeDecl, line);
    n->as.type_decl.name = copy_token_text(name_tok);
    n->as.type_decl.linkage = linkage;
    node_list_init(&n->as.type_decl.fields);
    node_list_init(&n->as.type_decl.methods);
    node_list_init(&n->as.type_decl.variants);

    if (check(p, TokStruct)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclStruct;
        parse_struct_body(p, n);
        match_tok(p, TokSemicolon); /* optional trailing ; */
    } else if (check(p, TokEnum)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclEnum;
        parse_enum_body(p, n);
        match_tok(p, TokSemicolon); /* optional trailing ; */
    } else {
        /* type alias */
        n->as.type_decl.decl_kind = TypeDeclAlias;
        n->as.type_decl.alias_type = parse_type(p);
        consume(p, TokSemicolon, "';'");
    }

    return n;
}

/* ── cinclude ── */

static node_t *parse_cinclude(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'cinclude' */

    /* header name — accept string literal */
    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        log_err("line %lu: expected header string after cinclude", p->current.line);
        p->had_error = True;
        return make_node(NodeCinclude, line);
    }
    advance_parser(p);
    token_t htok = p->previous;
    char *header = ast_strdup(htok.start + 1, htok.length - 2);

    char *alias = Null;
    if (match_tok(p, TokEq)) {
        token_t atok = consume(p, TokIdent, "alias name");
        alias = copy_token_text(atok);
    }
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeCinclude, line);
    n->as.cinclude.header = header;
    n->as.cinclude.alias = alias;
    return n;
}

/* ── imp ── */

static node_t *parse_imp(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    token_t mod_name = consume(p, TokIdent, "module name");
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeImpDecl, line);
    n->as.imp_decl.module_name = copy_token_text(mod_name);
    return n;
}

/* ── function declaration ── */

static node_t *parse_fn_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'fn' */

    token_t name_tok = consume(p, TokIdent, "function name");
    char *name = copy_token_text(name_tok);

    /* check for Type.method pattern */
    boolean_t is_method = False;
    char *struct_name = Null;
    if (check(p, TokDot)) {
        advance_parser(p);
        struct_name = name;
        is_method = True;

        /* method name: allow ident, new, rem */
        if (check(p, TokIdent)) {
            name = copy_token_text(p->current);
            advance_parser(p);
        } else if (check(p, TokNew)) {
            name = ast_strdup("new", 3);
            advance_parser(p);
        } else if (check(p, TokRem)) {
            name = ast_strdup("rem", 3);
            advance_parser(p);
        } else {
            log_err("line %lu: expected method name after '.'", p->current.line);
            p->had_error = True;
            name = ast_strdup("?", 1);
        }
    }

    consume(p, TokLParen, "'('");
    node_list_t params;
    node_list_init(&params);
    if (!check(p, TokRParen) && !check(p, TokVoid)) {
        type_info_t last_type = NO_TYPE;
        storage_t last_storage = StorageDefault;
        do {
            /* optional storage qualifier on parameter */
            storage_t param_storage = StorageDefault;
            if (check(p, TokStack))      { param_storage = StorageStack;  advance_parser(p); }
            else if (check(p, TokHeap)) { param_storage = StorageHeap;   advance_parser(p); }
            if (param_storage != StorageDefault) last_storage = param_storage;
            else param_storage = last_storage;
            if (can_start_param_type(p))
                last_type = parse_type(p);
            token_t pname = consume(p, TokIdent, "parameter name");
            node_t *param = make_node(NodeVarDecl, pname.line);
            param->as.var_decl.name = copy_token_text(pname);
            param->as.var_decl.type = last_type;
            param->as.var_decl.storage = param_storage;
            node_list_push(&params, param);
        } while (match_tok(p, TokComma));
    } else if (check(p, TokVoid)) {
        advance_parser(p);
    }
    consume(p, TokRParen, "')'");

    consume(p, TokColon, "':'");

    /* return type(s): single type or [type, type, ...] */
    usize_t ret_count = 0;
    type_info_t *ret_types = Null;
    if (check(p, TokLBracket)) {
        advance_parser(p);
        node_list_t tmp;
        node_list_init(&tmp);
        do {
            type_info_t rt = parse_type(p);
            node_t *dummy = make_node(NodeVarDecl, p->previous.line);
            dummy->as.var_decl.type = rt;
            node_list_push(&tmp, dummy);
        } while (match_tok(p, TokComma));
        consume(p, TokRBracket, "']'");
        ret_count = tmp.count;
        ret_types = alloc_type_array(ret_count);
        for (usize_t i = 0; i < ret_count; i++)
            ret_types[i] = tmp.items[i]->as.var_decl.type;
    } else {
        ret_count = 1;
        ret_types = alloc_type_array(1);
        ret_types[0] = parse_type(p);
    }

    node_t *body = parse_block(p);

    node_t *n = make_node(NodeFnDecl, line);
    n->as.fn_decl.name = name;
    n->as.fn_decl.linkage = linkage;
    n->as.fn_decl.return_types = ret_types;
    n->as.fn_decl.return_count = ret_count;
    n->as.fn_decl.params = params;
    n->as.fn_decl.body = body;
    n->as.fn_decl.is_method = is_method;
    n->as.fn_decl.struct_name = struct_name;
    return n;
}

/* ── top-level declarations ── */

static node_t *parse_top_decl(parser_t *p) {
    /* cinclude */
    if (check(p, TokCinclude)) return parse_cinclude(p);

    /* imp */
    if (check(p, TokImp)) return parse_imp(p);

    linkage_t linkage = LinkageNone;
    if (check(p, TokInt) || check(p, TokExt)) {
        linkage = check(p, TokInt) ? LinkageInternal : LinkageExternal;
        advance_parser(p);
    }

    /* type declaration */
    if (check(p, TokType)) return parse_type_decl(p, linkage);

    /* function declaration */
    if (check(p, TokFn)) return parse_fn_decl(p, linkage);

    /* global variable */
    return parse_var_decl(p, linkage);
}

/* ── entry point ── */

node_t *parse(const char *source) {
    parser_t p;
    init_lexer(&p.lexer, source);
    p.had_error = False;
    p.group_storage = StorageDefault;
    advance_parser(&p);

    consume(&p, TokMod, "'mod'");
    token_t mod_name = consume(&p, TokIdent, "module name");
    consume(&p, TokSemicolon, "';'");

    node_t *module = make_node(NodeModule, 1);
    module->as.module.name = copy_token_text(mod_name);
    node_list_init(&module->as.module.decls);

    while (!check(&p, TokEof)) {
        node_list_push(&module->as.module.decls, parse_top_decl(&p));
    }

    if (p.had_error) return Null;
    return module;
}
