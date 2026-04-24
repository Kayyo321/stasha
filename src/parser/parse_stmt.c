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

/* parse_body — accepts either a brace block or a '=>' one-liner.
   `=> stmt;` is sugar for `{ stmt; }`: wraps the single statement
   in a synthetic NodeBlock so callers always receive a block node. */
static node_t *parse_body(parser_t *p) {
    if (match_tok(p, TokFatArrow)) {
        usize_t line = p->current.line;
        node_list_t stmts;
        node_list_init(&stmts);
        node_list_push(&stmts, parse_statement(p));
        node_t *block = make_node(NodeBlock, line);
        block->as.block.stmts = stmts;
        return block;
    }
    return parse_block(p);
}

static node_t *parse_for_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);

    /* optional guard parens: for (init; cond; update) or for init; cond; update */
    boolean_t guarded = match_tok(p, TokLParen);

    /* init — could be a var decl, expression statement, or empty */
    node_t *init = Null;
    if (check(p, TokSemicolon)) {
        advance_parser(p); /* empty init: for (; ...) / for ; ... */
    } else if (can_start_var_decl(p)) {
        init = parse_var_decl(p, LinkageNone);
    } else {
        init = parse_expr(p);
        consume(p, TokSemicolon, "';'");
    }

    node_t *cond = parse_expr(p);
    consume(p, TokSemicolon, "';'");
    node_t *update = parse_expr(p);
    if (guarded) consume(p, TokRParen, "')'");
    node_t *body = parse_body(p);

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
    node_t *body = parse_body(p);

    node_t *n = make_node(NodeWhileStmt, line);
    n->as.while_stmt.cond = cond;
    n->as.while_stmt.body = body;
    return n;
}

static node_t *parse_foreach_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'foreach' */

    token_t iter_tok = consume(p, TokIdent, "iteration variable name");
    char *iter_name  = copy_token_text(iter_tok);

    consume(p, TokIn, "'in'");

    node_t *slice = parse_expr(p);
    node_t *body  = parse_body(p);

    node_t *n = make_node(NodeForeachStmt, line);
    n->as.foreach_stmt.iter_name = iter_name;
    n->as.foreach_stmt.slice     = slice;
    n->as.foreach_stmt.body      = body;
    return n;
}

static node_t *parse_do_while_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'do' */
    node_t *body = parse_body(p);
    consume(p, TokWhile, "'while'");
    /* optional guard parens: do { } while (cond); or do { } while cond; */
    boolean_t guarded = match_tok(p, TokLParen);
    node_t *cond = parse_expr(p);
    if (guarded) consume(p, TokRParen, "')'");
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeDoWhileStmt, line);
    n->as.do_while_stmt.body = body;
    n->as.do_while_stmt.cond = cond;
    return n;
}

static node_t *parse_inf_loop(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_t *body = parse_body(p);
    node_t *n = make_node(NodeInfLoop, line);
    n->as.inf_loop.body = body;
    return n;
}

