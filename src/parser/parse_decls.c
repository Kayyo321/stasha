/* ── fileheader (@[[...]]) parsing ── */

/* Accept identifier-like tokens for fileheader keys.  Includes real keywords
   that can reasonably name an attribute (e.g. `if`). */
static boolean_t is_fh_key_token(parser_t *p) {
    token_kind_t k = p->current.kind;
    return k == TokIdent || k == TokIf || k == TokFrom
        || k == TokNew || k == TokRem
        || k == TokLen || k == TokCap || k == TokMake
        || k == TokAppend || k == TokCopy
        || k == TokHash || k == TokEqu
        || k == TokDefault;
}

static boolean_t fh_key_is_doc(const char *k) {
    return strcmp(k, "doc") == 0 || strcmp(k, "returns") == 0 || strcmp(k, "params") == 0;
}

static fh_entry_t *fh_find(fileheader_t *fh, const char *k) {
    if (!fh) return Null;
    for (usize_t i = 0; i < fh->count; i++)
        if (strcmp(fh->items[i].key, k) == 0) return &fh->items[i];
    return Null;
}

/* Capture free-form text for doc/returns/params until we see a closing ']' at
   paren-depth 0, or an ident followed by ':' (next entry key).  Concatenates
   token texts with single-space separators (formatting lost — content kept). */
static char *fh_capture_freeform(parser_t *p) {
    char buf[2048];
    usize_t pos = 0;
    int paren_depth = 0;

    while (!check(p, TokEof)) {
        if (paren_depth == 0) {
            if (check(p, TokRBracket)) break;
            /* A top-level ident-or-keyword followed by ':' starts the next
               fileheader entry.  We deliberately don't terminate on a bare
               comma here — doc/returns/params may contain commas as natural
               prose. */
            if (is_fh_key_token(p)) {
                parser_state_t snap = save_state(p);
                advance_parser(p);
                boolean_t is_next_key = check(p, TokColon);
                restore_state(p, snap);
                if (is_next_key) break;
            }
        }
        if (check(p, TokLParen)) paren_depth++;
        else if (check(p, TokRParen) && paren_depth > 0) paren_depth--;

        token_t t = p->current;
        if (pos > 0 && pos + 1 < sizeof(buf) - 1) buf[pos++] = ' ';
        usize_t avail = sizeof(buf) - 1 - pos;
        usize_t clen = t.length < avail ? t.length : avail;
        /* for quoted strings, strip the quotes */
        if (t.kind == TokStackStr || t.kind == TokHeapStr) {
            if (t.length >= 2) {
                clen = t.length - 2;
                if (clen > avail) clen = avail;
                memcpy(buf + pos, t.start + 1, clen);
            }
        } else {
            memcpy(buf + pos, t.start, clen);
        }
        pos += clen;
        advance_parser(p);
    }
    buf[pos] = '\0';
    return ast_strdup(buf, pos);
}

/* Parse a single fileheader entry starting at current token (expects ident key). */
static void parse_fileheader_entry(parser_t *p, fileheader_t *fh) {
    if (!is_fh_key_token(p)) {
        diag_begin_error("expected fileheader attribute name, found '%.*s'",
                         (int)p->current.length, p->current.start);
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected attribute name here");
        diag_note("example: @[[export_name: \"foo\", weak]]");
        diag_finish();
        p->had_error = True;
        advance_parser(p);
        return;
    }

    fh_entry_t e;
    memset(&e, 0, sizeof(e));
    e.line = p->current.line;
    e.col  = p->current.col;
    e.key  = copy_token_text(p->current);
    advance_parser(p);

    if (!match_tok(p, TokColon)) {
        e.vkind = FhFlag;
        fileheader_push(fh, e);
        return;
    }

    if (fh_key_is_doc(e.key)) {
        e.vkind = FhText;
        e.str_val = fh_capture_freeform(p);
        fileheader_push(fh, e);
        return;
    }

    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        token_t t = p->current;
        advance_parser(p);
        e.vkind = FhStr;
        e.str_val = ast_strdup(t.start + 1, t.length - 2);
        fileheader_push(fh, e);
        return;
    }

    if (check(p, TokIntLit)) {
        token_t t = p->current;
        advance_parser(p);
        char numbuf[64];
        usize_t nlen = t.length < sizeof(numbuf) - 1 ? t.length : sizeof(numbuf) - 1;
        memcpy(numbuf, t.start, nlen);
        numbuf[nlen] = '\0';
        long val = strtol(numbuf, Null, 0);
        e.vkind = FhInt;
        e.int_val = val;
        fileheader_push(fh, e);
        return;
    }

    if (is_name_token(p)) {
        token_t first = p->current;
        advance_parser(p);

        /* call form: name(arg) — diagnostic: ignore("x"), init: before("x") */
        if (check(p, TokLParen)) {
            advance_parser(p);
            e.vkind = FhCall;
            e.str_val = copy_token_text(first);
            if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                token_t t = p->current;
                advance_parser(p);
                e.call_arg = ast_strdup(t.start + 1, t.length - 2);
            } else if (is_name_token(p)) {
                e.call_arg = copy_token_text(p->current);
                advance_parser(p);
            }
            consume(p, TokRParen, "')'");
            fileheader_push(fh, e);
            return;
        }

        /* dotted lhs: target.arch == "x86_64" */
        if (check(p, TokDot)) {
            advance_parser(p);
            token_t second = p->current;
            if (is_name_token(p)) advance_parser(p);
            consume(p, TokEqEq, "'==' in condition");
            token_t rhs_tok = p->current;
            if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                advance_parser(p);
                e.vkind = FhCond;
                char *lhs1 = copy_token_text(first);
                char *lhs2 = copy_token_text(second);
                if (strcmp(lhs1, "target") == 0 && strcmp(lhs2, "arch") == 0) {
                    e.cond.op = FhCondArchEq;
                } else if (strcmp(lhs1, "target") == 0 && strcmp(lhs2, "os") == 0) {
                    e.cond.op = FhCondOsEq;
                } else {
                    e.cond.op = FhCondAlwaysFalse;
                }
                e.cond.str_rhs = ast_strdup(rhs_tok.start + 1, rhs_tok.length - 2);
            } else {
                diag_begin_error("expected string literal on right-hand side of '=='");
                diag_span(SRC_LOC(rhs_tok.line, rhs_tok.col, rhs_tok.length),
                          True, "expected a string");
                diag_finish();
                p->had_error = True;
                e.vkind = FhCond;
                e.cond.op = FhCondAlwaysFalse;
            }
            fileheader_push(fh, e);
            return;
        }

        /* simple lhs: os/arch/pointer_width == value */
        if (check(p, TokEqEq)) {
            advance_parser(p);
            char *lhs = copy_token_text(first);
            e.vkind = FhCond;
            if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                token_t t = p->current;
                advance_parser(p);
                if (strcmp(lhs, "os") == 0 || strcmp(lhs, "platform") == 0)
                    e.cond.op = FhCondOsEq;
                else if (strcmp(lhs, "arch") == 0)
                    e.cond.op = FhCondArchEq;
                else
                    e.cond.op = FhCondAlwaysFalse;
                e.cond.str_rhs = ast_strdup(t.start + 1, t.length - 2);
            } else if (check(p, TokIntLit)) {
                token_t t = p->current;
                advance_parser(p);
                char numbuf[32];
                usize_t nlen = t.length < sizeof(numbuf) - 1 ? t.length : sizeof(numbuf) - 1;
                memcpy(numbuf, t.start, nlen);
                numbuf[nlen] = '\0';
                if (strcmp(lhs, "pointer_width") == 0)
                    e.cond.op = FhCondPtrWidthEq;
                else
                    e.cond.op = FhCondAlwaysFalse;
                e.cond.int_rhs = (int)strtol(numbuf, Null, 0);
            } else {
                e.cond.op = FhCondAlwaysFalse;
                diag_begin_error("expected string or integer after '=='");
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "expected condition value");
                diag_finish();
                p->had_error = True;
            }
            fileheader_push(fh, e);
            return;
        }

        /* bare ident value: abi: c, diagnostic: push */
        e.vkind = FhIdent;
        e.str_val = copy_token_text(first);
        fileheader_push(fh, e);
        return;
    }

    diag_begin_error("unexpected token '%.*s' in fileheader value",
                     (int)p->current.length, p->current.start);
    diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
              True, "expected a string, integer, identifier, or condition");
    diag_finish();
    p->had_error = True;
    advance_parser(p);
}

