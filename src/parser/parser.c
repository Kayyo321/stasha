#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "parser.h"

typedef struct {
    /* ── Stream mode (preferred) ── */
    const pp_token_t *stream;       /* NULL → use legacy lexer mode       */
    int               stream_idx;
    int               stream_count;
    pp_expansion_t   *current_expansion; /* expansion chain of current token */

    /* ── Legacy lexer mode ── */
    lexer_t lexer;

    /* ── Shared ── */
    token_t   current;
    token_t   previous;
    boolean_t had_error;
    boolean_t panic_mode;  /* suppress cascading errors until sync point */

    /* Current top-level module under construction — used by fileheader
       parsing to attach file-wide @[[...]] metadata.  Null outside run_parse. */
    node_t   *current_module;

    /* Sugar pack: when > 0, suppress trailing-closure parsing because the
       brace would belong to the surrounding control-flow construct
       (if/while/for/do-while condition, etc.).  Counter, not bool, so
       nesting works correctly. */
    int no_trailing_closure;
} parser_t;

/* ── save / restore for speculative parsing (casts) ── */

typedef struct {
    /* stream fields */
    int              stream_idx;
    pp_expansion_t  *current_expansion;
    /* lexer field */
    lexer_t          lexer;
    /* tokens */
    token_t          current;
    token_t          previous;
} parser_state_t;

static parser_state_t save_state(parser_t *p) {
    parser_state_t s;
    s.stream_idx         = p->stream_idx;
    s.current_expansion  = p->current_expansion;
    s.lexer              = p->lexer;
    s.current            = p->current;
    s.previous           = p->previous;
    return s;
}

static void restore_state(parser_t *p, parser_state_t s) {
    p->stream_idx        = s.stream_idx;
    p->current_expansion = s.current_expansion;
    p->lexer             = s.lexer;
    p->current           = s.current;
    p->previous          = s.previous;
}

/* ── helpers ── */

static void advance_parser(parser_t *p) {
    p->previous = p->current;

    if (p->stream) {
        /* ── Stream mode: read from preprocessed token array ── */
        for (;;) {
            if (p->stream_idx >= p->stream_count) {
                /* Past the end: synthesize EOF. */
                token_t eof;
                memset(&eof, 0, sizeof(eof));
                eof.kind = TokEof;
                p->current           = eof;
                p->current_expansion = NULL;
                return;
            }

            const pp_token_t *pt = &p->stream[p->stream_idx++];

            if (pt->tok.kind == TokError) {
                /* Lex error recorded during preprocessing. */
                const char *f = pt->tok.file;
                if (f && f != diag_get_file()) diag_set_file(f);
                diag_begin_error("%.*s",
                                 (int)pt->tok.length, pt->tok.start);
                diag_span(SRC_LOC(pt->tok.line, pt->tok.col, 1), True, "here");
                diag_finish();
                p->had_error = True;
                continue;
            }

            /* Switch diagnostic context when crossing a file boundary. */
            if (pt->tok.file && pt->tok.file != diag_get_file())
                diag_set_file(pt->tok.file);

            p->current           = pt->tok;
            p->current_expansion = pt->expansion;
            return;
        }
    } else {
        /* ── Legacy lexer mode ── */
        for (;;) {
            p->current = next_token(&p->lexer);
            if (p->current.kind != TokError) break;
            /* Lexer errors: unterminated string, unexpected character, etc. */
            diag_begin_error("%.*s",
                             (int)p->current.length, p->current.start);
            diag_span(SRC_LOC(p->current.line, p->current.col, 1),
                      True, "here");
            diag_finish();
            p->had_error = True;
        }
        p->current_expansion = NULL;
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
    /* Only emit diagnostic if not already in panic mode (suppress cascades) */
    if (!p->panic_mode) {
        if (p->current.kind == TokEof) {
            diag_begin_error("unexpected end of file, expected %s", msg);
            diag_span(SRC_LOC(p->current.line, p->current.col, 1), True,
                      "expected %s here", msg);
        } else {
            diag_begin_error("expected %s, found '%.*s'",
                             msg, (int)p->current.length, p->current.start);
            diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                      True, "expected %s", msg);
        }
        diag_finish();
    }
    p->had_error  = True;
    p->panic_mode = True;
    return p->current;
}

