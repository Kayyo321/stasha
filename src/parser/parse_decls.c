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
            field->as.var_decl.bitfield_width = 0;
            /* bitfield: type name: width */
            if (match_tok(p, TokColon)) {
                if (check(p, TokIntLit)) {
                    field->as.var_decl.bitfield_width = (int)parse_int_value(p->current);
                    advance_parser(p);
                } else {
                    log_err("line %lu: expected bitfield width", p->current.line);
                    p->had_error = True;
                }
            }
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
    n->as.type_decl.attr_flags = 0;
    n->as.type_decl.align_value = 0;
    node_list_init(&n->as.type_decl.fields);
    node_list_init(&n->as.type_decl.methods);
    node_list_init(&n->as.type_decl.variants);

    /* parse attributes before struct/union keyword: @packed @align(N) @c_layout */
    while (check(p, TokAt)) {
        advance_parser(p); /* consume '@' */
        if (check(p, TokIdent)) {
            const char *s = p->current.start;
            usize_t len = p->current.length;
            if (len == 6 && memcmp(s, "packed", 6) == 0) {
                n->as.type_decl.attr_flags |= AttrPacked;
                advance_parser(p);
            } else if (len == 8 && memcmp(s, "c_layout", 8) == 0) {
                n->as.type_decl.attr_flags |= AttrCLayout;
                advance_parser(p);
            } else if (len == 5 && memcmp(s, "align", 5) == 0) {
                advance_parser(p);
                consume(p, TokLParen, "'('");
                if (check(p, TokIntLit)) {
                    n->as.type_decl.align_value = (unsigned)parse_int_value(p->current);
                    advance_parser(p);
                }
                consume(p, TokRParen, "')'");
            } else {
                log_err("line %lu: unknown attribute '@%.*s'",
                        p->current.line, (int)len, s);
                p->had_error = True;
                advance_parser(p);
            }
        }
    }

    if (check(p, TokStruct)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclStruct;
        parse_struct_body(p, n);
        match_tok(p, TokSemicolon); /* optional trailing ; */
    } else if (check(p, TokUnion)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclUnion;
        parse_struct_body(p, n); /* reuse struct body parser for fields */
        match_tok(p, TokSemicolon);
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

/* ── lib ── */
/*
 * Syntax forms:
 *   lib "name";                    — C stdlib header, no alias
 *   lib "name" = alias;            — C stdlib header with alias
 *   lib "name" from "path";        — custom .a library; alias defaults to name
 *   lib "name" from "path" = alias; — custom .a library with explicit alias
 */
static node_t *parse_lib(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'lib' */

    /* library name — accept string literal */
    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        log_err("line %lu: expected library name string after lib", p->current.line);
        p->had_error = True;
        return make_node(NodeLib, line);
    }
    advance_parser(p);
    token_t ntok = p->previous;
    char *name = ast_strdup(ntok.start + 1, ntok.length - 2);

    /* optional 'from "path"' clause */
    char *path = Null;
    if (check(p, TokFrom)) {
        advance_parser(p); /* consume 'from' */
        if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
            log_err("line %lu: expected path string after 'from'", p->current.line);
            p->had_error = True;
            return make_node(NodeLib, line);
        }
        advance_parser(p);
        token_t ptok = p->previous;
        path = ast_strdup(ptok.start + 1, ptok.length - 2);
    }

    /* optional '= alias' clause */
    char *alias = Null;
    if (match_tok(p, TokEq)) {
        token_t atok = consume(p, TokIdent, "alias name");
        alias = copy_token_text(atok);
    }

    /* when 'from' is used without an explicit alias, default alias is the lib name */
    if (path && !alias)
        alias = name;

    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeLib, line);
    n->as.lib_decl.name  = name;
    n->as.lib_decl.alias = alias;
    n->as.lib_decl.path  = path;
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
    boolean_t is_variadic = False;
    if (!check(p, TokRParen) && !check(p, TokVoid)) {
        type_info_t last_type = NO_TYPE;
        storage_t last_storage = StorageDefault;
        do {
            /* variadic: ... must be last */
            if (check(p, TokDotDotDot)) {
                advance_parser(p);
                is_variadic = True;
                break;
            }
            /* optional storage qualifier on parameter */
            storage_t param_storage = StorageDefault;
            if (check(p, TokStack))      { param_storage = StorageStack;  advance_parser(p); }
            else if (check(p, TokHeap)) { param_storage = StorageHeap;   advance_parser(p); }
            if (param_storage != StorageDefault) last_storage = param_storage;
            else param_storage = last_storage;
            /* optional restrict qualifier */
            boolean_t is_restrict = False;
            if (match_tok(p, TokRestrict)) is_restrict = True;
            if (can_start_param_type(p))
                last_type = parse_type(p);
            token_t pname = consume(p, TokIdent, "parameter name");
            node_t *param = make_node(NodeVarDecl, pname.line);
            param->as.var_decl.name = copy_token_text(pname);
            param->as.var_decl.type = last_type;
            param->as.var_decl.storage = param_storage;
            if (is_restrict) param->as.var_decl.flags |= VdeclRestrict;
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
    n->as.fn_decl.is_variadic = is_variadic;
    return n;
}

/* ── switch statement ── */
/*
 * switch expr {
 *     case val1: { ... }
 *     case val2, val3: { ... }
 *     default: { ... }
 * }
 */
static node_t *parse_switch_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'switch' */
    node_t *expr = parse_expr(p);
    consume(p, TokLBrace, "'{'");

    node_t *n = make_node(NodeSwitchStmt, line);
    n->as.switch_stmt.expr = expr;
    node_list_init(&n->as.switch_stmt.cases);

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_t *c = make_node(NodeSwitchCase, p->current.line);
        node_list_init(&c->as.switch_case.values);
        c->as.switch_case.is_default = False;

        if (check(p, TokCase)) {
            advance_parser(p);
            do {
                node_list_push(&c->as.switch_case.values, parse_expr(p));
            } while (match_tok(p, TokComma));
            consume(p, TokColon, "':'");
        } else if (check(p, TokDefault)) {
            advance_parser(p);
            c->as.switch_case.is_default = True;
            consume(p, TokColon, "':'");
        } else {
            log_err("line %lu: expected 'case' or 'default'", p->current.line);
            p->had_error = True;
            advance_parser(p);
            continue;
        }
        c->as.switch_case.body = parse_block(p);
        node_list_push(&n->as.switch_stmt.cases, c);
    }

    consume(p, TokRBrace, "'}'");
    return n;
}

