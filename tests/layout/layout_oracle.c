/* Independent GCC/Clang C representation oracle against LLVM DataLayout and
 * the exact production union-storage primitive. No aggregate crosses an FFI. */
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen/cg_layout_storage.c"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static LLVMContextRef ctx;
static LLVMTargetDataRef data;
static void check_layout(LLVMTypeRef type, size_t size, size_t align) {
    CHECK(LLVMTypeIsSized(type));
    CHECK(LLVMABISizeOfType(data, type) == size);
    CHECK(LLVMABIAlignmentOfType(data, type) == align);
    LLVMTypeRef arr = LLVMArrayType2(type, 3);
    CHECK(LLVMABISizeOfType(data, arr) == size * 3);
}
#define PAIR(A,AT,B,BT) do { \
    struct S { A first; B second; }; \
    struct __attribute__((packed)) P { A first; B second; }; \
    LLVMTypeRef f[] = {AT, BT}; \
    LLVMTypeRef s = LLVMStructTypeInContext(ctx, f, 2, 0); \
    LLVMTypeRef p = LLVMStructTypeInContext(ctx, f, 2, 1); \
    check_layout(s, sizeof(struct S), _Alignof(struct S)); \
    check_layout(p, sizeof(struct P), _Alignof(struct P)); \
    CHECK(LLVMOffsetOfElement(data, s, 1) == offsetof(struct S, second)); \
    CHECK(LLVMOffsetOfElement(data, p, 1) == offsetof(struct P, second)); \
    union U { A first; B second; }; \
    LLVMTypeRef u = sts_layout_storage(ctx, data, f, 2, 0); \
    check_layout(u, sizeof(union U), _Alignof(union U)); \
} while (0)
#define ROW(A,AT) do { \
    PAIR(A,AT,uint8_t,i8); PAIR(A,AT,uint16_t,i16); \
    PAIR(A,AT,uint32_t,i32); PAIR(A,AT,uint64_t,i64); \
    PAIR(A,AT,float,f32); PAIR(A,AT,double,f64); PAIR(A,AT,void *,ptr); \
} while (0)
int main(void) {
    CHECK(!LLVMInitializeNativeTarget());
    ctx = LLVMContextCreate();
    char *triple = LLVMGetDefaultTargetTriple(), *error = NULL;
    LLVMTargetRef target;
    CHECK(!LLVMGetTargetFromTriple(triple, &target, &error));
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "generic", "",
        LLVMCodeGenLevelNone, LLVMRelocDefault, LLVMCodeModelDefault);
    CHECK(tm);
    data = LLVMCreateTargetDataLayout(tm);
    LLVMTypeRef i8=LLVMInt8TypeInContext(ctx), i16=LLVMInt16TypeInContext(ctx),
        i32=LLVMInt32TypeInContext(ctx), i64=LLVMInt64TypeInContext(ctx),
        f32=LLVMFloatTypeInContext(ctx), f64=LLVMDoubleTypeInContext(ctx),
        ptr=LLVMPointerTypeInContext(ctx,0);
    ROW(uint8_t,i8); ROW(uint16_t,i16); ROW(uint32_t,i32); ROW(uint64_t,i64);
    ROW(float,f32); ROW(double,f64); ROW(void *,ptr);
    struct Large { uint8_t bytes[65]; uint64_t last; };
    LLVMTypeRef lf[] = {LLVMArrayType2(i8,65), i64};
    LLVMTypeRef large=LLVMStructTypeInContext(ctx,lf,2,0);
    check_layout(large,sizeof(struct Large),_Alignof(struct Large));
    union U { struct Large large; uint64_t words[12]; uint8_t bytes[99]; };
    LLVMTypeRef uf[] = {large,LLVMArrayType2(i64,12),LLVMArrayType2(i8,99)};
    LLVMTypeRef u=sts_layout_storage(ctx,data,uf,3,0);
    check_layout(u,sizeof(union U),_Alignof(union U));
    union __attribute__((packed)) PU { struct Large large; uint64_t words[12]; uint8_t bytes[99]; };
    check_layout(sts_layout_storage(ctx,data,uf,3,1),sizeof(union PU),_Alignof(union PU));
    struct Tagged { int32_t tag; union U payload; };
    LLVMTypeRef tf[]={i32,u};
    LLVMTypeRef tagged=LLVMStructTypeInContext(ctx,tf,2,0);
    check_layout(tagged,sizeof(struct Tagged),_Alignof(struct Tagged));
    CHECK(LLVMOffsetOfElement(data,tagged,1)==offsetof(struct Tagged,payload));
    struct Nested { uint8_t prefix; union U items[2][3]; uint8_t suffix; };
    LLVMTypeRef nf[]={i8,LLVMArrayType2(LLVMArrayType2(u,3),2),i8};
    LLVMTypeRef nested=LLVMStructTypeInContext(ctx,nf,3,0);
    check_layout(nested,sizeof(struct Nested),_Alignof(struct Nested));
    CHECK(LLVMOffsetOfElement(data,nested,1)==offsetof(struct Nested,items));
    CHECK(LLVMOffsetOfElement(data,nested,2)==offsetof(struct Nested,suffix));
    /* The C oracle proves over-aligned array stride requirements. Stasha
       explicitly rejects these until all allocations/ABI paths honor them. */
#define OVERALIGN(N) do { \
    struct __attribute__((aligned(N))) O { uint8_t byte; }; \
    CHECK(_Alignof(struct O)==N); CHECK(sizeof(struct O)==N); \
    CHECK(sizeof(struct O[3])==3*N); \
} while (0)
    OVERALIGN(16); OVERALIGN(32); OVERALIGN(64);
    printf("layout oracle: %u checks passed; target=%s\n",checks,triple);
    LLVMDisposeTargetData(data); LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple); LLVMContextDispose(ctx);
    return 0;
}