static node_t *parse_if_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);
    node_t *cond = parse_expr(p);
    node_t *then_block = parse_body(p);
    node_t *else_block = Null;
    if (match_tok(p, TokElse)) {
        if (check(p, TokIf))
            else_block = parse_if_stmt(p);
        else
            else_block = parse_body(p);
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

    boolean_t to_stderr = False;
    if (check(p, TokErrorType)) {
        advance_parser(p); /* consume 'error' */
        consume(p, TokDot, "'.'");
        to_stderr = True;
    }

    consume(p, TokLParen, "'('");

    /* Optional '@' marks an inline-expression format string:
         print.(@'a + b = {a + b}');
       Each {expr} / {expr:spec} is sub-parsed and added to args; the
       stored fmt is rewritten to {} / {:spec} so gen_print stays unchanged. */
    boolean_t inline_fmt = False;
    if (check(p, TokAt)) {
        advance_parser(p); /* consume '@' */
        inline_fmt = True;
    }

    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        diag_begin_error("print.() requires a string literal as the first argument");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal here");
        diag_help(inline_fmt
            ? "example: print.(@'value = {x}')"
            : "example: print.('value = {x}')");
        diag_finish();
        p->had_error = True;
        return make_node(NodePrintStmt, line);
    }

    token_t fmt_tok = p->current;
    advance_parser(p);

    usize_t raw_len = fmt_tok.length - 2;
    const char *raw = fmt_tok.start + 1;

    node_t *n = make_node(NodePrintStmt, line);
    n->as.print_stmt.to_stderr = to_stderr;
    node_list_init(&n->as.print_stmt.args);

    if (inline_fmt) {
        /* Walk raw fmt: copy literals/escapes verbatim, sub-parse each
           {expr[:spec]} into args, emit {} or {:spec} into the output.
           Output cannot exceed raw_len, so reuse the arena-allocated copy
           as a scratch buffer (we overwrite from index 0). */
        char *out = ast_strdup(raw, raw_len);
        usize_t out_len = 0;

        for (usize_t i = 0; i < raw_len; ) {
            if (raw[i] == '\\' && i + 1 < raw_len) {
                out[out_len++] = raw[i++];
                out[out_len++] = raw[i++];
                continue;
            }
            if (raw[i] != '{') { out[out_len++] = raw[i++]; continue; }

            usize_t expr_start = i + 1;
            usize_t j = expr_start;
            int depth = 0;
            usize_t colon_pos = 0;
            boolean_t has_colon = False;
            boolean_t found_end = False;
            boolean_t bad = False;

            while (j < raw_len) {
                char c = raw[j];
                if (c == '\\' && j + 1 < raw_len) { j += 2; continue; }
                if (c == '(' || c == '[') { depth++; j++; continue; }
                if (c == ')' || c == ']') { if (depth > 0) depth--; j++; continue; }
                if (c == '{' && depth == 0) {
                    diag_begin_error("nested '{' inside print format placeholder");
                    diag_span(SRC_LOC(line, 0, 0), True,
                              "use '\\{' for a literal '{'");
                    diag_finish();
                    p->had_error = True;
                    bad = True;
                    break;
                }
                if (c == '}' && depth == 0) { found_end = True; break; }
                if (c == ':' && depth == 0 && !has_colon) { colon_pos = j; has_colon = True; }
                j++;
            }
            if (bad) break;
            if (!found_end) {
                diag_begin_error("unterminated '{' in print format string");
                diag_span(SRC_LOC(line, 0, 0), True, "missing '}' here");
                diag_finish();
                p->had_error = True;
                break;
            }

            usize_t expr_end = has_colon ? colon_pos : j;
            usize_t expr_len = expr_end - expr_start;
            if (expr_len == 0) {
                diag_begin_error("empty expression in print format placeholder");
                diag_span(SRC_LOC(line, 0, 0), True,
                          "expected an expression inside '{...}'");
                diag_finish();
                p->had_error = True;
            } else {
                char *expr_src = ast_strdup(raw + expr_start, expr_len);
                parser_t sub;
                memset(&sub, 0, sizeof(sub));
                sub.stream    = Null;
                sub.had_error = False;
                init_lexer(&sub.lexer, expr_src);
                advance_parser(&sub);
                node_t *expr_node = parse_expr(&sub);
                if (sub.had_error) p->had_error = True;
                node_list_push(&n->as.print_stmt.args, expr_node);
            }

            out[out_len++] = '{';
            if (has_colon) {
                /* copy ':spec' verbatim — colon_pos points at the ':' */
                usize_t spec_len = j - colon_pos;
                memcpy(out + out_len, raw + colon_pos, spec_len);
                out_len += spec_len;
            }
            out[out_len++] = '}';
            i = j + 1;
        }

        out[out_len] = '\0';
        n->as.print_stmt.fmt     = out;
        n->as.print_stmt.fmt_len = out_len;
    } else {
        /* Store raw fmt; gen_print decodes escapes and scans placeholders
           in a single pass so '\{' can be distinguished from a real '{'. */
        n->as.print_stmt.fmt     = ast_strdup(raw, raw_len);
        n->as.print_stmt.fmt_len = raw_len;
    }

    while (match_tok(p, TokComma)) {
        node_t *arg = parse_expr(p);
        if (inline_fmt) {
            diag_begin_error("print.(@'...') does not accept trailing arguments");
            diag_span(SRC_LOC(line, 0, 0), True,
                      "embed values directly in the format string instead");
            diag_finish();
            p->had_error = True;
        } else {
            node_list_push(&n->as.print_stmt.args, arg);
        }
    }

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

