#include <string.h>
#include "parser.h"

typedef struct {
    lexer_t lexer;
    token_t current;
    token_t previous;
    boolean_t had_error;
} parser_t;

static void advance_parser(parser_t *p) {
    p->previous = p->current;
    for (;;) {
        p->current = next_token(&p->lexer);
        if (p->current.kind != TokError) break;
        log_err("line %lu: %.*s", p->current.line, (int)p->current.length, p->current.start);
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
            p->current.line, msg, (int)p->current.length, p->current.start);
    p->had_error = True;
    return p->current;
}

static node_t *parse_expr(parser_t *p);
static node_t *parse_block(parser_t *p);
static node_t *parse_statement(parser_t *p);
static node_t *parse_var_decl(parser_t *p, linkage_t linkage);

static boolean_t is_type_token(token_kind_t k) {
    return k == TokI8 || k == TokI16 || k == TokI32 || k == TokI64
        || k == TokStr || k == TokBool || k == TokVoid;
}

static type_kind_t token_to_type(token_kind_t k) {
    switch (k) {
        case TokVoid: return TypeVoid;
        case TokI8:   return TypeI8;
        case TokI16:  return TypeI16;
        case TokI32:  return TypeI32;
        case TokI64:  return TypeI64;
        case TokStr:  return TypeStr;
        case TokBool: return TypeBool;
        default:      return TypeVoid;
    }
}

/* ── primary ── */

static node_t *parse_primary(parser_t *p) {
    if (check(p, TokIntLit)) {
        advance_parser(p);
        node_t *n = make_node(NodeIntLitExpr, p->previous.line);
        long val = 0;
        for (usize_t i = 0; i < p->previous.length; i++)
            val = val * 10 + (p->previous.start[i] - '0');
        n->as.int_lit.value = val;
        return n;
    }

    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        boolean_t is_heap = check(p, TokHeapStr);
        advance_parser(p);
        node_t *n = make_node(NodeStrLitExpr, p->previous.line);
        token_t t = p->previous;
        usize_t slen = t.length - 2;
        n->as.str_lit.value = ast_strdup(t.start + 1, slen);
        n->as.str_lit.is_heap = is_heap;
        return n;
    }

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

    if (check(p, TokIdent)) {
        advance_parser(p);
        node_t *n = make_node(NodeIdentExpr, p->previous.line);
        n->as.ident.name = copy_token_text(p->previous);
        return n;
    }

    if (match_tok(p, TokLParen)) {
        node_t *expr = parse_expr(p);
        consume(p, TokRParen, "')'");
        return expr;
    }

    log_err("line %lu: expected expression", p->current.line);
    p->had_error = True;
    return make_node(NodeIntLitExpr, p->current.line);
}

/* ── call ── */

static node_t *parse_call(parser_t *p) {
    node_t *expr = parse_primary(p);

    if (expr->kind == NodeIdentExpr && check(p, TokLParen)) {
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
        return n;
    }
    return expr;
}

/* ── postfix ── */

static node_t *parse_postfix(parser_t *p) {
    node_t *expr = parse_call(p);
    if (check(p, TokPlusPlus) || check(p, TokMinusMinus)) {
        token_kind_t op = p->current.kind;
        advance_parser(p);
        node_t *n = make_node(NodeUnaryPostfixExpr, expr->line);
        n->as.unary.op = op;
        n->as.unary.operand = expr;
        return n;
    }
    return expr;
}

/* ── unary ── */

