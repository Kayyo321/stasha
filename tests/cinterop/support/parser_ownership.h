/* Duplicate declarations and discarded anonymous/nested declarators must
 * release arrays even when they never become accepted result entries. */
int parser_duplicate(int a, int b, int c);
int parser_duplicate(int a, int b, int c);
struct ParserRecord { int a; int b; };
struct ParserRecord { int a; int b; };
enum ParserEnum { ParserA, ParserB };
enum ParserEnum { ParserA, ParserB };
typedef struct { int a; int b; } *ParserAnonymousHandle;