/* Parse the body of @[[ ... ]] — current token should be the first entry key. */
static void parse_fileheader_entries(parser_t *p, fileheader_t *fh) {
    while (!check(p, TokRBracket) && !check(p, TokEof)) {
        parse_fileheader_entry(p, fh);
        if (check(p, TokComma)) { advance_parser(p); continue; }
        if (check(p, TokRBracket)) break;
        if (is_fh_key_token(p)) continue; /* implicit separator */
        break;
    }
}

/* Detect and consume a `@[[...]]` group.  On success, appends entries to `fh`
   and returns True.  On no match, restores state and returns False. */
static boolean_t parse_fileheader_group(parser_t *p, fileheader_t *fh) {
    if (!check(p, TokAt)) return False;
    parser_state_t snap = save_state(p);
    advance_parser(p); /* '@' */
    if (!check(p, TokLBracket)) { restore_state(p, snap); return False; }
    advance_parser(p); /* first '[' */
    if (!check(p, TokLBracket)) { restore_state(p, snap); return False; }
    advance_parser(p); /* second '[' */

    parse_fileheader_entries(p, fh);
    consume(p, TokRBracket, "']'");
    consume(p, TokRBracket, "']'");
    return True;
}

/* Apply file-wide fileheader entries to a module node. */
static void apply_module_headers(node_t *module, fileheader_t *src) {
    if (!module || !src) return;
    for (usize_t i = 0; i < src->count; i++) {
        fh_entry_t *e = &src->items[i];
        if (strcmp(e->key, "freestanding") == 0) {
            module->as.module.freestanding = True;
        } else if (strcmp(e->key, "org") == 0 && e->vkind == FhInt) {
            module->as.module.has_org = True;
            module->as.module.org_addr = e->int_val;
        }
    }
}

/* Build a NodeInitBlock / NodeExitBlock, given the trailing @[[init...]] /
   @[[exit...]] entries and current stream position (`{` or string then `{`). */
static node_t *parse_lifecycle_block(parser_t *p, fileheader_t *pending) {
    boolean_t is_init = False;
    boolean_t is_exit = False;
    char *before_name = Null;
    char *after_name  = Null;

    for (usize_t i = 0; i < pending->count; i++) {
        fh_entry_t *e = &pending->items[i];
        if (strcmp(e->key, "init") == 0) is_init = True;
        else if (strcmp(e->key, "exit") == 0) is_exit = True;
        else if (strcmp(e->key, "before") == 0 && e->vkind == FhStr)
            before_name = e->str_val;
        else if (strcmp(e->key, "after") == 0 && e->vkind == FhStr)
            after_name = e->str_val;
        /* init: before("foo") comes as FhCall with key="init", str_val="before", call_arg="foo" */
        if ((strcmp(e->key, "init") == 0 || strcmp(e->key, "exit") == 0)
                && e->vkind == FhCall && e->str_val) {
            if (strcmp(e->str_val, "before") == 0) before_name = e->call_arg;
            else if (strcmp(e->str_val, "after") == 0) after_name = e->call_arg;
        }
    }

    char *title = Null;
    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        token_t t = p->current;
        advance_parser(p);
        title = ast_strdup(t.start + 1, t.length - 2);
    }

    node_t *body = parse_block(p);
    node_kind_t kind = is_exit ? NodeExitBlock : NodeInitBlock;
    node_t *n = make_node(kind, body ? body->line : 0);
    n->as.lifecycle_block.title = title;
    n->as.lifecycle_block.before_name = before_name;
    n->as.lifecycle_block.after_name = after_name;
    n->as.lifecycle_block.body = body;
    n->headers = fileheader_alloc();
    *(n->headers) = *pending;
    (void)is_init;
    return n;
}

/* ── compile-time condition evaluator (used at parse time for @comptime if in structs) ── */

static boolean_t eval_comptime_condition(const char *key, const char *value) {
    if (strcmp(key, "os") == 0 || strcmp(key, "platform") == 0) {
#if defined(__APPLE__)
        return strcmp(value, "macos") == 0 || strcmp(value, "darwin") == 0;
#elif defined(__linux__)
        return strcmp(value, "linux") == 0;
#elif defined(_WIN32)
        return strcmp(value, "windows") == 0;
#else
        return False;
#endif
    }
    if (strcmp(key, "arch") == 0) {
#if defined(__x86_64__) || defined(_M_X64)
        return strcmp(value, "x86_64") == 0 || strcmp(value, "amd64") == 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
        return strcmp(value, "aarch64") == 0 || strcmp(value, "arm64") == 0;
#elif defined(__i386__) || defined(_M_IX86)
        return strcmp(value, "x86") == 0 || strcmp(value, "i386") == 0;
#else
        return False;
#endif
    }
    return False;
}

/* ── parse a list of generic type parameter names: [T, U, ...] ── */
/* Called after '[' has been consumed. Fills in type_params/type_param_count on node. */
static void parse_type_params_into(parser_t *p,
                                   char **params_out, usize_t *count_out,
                                   usize_t max_params) {
    *count_out = 0;
    while (!check(p, TokRBracket) && !check(p, TokEof)) {
        if (*count_out >= max_params) {
            diag_begin_error("too many type parameters (max %zu)", max_params);
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "too many parameters");
            diag_finish();
            break;
        }
        token_t ptok = consume(p, TokIdent, "type parameter name");
        params_out[(*count_out)++] = copy_token_text(ptok);
        /* consume optional .[constraint1, constraint2] syntax */
        if (check(p, TokDot)) {
            parser_state_t saved = save_state(p);
            advance_parser(p); /* consume '.' */
            if (match_tok(p, TokLBracket)) {
                /* consume constraint list */
                while (!check(p, TokRBracket) && !check(p, TokEof)) {
                    advance_parser(p); /* skip constraint name */
                    if (!match_tok(p, TokComma)) break;
                }
                consume(p, TokRBracket, "']'");
            } else {
                restore_state(p, saved);
            }
        }
        if (!match_tok(p, TokComma)) break;
    }
    consume(p, TokRBracket, "']'");
}

/* ── struct body parsing ── */

