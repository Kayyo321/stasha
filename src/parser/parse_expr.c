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

static node_t *parse_compound_init(parser_t *p, usize_t line) {
    node_t *n = make_node(NodeCompoundInit, line);
    node_list_init(&n->as.compound_init.items);

    consume(p, TokLBrace, "'{'");
    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_t *item = Null;

        if (match_tok(p, TokDotDot)) {
            item = make_node(NodeSpreadExpr, p->previous.line);
            item->as.spread_expr.expr = parse_expr(p);
        } else if (match_tok(p, TokDot)) {
            token_t name_tok = consume(p, TokIdent, "field name");
            consume(p, TokEq, "'='");
            item = make_node(NodeInitField, name_tok.line);
            item->as.init_field.name = copy_token_text(name_tok);
            item->as.init_field.value = parse_expr(p);
        } else if (match_tok(p, TokLBracket)) {
            item = make_node(NodeInitIndex, p->previous.line);
            item->as.init_index.index = parse_expr(p);
            consume(p, TokRBracket, "']'");
            consume(p, TokEq, "'='");
            item->as.init_index.value = parse_expr(p);
        } else {
            item = parse_expr(p);
        }

        node_list_push(&n->as.compound_init.items, item);
        if (!match_tok(p, TokComma)) break;
    }

    consume(p, TokRBrace, "'}'");
    return n;
}

