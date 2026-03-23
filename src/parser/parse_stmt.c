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

    /* init — could be a var decl, expression statement, or empty */
    node_t *init = Null;
    if (check(p, TokSemicolon)) {
        advance_parser(p); /* empty init: for (; ...) */
    } else if (can_start_var_decl(p)) {
        init = parse_var_decl(p, LinkageNone);
    } else {
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
    node_t *cond = parse_expr(p);
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
    node_t *cond = parse_expr(p);
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

static node_t *parse_print_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'print' */
    consume(p, TokDot, "'.'");
    consume(p, TokLParen, "'('");

    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        diag_begin_error("print.() requires a string literal as the first argument");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal here");
        diag_help("example: print.('value = {x}')");
        diag_finish();
        p->had_error = True;
        return make_node(NodePrintStmt, line);
    }

    token_t fmt_tok = p->current;
    advance_parser(p);

    /* Store the raw format string (no escape processing): gen_print handles
       escape sequences and placeholder scanning in a single pass so that
       \{ can be distinguished from a real { placeholder. */
    usize_t fmt_len = fmt_tok.length - 2;
    char *fmt = ast_strdup(fmt_tok.start + 1, fmt_len);

    node_t *n = make_node(NodePrintStmt, line);
    n->as.print_stmt.fmt     = fmt;
    n->as.print_stmt.fmt_len = fmt_len;
    node_list_init(&n->as.print_stmt.args);

    while (match_tok(p, TokComma))
        node_list_push(&n->as.print_stmt.args, parse_expr(p));

    consume(p, TokRParen, "')'");
    consume(p, TokSemicolon, "';'");
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
    int flags = 0;

    if (match_tok(p, TokStack))      storage = StorageStack;
    else if (match_tok(p, TokHeap))  storage = StorageHeap;
    else if (p->group_storage != StorageDefault) storage = p->group_storage;

    if (match_tok(p, TokAtomic))   flags |= VdeclAtomic;
    if (match_tok(p, TokConst))    flags |= VdeclConst;
    if (match_tok(p, TokFinal))    flags |= VdeclFinal;
    if (match_tok(p, TokVolatile)) flags |= VdeclVolatile;
    if (match_tok(p, TokTls))      flags |= VdeclTls;

    /* let binding: infer type(s) from initializer
       single:  stack let name   = expr;
       multi:   stack let [x, y] = fn_call(); */
    if (match_tok(p, TokLet)) {
        flags |= VdeclLet;

        /* single-variable form: stack let name = expr; */
        if (check(p, TokIdent)) {
            token_t name_tok = p->current;
            advance_parser(p);
            consume(p, TokEq, "'='");
            node_t *init = parse_expr(p);
            consume(p, TokSemicolon, "';'");
            node_t *n = make_node(NodeVarDecl, line);
            n->as.var_decl.name    = copy_token_text(name_tok);
            n->as.var_decl.type    = NO_TYPE; /* filled in by codegen */
            n->as.var_decl.storage = storage;
            n->as.var_decl.linkage = linkage;
            n->as.var_decl.flags   = flags;
            n->as.var_decl.init    = init;
            return n;
        }

        /* multi-assign form: stack let [x, y] = fn_call(); */
        consume(p, TokLBracket, "'['");
        node_list_t targets;
        node_list_init(&targets);
        node_list_t values;
        node_list_init(&values);
        do {
            token_t name_tok = consume(p, TokIdent, "variable name");
            node_t *var = make_node(NodeVarDecl, name_tok.line);
            var->as.var_decl.name    = copy_token_text(name_tok);
            var->as.var_decl.type    = NO_TYPE; /* filled in by codegen */
            var->as.var_decl.storage = storage;
            var->as.var_decl.linkage = linkage;
            var->as.var_decl.flags   = flags;
            node_list_push(&targets, var);
        } while (match_tok(p, TokComma));
        consume(p, TokRBracket, "']'");
        consume(p, TokEq, "'='");
        node_list_push(&values, parse_expr(p));
        consume(p, TokSemicolon, "';'");
        node_t *n = make_node(NodeMultiAssign, line);
        n->as.multi_assign.targets = targets;
        n->as.multi_assign.values  = values;
        return n;
    }

    if (!can_start_type(p)) {
        diag_begin_error("expected a type in variable declaration");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a type here");
        diag_note("variable declarations require a storage qualifier and type: stack i32 x = 0;");
        diag_finish();
        p->had_error = True;
        advance_parser(p); /* skip bad token to avoid infinite loop */
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
            var->as.var_decl.flags = flags;
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

    /* array: type name[size] — size may be an int literal or a named const */
    long array_size = 0;
    char *array_size_name = Null;
    if (match_tok(p, TokLBracket)) {
        flags |= VdeclArray;
        if (check(p, TokIntLit)) {
            array_size = parse_int_value(p->current);
            advance_parser(p);
        } else if (check(p, TokIdent)) {
            array_size_name = copy_token_text(p->current);
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
    n->as.var_decl.flags = flags;
    n->as.var_decl.array_size = array_size;
    n->as.var_decl.array_size_name = array_size_name;
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
    if (check(p, TokLBrace))       return parse_block(p);
    if (check(p, TokFor))          return parse_for_stmt(p);
    if (check(p, TokWhile))        return parse_while_stmt(p);
    if (check(p, TokDo))           return parse_do_while_stmt(p);
    if (check(p, TokInf))          return parse_inf_loop(p);
    if (check(p, TokIf))           return parse_if_stmt(p);
    if (check(p, TokRet))          return parse_ret_stmt(p);
    if (check(p, TokPrint))        return parse_print_stmt(p);
    if (check(p, TokMatch))        return parse_match_stmt(p);
    if (check(p, TokDefer))        return parse_defer_stmt(p);
    if (check(p, TokSwitch))       return parse_switch_stmt(p);
    if (check(p, TokAsm))          return parse_asm_stmt(p);
    if (check(p, TokComptimeIf))   return parse_comptime_if(p);

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

    /* future.op(...) is an expression statement, not a var decl — peek ahead */
    if (check(p, TokFuture)) {
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t is_future_op = check(p, TokDot);
        restore_state(p, snap);
        if (is_future_op) return parse_expr_stmt(p);
    }

    if (can_start_var_decl(p)) return parse_var_decl(p, LinkageNone);
    return parse_expr_stmt(p);
}