static void parse_struct_body(parser_t *p, node_t *decl) {
    consume(p, TokLBrace, "'{'");
    boolean_t in_comptime_section = False;

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        /* @comptime: section marker OR @comptime if inside a @comptime: section */
        if (check(p, TokAt)) {
            parser_state_t saved = save_state(p);
            advance_parser(p); /* consume '@' */
            if (check(p, TokIdent) && p->current.length == 8
                    && memcmp(p->current.start, "comptime", 8) == 0) {
                advance_parser(p); /* consume 'comptime' */
                if (match_tok(p, TokColon)) {
                    /* @comptime: — enter comptime section */
                    in_comptime_section = True;
                    continue;
                }
                if (check(p, TokIf) && in_comptime_section) {
                    /* @comptime if inside @comptime: section — evaluate at parse time */
                    advance_parser(p); /* consume 'if' */
                    token_t key_tok = consume(p, TokIdent, "comptime key");
                    char *key = copy_token_text(key_tok);
                    consume(p, TokEqEq, "'=='");
                    char *value = Null;
                    if (check(p, TokStackStr) || check(p, TokHeapStr)) {
                        advance_parser(p);
                        token_t t = p->previous;
                        value = ast_strdup(t.start + 1, t.length - 2);
                    } else {
                        value = ast_strdup("", 0);
                        p->had_error = True;
                    }
                    boolean_t cond_match = eval_comptime_condition(key, value);

                    /* parse then-block fields */
                    consume(p, TokLBrace, "'{'");
                    while (!check(p, TokRBrace) && !check(p, TokEof)) {
                        if (cond_match) {
                            /* parse and keep the field */
                            linkage_t fl = LinkageNone;
                            storage_t fstore = StorageDefault;
                            if (check(p, TokInt) || check(p, TokExt)) {
                                fl = check(p, TokInt) ? LinkageInternal : LinkageExternal;
                                advance_parser(p);
                                if (check(p, TokStack))      { fstore = StorageStack; advance_parser(p); }
                                else if (check(p, TokHeap)) { fstore = StorageHeap;  advance_parser(p); }
                            } else if (check(p, TokStack) || check(p, TokHeap)) {
                                if (check(p, TokStack))      { fstore = StorageStack; advance_parser(p); }
                                else                        { fstore = StorageHeap;  advance_parser(p); }
                                if (check(p, TokInt) || check(p, TokExt)) {
                                    fl = check(p, TokInt) ? LinkageInternal : LinkageExternal;
                                    advance_parser(p);
                                }
                            }
                            type_info_t ftype = parse_type(p);
                            token_t fname = consume_name(p, "field name");
                            node_t *field = make_node(NodeVarDecl, fname.line);
                            ast_set_loc(field, fname);
                            field->as.var_decl.name = copy_token_text(fname);
                            field->as.var_decl.type = ftype;
                            field->as.var_decl.storage = fstore;
                            field->as.var_decl.linkage = fl;
                            field->as.var_decl.flags |= VdeclComptimeField;
                            if (match_tok(p, TokEq))
                                field->as.var_decl.init = parse_expr(p);
                            consume(p, TokSemicolon, "';'");
                            node_list_push(&decl->as.type_decl.fields, field);
                        } else {
                            /* skip to ';' */
                            while (!check(p, TokSemicolon) && !check(p, TokRBrace) && !check(p, TokEof))
                                advance_parser(p);
                            match_tok(p, TokSemicolon);
                        }
                    }
                    consume(p, TokRBrace, "'}'");

                    /* optional else branch */
                    if (match_tok(p, TokElse)) {
                        consume(p, TokLBrace, "'{'");
                        while (!check(p, TokRBrace) && !check(p, TokEof)) {
                            if (!cond_match) {
                                linkage_t fl = LinkageNone;
                                storage_t fstore = StorageDefault;
                                if (check(p, TokInt) || check(p, TokExt)) {
                                    fl = check(p, TokInt) ? LinkageInternal : LinkageExternal;
                                    advance_parser(p);
                                    if (check(p, TokStack))      { fstore = StorageStack; advance_parser(p); }
                                    else if (check(p, TokHeap)) { fstore = StorageHeap;  advance_parser(p); }
                                } else if (check(p, TokStack) || check(p, TokHeap)) {
                                    if (check(p, TokStack))      { fstore = StorageStack; advance_parser(p); }
                                    else                        { fstore = StorageHeap;  advance_parser(p); }
                                    if (check(p, TokInt) || check(p, TokExt)) {
                                        fl = check(p, TokInt) ? LinkageInternal : LinkageExternal;
                                        advance_parser(p);
                                    }
                                }
                                type_info_t ftype = parse_type(p);
                                token_t fname = consume_name(p, "field name");
                                node_t *field = make_node(NodeVarDecl, fname.line);
                                ast_set_loc(field, fname);
                                field->as.var_decl.name = copy_token_text(fname);
                                field->as.var_decl.type = ftype;
                                field->as.var_decl.storage = fstore;
                                field->as.var_decl.linkage = fl;
                                field->as.var_decl.flags |= VdeclComptimeField;
                                if (match_tok(p, TokEq))
                                    field->as.var_decl.init = parse_expr(p);
                                consume(p, TokSemicolon, "';'");
                                node_list_push(&decl->as.type_decl.fields, field);
                            } else {
                                while (!check(p, TokSemicolon) && !check(p, TokRBrace) && !check(p, TokEof))
                                    advance_parser(p);
                                match_tok(p, TokSemicolon);
                            }
                        }
                        consume(p, TokRBrace, "'}'");
                    }
                    continue;
                }
            }
            restore_state(p, saved);
        }

        /* accept either order: [int|ext] [stack|heap] T  or  [stack|heap] [int|ext] T */
        linkage_t field_link = LinkageNone;
        storage_t field_storage = StorageDefault;
        if (check(p, TokInt) || check(p, TokExt)) {
            field_link = check(p, TokInt) ? LinkageInternal : LinkageExternal;
            advance_parser(p);
            if (check(p, TokStack))      { field_storage = StorageStack; advance_parser(p); }
            else if (check(p, TokHeap)) { field_storage = StorageHeap;  advance_parser(p); }
        } else if (check(p, TokStack) || check(p, TokHeap)) {
            if (check(p, TokStack))      { field_storage = StorageStack; advance_parser(p); }
            else                        { field_storage = StorageHeap;  advance_parser(p); }
            if (check(p, TokInt) || check(p, TokExt)) {
                field_link = check(p, TokInt) ? LinkageInternal : LinkageExternal;
                advance_parser(p);
            }
        }

        /* inline method — not allowed in @comptime: section */
        if (check(p, TokFn)) {
            advance_parser(p);
            /* accept ident or built-in keywords used as method names (hash, equ, new, rem, ...) */
            token_t fn_name;
            if (check(p, TokIdent) || check(p, TokHash) || check(p, TokEqu)
                    || check(p, TokNew) || check(p, TokRem) || check(p, TokFrom)
                    || check(p, TokPrint)) {
                fn_name = p->current; advance_parser(p);
            } else {
                fn_name = consume(p, TokIdent, "method name");
            }
            /* allow "fn qualifier.method_name" — qualifier could be struct name or interface name */
            char *iface_qual = Null;
            if (check(p, TokDot)) {
                iface_qual = copy_token_text(fn_name); /* save qualifier */
                advance_parser(p); /* consume '.' */
                if (check(p, TokIdent) || check(p, TokHash) || check(p, TokEqu)
                        || check(p, TokNew) || check(p, TokRem) || check(p, TokFrom)
                        || check(p, TokPrint)) {
                    fn_name = p->current; advance_parser(p);
                } else {
                    fn_name = consume(p, TokIdent, "method name");
                }
                /* handle double dot: fn StructName.iface_name.method_name — skip extra prefix */
                if (check(p, TokDot)) {
                    advance_parser(p); /* consume '.' */
                    iface_qual = copy_token_text(fn_name); /* now iface_qual is the middle name */
                    if (check(p, TokIdent) || check(p, TokHash) || check(p, TokEqu)
                            || check(p, TokNew) || check(p, TokRem) || check(p, TokFrom)
                            || check(p, TokPrint)) {
                        fn_name = p->current; advance_parser(p);
                    } else {
                        fn_name = consume(p, TokIdent, "method name");
                    }
                }
            }
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
                    token_t pname = consume_name(p, "parameter name");
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

            usize_t ret_count = 0;
            type_info_t *ret_types = Null;
            /* optional return type: ': type' or ': [types]' */
            if (match_tok(p, TokColon)) {
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
            } else {
                /* no return type specified — default to void */
                ret_count = 1;
                ret_types = alloc_type_array(1);
                ret_types[0] = NO_TYPE; /* void */
            }

            /* expression body: => expr; */
            node_t *body = Null;
            if (match_tok(p, TokFatArrow)) {
                usize_t body_line = p->current.line;
                node_t *expr = parse_expr(p);
                consume(p, TokSemicolon, "';'");
                node_t *ret_node = make_node(NodeRetStmt, body_line);
                node_list_init(&ret_node->as.ret_stmt.values);
                node_list_push(&ret_node->as.ret_stmt.values, expr);
                body = make_node(NodeBlock, body_line);
                node_list_init(&body->as.block.stmts);
                node_list_push(&body->as.block.stmts, ret_node);
            } else {
                body = parse_block(p);
            }
            node_t *method = make_node(NodeFnDecl, fn_name.line);
            ast_set_loc(method, fn_name);
            method->as.fn_decl.name = copy_token_text(fn_name);
            method->as.fn_decl.linkage = field_link;
            method->as.fn_decl.return_types = ret_types;
            method->as.fn_decl.return_count = ret_count;
            method->as.fn_decl.params = params;
            method->as.fn_decl.body = body;
            method->as.fn_decl.is_method = True;
            method->as.fn_decl.struct_name = decl->as.type_decl.name;
            method->as.fn_decl.iface_qualifier = iface_qual;
            node_list_push(&decl->as.type_decl.methods, method);
            continue;
        }

        /* field declaration: [int|ext] type name [, name, ...] ; */
        if (!can_start_type(p)) {
            if (!p->panic_mode) {
                diag_begin_error("expected a field type or method declaration in struct");
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "expected a type or 'fn' here");
                diag_finish();
            }
            p->had_error  = True;
            p->panic_mode = True;
            skip_to_recovery_point(p);
            continue;
        }
        type_info_t ftype = parse_type(p);

        do {
            token_t fname = consume_name(p, "field name");
            node_t *field = make_node(NodeVarDecl, fname.line);
            ast_set_loc(field, fname);
            field->as.var_decl.name = copy_token_text(fname);
            field->as.var_decl.type = ftype;
            field->as.var_decl.storage = field_storage;
            field->as.var_decl.linkage = field_link;
            field->as.var_decl.bitfield_width = 0;
            if (in_comptime_section)
                field->as.var_decl.flags |= VdeclComptimeField;
            /* array field: type name[d0][d1]... — arbitrary nesting depth up to 8 */
            if (!in_comptime_section) {
                int _ndim = 0;
                while (check(p, TokLBracket) && _ndim < 8) {
                    advance_parser(p); /* consume '[' */
                    if (check(p, TokIntLit)) {
                        field->as.var_decl.array_sizes[_ndim] = parse_int_value(p->current);
                        advance_parser(p);
                    } else if (check(p, TokIdent)) {
                        field->as.var_decl.array_size_names[_ndim] = copy_token_text(p->current);
                        advance_parser(p);
                    }
                    consume(p, TokRBracket, "']'");
                    _ndim++;
                }
                if (_ndim > 0) {
                    field->as.var_decl.flags     |= VdeclArray;
                    field->as.var_decl.array_ndim  = _ndim;
                }
            }
            /* bitfield: type name: width */
            if (!in_comptime_section && match_tok(p, TokColon)) {
                if (check(p, TokIntLit)) {
                    field->as.var_decl.bitfield_width = (int)parse_int_value(p->current);
                    advance_parser(p);
                } else {
                    diag_begin_error("expected an integer literal for bitfield width");
                    diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                              True, "expected a number like 3");
                    diag_help("example: i32 flags: 3;");
                    diag_finish();
                    p->had_error = True;
                }
            }
            /* @comptime: fields may have a constant initializer: = <expr> */
            if (in_comptime_section && match_tok(p, TokEq))
                field->as.var_decl.init = parse_expr(p);
            node_list_push(&decl->as.type_decl.fields, field);
        } while (match_tok(p, TokComma));
        consume(p, TokSemicolon, "';'");
    }
    consume(p, TokRBrace, "'}'");
}