/* ── type helpers ── */

static boolean_t is_builtin_type_token(token_kind_t k) {
    return k == TokI8  || k == TokI16 || k == TokI32 || k == TokI64
        || k == TokU8  || k == TokU16 || k == TokU32 || k == TokU64
        || k == TokF32 || k == TokF64
        || k == TokBool || k == TokVoid || k == TokErrorType || k == TokFuture
        || k == TokStream
        || k == TokAny || k == TokZone;
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
        case TokErrorType: return TypeError;
        case TokFuture:    return TypeFuture;
        case TokStream:    return TypeStream;
        case TokZone:      return TypeZone;
        default:      return TypeVoid;
    }
}

/* ── comptime_type_name: convert a type_info_t to a short name for generic mangling ── */

static const char *comptime_type_name(type_info_t ti) {
    if (ti.is_pointer) return "ptr";
    switch (ti.base) {
        case TypeVoid:  return "void";
        case TypeBool:  return "bool";
        case TypeI8:    return "i8";
        case TypeI16:   return "i16";
        case TypeI32:   return "i32";
        case TypeI64:   return "i64";
        case TypeU8:    return "u8";
        case TypeU16:   return "u16";
        case TypeU32:   return "u32";
        case TypeU64:   return "u64";
        case TypeF32:   return "f32";
        case TypeF64:   return "f64";
        case TypeUser:  return ti.user_name ? ti.user_name : "user";
        case TypeFuture: return "future";
        case TypeStream: return "stream";
        case TypeSlice: return "slice";
        default:        return "type";
    }
}