static node_t *parse_primary(parser_t *p) {
    if (check(p, TokDot)) {
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokLBrace))
            return parse_compound_init(p, line);

        diag_begin_error("expected '{' after '.'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "compound initializers start with '.{'");
        diag_finish();
        p->had_error = True;
        return make_node(NodeIntLitExpr, line);
    }

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

    /* new.(size) [in zone_name] */
    if (check(p, TokNew)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *size = parse_expr(p);
        consume(p, TokRParen, "')'");
        /* optional: in zone_expr  (bare ident, s.field, or this.field) */
        if (check(p, TokIdent) && p->current.length == 2
                && memcmp(p->current.start, "in", 2) == 0) {
            advance_parser(p); /* consume 'in' */
            /* parse_postfix handles: ident, this.field, s.field, etc. */
            node_t *zone_expr = parse_postfix(p);
            node_t *n = make_node(NodeNewInZone, line);
            n->as.new_in_zone.size      = size;
            n->as.new_in_zone.zone_expr = zone_expr;
            return n;
        }
        node_t *n = make_node(NodeNewExpr, line);
        n->as.new_expr.size = size;
        return n;
    }

    /* 'zone' in expression context is not valid.
       Lexical zones (zone name { }) and manual zones (zone name;) are statements.
       Use rem.(zone_name) to free a zone and mov.(zone_name, ptr, size) to escape. */
    if (check(p, TokZone)) {
        usize_t line = p->current.line;
        diag_begin_error("'zone' is not an expression — use rem.() to free a zone");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "did you mean 'rem.(zone_name);' or 'mov.(zone_name, ptr, size)'?");
        diag_finish();
        p->had_error = True;
        advance_parser(p); /* skip 'zone' to avoid infinite loop */
        return make_node(NodeNilExpr, line);
    }

    /* hash.(expr) — universal hash */
    if (check(p, TokHash)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *operand = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeHashExpr, line);
        n->as.hash_expr.operand = operand;
        return n;
    }

    /* equ.(a, b) — universal equality */
    if (check(p, TokEqu)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *left = parse_expr(p);
        consume(p, TokComma, "','");
        node_t *right = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeEquExpr, line);
        n->as.equ_expr.left  = left;
        n->as.equ_expr.right = right;
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

    /* error.('fmt', args...) — create an error value with a formatted message */
    if (check(p, TokErrorType)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
            diag_begin_error("error.() requires a string literal as the first argument");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a string literal here");
            diag_help("example: error.('something went wrong')");
            diag_finish();
            p->had_error = True;
            return make_node(NodeErrorExpr, line);
        }
        token_t fmt_tok = p->current;
        advance_parser(p);
        usize_t fmt_len = fmt_tok.length - 2;
        char *fmt = ast_strdup(fmt_tok.start + 1, fmt_len);
        node_t *n = make_node(NodeErrorExpr, line);
        n->as.error_expr.fmt     = fmt;
        n->as.error_expr.fmt_len = fmt_len;
        node_list_init(&n->as.error_expr.args);
        while (match_tok(p, TokComma))
            node_list_push(&n->as.error_expr.args, parse_expr(p));
        consume(p, TokRParen, "')'");
        return n;
    }

    /* expect.(expr) — test assertion */
    if (check(p, TokExpect)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *expr = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeExpectExpr, line);
        n->as.expect_expr.expr = expr;
        return n;
    }

    /* expect_eq.(a, b) */
    if (check(p, TokExpectEq)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *left = parse_expr(p);
        consume(p, TokComma, "','");
        node_t *right = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeExpectEqExpr, line);
        n->as.expect_eq.left = left;
        n->as.expect_eq.right = right;
        return n;
    }

    /* expect_neq.(a, b) */
    if (check(p, TokExpectNeq)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *left = parse_expr(p);
        consume(p, TokComma, "','");
        node_t *right = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeExpectNeqExpr, line);
        n->as.expect_neq.left = left;
        n->as.expect_neq.right = right;
        return n;
    }

    /* test_fail.('msg') */
    if (check(p, TokTestFail)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *msg = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeTestFailExpr, line);
        n->as.test_fail.message = msg;
        return n;
    }

    /* comptime_assert.(expr) or comptime_assert.(expr, 'msg') */
    if (check(p, TokComptimeAssert)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *expr = parse_expr(p);
        char *msg = Null;
        if (match_tok(p, TokComma)) {
            if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                advance_parser(p);
                token_t t = p->previous;
                msg = ast_strdup(t.start + 1, t.length - 2);
            }
        }
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeComptimeAssert, line);
        n->as.comptime_assert.expr = expr;
        n->as.comptime_assert.message = msg;
        return n;
    }

    /* mov.(ptr, new_size)               — realloc
       mov.(zone_name, ptr, size)         — escape pointer from zone to heap */
    if (check(p, TokMov)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *first = parse_expr(p);
        consume(p, TokComma, "','");
        node_t *second = parse_expr(p);
        node_t *ptr = first;
        node_t *sz  = second;
        char   *zone_name = Null;
        if (match_tok(p, TokComma)) {
            /* 3-arg form: mov.(zone_name, ptr, size) */
            node_t *third = parse_expr(p);
            if (first->kind == NodeIdentExpr) {
                zone_name = first->as.ident.name;
                ptr = second;
                sz  = third;
            } else {
                diag_begin_error("first argument of 3-arg mov.() must be a zone name");
                diag_span(SRC_LOC(line, 0, 0), True, "here");
                diag_finish();
                p->had_error = True;
            }
        }
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeMovExpr, line);
        n->as.mov_expr.ptr       = ptr;
        n->as.mov_expr.size      = sz;
        n->as.mov_expr.zone_name = zone_name;
        return n;
    }

    /* thread.(fn)(args) — dispatch function to thread pool, returns future */
    if (check(p, TokThread)) {
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
        node_t *n = make_node(NodeThreadCall, line);
        n->as.thread_call.callee = name;
        n->as.thread_call.args   = args;
        return n;
    }

    /* future.op(handle) / future.get.(Type)(handle) */
    if (check(p, TokFuture)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");

        /* read operation name */
        token_t op_tok = consume(p, TokIdent, "future operation (wait, ready, get, drop)");
        char op_name[16];
        usize_t op_len = op_tok.length < 15 ? op_tok.length : 15;
        memcpy(op_name, op_tok.start, op_len);
        op_name[op_len] = '\0';

        future_op_t op;
        type_info_t get_type = NO_TYPE;
        boolean_t   typed_get = False;

        if (strcmp(op_name, "wait") == 0) {
            op = FutureWait;
        } else if (strcmp(op_name, "ready") == 0) {
            op = FutureReady;
        } else if (strcmp(op_name, "drop") == 0) {
            op = FutureDrop;
        } else if (strcmp(op_name, "get") == 0) {
            /* future.get.(Type)(handle)  — typed get
               future.get(handle)         — raw void* get */
            if (check(p, TokDot)) {
                advance_parser(p); /* consume '.' */
                consume(p, TokLParen, "'('");
                get_type  = parse_type(p);
                consume(p, TokRParen, "')'");
                op        = FutureGet;
                typed_get = True;
            } else {
                op = FutureGetRaw;
            }
        } else {
            diag_begin_error("unknown future operation '%s'", op_name);
            diag_span(SRC_LOC(op_tok.line, op_tok.col, op_tok.length), True,
                      "expected: wait, ready, get, drop");
            diag_finish();
            op = FutureWait; /* recover */
        }

        consume(p, TokLParen, "'('");
        node_t *handle = parse_expr(p);
        consume(p, TokRParen, "')'");

        node_t *n = make_node(NodeFutureOp, line);
        n->as.future_op.op       = op;
        n->as.future_op.handle   = handle;
        n->as.future_op.get_type = get_type;
        (void)typed_get;
        return n;
    }

    /* any.(expr) — extract runtime type tag from an any.[...] value */
    if (check(p, TokAny)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'any' */
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *operand = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeAnyTypeExpr, line);
        n->as.any_type_expr.operand = operand;
        return n;
    }

    /* make.([]T, len) / make.([]T, len, cap) — allocate an owned slice.
       Speculative: only treat as builtin if followed by '.(' */
    if (check(p, TokMake)) {
        parser_state_t snap = save_state(p);
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLParen)) {
                advance_parser(p);
                /* parse element type: expects []T */
                consume(p, TokLBracket, "'['");
                consume(p, TokRBracket, "']'");
                type_info_t elem_ti = parse_type(p);
                consume(p, TokComma, "','");
                node_t *len_expr = parse_expr(p);
                node_t *cap_expr = Null;
                if (match_tok(p, TokComma))
                    cap_expr = parse_expr(p);
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeMakeExpr, line);
                n->as.make_expr.elem_type = elem_ti;
                n->as.make_expr.len       = len_expr;
                n->as.make_expr.cap       = cap_expr;
                return n;
            }
        }
        restore_state(p, snap);
    }

    /* append.(slice, val) — append value to slice, return new slice.
       Speculative: only treat as builtin if followed by '.(' */
    if (check(p, TokAppend)) {
        parser_state_t snap = save_state(p);
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLParen)) {
                advance_parser(p);
                node_t *slice = parse_expr(p);
                consume(p, TokComma, "','");
                node_t *val = parse_expr(p);
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeAppendExpr, line);
                n->as.append_expr.slice = slice;
                n->as.append_expr.val   = val;
                return n;
            }
        }
        restore_state(p, snap);
    }

    /* copy.(dst, src) — copy min(len(dst),len(src)) elements, return count.
       Speculative: only treat as builtin if followed by '.(' */
    if (check(p, TokCopy)) {
        parser_state_t snap = save_state(p);
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLParen)) {
                advance_parser(p);
                node_t *dst = parse_expr(p);
                consume(p, TokComma, "','");
                node_t *src = parse_expr(p);
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeCopyExpr, line);
                n->as.copy_expr.dst = dst;
                n->as.copy_expr.src = src;
                return n;
            }
        }
        restore_state(p, snap);
    }

    /* len.(s) — current element count.
       Speculative: only treat as builtin if followed by '.(' */
    if (check(p, TokLen)) {
        parser_state_t snap = save_state(p);
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLParen)) {
                advance_parser(p);
                node_t *operand = parse_expr(p);
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeLenExpr, line);
                n->as.len_expr.operand = operand;
                return n;
            }
        }
        restore_state(p, snap);
    }

    /* cap.(s) — backing capacity.
       Speculative: only treat as builtin if followed by '.(' */
    if (check(p, TokCap)) {
        parser_state_t snap = save_state(p);
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLParen)) {
                advance_parser(p);
                node_t *operand = parse_expr(p);
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeCapExpr, line);
                n->as.len_expr.operand = operand;
                return n;
            }
        }
        restore_state(p, snap);
    }

    /* this — self-reference inside method bodies */
    if (check(p, TokThis)) {
        token_t this_tok = p->current;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *n = make_node(NodeIdentExpr, line);
        ast_set_loc(n, this_tok);
        n->as.ident.name = ast_strdup("this", 4);
        return n;
    }

    /* identifier — may be designated initializer: Type { .x = 1, .y = 2 } */
    /* soft keywords used as variable names: from, new, rem, len, cap, make, append, copy */
    if (check(p, TokFrom) || check(p, TokNew) || check(p, TokRem)
            || check(p, TokLen) || check(p, TokCap)
            || check(p, TokMake) || check(p, TokAppend) || check(p, TokCopy)) {
        token_t ident_tok = p->current;
        usize_t line = p->current.line;
        char *name = copy_token_text(p->current);
        advance_parser(p);
        node_t *n = make_node(NodeIdentExpr, line);
        ast_set_loc(n, ident_tok);
        n->as.ident.name = name;
        return n;
    }

    if (check(p, TokIdent)) {
        token_t ident_tok = p->current;
        advance_parser(p);
        char *name = copy_token_text(p->previous);
        usize_t line = p->previous.line;

        /* designated initializer: Type { .field = val, ... } */
        if (check(p, TokLBrace)) {
            parser_state_t snap = save_state(p);
            advance_parser(p); /* consume '{' */
            if (check(p, TokDot)) {
                /* looks like designated init */
                node_t *n = make_node(NodeDesigInit, line);
                n->as.desig_init.type_name = name;
                node_list_init(&n->as.desig_init.fields);
                node_list_init(&n->as.desig_init.values);
                do {
                    consume(p, TokDot, "'.'");
                    token_t fname = consume_name(p, "field name");
                    node_t *fn_node = make_node(NodeIdentExpr, fname.line);
                    ast_set_loc(fn_node, fname);
                    fn_node->as.ident.name = copy_token_text(fname);
                    node_list_push(&n->as.desig_init.fields, fn_node);
                    consume(p, TokEq, "'='");
                    node_list_push(&n->as.desig_init.values, parse_expr(p));
                } while (match_tok(p, TokComma) && check(p, TokDot));
                consume(p, TokRBrace, "'}'");
                return n;
            }
            /* not a designated init — restore and fall through */
            restore_state(p, snap);
        }

        node_t *n = make_node(NodeIdentExpr, line);
        ast_set_loc(n, ident_tok);
        n->as.ident.name = name;
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

        if (is_builtin_type_token(p->current.kind) || check(p, TokIdent)) {
            boolean_t was_ident = check(p, TokIdent);
            boolean_t err_before_type = p->had_error;
            type_info_t cast_type = parse_type(p);
            /* For user-defined types (ident), only accept as a cast when the
             * parsed type is a pointer type (e.g. (K *r)p, (MyStruct *rw)q).
             * Bare non-pointer ident casts like (Foo)x are too ambiguous with
             * a grouped expression, so fall back to grouped-expression parsing.
             * Builtin types (i32, u8, …) keep the existing any-type behaviour. */
            if (check(p, TokRParen) && p->had_error == err_before_type
                    && (!was_ident || cast_type.is_pointer)) {
                usize_t line = p->previous.line;
                advance_parser(p); /* consume ')' */
                node_t *expr = parse_unary(p); /* unary so (type)&x works */
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

    diag_begin_error("expected an expression, found '%.*s'",
                     (int)p->current.length, p->current.start);
    diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
              True, "not a valid expression start");
    diag_finish();
    p->had_error = True;
    advance_parser(p); /* skip the bad token so we don't loop forever */
    return make_node(NodeIntLitExpr, p->previous.line);
}

/* ── postfix: calls, indexing, member access, ++/-- ── */

static node_t *parse_postfix(parser_t *p) {
    node_t *expr = parse_primary(p);

    for (;;) {
        /* self-method call: Type.(method)(args) */
        if (check(p, TokLParen) && expr->kind == NodeSelfMemberExpr) {
            usize_t line = expr->line;
            char *type_name = expr->as.self_member.type_name;
            char *method_name = expr->as.self_member.field;
            advance_parser(p);
            node_list_t args;
            node_list_init(&args);
            if (!check(p, TokRParen)) {
                do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
            }
            consume(p, TokRParen, "')'");
            node_t *n = make_node(NodeSelfMethodCall, line);
            n->as.self_method_call.type_name = type_name;
            n->as.self_method_call.method = method_name;
            n->as.self_method_call.args = args;
            expr = n;
            continue;
        }

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
            n->col = expr->col;
            n->source_file = expr->source_file;
            n->as.call.callee = name;
            n->as.call.args = args;
            expr = n;
            continue;
        }

        /* array/slice index: expr[idx] or slice expression expr[lo:hi] */
        if (check(p, TokLBracket)) {
            usize_t line = p->current.line;
            advance_parser(p); /* consume '[' */

            /* arr[:] — whole slice with no lo */
            if (check(p, TokColon)) {
                advance_parser(p); /* consume ':' */
                node_t *hi = Null;
                if (!check(p, TokRBracket))
                    hi = parse_expr(p);
                consume(p, TokRBracket, "']'");
                node_t *n = make_node(NodeSliceExpr, line);
                n->as.slice_expr.object = expr;
                n->as.slice_expr.lo     = Null;
                n->as.slice_expr.hi     = hi;
                expr = n;
                continue;
            }

            /* buf[unchecked: i] — bounds-check-free index */
            if (check(p, TokUnchecked)) {
                advance_parser(p); /* consume 'unchecked' */
                consume(p, TokColon, "':' after 'unchecked'");
                node_t *index = parse_expr(p);
                consume(p, TokRBracket, "']'");
                node_t *n = make_node(NodeFlaggedIndex, line);
                n->as.flagged_index.object = expr;
                n->as.flagged_index.index  = index;
                expr = n;
                continue;
            }

            node_t *index = parse_expr(p);

            /* check for ':' after the first expression — this is a slice */
            if (check(p, TokColon)) {
                advance_parser(p); /* consume ':' */
                node_t *hi = Null;
                if (!check(p, TokRBracket))
                    hi = parse_expr(p);
                consume(p, TokRBracket, "']'");
                node_t *n = make_node(NodeSliceExpr, line);
                n->as.slice_expr.object = expr;
                n->as.slice_expr.lo     = index; /* lo */
                n->as.slice_expr.hi     = hi;
                expr = n;
                continue;
            }

            /* plain index */
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

            /* generic instantiation in expression context: ident.[T1, T2] → ident_G_T1_G_T2 */
            if (check(p, TokLBracket) && expr->kind == NodeIdentExpr) {
                advance_parser(p); /* consume '[' */
                char mangled[512];
                usize_t mlen = strlen(expr->as.ident.name);
                if (mlen < sizeof(mangled) - 1)
                    memcpy(mangled, expr->as.ident.name, mlen);
                while (!check(p, TokRBracket) && !check(p, TokEof)) {
                    type_info_t arg = parse_type(p);
                    const char *aname = comptime_type_name(arg);
                    usize_t alen = strlen(aname);
                    if (mlen + 3 + alen < sizeof(mangled) - 1) {
                        memcpy(mangled + mlen, "_G_", 3); mlen += 3;
                        memcpy(mangled + mlen, aname, alen); mlen += alen;
                    }
                    if (!match_tok(p, TokComma)) break;
                }
                consume(p, TokRBracket, "']'");
                mangled[mlen] = '\0';
                expr->as.ident.name = ast_strdup(mangled, mlen);
                /* check for designated initializer: Type.[T] { .field = val, ... } */
                if (check(p, TokLBrace)) {
                    parser_state_t di_snap = save_state(p);
                    advance_parser(p); /* consume '{' */
                    if (check(p, TokDot)) {
                        node_t *di = make_node(NodeDesigInit, expr->line);
                        di->as.desig_init.type_name = expr->as.ident.name;
                        node_list_init(&di->as.desig_init.fields);
                        node_list_init(&di->as.desig_init.values);
                        do {
                            consume(p, TokDot, "'.'");
                            token_t fname = consume_name(p, "field name");
                            node_t *fn_node = make_node(NodeIdentExpr, fname.line);
                            ast_set_loc(fn_node, fname);
                            fn_node->as.ident.name = copy_token_text(fname);
                            node_list_push(&di->as.desig_init.fields, fn_node);
                            consume(p, TokEq, "'='");
                            node_list_push(&di->as.desig_init.values, parse_expr(p));
                        } while (match_tok(p, TokComma) && check(p, TokDot));
                        consume(p, TokRBrace, "'}'");
                        expr = di;
                        continue;
                    }
                    restore_state(p, di_snap);
                }
                continue;
            }

            /* this.field / this.method() — self-access via 'this' keyword */
            if (expr->kind == NodeIdentExpr
                    && strcmp(expr->as.ident.name, "this") == 0
                    && !check(p, TokLParen)) {
                /* consume method/field name */
                char *field_name;
                token_t field_tok;
                if (is_name_token(p)) {
                    field_tok = p->current;
                    field_name = copy_token_text(p->current);
                    advance_parser(p);
                } else {
                    field_tok = p->current;
                    field_name = ast_strdup("?", 1);
                    diag_begin_error("expected field or method name after 'this.'");
                    diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                              True, "expected an identifier here");
                    diag_finish();
                    p->had_error = True;
                }
                /* this.method(args) → NodeSelfMethodCall with type_name = NULL */
                if (check(p, TokLParen)) {
                    advance_parser(p);
                    node_list_t args;
                    node_list_init(&args);
                    if (!check(p, TokRParen)) {
                        do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
                    }
                    consume(p, TokRParen, "')'");
                    node_t *n = make_node(NodeSelfMethodCall, line);
                    n->col = field_tok.col;
                    n->source_file = field_tok.file ? ast_strdup(field_tok.file, strlen(field_tok.file)) : Null;
                    n->as.self_method_call.type_name = Null;
                    n->as.self_method_call.method = field_name;
                    n->as.self_method_call.args = args;
                    expr = n;
                    continue;
                }
                /* this.field → NodeSelfMemberExpr with type_name = NULL */
                node_t *n = make_node(NodeSelfMemberExpr, line);
                n->col = field_tok.col;
                n->source_file = field_tok.file ? ast_strdup(field_tok.file, strlen(field_tok.file)) : Null;
                n->as.self_member.type_name = Null;
                n->as.self_member.field = field_name;
                expr = n;
                continue;
            }

            /* error propagation call: expr.?(args) */
            if (check(p, TokQuestion)) {
                advance_parser(p); /* consume '?' */
                consume(p, TokLParen, "'('");
                node_list_t args;
                node_list_init(&args);
                if (!check(p, TokRParen)) {
                    do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
                }
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeErrPropCall, line);
                if (expr->kind == NodeIdentExpr)
                    n->as.err_prop_call.callee = expr->as.ident.name;
                else
                    n->as.err_prop_call.callee = ast_strdup("?", 1);
                n->as.err_prop_call.args = args;
                expr = n;
                continue;
            }

            /* self-member: Type.(field)  vs  constructor call: Type.(args) */
            if (check(p, TokLParen)) {
                /* Speculate: self-member if exactly one ident inside parens */
                parser_state_t snap = save_state(p);
                boolean_t saved_err = p->had_error;
                advance_parser(p); /* consume '(' */
                boolean_t is_self_member = False;
                token_t field_tok = p->current;
                if (is_name_token(p)) {
                    advance_parser(p);
                    if (check(p, TokRParen)) {
                        /* Exactly (ident) — self member access */
                        advance_parser(p); /* consume ')' */
                        is_self_member = True;
                    }
                }
                if (is_self_member) {
                    node_t *n = make_node(NodeSelfMemberExpr, line);
                    n->col = field_tok.col;
                    n->source_file = field_tok.file ? ast_strdup(field_tok.file, strlen(field_tok.file)) : Null;
                    if (expr->kind == NodeIdentExpr)
                        n->as.self_member.type_name = expr->as.ident.name;
                    else
                        n->as.self_member.type_name = ast_strdup("?", 1);
                    n->as.self_member.field = copy_token_text(field_tok);
                    expr = n;
                    continue;
                }
                /* Not a self-member — constructor call: type_name.(args) */
                restore_state(p, snap);
                p->had_error = saved_err;
                advance_parser(p); /* consume '(' */
                char *type_name = Null;
                if (expr->kind == NodeIdentExpr) type_name = expr->as.ident.name;
                else type_name = ast_strdup("?", 1);
                node_list_t args;
                node_list_init(&args);
                if (!check(p, TokRParen)) {
                    do { node_list_push(&args, parse_expr(p)); } while (match_tok(p, TokComma));
                }
                consume(p, TokRParen, "')'");
                node_t *n = make_node(NodeConstructorCall, line);
                n->as.ctor_call.type_name = type_name;
                n->as.ctor_call.args = args;
                expr = n;
                continue;
            }

            /* consume method/field name — accept contextual keywords as method names */
            char *field_name;
            token_t field_tok;
            if (is_name_token(p)) {
                field_tok = p->current;
                field_name = copy_token_text(p->current);
                advance_parser(p);
            } else if (check(p, TokPrint)) {
                field_tok = p->current;
                field_name = ast_strdup("print", 5);
                advance_parser(p);
            } else {
                field_tok = p->current;
                field_name = ast_strdup("?", 1);
                diag_begin_error("expected field or method name after '.'");
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "expected an identifier here");
                diag_finish();
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
                n->col = field_tok.col;
                n->source_file = field_tok.file ? ast_strdup(field_tok.file, strlen(field_tok.file)) : Null;
                n->as.method_call.object = expr;
                n->as.method_call.method = field_name;
                n->as.method_call.args = args;
                expr = n;
                continue;
            }

            /* member access: expr.field */
            node_t *n = make_node(NodeMemberExpr, line);
            n->col = field_tok.col;
            n->source_file = field_tok.file ? ast_strdup(field_tok.file, strlen(field_tok.file)) : Null;
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

        /* postfix ? — error propagation: expr?
           Disambiguate from ternary (cond ? a : b) by peeking one token ahead.
           If after '?' the next token could start an expression, it is ternary;
           leave '?' for parse_ternary to consume.  Otherwise it is error prop. */
        if (check(p, TokQuestion)) {
            parser_state_t snap = save_state(p);
            advance_parser(p); /* tentatively consume '?' */
            token_kind_t nk = p->current.kind;
            boolean_t next_starts_expr =
                (nk == TokIdent || nk == TokIntLit || nk == TokFloatLit ||
                 nk == TokStackStr || nk == TokHeapStr || nk == TokCharLit ||
                 nk == TokTrue || nk == TokFalse || nk == TokLParen ||
                 nk == TokBang || nk == TokMinus || nk == TokTilde ||
                 nk == TokAmp || nk == TokNew || nk == TokSizeof ||
                 nk == TokNil || nk == TokMov || nk == TokThis ||
                 nk == TokAny || nk == TokLBracket || nk == TokDot);
            if (next_starts_expr) {
                restore_state(p, snap);
                break; /* leave '?' for ternary */
            }
            /* confirmed error propagation postfix */
            node_t *n = make_node(NodeErrProp, expr->line);
            n->as.err_prop.operand = expr;
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
    /* pointer dereference: *expr  or  *.(expr) */
    if (check(p, TokStar)) {
        usize_t line = p->current.line;
        advance_parser(p);
        if (check(p, TokDot)) {
            /* *.(expr) — explicit call-style dereference */
            advance_parser(p); /* consume '.' */
            consume(p, TokLParen, "'('");
            node_t *operand = parse_expr(p);
            consume(p, TokRParen, "')'");
            node_t *n = make_node(NodeUnaryPrefixExpr, line);
            n->as.unary.op = TokStar;
            n->as.unary.operand = operand;
            return n;
        }
        node_t *operand = parse_unary(p);
        node_t *n = make_node(NodeUnaryPrefixExpr, line);
        n->as.unary.op = TokStar;
        n->as.unary.operand = operand;
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
    while (check(p, TokStar) || check(p, TokSlash) || check(p, TokPercent)
           || check(p, TokStarPercent) || check(p, TokStarBang)) {
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
    while (check(p, TokPlus) || check(p, TokMinus)
           || check(p, TokPlusPercent) || check(p, TokMinusPercent)
           || check(p, TokPlusBang) || check(p, TokMinusBang)) {
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

    if (check(p, TokDotDot) || check(p, TokDotDotEq)) {
        boolean_t inclusive = check(p, TokDotDotEq);
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_addition(p);
        node_t *step = Null;
        if (match_tok(p, TokColon))
            step = parse_addition(p);
        node_t *n = make_node(NodeRangeExpr, line);
        n->as.range_expr.start = left;
        n->as.range_expr.end = right;
        n->as.range_expr.step = step;
        n->as.range_expr.inclusive = inclusive;
        left = n;
    }

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

/* ── equality: == != .== ── */

static node_t *parse_equality(parser_t *p) {
    node_t *left = parse_relational(p);
    while (check(p, TokEqEq) || check(p, TokBangEq) || check(p, TokDotEqEq)) {
        token_kind_t op = p->current.kind;
        usize_t line = p->current.line;
        advance_parser(p);
        node_t *right = parse_relational(p);
        if (op == TokDotEqEq) {
            node_t *n = make_node(NodeEquExpr, line);
            n->as.equ_expr.left  = left;
            n->as.equ_expr.right = right;
            left = n;
        } else {
            node_t *n = make_node(NodeBinaryExpr, line);
            n->as.binary.op = op;
            n->as.binary.left = left;
            n->as.binary.right = right;
            left = n;
        }
    }
    return left;
}

/* ── comparison chain helpers ── */

static boolean_t is_cmp_op_tok(token_kind_t k) {
    return k == TokLt || k == TokGt || k == TokLtEq || k == TokGtEq
        || k == TokEqEq || k == TokBangEq;
}

static boolean_t is_chain_literal(token_kind_t k) {
    return k == TokIntLit || k == TokFloatLit || k == TokTrue || k == TokFalse
        || k == TokCharLit || k == TokStackStr || k == TokHeapStr;
}

/* True when the base expression may have side effects and must be evaluated once */
static boolean_t is_complex_base(node_t *n) {
    switch (n->kind) {
        case NodeIdentExpr:
        case NodeIntLitExpr:
        case NodeFloatLitExpr:
        case NodeBoolLitExpr:
        case NodeCharLitExpr:
        case NodeStrLitExpr:
        case NodeMemberExpr:
        case NodeSelfMemberExpr:
            return False;
        default:
            return True;
    }
}

/* ── comparison chain: x > 10 and < 20  /  x == 1 or 2 or 3 ── */

static node_t *parse_cmp_chain(parser_t *p) {
    node_t *first = parse_equality(p);

    /* fast path: no and/or keyword — return as-is */
    if (!check(p, TokAnd) && !check(p, TokOr))
        return first;

    /* the expression before 'and'/'or' must be a comparison binary */
    if (first->kind != NodeBinaryExpr || !is_cmp_op_tok(first->as.binary.op)) {
        diag_begin_error("'%.*s' must follow a comparison expression",
                         (int)p->current.length, p->current.start);
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a comparison like 'x > 10' before '%.*s'",
                  (int)p->current.length, p->current.start);
        diag_note("use '&&' or '||' to combine non-comparison boolean expressions");
        diag_finish();
        p->had_error = True;
        return first;
    }

    node_t *chain = make_node(NodeCmpChain, first->line);
    chain->as.cmp_chain.base_expr  = first->as.binary.left;
    chain->as.cmp_chain.needs_tmp  = is_complex_base(first->as.binary.left);
    chain->as.cmp_chain.cmp_ops[0]    = first->as.binary.op;
    chain->as.cmp_chain.rhs_nodes[0]  = first->as.binary.right;
    chain->as.cmp_chain.logical_ops[0] = 0; /* unused — no connector before first */
    chain->as.cmp_chain.count = 1;

    while (check(p, TokAnd) || check(p, TokOr)) {
        usize_t cnt = chain->as.cmp_chain.count;
        if (cnt >= CMP_CHAIN_MAX) {
            diag_begin_error("comparison chain exceeds maximum of %d conditions", CMP_CHAIN_MAX);
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "chain too long here");
            diag_finish();
            p->had_error = True;
            break;
        }

        int logical = check(p, TokAnd) ? 0 : 1; /* 0=AND, 1=OR */
        advance_parser(p); /* consume 'and'/'or' */

        token_kind_t cmp_op;
        node_t *rhs;

        if (is_cmp_op_tok(p->current.kind)) {
            /* 'and < 20' — explicit operator, reuse base */
            cmp_op = p->current.kind;
            advance_parser(p);
            rhs = parse_shift(p); /* parse RHS below equality level to avoid re-triggering chain */
        } else if (is_chain_literal(p->current.kind)) {
            /* 'or 2' — bare literal, implied '==' */
            cmp_op = TokEqEq;
            rhs = parse_primary(p);
        } else {
            /* 'and y < 20' or other invalid form — new variable not allowed */
            diag_begin_error("invalid comparison chain after '%s'",
                             logical == 0 ? "and" : "or");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a comparison operator (<, >, <=, >=, ==, !=) or a literal here");
            diag_note("to compare a different expression, use '&&' or '||' instead of 'and'/'or'");
            diag_finish();
            p->had_error = True;
            break;
        }

        chain->as.cmp_chain.logical_ops[cnt] = logical;
        chain->as.cmp_chain.cmp_ops[cnt]     = cmp_op;
        chain->as.cmp_chain.rhs_nodes[cnt]   = rhs;
        chain->as.cmp_chain.count++;
    }

    return chain;
}

/* ── bitwise AND: & ── */

static node_t *parse_bitwise_and(parser_t *p) {
    node_t *left = parse_cmp_chain(p);
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
        || expr->kind == NodeMemberExpr || expr->kind == NodeSelfMemberExpr
        || (expr->kind == NodeUnaryPrefixExpr && expr->as.unary.op == TokStar)) {
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