/* ── peek helpers ── */

/* Returns True if the token immediately following the current one is TokIntLit.
   Uses save/restore so no tokens are consumed. */
static boolean_t peek_is_int_lit(parser_t *p) {
    parser_state_t snap = save_state(p);
    advance_parser(p);
    boolean_t result = check(p, TokIntLit);
    restore_state(p, snap);
    return result;
}

/* ── match statement ── */

static node_t *parse_match_stmt(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'match' */

    p->no_trailing_closure++;
    node_t *subject = parse_expr(p);
    p->no_trailing_closure--;
    consume(p, TokLBrace, "'{'");

    node_t *n = make_node(NodeMatchStmt, line);
    n->as.match_stmt.expr = subject;
    node_list_init(&n->as.match_stmt.arms);

    /* Detect whether this is an any-type match (subject is NodeAnyTypeExpr) */
    boolean_t is_any_match = (subject->kind == NodeAnyTypeExpr);

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_t *arm = make_node(NodeMatchArm, p->current.line);
        arm->is_any_arm = False;
        arm->any_type_name = Null;
        arm->any_bind_name = Null;
        arm->any_bind_ti   = NO_TYPE;
        arm->any_bind_storage = StorageDefault;
        arm->as.match_arm.guard_expr    = Null;
        arm->as.match_arm.is_literal    = False;
        arm->as.match_arm.literal_value = 0;

        /* wildcard arm: _ => { ... } */
        if (check(p, TokIdent) &&
                p->current.length == 1 && p->current.start[0] == '_') {
            advance_parser(p);
            arm->as.match_arm.is_wildcard = True;
            arm->as.match_arm.enum_name    = Null;
            arm->as.match_arm.variant_name = Null;
            arm->as.match_arm.bind_name    = Null;

        /* integer literal arm: 0 => { ... }, -1 => { ... } */
        } else if (check(p, TokIntLit) ||
                   (check(p, TokMinus) && peek_is_int_lit(p))) {
            arm->as.match_arm.is_wildcard = False;
            arm->as.match_arm.is_literal  = True;
            arm->as.match_arm.enum_name   = Null;
            arm->as.match_arm.variant_name = Null;
            arm->as.match_arm.bind_name   = Null;
            long sign = 1;
            if (match_tok(p, TokMinus)) sign = -1;
            long val = 0;
            for (usize_t i = 0; i < p->current.length; i++)
                val = val * 10 + (p->current.start[i] - '0');
            arm->as.match_arm.literal_value = sign * val;
            advance_parser(p); /* consume int literal */

        } else if (is_any_match && (is_builtin_type_token(p->current.kind) || check(p, TokIdent))) {
            /* any-type arm: TypeName(storage TypeName binding) => { ... } */
            arm->as.match_arm.is_wildcard = False;
            arm->is_any_arm = True;
            /* type name being matched */
            if (is_builtin_type_token(p->current.kind)) {
                arm->any_type_name = copy_token_text(p->current);
                advance_parser(p);
            } else {
                token_t ttok = consume(p, TokIdent, "type name");
                arm->any_type_name = copy_token_text(ttok);
            }
            arm->as.match_arm.variant_name = arm->any_type_name;
            arm->as.match_arm.enum_name = Null;
            arm->as.match_arm.bind_name = Null;
            /* optional binding: (storage type name) */
            if (match_tok(p, TokLParen)) {
                storage_t ps = StorageDefault;
                if (match_tok(p, TokStack))      ps = StorageStack;
                else if (match_tok(p, TokHeap)) ps = StorageHeap;
                arm->any_bind_storage = ps;
                arm->any_bind_ti = parse_type(p);
                token_t bname = consume(p, TokIdent, "binding name");
                arm->any_bind_name = copy_token_text(bname);
                arm->as.match_arm.bind_name = arm->any_bind_name;
                consume(p, TokRParen, "')'");
            }

        } else if (check(p, TokIdent)) {
            /* Speculate: if ident is NOT followed by '.', treat as binding wildcard.
               If followed by '.', treat as enum pattern EnumName.Variant. */
            parser_state_t snap = save_state(p);
            token_t etok = p->current;
            advance_parser(p);
            if (!check(p, TokDot)) {
                /* binding wildcard: v => { use v } */
                arm->as.match_arm.is_wildcard = True;
                arm->as.match_arm.enum_name    = Null;
                arm->as.match_arm.variant_name = Null;
                arm->as.match_arm.bind_name    = copy_token_text(etok);
            } else {
                /* enum pattern: EnumName.Variant  or  EnumName.Variant(bind) */
                consume(p, TokDot, "'.'");
                token_t vtok = consume(p, TokIdent, "variant name");
                arm->as.match_arm.is_wildcard  = False;
                arm->as.match_arm.enum_name    = copy_token_text(etok);
                arm->as.match_arm.variant_name = copy_token_text(vtok);
                arm->as.match_arm.bind_name    = Null;
                if (match_tok(p, TokLParen)) {
                    token_t btok = consume(p, TokIdent, "binding name");
                    arm->as.match_arm.bind_name = copy_token_text(btok);
                    consume(p, TokRParen, "')'");
                }
            }
            (void)snap; /* snap used implicitly; save_state for restore if needed */
        } else {
            if (!p->panic_mode) {
                diag_begin_error("expected match arm pattern");
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "expected '_', integer literal, or EnumName.Variant");
                diag_finish();
            }
            p->had_error  = True;
            p->panic_mode = True;
            skip_to_recovery_point(p);
            arm->as.match_arm.is_wildcard = True;
            arm->as.match_arm.enum_name   = Null;
            arm->as.match_arm.variant_name = Null;
            arm->as.match_arm.bind_name   = Null;
        }

        /* optional guard clause: if expr */
        if (match_tok(p, TokIf)) {
            arm->as.match_arm.guard_expr = parse_expr(p);
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
        ast_set_loc(v, vname);
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

/* ── interface body parsing ── */
/* Parses: { method_name(params): ret_type; ... }
   Methods have no body — they are signatures only. */
static void parse_interface_body(parser_t *p, node_t *decl) {
    consume(p, TokLBrace, "'{'");
    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        usize_t line = p->current.line;
        /* method name */
        token_t mname;
        if (check(p, TokIdent) || check(p, TokNew) || check(p, TokRem)) {
            mname = p->current; advance_parser(p);
        } else {
            mname = consume(p, TokIdent, "method name");
        }
        consume(p, TokLParen, "'('");

        node_list_t params;
        node_list_init(&params);
        if (!check(p, TokRParen) && !check(p, TokVoid)) {
            type_info_t ptype = NO_TYPE;
            storage_t last_ps = StorageDefault;
            do {
                storage_t ps = StorageDefault;
                if (check(p, TokStack))      { ps = StorageStack; advance_parser(p); }
                else if (check(p, TokHeap)) { ps = StorageHeap;  advance_parser(p); }
                if (ps != StorageDefault) last_ps = ps;
                else ps = last_ps;
                if (can_start_param_type(p))
                    ptype = parse_type(p);
                /* optional param name */
                if (check(p, TokIdent)) {
                    token_t pname = p->current; advance_parser(p);
                    /* handle grouped names: type a, b — if next token is comma+ident, it's grouped */
                    node_t *param = make_node(NodeVarDecl, pname.line);
                    ast_set_loc(param, pname);
                    param->as.var_decl.name = copy_token_text(pname);
                    param->as.var_decl.type = ptype;
                    param->as.var_decl.storage = ps;
                    node_list_push(&params, param);
                    /* check for grouped param names */
                    while (check(p, TokComma)) {
                        parser_state_t snap = save_state(p);
                        advance_parser(p); /* consume comma */
                        if (is_name_token(p) && !can_start_param_type(p)) {
                            token_t gname = p->current; advance_parser(p);
                            node_t *gparam = make_node(NodeVarDecl, gname.line);
                            ast_set_loc(gparam, gname);
                            gparam->as.var_decl.name = copy_token_text(gname);
                            gparam->as.var_decl.type = ptype;
                            gparam->as.var_decl.storage = ps;
                            node_list_push(&params, gparam);
                        } else {
                            restore_state(p, snap);
                            break;
                        }
                    }
                    continue; /* skip the outer do-while match_tok */
                } else {
                    /* no name, just type */
                    node_t *param = make_node(NodeVarDecl, line);
                    param->col = 1;
                    param->source_file = diag_get_file() ? ast_strdup(diag_get_file(), strlen(diag_get_file())) : Null;
                    param->as.var_decl.name = ast_strdup("_", 1);
                    param->as.var_decl.type = ptype;
                    param->as.var_decl.storage = ps;
                    node_list_push(&params, param);
                }
            } while (match_tok(p, TokComma));
        } else if (check(p, TokVoid)) {
            advance_parser(p);
        }
        consume(p, TokRParen, "')'");

        /* optional return type: ': type' */
        type_info_t ret_type = NO_TYPE;
        if (match_tok(p, TokColon)) {
            ret_type = parse_type(p);
        }
        consume(p, TokSemicolon, "';'");

        node_t *fn = make_node(NodeFnDecl, line);
        ast_set_loc(fn, mname);
        fn->as.fn_decl.name = copy_token_text(mname);
        fn->as.fn_decl.linkage = LinkageExternal;
        fn->as.fn_decl.is_method = False;
        fn->as.fn_decl.struct_name = Null;
        fn->as.fn_decl.body = Null;
        fn->as.fn_decl.params = params;
        fn->as.fn_decl.iface_qualifier = Null;
        fn->as.fn_decl.return_types = alloc_type_array(1);
        fn->as.fn_decl.return_types[0] = ret_type;
        fn->as.fn_decl.return_count = 1;
        node_list_push(&decl->as.type_decl.methods, fn);
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
    ast_set_loc(n, name_tok);
    n->as.type_decl.name = copy_token_text(name_tok);
    n->as.type_decl.linkage = linkage;
    n->as.type_decl.attr_flags = 0;
    n->as.type_decl.align_value = 0;
    node_list_init(&n->as.type_decl.fields);
    node_list_init(&n->as.type_decl.methods);
    node_list_init(&n->as.type_decl.variants);

    /* parse attributes before struct/union keyword: @packed @align(N) @c_layout @comptime[T] */
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
            } else if (len == 8 && memcmp(s, "comptime", 8) == 0) {
                /* @comptime[T, U, ...] — generic type parameters */
                advance_parser(p); /* consume 'comptime' */
                if (match_tok(p, TokLBracket)) {
                    parse_type_params_into(p, n->as.type_decl.type_params,
                                          &n->as.type_decl.type_param_count, 8);
                }
            } else {
                diag_begin_error("unknown attribute '@%.*s'", (int)len, s);
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "not a recognised attribute");
                diag_note("valid struct attributes: @packed, @c_layout, @align(N), @comptime[T]");
                diag_finish();
                p->had_error = True;
                advance_parser(p);
            }
        }
    }

    if (check(p, TokInterface)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclInterface;
        /* optional parent list: .[parent1, parent2] */
        if (check(p, TokDot)) {
            parser_state_t saved = save_state(p);
            advance_parser(p); /* consume '.' */
            if (match_tok(p, TokLBracket)) {
                while (!check(p, TokRBracket) && !check(p, TokEof)) {
                    if (n->as.type_decl.impl_iface_count < 8) {
                        token_t pname = consume(p, TokIdent, "interface parent name");
                        n->as.type_decl.impl_ifaces[n->as.type_decl.impl_iface_count++] =
                            copy_token_text(pname);
                    }
                    if (!match_tok(p, TokComma)) break;
                }
                consume(p, TokRBracket, "']'");
            } else {
                restore_state(p, saved);
            }
        }
        parse_interface_body(p, n);
        match_tok(p, TokSemicolon);
    } else if (check(p, TokStruct)) {
        advance_parser(p);
        n->as.type_decl.decl_kind = TypeDeclStruct;
        /* optional interface implementation list: .[iface1, iface2] */
        if (check(p, TokDot)) {
            parser_state_t saved = save_state(p);
            advance_parser(p); /* consume '.' */
            if (match_tok(p, TokLBracket)) {
                while (!check(p, TokRBracket) && !check(p, TokEof)) {
                    if (n->as.type_decl.impl_iface_count < 8) {
                        token_t iname = consume(p, TokIdent, "interface name");
                        n->as.type_decl.impl_ifaces[n->as.type_decl.impl_iface_count++] =
                            copy_token_text(iname);
                    }
                    if (!match_tok(p, TokComma)) break;
                }
                consume(p, TokRBracket, "']'");
            } else {
                restore_state(p, saved);
            }
        }
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
        diag_begin_error("expected a library name string after 'lib'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal");
        diag_help("example: lib \"stdio\" = io;  or  lib \"mylib\" from \"libmylib.a\";");
        diag_finish();
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
            diag_begin_error("expected a path string after 'from'");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a string literal path to the .a archive");
            diag_finish();
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
    ast_set_loc(n, ntok);
    n->as.lib_decl.name  = name;
    n->as.lib_decl.alias = alias;
    n->as.lib_decl.path  = path;
    return n;
}