/* watch.(T name) => { body }  — register typed handler.
   Body is parsed via parse_body so `=> stmt;` and `{ block }` both work. */
static node_t *parse_watch_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);                       /* consume 'watch' */
    consume(p, TokDot,    "'.'");
    consume(p, TokLParen, "'('");
    type_info_t type = parse_type(p);
    token_t name_tok = consume(p, TokIdent, "handler parameter name");
    char *param_name = copy_token_text(name_tok);
    consume(p, TokRParen, "')'");
    node_t *body = parse_body(p);
    node_t *n = make_node(NodeWatchStmt, line);
    n->as.watch_stmt.type       = type;
    n->as.watch_stmt.param_name = param_name;
    n->as.watch_stmt.body       = body;
    return n;
}

/* send.(value); — synchronous dispatch to handlers of value's static type. */
static node_t *parse_send_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);                       /* consume 'send' */
    consume(p, TokDot,    "'.'");
    consume(p, TokLParen, "'('");
    node_t *value = parse_expr(p);
    consume(p, TokRParen, "')'");
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeSendStmt, line);
    n->as.send_stmt.value = value;
    return n;
}

/* quit.(code); — exit(code) guarded against reentry from @[[exit]] blocks. */
static node_t *parse_quit_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p);                       /* consume 'quit' */
    consume(p, TokDot,    "'.'");
    consume(p, TokLParen, "'('");
    node_t *code = parse_expr(p);
    consume(p, TokRParen, "')'");
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeQuitStmt, line);
    n->as.quit_stmt.code = code;
    return n;
}

/* ── variable declaration ── */

static node_t *parse_var_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    storage_t storage = StorageDefault;
    int flags = 0;

    if (match_tok(p, TokStack))      storage = StorageStack;
    else if (match_tok(p, TokHeap))  storage = StorageHeap;

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
            ast_set_loc(n, name_tok);
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
            ast_set_loc(var, name_tok);
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
        if (!p->panic_mode) {
            diag_begin_error("expected a type in variable declaration");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a type here");
            diag_note("non-pointer variables need only a type: i32 x = 0;  pointer variables need stack/heap: stack i32 *rw p;");
            diag_finish();
        }
        p->had_error  = True;
        p->panic_mode = True;
        skip_to_recovery_point(p);
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
            ast_set_loc(var, name_tok);
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

    token_t name_tok = consume_name(p, "variable name");
    char *name = copy_token_text(name_tok);

    /* array: type name[d0][d1]... — arbitrary nesting depth up to 8 */
    int  arr_ndim = 0;
    long arr_sizes[8]       = {0,0,0,0,0,0,0,0};
    char *arr_size_names[8] = {Null,Null,Null,Null,Null,Null,Null,Null};
    while (check(p, TokLBracket) && arr_ndim < 8) {
        advance_parser(p); /* consume '[' */
        if (check(p, TokIntLit)) {
            arr_sizes[arr_ndim] = parse_int_value(p->current);
            advance_parser(p);
        } else if (check(p, TokIdent)) {
            arr_size_names[arr_ndim] = copy_token_text(p->current);
            advance_parser(p);
        }
        consume(p, TokRBracket, "']'");
        arr_ndim++;
    }
    if (arr_ndim > 0) flags |= VdeclArray;

    node_t *init_expr = Null;
    if (match_tok(p, TokEq)) init_expr = parse_expr(p);
    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeVarDecl, line);
    ast_set_loc(n, name_tok);
    n->as.var_decl.name    = name;
    n->as.var_decl.type    = type;
    n->as.var_decl.storage = storage;
    n->as.var_decl.linkage = linkage;
    n->as.var_decl.flags   = flags;
    n->as.var_decl.array_ndim = arr_ndim;
    for (int _i = 0; _i < arr_ndim; _i++) {
        n->as.var_decl.array_sizes[_i]      = arr_sizes[_i];
        n->as.var_decl.array_size_names[_i] = arr_size_names[_i];
    }
    n->as.var_decl.init = init_expr;
    return n;
}