static type_info_t parse_type(parser_t *p) {
    type_info_t info = NO_TYPE;

    /* nullable pointer type: ?T *p — the ? prefixes the entire type */
    boolean_t nullable = False;
    if (check(p, TokQuestion)) {
        advance_parser(p); /* consume '?' */
        nullable = True;
    }

    /* slice type: []T — [ immediately followed by ] */
    if (check(p, TokLBracket)) {
        /* peek at what follows the '[' without consuming */
        parser_state_t snap = save_state(p);
        advance_parser(p); /* consume '[' */
        if (check(p, TokRBracket)) {
            advance_parser(p); /* consume ']' */
            type_info_t elem = parse_type(p);
            type_info_t ti = NO_TYPE;
            ti.base = TypeSlice;
            ti.elem_type = alloc_type_array(1);
            ti.elem_type[0] = elem;
            return ti;
        }
        /* not a slice — restore */
        restore_state(p, snap);
    }

    /* function pointer type: fn*(params): ret_type */
    if (check(p, TokFn)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'fn' */

        if (!check(p, TokStar)) {
            diag_begin_error("expected '*' after 'fn' in function pointer type");
            diag_span(SRC_LOC(line, p->previous.col, 2), True,
                      "add '*' here to make a function pointer");
            diag_help("function pointer syntax: fn*(params): ret_type");
            diag_finish();
            p->had_error = True;
            return info;
        }
        advance_parser(p); /* consume '*' */

        consume(p, TokLParen, "'(' in function pointer type");

        /* collect up to 32 parameters (stack-allocated temp buffer) */
        fn_ptr_param_t tmp_params[32];
        usize_t param_count = 0;

        if (!check(p, TokRParen) && !check(p, TokVoid)) {
            do {
                if (param_count >= 32) {
                    diag_begin_error("function pointer type exceeds maximum of 32 parameters");
                    diag_span(SRC_LOC(line, 1, 0), True, "in this function pointer type");
                    diag_finish();
                    p->had_error = True;
                    break;
                }
                storage_t ps = StorageDefault;
                if (match_tok(p, TokStack))      ps = StorageStack;
                else if (match_tok(p, TokHeap)) ps = StorageHeap;

                type_info_t ptype = parse_type(p);
                tmp_params[param_count].storage = ps;
                tmp_params[param_count].type    = ptype;
                param_count++;
            } while (match_tok(p, TokComma));
        } else if (check(p, TokVoid)) {
            advance_parser(p); /* void = no params */
        }

        consume(p, TokRParen, "')'");
        consume(p, TokColon, "':'");
        type_info_t ret_type = parse_type(p);

        fn_ptr_desc_t *desc = alloc_fn_ptr_desc(param_count);
        desc->ret_type = ret_type;
        for (usize_t i = 0; i < param_count; i++)
            desc->params[i] = tmp_params[i];

        info.base         = TypeFnPtr;
        info.fn_ptr_desc  = desc;
        return info;
    }

    /* optional storage qualifier prefix on pointer return types (e.g. heap u8 *r) */
    if (check(p, TokHeap) || check(p, TokStack)) {
        advance_parser(p);
    }

    /* any.[T1, T2, ...] — inline tagged-union type */
    if (check(p, TokAny)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'any' */
        if (check(p, TokDot)) {
            advance_parser(p); /* consume '.' */
            if (check(p, TokLBracket)) {
                advance_parser(p); /* consume '[' */
                char mangled[512];
                usize_t mlen = 3;
                memcpy(mangled, "any", 3);
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
                info.base = TypeUser;
                info.user_name = ast_strdup(mangled, mlen);
                (void)line;
                goto parse_type_ptr;
            }
        }
        /* bare 'any' without type list — treat as i64 placeholder */
        info.base = TypeI64;
        goto parse_type_ptr;
    }

    if (is_builtin_type_token(p->current.kind)) {
        token_kind_t tk = p->current.kind;
        info.base = token_to_type(tk);
        advance_parser(p);

        /* future.[T] / stream.[T] — typed coroutine handles. ABI-identical
           to opaque pointers; T is recorded in elem_type for later passes. */
        if ((tk == TokFuture || tk == TokStream) && check(p, TokDot)) {
            parser_state_t snap = save_state(p);
            advance_parser(p); /* consume '.' */
            if (check(p, TokLBracket)) {
                advance_parser(p); /* consume '[' */
                type_info_t elem = parse_type(p);
                consume(p, TokRBracket, "']'");
                info.elem_type = alloc_type_array(1);
                info.elem_type[0] = elem;
            } else {
                restore_state(p, snap);
            }
        }
    } else if (check(p, TokIdent)) {
        info.base = TypeUser;
        info.user_name = copy_token_text(p->current);
        advance_parser(p);

        /* generic instantiation: TypeName.[T1, T2, ...] → "TypeName_G_T1_G_T2" */
        if (check(p, TokDot)) {
            parser_state_t snap = save_state(p);
            advance_parser(p); /* consume '.' */
            if (check(p, TokLBracket)) {
                advance_parser(p); /* consume '[' */
                char mangled[512];
                usize_t mlen = strlen(info.user_name);
                if (mlen < sizeof(mangled) - 1)
                    memcpy(mangled, info.user_name, mlen);
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
                info.user_name = ast_strdup(mangled, mlen);
            } else {
                restore_state(p, snap);
            }
        }
    } else {
        diag_begin_error("expected a type, found '%.*s'",
                         (int)p->current.length, p->current.start);
        diag_span(SRC_LOC(p->current.line, p->current.col, p->current.length),
                  True, "expected a type here");
        diag_note("valid types: i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, bool, void, error, or a struct/enum name");
        diag_finish();
        p->had_error = True;
        return info;
    }

    parse_type_ptr:
    /* Pointer levels: *, *r, *w, *rw, each optionally followed by + for arith.
     * Multiple levels are allowed: **, *r *rw, *rw+ *r, etc.
     * Reading left to right: first * = innermost level, last * = outermost level
     * (the level the variable itself is).  ptr_perms[0] = outermost perms,
     * ptr_perms[depth-1] = innermost perms.  ptr_perm mirrors ptr_perms[0]. */
    {
        ptr_perm_t tmp[8];
        int depth = 0;
        while (check(p, TokStar) && depth < 8) {
            advance_parser(p);
            ptr_perm_t perm = PtrRead | PtrWrite | PtrArith; /* bare * = rw+ */
            if (check(p, TokIdent)) {
                const char *s = p->current.start;
                usize_t len = p->current.length;
                if (len == 1 && s[0] == 'r') {
                    perm = PtrRead; advance_parser(p);
                } else if (len == 1 && s[0] == 'w') {
                    perm = PtrWrite; advance_parser(p);
                } else if (len == 2 && s[0] == 'r' && s[1] == 'w') {
                    perm = PtrReadWrite; advance_parser(p);
                }
            }
            /* optional + grants pointer arithmetic for this level */
            if (check(p, TokPlus)) { perm |= PtrArith; advance_parser(p); }
            tmp[depth++] = perm;
        }
        if (depth > 0) {
            info.is_pointer = True;
            info.ptr_depth  = depth;
            /* Reverse: tmp[0]=first *=innermost → ptr_perms[depth-1];
               tmp[depth-1]=last *=outermost → ptr_perms[0]. */
            for (int i = 0; i < depth; i++)
                info.ptr_perms[i] = tmp[depth - 1 - i];
            info.ptr_perm = info.ptr_perms[0];
        }
    }

    /* A leading '?' only makes sense on a pointer type */
    if (nullable) {
        if (!info.is_pointer) {
            diag_begin_error("'?' nullable qualifier is only valid on pointer types");
            diag_span(SRC_LOC(p->previous.line, p->previous.col, 1), True,
                      "not a pointer type");
            diag_note("write '?T *p' to declare a nullable pointer");
            diag_finish();
            p->had_error = True;
        } else {
            info.nullable = True;
        }
    }

    return info;
}