/* ── inline assembly ── */
/* asm { "assembly code" } or asm("code", "constraints", operands...) */
static node_t *parse_asm_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'asm' */

    node_t *n = make_node(NodeAsmStmt, line);
    node_list_init(&n->as.asm_stmt.operands);
    n->as.asm_stmt.constraints = Null;

    if (check(p, TokLBrace)) {
        /* asm { "code" } */
        advance_parser(p);
        if (check(p, TokStackStr) || check(p, TokHeapStr)) {
            advance_parser(p);
            token_t t = p->previous;
            n->as.asm_stmt.code = ast_strdup(t.start + 1, t.length - 2);
        } else {
            log_err("line %lu: expected assembly string in asm block", p->current.line);
            p->had_error = True;
            n->as.asm_stmt.code = ast_strdup("", 0);
        }
        consume(p, TokRBrace, "'}'");
    } else {
        /* asm("code", "constraints", operands...) */
        consume(p, TokLParen, "'('");
        if (check(p, TokStackStr) || check(p, TokHeapStr)) {
            advance_parser(p);
            token_t t = p->previous;
            n->as.asm_stmt.code = ast_strdup(t.start + 1, t.length - 2);
        } else {
            n->as.asm_stmt.code = ast_strdup("", 0);
        }
        if (match_tok(p, TokComma)) {
            if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                advance_parser(p);
                token_t t = p->previous;
                n->as.asm_stmt.constraints = ast_strdup(t.start + 1, t.length - 2);
            }
            while (match_tok(p, TokComma))
                node_list_push(&n->as.asm_stmt.operands, parse_expr(p));
        }
        consume(p, TokRParen, "')'");
        consume(p, TokSemicolon, "';'");
    }

    return n;
}