static node_t *parse_unary(parser_t *p) {
    if (check(p, TokPlusPlus) || check(p, TokMinusMinus)
        || check(p, TokBang) || check(p, TokMinus)) {
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

/* ── multiplicative ── */

static node_t *parse_multiplication(parser_t *p) {
    node_t *left = parse_unary(p);
    while (check(p, TokStar) || check(p, TokSlash)) {
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

/* ── additive ── */

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

/* ── comparison ── */

static node_t *parse_comparison(parser_t *p) {
    node_t *left = parse_addition(p);
    while (check(p, TokLt) || check(p, TokGt) || check(p, TokLtEq)
           || check(p, TokGtEq) || check(p, TokEqEq) || check(p, TokBangEq)) {
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

/* ── assignment ── */

static node_t *parse_assignment(parser_t *p) {
    node_t *expr = parse_comparison(p);

    if (expr->kind == NodeIdentExpr) {
        if (check(p, TokEq)) {
            advance_parser(p);
            node_t *value = parse_assignment(p);
            node_t *n = make_node(NodeAssignExpr, expr->line);
            n->as.assign.target = expr;
            n->as.assign.value = value;
            return n;
        }
        if (check(p, TokPlusEq) || check(p, TokMinusEq)) {
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
    node_t *init = parse_var_decl(p, LinkageNone);
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

static node_t *parse_ret_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_t *value = Null;
    if (!check(p, TokSemicolon)) value = parse_expr(p);
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeRetStmt, line);
    n->as.ret_stmt.value = value;
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

static boolean_t can_start_var_decl(parser_t *p) {
    return check(p, TokStack) || check(p, TokHeap) || check(p, TokAtomic)
        || is_type_token(p->current.kind);
}

static node_t *parse_var_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    storage_t storage = StorageDefault;
    boolean_t is_atomic = False;

    if (match_tok(p, TokStack))      storage = StorageStack;
    else if (match_tok(p, TokHeap))  storage = StorageHeap;

    if (match_tok(p, TokAtomic)) is_atomic = True;

    if (!is_type_token(p->current.kind)) {
        log_err("line %lu: expected type", p->current.line);
        p->had_error = True;
        return make_node(NodeVarDecl, line);
    }
    type_kind_t type = token_to_type(p->current.kind);
    advance_parser(p);

    token_t name_tok = consume(p, TokIdent, "variable name");
    char *name = copy_token_text(name_tok);

    node_t *init_expr = Null;
    if (match_tok(p, TokEq)) init_expr = parse_expr(p);
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeVarDecl, line);
    n->as.var_decl.name = name;
    n->as.var_decl.type = type;
    n->as.var_decl.storage = storage;
    n->as.var_decl.linkage = linkage;
    n->as.var_decl.is_atomic = is_atomic;
    n->as.var_decl.init = init_expr;
    return n;
}

static node_t *parse_statement(parser_t *p) {
    if (check(p, TokFor))   return parse_for_stmt(p);
    if (check(p, TokRet))   return parse_ret_stmt(p);
    if (check(p, TokDebug)) return parse_debug_stmt(p);
    if (can_start_var_decl(p)) return parse_var_decl(p, LinkageNone);
    return parse_expr_stmt(p);
}

/* ── top-level declarations ── */

static node_t *parse_fn_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    advance_parser(p);

    token_t name_tok = consume(p, TokIdent, "function name");
    char *name = copy_token_text(name_tok);

    consume(p, TokLParen, "'('");
    node_list_t params;
    node_list_init(&params);
    if (!check(p, TokRParen) && !check(p, TokVoid)) {
        do {
            type_kind_t ptype = token_to_type(p->current.kind);
            advance_parser(p);
            token_t pname = consume(p, TokIdent, "parameter name");
            node_t *param = make_node(NodeVarDecl, pname.line);
            param->as.var_decl.name = copy_token_text(pname);
            param->as.var_decl.type = ptype;
            node_list_push(&params, param);
        } while (match_tok(p, TokComma));
    } else if (check(p, TokVoid)) {
        advance_parser(p);
    }
    consume(p, TokRParen, "')'");

    consume(p, TokColon, "':'");
    type_kind_t return_type = token_to_type(p->current.kind);
    advance_parser(p);

    node_t *body = parse_block(p);

    node_t *n = make_node(NodeFnDecl, line);
    n->as.fn_decl.name = name;
    n->as.fn_decl.linkage = linkage;
    n->as.fn_decl.return_type = return_type;
    n->as.fn_decl.params = params;
    n->as.fn_decl.body = body;
    return n;
}

static node_t *parse_top_decl(parser_t *p) {
    linkage_t linkage = LinkageNone;
    if (check(p, TokInt) || check(p, TokExt)) {
        linkage = check(p, TokInt) ? LinkageInternal : LinkageExternal;
        advance_parser(p);
    }
    if (check(p, TokFn)) return parse_fn_decl(p, linkage);
    return parse_var_decl(p, linkage);
}

/* ── entry point ── */

node_t *parse(const char *source) {
    parser_t p;
    init_lexer(&p.lexer, source);
    p.had_error = False;
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