/* ── libimp ──
 *
 * Syntax forms:
 *   libimp "name" from "path";   — lib + imp in one, archive at path
 *   libimp "name" from std;      — lib + imp in one, archive from stdlib
 *
 * The module name is always the same as the library name.
 */
static node_t *parse_libimp(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'libimp' */

    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        diag_begin_error("expected a library name string after 'libimp'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal");
        diag_help("example: libimp \"vec\" from \"libvec.a\";");
        diag_finish();
        p->had_error = True;
        return make_node(NodeLibImp, line);
    }
    advance_parser(p);
    token_t ntok = p->previous;
    char *name = ast_strdup(ntok.start + 1, ntok.length - 2);

    if (!check(p, TokFrom)) {
        diag_begin_error("expected 'from' after libimp name");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected 'from' here");
        diag_help("example: libimp \"vec\" from \"libvec.a\";");
        diag_finish();
        p->had_error = True;
        return make_node(NodeLibImp, line);
    }
    advance_parser(p); /* consume 'from' */

    boolean_t from_std = False;
    char *path = Null;

    if (check(p, TokStd)) {
        from_std = True;
        advance_parser(p); /* consume 'std' */
    } else if (check(p, TokStackStr) || check(p, TokHeapStr)) {
        advance_parser(p);
        token_t ptok = p->previous;
        path = ast_strdup(ptok.start + 1, ptok.length - 2);
    } else {
        diag_begin_error("expected a path string or 'std' after 'from'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a path or 'std'");
        diag_help("use 'from std' for stdlib modules, or 'from \"path/to/lib.a\"' for custom libraries");
        diag_finish();
        p->had_error = True;
        return make_node(NodeLibImp, line);
    }

    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeLibImp, line);
    ast_set_loc(n, ntok);
    n->as.libimp_decl.name     = name;
    n->as.libimp_decl.path     = path;
    n->as.libimp_decl.from_std = from_std;
    return n;
}