/* ── compile-time conditional compilation ── */
/*
 * comptime_if platform == "macos" { ... }
 * comptime_if arch == "aarch64" { ... } else { ... }
 */
static node_t *parse_comptime_if(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'comptime_if' */

    /* key == "value" */
    token_t key_tok = consume(p, TokIdent, "comptime key (platform, arch, os)");
    char *key = copy_token_text(key_tok);
    consume(p, TokEqEq, "'=='");
    char *value = Null;
    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        advance_parser(p);
        token_t t = p->previous;
        value = ast_strdup(t.start + 1, t.length - 2);
    } else {
        log_err("line %lu: expected string after '==' in comptime_if", p->current.line);
        p->had_error = True;
        value = ast_strdup("", 0);
    }

    node_t *body = parse_block(p);
    node_t *else_body = Null;
    if (match_tok(p, TokElse))
        else_body = parse_block(p);

    node_t *n = make_node(NodeComptimeIf, line);
    n->as.comptime_if.key = key;
    n->as.comptime_if.value = value;
    n->as.comptime_if.body = body;
    n->as.comptime_if.else_body = else_body;
    return n;
}

/* ── top-level declarations ── */

static node_t *parse_test_block(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'test' */
    /* test name is a string literal */
    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        log_err("line %lu: expected test name string after 'test'", p->current.line);
        p->had_error = True;
        return make_node(NodeTestBlock, line);
    }
    advance_parser(p);
    token_t name_tok = p->previous;
    char *name = ast_strdup(name_tok.start + 1, name_tok.length - 2);
    node_t *body = parse_block(p);
    node_t *n = make_node(NodeTestBlock, line);
    n->as.test_block.name = name;
    n->as.test_block.body = body;
    return n;
}

static node_t *parse_top_decl(parser_t *p) {
    /* lib */
    if (check(p, TokLib)) return parse_lib(p);

    /* imp */
    if (check(p, TokImp)) return parse_imp(p);

    /* test block */
    if (check(p, TokTest)) return parse_test_block(p);

    /* comptime_if at top level */
    if (check(p, TokComptimeIf)) return parse_comptime_if(p);

    /* comptime_assert at top level (as statement) */
    if (check(p, TokComptimeAssert)) return parse_expr_stmt(p);

    /* attributes: @weak, @hidden (collected and passed to fn/var decl) */
    int attr_flags = 0;
    while (check(p, TokAt)) {
        advance_parser(p);
        if (check(p, TokIdent)) {
            const char *s = p->current.start;
            usize_t len = p->current.length;
            if (len == 4 && memcmp(s, "weak", 4) == 0) {
                attr_flags |= AttrWeak;
                advance_parser(p);
            } else if (len == 6 && memcmp(s, "hidden", 6) == 0) {
                attr_flags |= AttrHidden;
                advance_parser(p);
            } else {
                log_err("line %lu: unknown attribute '@%.*s'",
                        p->current.line, (int)len, s);
                p->had_error = True;
                advance_parser(p);
            }
        }
    }

    linkage_t linkage = LinkageNone;
    if (check(p, TokInt) || check(p, TokExt)) {
        linkage = check(p, TokInt) ? LinkageInternal : LinkageExternal;
        advance_parser(p);
    }

    /* type declaration */
    if (check(p, TokType)) return parse_type_decl(p, linkage);

    /* function declaration */
    if (check(p, TokFn)) {
        node_t *fn = parse_fn_decl(p, linkage);
        fn->as.fn_decl.attr_flags = attr_flags;
        return fn;
    }

    /* global variable */
    {
        node_t *var = parse_var_decl(p, linkage);
        if (var->kind == NodeVarDecl)
            var->as.var_decl.attr_flags = attr_flags;
        return var;
    }
}