/* ── forward declarations ── */

static node_t *parse_expr(parser_t *p);
static node_t *parse_postfix(parser_t *p);
static node_t *parse_unary(parser_t *p);
static node_t *parse_cmp_chain(parser_t *p);
static node_t *parse_block(parser_t *p);
static node_t *parse_statement(parser_t *p);
static node_t *parse_match_stmt(parser_t *p);
static node_t *parse_var_decl(parser_t *p, linkage_t linkage);
static node_t *parse_defer_stmt(parser_t *p);
static node_t *parse_switch_stmt(parser_t *p);
static node_t *parse_asm_stmt(parser_t *p);
static node_t *parse_comptime_if(parser_t *p);
static node_t *parse_comptime_if_body(parser_t *p);
static node_t *parse_comptime_assert(parser_t *p);
static node_t *parse_at_comptime_assert(parser_t *p);

static boolean_t can_start_type(parser_t *p) {
    /* nullable prefix '?' also starts a type */
    if (check(p, TokQuestion)) return True;
    if (check(p, TokLBracket)) {
        /* []T slice type — peek to confirm next is ']' */
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t is_slice = check(p, TokRBracket);
        restore_state(p, snap);
        if (is_slice) return True;
    }
    return is_builtin_type_token(p->current.kind) || check(p, TokIdent) || check(p, TokFn)
        || check(p, TokAny);
}

/* In a parameter list, a bare identifier is only a type if followed by
   another identifier or '*' (i.e. "Type name" or "Type * name").
   If followed by ',' or ')', it must be a grouped parameter name. */
static boolean_t can_start_param_type(parser_t *p) {
    if (check(p, TokQuestion)) return True; /* nullable pointer prefix */
    if (is_builtin_type_token(p->current.kind)) return True;
    if (check(p, TokFn)) return True; /* fn* function pointer type */
    /* []T slice type */
    if (check(p, TokLBracket)) {
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t is_slice = check(p, TokRBracket);
        restore_state(p, snap);
        if (is_slice) return True;
    }
    if (!check(p, TokIdent)) return False;
    parser_state_t snap = save_state(p);
    advance_parser(p);
    boolean_t result = check(p, TokIdent) || check(p, TokStar);
    /* also allow generic instantiation: Ident.[  */
    if (!result && check(p, TokDot)) {
        advance_parser(p);
        result = check(p, TokLBracket);
    }
    restore_state(p, snap);
    return result;
}