/* ── cheader ── */
static node_t *parse_cheader(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'cheader' */

    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        diag_begin_error("expected a header path string after 'cheader'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal");
        diag_help("example: cheader \"stdio.h\"; or cheader \"curl/curl.h\" search \"/usr/include:/opt/homebrew/include\";");
        diag_finish();
        p->had_error = True;
        return make_node(NodeCHeader, line);
    }

    advance_parser(p);
    token_t path_tok = p->previous;
    char *path = ast_strdup(path_tok.start + 1, path_tok.length - 2);
    char *search_dirs = Null;

    if (check(p, TokIdent) && p->current.length == 6
            && memcmp(p->current.start, "search", 6) == 0) {
        advance_parser(p); /* consume 'search' */
        if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
            diag_begin_error("expected a search path string after 'search'");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a colon-separated string");
            diag_finish();
            p->had_error = True;
            return make_node(NodeCHeader, line);
        }
        advance_parser(p);
        token_t search_tok = p->previous;
        search_dirs = ast_strdup(search_tok.start + 1, search_tok.length - 2);
    }

    consume(p, TokSemicolon, "';'");

    node_t *n = make_node(NodeCHeader, line);
    ast_set_loc(n, path_tok);
    n->as.cheader_decl.path = path;
    n->as.cheader_decl.search_dirs = search_dirs;
    return n;
}

/* ── imp ── */

static node_t *parse_imp(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'imp' */
    char *mod_name = parse_dotted_name(p);
    /* Optional: = alias_name  (e.g. imp ex_preproc = pp;) */
    if (match_tok(p, TokEq))
        advance_parser(p); /* consume the alias ident; we don't store it */
    consume(p, TokSemicolon, "';'");
    node_t *n = make_node(NodeImpDecl, line);
    n->col = 1;
    n->source_file = diag_get_file() ? ast_strdup(diag_get_file(), strlen(diag_get_file())) : Null;
    n->as.imp_decl.module_name = mod_name;
    return n;
}

/* ── function declaration ── */

