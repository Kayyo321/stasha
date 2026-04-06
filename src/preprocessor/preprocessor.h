#ifndef PreprocessorH
#define PreprocessorH

/*
 * preprocess — expand @comptime macros before parsing.
 *
 * Supported constructs:
 *   @comptime fn name! { ([@param, ...]) => { body }; }
 *   @comptime let name! = <tokens>;
 *
 * Invocations:
 *   name.!(arg1, arg2)   — expands fn macro body with parameter substitution
 *   name.!               — expands let alias (token replacement)
 *
 * Returns a new heap-allocated (malloc) source string with all @comptime
 * definitions removed and invocations expanded inline.
 * The caller must free() the returned pointer.
 * Returns NULL on allocation failure.
 */
char *preprocess(const char *source);

#endif