/* Returns true if the current token can be used as an identifier/name.
   Slice builtins (len, cap, make, append, copy) are contextual keywords —
   they parse as special expressions but are also valid names in all other
   contexts (field names, parameter names, variable names, etc.). */
static boolean_t is_name_token(parser_t *p) {
    token_kind_t k = p->current.kind;
    return k == TokIdent || k == TokFrom || k == TokNew || k == TokRem
        || k == TokLen || k == TokCap || k == TokMake || k == TokAppend || k == TokCopy
        || k == TokHash || k == TokEqu;
}

/* accept an identifier or a contextual keyword used as a name */
static token_t consume_name(parser_t *p, const char *msg) {
    if (is_name_token(p)) {
        advance_parser(p);
        return p->previous;
    }
    return consume(p, TokIdent, msg);
}

/* ── error recovery ── */

/* Synchronize to the next top-level declaration boundary.
 * Clears panic_mode so the parser can report the next real error. */
static void synchronize(parser_t *p) {
    p->panic_mode = False;
    while (!check(p, TokEof)) {
        /* A semicolon ends the previous construct — next token starts fresh. */
        if (p->previous.kind == TokSemicolon) return;
        /* Tokens that typically begin a new declaration or block. */
        switch (p->current.kind) {
            case TokFn: case TokType: case TokLib: case TokImp:
            case TokExt: case TokInt: case TokMod: case TokTest:
                return;
            default:
                advance_parser(p);
        }
    }
}

/* Skip to the next statement/field boundary, respecting brace nesting.
 * Stops after consuming a ';' at depth 0, or when hitting '}' at depth 0. */
static void skip_to_recovery_point(parser_t *p) {
    p->panic_mode = False;
    int depth = 0;
    while (!check(p, TokEof)) {
        if (check(p, TokLBrace)) { depth++; advance_parser(p); continue; }
        if (check(p, TokRBrace)) {
            if (depth == 0) return;   /* don't consume — caller owns the '}' */
            depth--;
            advance_parser(p);
            continue;
        }
        if (depth == 0 && check(p, TokSemicolon)) {
            advance_parser(p);        /* consume the ';' */
            return;
        }
        advance_parser(p);
    }
}

static boolean_t can_start_var_decl(parser_t *p) {
    /* nullable pointer type: ?T *name */
    if (check(p, TokQuestion)) return True;
    /* storage qualifiers prefix pointer declarations */
    if (check(p, TokStack) || check(p, TokHeap)) return True;
    /* 'let' alone starts an inferred-type var decl */
    if (check(p, TokLet)) return True;
    /* flag qualifiers: const, atomic, etc. always start a decl */
    if (check(p, TokAtomic) || check(p, TokConst) || check(p, TokFinal)
            || check(p, TokVolatile) || check(p, TokTls)) return True;
    /* built-in type tokens always start a decl */
    if (is_builtin_type_token(p->current.kind)) return True;
    /* user-defined type: look one or two tokens ahead.
       "Ident Ident", "Ident *", or "Ident . [" (generic) signals a var decl. */
    if (check(p, TokIdent)) {
        parser_state_t snap = save_state(p);
        advance_parser(p);
        boolean_t result = check(p, TokIdent) || check(p, TokStar);
        /* generic instantiation: TypeName.[T1,T2,...] VarName
           Only a var-decl if ']' is followed by an identifier (the var name),
           NOT if followed by '(' which would be a function call. */
        if (!result && check(p, TokDot)) {
            advance_parser(p);
            if (check(p, TokLBracket)) {
                advance_parser(p); /* consume '[' */
                /* scan past the type arguments to find ']' */
                int depth = 1;
                while (!check(p, TokEof) && depth > 0) {
                    if (check(p, TokLBracket)) depth++;
                    else if (check(p, TokRBracket)) depth--;
                    if (depth > 0) advance_parser(p);
                }
                if (check(p, TokRBracket)) advance_parser(p); /* consume ']' */
                /* var decl only if followed by an identifier, not '(' */
                result = check(p, TokIdent);
            }
        }
        restore_state(p, snap);
        return result;
    }
    return False;
}