static node_t *parse_fn_decl(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'fn' */

    /* optional @comptime[T, U, ...] type parameters after 'fn' */
    char *fn_type_params[8];
    usize_t fn_type_param_count = 0;
    if (check(p, TokAt)) {
        parser_state_t snap = save_state(p);
        advance_parser(p); /* consume '@' */
        if (check(p, TokIdent) && p->current.length == 8
                && memcmp(p->current.start, "comptime", 8) == 0) {
            advance_parser(p); /* consume 'comptime' */
            if (check(p, TokLBracket)) {
                advance_parser(p); /* consume '[' */
                parse_type_params_into(p, fn_type_params, &fn_type_param_count, 8);
            } else {
                restore_state(p, snap);
            }
        } else {
            restore_state(p, snap);
        }
    }

    token_t name_tok = consume(p, TokIdent, "function name");
    token_t decl_name_tok = name_tok;
    char *name = copy_token_text(name_tok);

    /* check for Type.method pattern */
    boolean_t is_method = False;
    char *struct_name = Null;
    if (check(p, TokDot)) {
        advance_parser(p);
        struct_name = name;
        is_method = True;

        /* method name: allow ident and contextual keywords used as method names */
        if (check(p, TokIdent)) {
            decl_name_tok = p->current;
            name = copy_token_text(p->current);
            advance_parser(p);
        } else if (check(p, TokNew)) {
            decl_name_tok = p->current;
            name = ast_strdup("new", 3);
            advance_parser(p);
        } else if (check(p, TokRem)) {
            decl_name_tok = p->current;
            name = ast_strdup("rem", 3);
            advance_parser(p);
        } else if (check(p, TokPrint)) {
            decl_name_tok = p->current;
            name = ast_strdup("print", 5);
            advance_parser(p);
        } else if (check(p, TokFrom)) {
            decl_name_tok = p->current;
            name = ast_strdup("from", 4);
            advance_parser(p);
        } else if (check(p, TokHash)) {
            decl_name_tok = p->current;
            name = ast_strdup("hash", 4);
            advance_parser(p);
        } else if (check(p, TokEqu)) {
            decl_name_tok = p->current;
            name = ast_strdup("equ", 3);
            advance_parser(p);
        } else {
            diag_begin_error("expected a method name after '.'");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected an identifier");
            diag_finish();
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
            /* optional parameter attributes: @frees, @restrict */
            boolean_t is_frees    = False;
            boolean_t is_restrict = False;
            while (check(p, TokAt)) {
                parser_state_t attr_snap = save_state(p);
                advance_parser(p); /* consume '@' */
                if (check(p, TokIdent) && p->current.length == 5
                        && memcmp(p->current.start, "frees", 5) == 0) {
                    advance_parser(p); /* consume 'frees' identifier */
                    is_frees = True;
                } else if (check(p, TokRestrict)) {
                    advance_parser(p); /* consume 'restrict' */
                    is_restrict = True;
                } else if (check(p, TokIdent)) {
                    /* unknown attribute — warn and skip */
                    diag_begin_error("unknown parameter attribute '@%.*s'",
                                     (int)p->current.length, p->current.start);
                    diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                              True, "not a recognised parameter attribute");
                    diag_note("valid parameter attributes: @frees, @restrict");
                    diag_finish();
                    p->had_error = True;
                    advance_parser(p);
                } else {
                    restore_state(p, attr_snap);
                    break;
                }
            }
            /* optional storage qualifier on parameter */
            storage_t param_storage = StorageDefault;
            if (check(p, TokStack))      { param_storage = StorageStack;  advance_parser(p); }
            else if (check(p, TokHeap)) { param_storage = StorageHeap;   advance_parser(p); }
            if (param_storage != StorageDefault) last_storage = param_storage;
            else param_storage = last_storage;
            /* legacy @restrict keyword after storage qualifier */
            if (!is_restrict && match_tok(p, TokRestrict)) is_restrict = True;
            if (can_start_param_type(p))
                last_type = parse_type(p);
            token_t pname = consume_name(p, "parameter name");
            node_t *param = make_node(NodeVarDecl, pname.line);
            ast_set_loc(param, pname);
            param->as.var_decl.name    = copy_token_text(pname);
            param->as.var_decl.type    = last_type;
            param->as.var_decl.storage = param_storage;
            if (is_restrict) param->as.var_decl.flags |= VdeclRestrict;
            if (is_frees)    param->as.var_decl.flags |= VdeclFrees;
            /* array parameter: name[d0][d1]... */
            {
                int _pndim = 0;
                while (check(p, TokLBracket) && _pndim < 8) {
                    advance_parser(p); /* consume '[' */
                    if (check(p, TokIntLit)) {
                        param->as.var_decl.array_sizes[_pndim] = parse_int_value(p->current);
                        advance_parser(p);
                    } else if (check(p, TokIdent)) {
                        param->as.var_decl.array_size_names[_pndim] = copy_token_text(p->current);
                        advance_parser(p);
                    }
                    consume(p, TokRBracket, "']'");
                    _pndim++;
                }
                if (_pndim > 0) {
                    param->as.var_decl.flags     |= VdeclArray;
                    param->as.var_decl.array_ndim  = _pndim;
                }
            }
            node_list_push(&params, param);
        } while (match_tok(p, TokComma));
    } else if (check(p, TokVoid)) {
        advance_parser(p);
    }
    consume(p, TokRParen, "')'");

    /* return type — optional: if '{', ';', or '=>' follows the params, default to void */
    usize_t ret_count = 1;
    type_info_t *ret_types = alloc_type_array(1);
    if (match_tok(p, TokColon)) {
        /* explicit return type(s): single type or [type, type, ...] */
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
            ret_types[0] = parse_type(p);
        }
    } else {
        ret_types[0] = NO_TYPE; /* void */
    }

    /* expression-bodied function: fn name(...): T => expr; */
    node_t *body = Null;
    if (match_tok(p, TokFatArrow)) {
        usize_t body_line = p->current.line;
        node_t *expr = parse_expr(p);
        consume(p, TokSemicolon, "';'");
        /* synthesize: { ret expr; } */
        node_t *ret_node = make_node(NodeRetStmt, body_line);
        node_list_init(&ret_node->as.ret_stmt.values);
        node_list_push(&ret_node->as.ret_stmt.values, expr);
        body = make_node(NodeBlock, body_line);
        node_list_init(&body->as.block.stmts);
        node_list_push(&body->as.block.stmts, ret_node);
    } else if (linkage == LinkageExternal && match_tok(p, TokSemicolon)) {
        /* body-less extern declaration: ext fn foo(...): T; */
        body = Null;
    } else {
        body = parse_block(p);
    }

    node_t *n = make_node(NodeFnDecl, line);
    ast_set_loc(n, decl_name_tok);
    n->as.fn_decl.name = name;
    n->as.fn_decl.linkage = linkage;
    n->as.fn_decl.return_types = ret_types;
    n->as.fn_decl.return_count = ret_count;
    n->as.fn_decl.params = params;
    n->as.fn_decl.body = body;
    n->as.fn_decl.is_method = is_method;
    n->as.fn_decl.struct_name = struct_name;
    n->as.fn_decl.is_variadic = is_variadic;
    n->as.fn_decl.type_param_count = fn_type_param_count;
    for (usize_t _i = 0; _i < fn_type_param_count; _i++)
        n->as.fn_decl.type_params[_i] = fn_type_params[_i];
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
    p->no_trailing_closure++;
    node_t *expr = parse_expr(p);
    p->no_trailing_closure--;
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
            if (!p->panic_mode) {
                diag_begin_error("expected 'case' or 'default' inside switch block");
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "unexpected token");
                diag_help("switch blocks may only contain 'case <value>:' or 'default:' clauses");
                diag_finish();
            }
            p->had_error  = True;
            p->panic_mode = True;
            skip_to_recovery_point(p);
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
            diag_begin_error("expected assembly string in asm block");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected a string literal");
            diag_help("example: asm { \"nop\" }");
            diag_finish();
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
 * The body parser: key == "value" { ... } [else { ... } | else @comptime if ...]
 * Called after the 'comptime_if' / '@comptime if' keyword has been consumed.
 */
static node_t *parse_comptime_if_body(parser_t *p) {
    usize_t line = p->current.line;

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
        diag_begin_error("expected a string literal after '==' in comptime_if");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string value");
        diag_note("valid comptime_if keys: os, arch, platform");
        diag_help("example: @comptime if os == \"macos\" { ... }");
        diag_finish();
        p->had_error = True;
        value = ast_strdup("", 0);
    }

    node_t *body = parse_block(p);
    node_t *else_body = Null;
    if (match_tok(p, TokElse)) {
        /* Allow chaining: else @comptime if ... { ... } */
        if (check(p, TokAt)) {
            parser_state_t snap = save_state(p);
            advance_parser(p);
            if (check(p, TokIdent) && p->current.length == 8
                    && memcmp(p->current.start, "comptime", 8) == 0) {
                advance_parser(p);
                if (check(p, TokIf)) {
                    advance_parser(p);
                    else_body = parse_comptime_if_body(p);
                } else {
                    restore_state(p, snap);
                    else_body = parse_block(p);
                }
            } else {
                restore_state(p, snap);
                else_body = parse_block(p);
            }
        } else if (check(p, TokComptimeIf)) {
            else_body = parse_comptime_if(p); /* old-style chaining */
        } else {
            else_body = parse_block(p);
        }
    }

    node_t *n = make_node(NodeComptimeIf, line);
    n->as.comptime_if.key = key;
    n->as.comptime_if.value = value;
    n->as.comptime_if.body = body;
    n->as.comptime_if.else_body = else_body;
    return n;
}

/*
 * Old-style keyword: comptime_if platform == "macos" { ... }
 */
static node_t *parse_comptime_if(parser_t *p) {
    advance_parser(p); /* consume 'comptime_if' */
    return parse_comptime_if_body(p);
}