static node_t *parse_with_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'with' */

    /* Parse the binding declaration (var decl, no semicolon consumed yet) */
    node_t *decl = Null;

    /* Bare multi-binding: [a, b] = expr  (no type prefix, no let, no qualifier) */
    if (check(p, TokLBracket)) {
        advance_parser(p);
        node_list_t targets;
        node_list_init(&targets);
        node_list_t values;
        node_list_init(&values);
        do {
            token_t name_tok = consume(p, TokIdent, "variable name");
            node_t *var = make_node(NodeVarDecl, name_tok.line);
            ast_set_loc(var, name_tok);
            var->as.var_decl.name    = copy_token_text(name_tok);
            var->as.var_decl.type    = NO_TYPE;
            var->as.var_decl.storage = StorageDefault;
            var->as.var_decl.flags   = VdeclLet;
            node_list_push(&targets, var);
        } while (match_tok(p, TokComma));
        consume(p, TokRBracket, "']'");
        consume(p, TokEq, "'='");
        node_list_push(&values, parse_expr(p));
        node_t *ma = make_node(NodeMultiAssign, line);
        ma->as.multi_assign.targets = targets;
        ma->as.multi_assign.values  = values;
        decl = ma;
    }
    /* Bare single inferred binding: name = expr  (ident followed directly by '=') */
    else if (check(p, TokIdent)) {
        parser_state_t snap = save_state(p);
        token_t name_tok = p->current;
        advance_parser(p);
        if (check(p, TokEq)) {
            advance_parser(p); /* consume '=' */
            node_t *init = parse_expr(p);
            node_t *var = make_node(NodeVarDecl, name_tok.line);
            ast_set_loc(var, name_tok);
            var->as.var_decl.name    = copy_token_text(name_tok);
            var->as.var_decl.type    = NO_TYPE;
            var->as.var_decl.storage = StorageDefault;
            var->as.var_decl.flags   = VdeclLet;
            var->as.var_decl.init    = init;
            decl = var;
        } else {
            restore_state(p, snap);
        }
    }

    if (decl == Null && can_start_var_decl(p)) {
        /* parse as var decl but manually — we stop before the ';' at the ';' sep */
        storage_t storage = StorageDefault;
        int flags = 0;
        if (match_tok(p, TokStack))      storage = StorageStack;
        else if (match_tok(p, TokHeap)) storage = StorageHeap;

        if (match_tok(p, TokLet)) {
            flags |= VdeclLet;
            /* multi-binding: stack let [a, b] = expr */
            if (check(p, TokLBracket)) {
                advance_parser(p);
                node_list_t targets;
                node_list_init(&targets);
                node_list_t values;
                node_list_init(&values);
                do {
                    token_t name_tok = consume(p, TokIdent, "variable name");
                    node_t *var = make_node(NodeVarDecl, name_tok.line);
                    ast_set_loc(var, name_tok);
                    var->as.var_decl.name    = copy_token_text(name_tok);
                    var->as.var_decl.type    = NO_TYPE;
                    var->as.var_decl.storage = storage;
                    var->as.var_decl.flags   = flags;
                    node_list_push(&targets, var);
                } while (match_tok(p, TokComma));
                consume(p, TokRBracket, "']'");
                consume(p, TokEq, "'='");
                node_list_push(&values, parse_expr(p));
                node_t *ma = make_node(NodeMultiAssign, line);
                ma->as.multi_assign.targets = targets;
                ma->as.multi_assign.values  = values;
                decl = ma;
            } else {
                /* single: stack let name = expr */
                token_t name_tok = consume(p, TokIdent, "variable name");
                consume(p, TokEq, "'='");
                node_t *init = parse_expr(p);
                node_t *var = make_node(NodeVarDecl, name_tok.line);
                ast_set_loc(var, name_tok);
                var->as.var_decl.name    = copy_token_text(name_tok);
                var->as.var_decl.type    = NO_TYPE;
                var->as.var_decl.storage = storage;
                var->as.var_decl.flags   = flags;
                var->as.var_decl.init    = init;
                decl = var;
            }
        } else {
            /* type-inferred forms without 'let':
               stack [a, b] = expr  — multi-bind inferred
               stack name = expr    — single-bind inferred (ident immediately followed by '=') */
            if (check(p, TokLBracket)) {
                /* multi-bind, type inferred (same as stack let [a, b] = expr) */
                flags |= VdeclLet;
                advance_parser(p);
                node_list_t targets;
                node_list_init(&targets);
                node_list_t values;
                node_list_init(&values);
                do {
                    token_t name_tok = consume(p, TokIdent, "variable name");
                    node_t *var = make_node(NodeVarDecl, name_tok.line);
                    ast_set_loc(var, name_tok);
                    var->as.var_decl.name    = copy_token_text(name_tok);
                    var->as.var_decl.type    = NO_TYPE;
                    var->as.var_decl.storage = storage;
                    var->as.var_decl.flags   = flags;
                    node_list_push(&targets, var);
                } while (match_tok(p, TokComma));
                consume(p, TokRBracket, "']'");
                consume(p, TokEq, "'='");
                node_list_push(&values, parse_expr(p));
                node_t *ma = make_node(NodeMultiAssign, line);
                ma->as.multi_assign.targets = targets;
                ma->as.multi_assign.values  = values;
                decl = ma;
            } else {
                /* peek: if ident is immediately followed by '=', type-inferred single bind */
                boolean_t single_inferred = False;
                if (check(p, TokIdent)) {
                    parser_state_t snap = save_state(p);
                    token_t name_tok = p->current;
                    advance_parser(p);
                    if (check(p, TokEq)) {
                        single_inferred = True;
                        advance_parser(p); /* consume '=' */
                        node_t *init = parse_expr(p);
                        node_t *var = make_node(NodeVarDecl, name_tok.line);
                        ast_set_loc(var, name_tok);
                        var->as.var_decl.name    = copy_token_text(name_tok);
                        var->as.var_decl.type    = NO_TYPE;
                        var->as.var_decl.storage = storage;
                        var->as.var_decl.flags   = flags | VdeclLet;
                        var->as.var_decl.init    = init;
                        decl = var;
                    } else {
                        restore_state(p, snap);
                    }
                }
                if (!single_inferred) {
                    /* typed: stack T name = expr  or  stack T [a, b] = expr */
                    type_info_t type = parse_type(p);
                    if (check(p, TokLBracket)) {
                        /* multi: stack T [a, b] = expr */
                        advance_parser(p);
                        node_list_t targets;
                        node_list_init(&targets);
                        node_list_t values;
                        node_list_init(&values);
                        do {
                            token_t name_tok = consume(p, TokIdent, "variable name");
                            node_t *var = make_node(NodeVarDecl, name_tok.line);
                            ast_set_loc(var, name_tok);
                            var->as.var_decl.name    = copy_token_text(name_tok);
                            var->as.var_decl.type    = type;
                            var->as.var_decl.storage = storage;
                            var->as.var_decl.flags   = flags;
                            node_list_push(&targets, var);
                        } while (match_tok(p, TokComma));
                        consume(p, TokRBracket, "']'");
                        consume(p, TokEq, "'='");
                        node_list_push(&values, parse_expr(p));
                        node_t *ma = make_node(NodeMultiAssign, line);
                        ma->as.multi_assign.targets = targets;
                        ma->as.multi_assign.values  = values;
                        decl = ma;
                    } else {
                        token_t name_tok = consume(p, TokIdent, "variable name");
                        consume(p, TokEq, "'='");
                        node_t *init = parse_expr(p);
                        node_t *var = make_node(NodeVarDecl, name_tok.line);
                        ast_set_loc(var, name_tok);
                        var->as.var_decl.name    = copy_token_text(name_tok);
                        var->as.var_decl.type    = type;
                        var->as.var_decl.storage = storage;
                        var->as.var_decl.flags   = flags;
                        var->as.var_decl.init    = init;
                        decl = var;
                    }
                }
            }
        }
    } else if (decl == Null) {
        diag_begin_error("expected a variable declaration after 'with'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a type (or 'stack'/'heap' for pointer vars) here");
        diag_finish();
        p->had_error = True;
        decl = make_node(NodeVarDecl, line);
    }

    /* semicolon separator between binding and condition */
    consume(p, TokSemicolon, "';' after with binding");

    /* Optional condition expression — if omitted / trivially true, emit 'true' */
    node_t *cond = Null;
    if (!check(p, TokLBrace)) {
        cond = parse_expr(p);
    } else {
        node_t *true_lit = make_node(NodeBoolLitExpr, line);
        true_lit->as.bool_lit.value = True;
        cond = true_lit;
    }

    node_t *body = parse_block(p);
    node_t *else_block = Null;
    if (match_tok(p, TokElse)) {
        else_block = parse_block(p);
    }

    node_t *n = make_node(NodeWithStmt, line);
    n->as.with_stmt.decl = decl;
    n->as.with_stmt.cond = cond;
    n->as.with_stmt.body = body;
    n->as.with_stmt.else_block = else_block;
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