/*
 * Parse a dot-separated module name: ident ('.' ident)*
 * Returns a heap-allocated string like "printer.typewriter".
 */
static char *parse_dotted_name(parser_t *p) {
    char buf[512];
    usize_t pos = 0;

    token_t first = consume(p, TokIdent, "module name");
    usize_t flen = first.length < sizeof(buf) - 1 ? first.length : sizeof(buf) - 1;
    memcpy(buf, first.start, flen);
    pos = flen;
    buf[pos] = '\0';

    while (check(p, TokDot)) {
        parser_state_t saved = save_state(p);
        advance_parser(p); /* consume '.' */
        if (!check(p, TokIdent)) {
            restore_state(p, saved);
            break;
        }
        token_t seg = p->current;
        advance_parser(p);
        usize_t slen = seg.length;
        if (pos + 1 + slen < sizeof(buf) - 1) {
            buf[pos++] = '.';
            memcpy(buf + pos, seg.start, slen);
            pos += slen;
            buf[pos] = '\0';
        }
    }

    return ast_strdup(buf, pos);
}

#include "parse_expr.c"
#include "parse_stmt.c"
#include "parse_decls.c"

/* ── internal parse body (shared by both entry points) ── */

static node_t *run_parse(parser_t *p) {
    node_t *module = make_node(NodeModule, 1);
    node_list_init(&module->as.module.decls);
    module->as.module.name = Null;
    module->as.module.freestanding = False;
    module->as.module.has_org = False;
    p->current_module = module;

    /* Accept any leading file-wide `@[[...]];` groups before `mod`. */
    while (check(p, TokAt)) {
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

        if (!match_tok(p, TokSemicolon)) {
            /* Not file-wide — rewind so parse_top_decl handles it. */
            restore_state(p, snap);
            break;
        }

        if (module->headers == Null) module->headers = fileheader_alloc();
        fileheader_merge(module->headers, &group);
        apply_module_headers(module, &group);
    }

    /* `mod name;` is mandatory unless the module is freestanding. */
    if (check(p, TokMod)) {
        advance_parser(p);
        module->as.module.name = parse_dotted_name(p);
        consume(p, TokSemicolon, "';'");
    } else if (!module->as.module.freestanding) {
        /* Emit the same diagnostic as before. */
        consume(p, TokMod, "'mod'");
    } else {
        /* Freestanding modules default to an empty name — codegen skips
           anything that needs a module identifier. */
        module->as.module.name = ast_strdup("", 0);
    }

    while (!check(p, TokEof)) {
        node_t *decl = parse_top_decl(p);
        if (decl) {
            node_list_push(&module->as.module.decls, decl);
        } else {
            /* Recovery: skip to the next top-level declaration boundary. */
            synchronize(p);
        }
    }

    /* Return partial AST even on error so codegen can report additional
     * diagnostics.  Callers check get_error_count() for the final verdict. */
    if (p->had_error) return Null;
    return module;
}

/* ── entry point: legacy (raw source string) ── */

node_t *parse(const char *source) {
    /* Register source text so diagnostics can print code snippets. */
    diag_set_source(source);

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.stream    = NULL;  /* lexer mode */
    p.had_error = False;
    init_lexer(&p.lexer, source);
    advance_parser(&p);

    return run_parse(&p);
}

/* ── entry point: stream-based (preferred) ── */

node_t *parse_from_stream(const pp_stream_t *stream) {
    if (!stream || stream->count == 0) return Null;

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.stream       = stream->tokens;
    p.stream_count = stream->count;
    p.stream_idx   = 0;
    p.had_error    = False;
    advance_parser(&p);

    return run_parse(&p);
}
