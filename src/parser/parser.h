#ifndef ParserH
#define ParserH

#include "../ast/ast.h"
#include "../preprocessor/preprocessor.h"

/*
 * parse — legacy entry point: lex + parse from raw source text.
 * Source location tracking is limited to a single file.
 * Kept for callers that do not use the preprocessor.
 */
node_t *parse(const char *source);

/*
 * parse_from_stream — preferred entry point.
 * Reads tokens from a pp_stream_t produced by pp_process().
 * Every token already carries correct file/line/col (even across macro
 * expansions and imported files), so all parser error messages point to
 * the right original location.
 */
node_t *parse_from_stream(const pp_stream_t *stream);

#endif
