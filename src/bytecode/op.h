#ifndef OpH
#define OpH

#include <stdint.h>

typedef enum : uint8_t {
    OpNoop,
    OpJmp,
    OpAdd,
    //Etc
} op_code_t;

#endif