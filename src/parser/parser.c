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
        || k == TokBool || k == TokVoid || k == TokErrorType;
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
        default:      return TypeVoid;
    }
}

static type_info_t parse_type(parser_t *p) {
    type_info_t info = NO_TYPE;

    /* function pointer type: fn*(params): ret_type */
    if (check(p, TokFn)) {
        usize_t line = p->current.line;
        advance_parser(p); /* consume 'fn' */

        if (!check(p, TokStar)) {
            log_err("line %lu: expected '*' after 'fn' in function pointer type", line);
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
                    log_err("line %lu: function pointer type exceeds maximum of 32 parameters", line);
                    p->had_error = True;
                    break;
                }
                storage_t ps = StorageStack; /* default */
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

    /* pointer: * or *r or *w or *rw, optionally followed by + for arith */
    if (check(p, TokStar)) {
        advance_parser(p);
        info.is_pointer = True;
        info.ptr_perm = PtrRead | PtrWrite | PtrArith; /* bare * = rw+ */

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

        /* optional + suffix grants pointer arithmetic */
        if (check(p, TokPlus)) {
            info.ptr_perm |= PtrArith;
            advance_parser(p);
        }
    }

    return info;
}

/* ── forward declarations ── */

static node_t *parse_expr(parser_t *p);
static node_t *parse_postfix(parser_t *p);
static node_t *parse_block(parser_t *p);
static node_t *parse_statement(parser_t *p);
static node_t *parse_match_stmt(parser_t *p);
static node_t *parse_var_decl(parser_t *p, linkage_t linkage);
static node_t *parse_defer_stmt(parser_t *p);
static node_t *parse_storage_group(parser_t *p, storage_t storage);
static node_t *parse_switch_stmt(parser_t *p);
static node_t *parse_asm_stmt(parser_t *p);
static node_t *parse_comptime_if(parser_t *p);
static node_t *parse_comptime_assert(parser_t *p);

static boolean_t can_start_type(parser_t *p) {
    return is_builtin_type_token(p->current.kind) || check(p, TokIdent) || check(p, TokFn);
}

/* In a parameter list, a bare identifier is only a type if followed by
   another identifier or '*' (i.e. "Type name" or "Type * name").
   If followed by ',' or ')', it must be a grouped parameter name. */
static boolean_t can_start_param_type(parser_t *p) {
    if (is_builtin_type_token(p->current.kind)) return True;
    if (check(p, TokFn)) return True; /* fn* function pointer type */
    if (!check(p, TokIdent)) return False;
    parser_state_t snap = save_state(p);
    advance_parser(p);
    boolean_t result = check(p, TokIdent) || check(p, TokStar);
    restore_state(p, snap);
    return result;
}

/* accept an identifier or a contextual keyword used as a name (from, new, rem) */
static token_t consume_name(parser_t *p, const char *msg) {
    if (check(p, TokIdent) || check(p, TokFrom) || check(p, TokNew) || check(p, TokRem)) {
        advance_parser(p);
        return p->previous;
    }
    return consume(p, TokIdent, msg);
}

static boolean_t can_start_var_decl(parser_t *p) {
    return check(p, TokStack) || check(p, TokHeap) || check(p, TokAtomic)
        || check(p, TokConst)  || check(p, TokFinal)
        || check(p, TokVolatile) || check(p, TokTls)
        || is_builtin_type_token(p->current.kind);
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

/* ── entry point ── */

node_t *parse(const char *source) {
    parser_t p;
    init_lexer(&p.lexer, source);
    p.had_error = False;
    p.group_storage = StorageDefault;
    advance_parser(&p);

    consume(&p, TokMod, "'mod'");
    char *mod_name = parse_dotted_name(&p);
    consume(&p, TokSemicolon, "';'");

    node_t *module = make_node(NodeModule, 1);
    module->as.module.name = mod_name;
    node_list_init(&module->as.module.decls);

    while (!check(&p, TokEof)) {
        node_list_push(&module->as.module.decls, parse_top_decl(&p));
    }

    if (p.had_error) return Null;
    return module;
}
