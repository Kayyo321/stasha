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

    /* error.('message') — create an error value */
    if (check(p, TokErrorType)) {
        usize_t line = p->current.line;
        advance_parser(p);
        consume(p, TokDot, "'.'");
        consume(p, TokLParen, "'('");
        node_t *msg = parse_expr(p);
        consume(p, TokRParen, "')'");
        node_t *n = make_node(NodeErrorExpr, line);
        n->as.error_expr.message = msg;
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

    /* identifier — may be designated initializer: Type { .x = 1, .y = 2 } */
    if (check(p, TokIdent)) {
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
                    token_t fname = consume(p, TokIdent, "field name");
                    node_t *fn_node = make_node(NodeIdentExpr, fname.line);
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

        if (is_builtin_type_token(p->current.kind)) {
            boolean_t err_before_type = p->had_error;
            type_info_t cast_type = parse_type(p);
            if (check(p, TokRParen) && p->had_error == err_before_type) {
                usize_t line = p->previous.line;
                advance_parser(p); /* consume ')' */
                node_t *expr = parse_postfix(p); /* postfix (.field, [idx]) binds tighter than cast */
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

            /* consume method/field name — accept keywords new/rem/print as method names */
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
            } else if (check(p, TokPrint)) {
                field_name = ast_strdup("print", 5);
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
