#ifndef CHeaderH
#define CHeaderH

#include "../common/common.h"

typedef enum {
    CTypeUnsupported = 0,
    CTypeVoid,
    CTypeBool,
    CTypeChar,
    CTypeSChar,
    CTypeUChar,
    CTypeShort,
    CTypeUShort,
    CTypeInt,
    CTypeUInt,
    CTypeLong,
    CTypeULong,
    CTypeLongLong,
    CTypeULongLong,
    CTypeFloat,
    CTypeDouble,
    CTypeLongDouble,
    CTypeStructRef,
    CTypeUnionRef,
    CTypeTypedefRef,
    CTypeEnumRef,
    CTypePointer,
    CTypeArray,
} c_type_kind_t;

typedef struct c_type c_type_t;

struct c_type {
    c_type_kind_t kind;
    boolean_t is_const;
    char *name;
    long array_len;
    c_type_t *elem;
};

typedef struct {
    char *name;
    c_type_t type;
    long array_len;
    boolean_t is_bitfield;
    long bit_width;
} c_field_t;

typedef struct {
    char *name;
    c_type_t type;
} c_param_t;

typedef struct {
    char *name;
    c_type_t ret;
    c_param_t *params;
    usize_t param_count;
    boolean_t is_variadic;
    boolean_t from_user_header;
    heap_t params_heap; /* internal */
} c_fn_t;

typedef struct {
    char *name;
    c_field_t *fields;
    usize_t field_count;
    boolean_t is_union;
    boolean_t from_user_header; /* true iff defined in the cheader path
                                   the user wrote, vs. transitively
                                   pulled in via #include */
    heap_t fields_heap; /* internal */
} c_struct_t;

typedef struct {
    char *name;
    c_type_t actual;
    boolean_t from_user_header;
} c_typedef_t;

typedef struct {
    char *name;
    long value;
} c_enum_variant_t;

typedef struct {
    char *name;
    c_enum_variant_t *variants;
    usize_t variant_count;
    heap_t variants_heap; /* internal */
} c_enum_t;

typedef struct {
    char *name;
    long value;
} c_const_t;

typedef struct {
    char *name;
    c_type_t type;
    long array_len;
    boolean_t from_user_header;
} c_global_t;

typedef struct {
    c_fn_t      *fns;
    usize_t      fn_count;
    c_struct_t  *structs;
    usize_t      struct_count;
    c_typedef_t *tdefs;
    usize_t      tdef_count;
    c_enum_t    *enums;
    usize_t      enum_count;
    c_const_t   *consts;
    usize_t      const_count;
    c_global_t  *globals;
    usize_t      global_count;
    heap_t       fns_heap;     /* internal */
    heap_t       structs_heap; /* internal */
    heap_t       tdefs_heap;   /* internal */
    heap_t       enums_heap;   /* internal */
    heap_t       consts_heap;  /* internal */
    heap_t       globals_heap; /* internal */
    heap_t      *owned_heaps;  /* internal */
    usize_t      owned_heap_count;
    usize_t      owned_heap_cap;
    heap_t       owned_heaps_heap;
} cheader_result_t;

static boolean_t parse_cheader_file(const char *header_path,
                                    const char *search_dirs,
                                    const char *input_path,
                                    cheader_result_t *out,
                                    char *resolved_path_out,
                                    usize_t resolved_path_cap);
static void free_cheader_result(cheader_result_t *result);

#endif