/* parse @comptime assert.(expr [, 'msg']) — returns NodeComptimeAssert */
static node_t *parse_at_comptime_assert(parser_t *p) {
    usize_t line = p->previous.line; /* line of '@' */
    /* caller has consumed '@' and 'comptime'; we must consume 'assert' */
    if (!check(p, TokIdent) || p->current.length != 6
            || memcmp(p->current.start, "assert", 6) != 0) {
        diag_begin_error("expected 'assert' after '@comptime'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected 'assert' here");
        diag_finish();
        p->had_error = True;
        return make_node(NodeComptimeAssert, line);
    }
    advance_parser(p); /* consume 'assert' */
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

/* ── top-level declarations ── */

static node_t *parse_test_block(parser_t *p) {
    usize_t line = p->current.line;
    advance_parser(p); /* consume 'test' */
    /* test name is a string literal */
    if (!check(p, TokStackStr) && !check(p, TokHeapStr)) {
        diag_begin_error("expected a test name string after 'test'");
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a string literal");
        diag_help("example: test 'my test' { expect.(x == 1); }");
        diag_finish();
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

static node_t *parse_top_decl(parser_t *p); /* forward decl — used by parse_submod_block */

static node_t *parse_submod_block(parser_t *p, linkage_t linkage) {
    usize_t line = p->current.line;
    token_t name_tok = consume(p, TokIdent, "sub-module name");
    char *name = copy_token_text(name_tok);

    consume(p, TokLBrace, "'{'");

    node_t *n = make_node(NodeSubMod, line);
    n->as.submod.name    = name;
    n->as.submod.linkage = linkage;
    node_list_init(&n->as.submod.decls);

    while (!check(p, TokRBrace) && !check(p, TokEof)) {
        node_t *decl = parse_top_decl(p);
        if (!decl) { synchronize(p); continue; }
        /* tag every child with this submod's name for mangling */
        decl->module_name = name;
        /* hide internal submod members from importers */
        if (linkage == LinkageInternal)
            decl->hidden_from_import = True;
        node_list_push(&n->as.submod.decls, decl);
    }

    consume(p, TokRBrace, "'}'");
    return n;
}

static node_t *parse_top_decl(parser_t *p) {
    /* ── Collect leading @[[...]] fileheader groups ── */
    fileheader_t pending;
    fileheader_init(&pending);

    for (;;) {
        /* Only try fileheader path if current is `@` followed by `[[` */
        if (!check(p, TokAt)) break;
        parser_state_t snap = save_state(p);
        advance_parser(p); /* '@' */
        if (!check(p, TokLBracket)) { restore_state(p, snap); break; }
        advance_parser(p);
        if (!check(p, TokLBracket)) { restore_state(p, snap); break; }
        advance_parser(p);

        fileheader_t group;
        fileheader_init(&group);
        parse_fileheader_entries(p, &group);
        consume(p, TokRBracket, "']'");
        consume(p, TokRBracket, "']'");

        /* File-wide: `@[[...]];` — merge into module and continue loop. */
        if (match_tok(p, TokSemicolon)) {
            if (p->current_module) {
                if (p->current_module->headers == Null)
                    p->current_module->headers = fileheader_alloc();
                fileheader_merge(p->current_module->headers, &group);
                apply_module_headers(p->current_module, &group);
            }
            continue;
        }

        /* Lifecycle block: @[[init ...]] { ... }  or  @[[init ...]] "title" { ... } */
        {
            boolean_t has_init = fh_find(&group, "init") != Null;
            boolean_t has_exit = fh_find(&group, "exit") != Null;
            if ((has_init || has_exit) &&
                (check(p, TokLBrace) || check(p, TokStackStr) || check(p, TokHeapStr))) {
                fileheader_merge(&pending, &group);
                return parse_lifecycle_block(p, &pending);
            }
        }

        /* Decl-scoped: accumulate and fall through to next iteration (more headers)
           or to the actual declaration parser below. */
        fileheader_merge(&pending, &group);
    }

    node_t *result = Null;

    /* lib */
    if (check(p, TokLib)) { result = parse_lib(p); goto attach_headers; }

    /* cheader */
    if (check(p, TokCHeader)) { result = parse_cheader(p); goto attach_headers; }

    /* imp */
    if (check(p, TokImp)) { result = parse_imp(p); goto attach_headers; }

    /* libimp */
    if (check(p, TokLibImp)) { result = parse_libimp(p); goto attach_headers; }

    /* test block */
    if (check(p, TokTest)) { result = parse_test_block(p); goto attach_headers; }

    /* comptime_if at top level (old keyword style) */
    if (check(p, TokComptimeIf)) { result = parse_comptime_if(p); goto attach_headers; }

    /* comptime_assert at top level (old keyword style) */
    if (check(p, TokComptimeAssert)) {
        /* parse directly to NodeComptimeAssert so the codegen loop finds it */
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
        consume(p, TokSemicolon, "';'");
        node_t *n = make_node(NodeComptimeAssert, line);
        n->as.comptime_assert.expr = expr;
        n->as.comptime_assert.message = msg;
        result = n;
        goto attach_headers;
    }

    /* global zone declaration: zone name; */
    if (check(p, TokZone)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'zone' */
        token_t name_tok = consume(p, TokIdent, "zone name");
        consume(p, TokSemicolon, "';'");
        node_t *n = make_node(NodeZoneStmt, line);
        n->as.zone_stmt.name = copy_token_text(name_tok);
        result = n;
        goto attach_headers;
    }

    /* @comptime if / @comptime assert at top level (new attribute syntax) */
    if (check(p, TokAt)) {
        parser_state_t snap = save_state(p);
        advance_parser(p); /* consume '@' */
        if (check(p, TokIdent) && p->current.length == 8
                && memcmp(p->current.start, "comptime", 8) == 0) {
            advance_parser(p); /* consume 'comptime' */
            if (check(p, TokIf)) {
                advance_parser(p); /* consume 'if' */
                result = parse_comptime_if_body(p);
                goto attach_headers;
            }
            if (check(p, TokIdent) && p->current.length == 6
                    && memcmp(p->current.start, "assert", 6) == 0) {
                node_t *ca = parse_at_comptime_assert(p);
                consume(p, TokSemicolon, "';'");
                result = ca;
                goto attach_headers;
            }
        }
        restore_state(p, snap);
    }

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
                diag_begin_error("unknown function/variable attribute '@%.*s'", (int)len, s);
                diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                          True, "not a recognised attribute");
                diag_note("valid fn/var attributes: @weak, @hidden, @restrict");
                diag_finish();
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

    /* [int|ext] mod name { ... } — inline sub-module block */
    if (check(p, TokMod)) {
        advance_parser(p); /* consume 'mod' */
        result = parse_submod_block(p, linkage);
        goto attach_headers;
    }

    /* async fn ... — marker enabling async.()/await.() shorthand + typed future */
    boolean_t is_async = False;
    if (check(p, TokAsync)) {
        advance_parser(p);
        is_async = True;
        if (!check(p, TokFn)) {
            diag_begin_error("'async' must be followed by 'fn'");
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected 'fn' here");
            diag_help("declare an async function: async fn name(...): T { ... }");
            diag_finish();
            p->had_error = True;
        }
    }

    /* type declaration */
    if (check(p, TokType)) { result = parse_type_decl(p, linkage); goto attach_headers; }

    /* function declaration */
    if (check(p, TokFn)) {
        node_t *fn = parse_fn_decl(p, linkage);
        fn->as.fn_decl.attr_flags = attr_flags;
        fn->as.fn_decl.is_async   = is_async;
        result = fn;
        goto attach_headers;
    }

    /* global variable */
    {
        node_t *var = parse_var_decl(p, linkage);
        if (var->kind == NodeVarDecl)
            var->as.var_decl.attr_flags = attr_flags;
        result = var;
    }

attach_headers:
    if (result && pending.count > 0) {
        if (result->headers == Null)
            result->headers = fileheader_alloc();
        fileheader_merge(result->headers, &pending);
    }
    return result;
}