static node_t *parse_statement(parser_t *p) {
    if (check(p, TokLBrace))       return parse_block(p);
    if (check(p, TokFor))          return parse_for_stmt(p);
    if (check(p, TokForeach))      return parse_foreach_stmt(p);
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
    if (check(p, TokWatch))        return parse_watch_stmt(p);
    if (check(p, TokSend))         return parse_send_stmt(p);
    if (check(p, TokQuit))         return parse_quit_stmt(p);

    /* unsafe { body } */
    if (check(p, TokUnsafe)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'unsafe' */
        node_t *body = parse_block(p);
        node_t *n = make_node(NodeUnsafeBlock, line);
        n->as.unsafe_block.body = body;
        return n;
    }

    /* zone name { body }  — lexical zone: freed at closing brace */
    /* zone name;          — manual zone variable declaration      */
    if (check(p, TokZone)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'zone' */

        /* zone.free / zone.move no longer exist — rem.() and mov.() handle those */
        if (check(p, TokDot)) {
            diag_begin_error("'zone.free' and 'zone.move' have been removed");
            diag_span(SRC_LOC(line, 0, 0), True, "use rem.(zone_name) to free a zone");
            diag_help("mov.(zone_name, ptr, size) escapes a pointer out of a zone");
            diag_finish();
            p->had_error = True;
            skip_to_recovery_point(p);
            return make_node(NodeExprStmt, line);
        }

        token_t name_tok = consume(p, TokIdent, "zone name");
        char *zone_name = copy_token_text(name_tok);

        if (check(p, TokLBrace)) {
            /* lexical zone: zone name { body } */
            node_t *body = parse_block(p);
            node_t *n = make_node(NodeZoneDecl, line);
            n->as.zone_decl.name = zone_name;
            n->as.zone_decl.body = body;
            return n;
        }

        /* manual zone statement: zone name; */
        consume(p, TokSemicolon, "';'");
        node_t *n = make_node(NodeZoneStmt, line);
        n->as.zone_stmt.name = zone_name;
        return n;
    }

    /* @comptime if / @comptime assert in statement context */
    if (check(p, TokAt)) {
        parser_state_t snap = save_state(p);
        advance_parser(p); /* consume '@' */
        if (check(p, TokIdent) && p->current.length == 8
                && memcmp(p->current.start, "comptime", 8) == 0) {
            advance_parser(p); /* consume 'comptime' */
            if (check(p, TokIf)) {
                advance_parser(p); /* consume 'if' */
                return parse_comptime_if_body(p);
            }
            if (check(p, TokIdent) && p->current.length == 6
                    && memcmp(p->current.start, "assert", 6) == 0) {
                node_t *ca = parse_at_comptime_assert(p);
                consume(p, TokSemicolon, "';'");
                return ca;
            }
        }
        restore_state(p, snap);
    }

    if (check(p, TokWith))  return parse_with_stmt(p);
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

    /* future.op(...) is an expression statement, not a var decl — peek ahead.
       Distinguish from `future.[T] name = …;` which is a typed-future decl. */
    if (check(p, TokFuture)) {
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t is_future_op = False;
        if (check(p, TokDot)) {
            advance_parser(p); /* consume '.' */
            /* '.' followed by '[' is a typed future type (future.[T]) */
            is_future_op = !check(p, TokLBracket);
        }
        restore_state(p, snap);
        if (is_future_op) return parse_expr_stmt(p);
    }

    /* await(...) / await.(...) / await.all(...) / await.any(...) — expression statements */
    if (check(p, TokAwait)) return parse_expr_stmt(p);
    /* async.(...) in statement position is an expression statement */
    if (check(p, TokAsync)) {
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t is_expr = check(p, TokDot);
        restore_state(p, snap);
        if (is_expr) return parse_expr_stmt(p);
    }

    if (can_start_var_decl(p)) return parse_var_decl(p, LinkageNone);
    return parse_expr_stmt(p);
}
