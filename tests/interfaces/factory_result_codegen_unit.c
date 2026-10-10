/* Repeated complete compiler invocations under ASan/UBSan. All fixtures emit
 * only IR; invalid/deferred source is never linked or executed. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#include <unistd.h>
#ifndef FACTORY_UNIT_EXPECT_PROMOTION
#define FACTORY_UNIT_EXPECT_PROMOTION 0
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}++checks;}while(0)
static unsigned checks;
static const char *prefix="mod repeatfactory; type I: interface {m(void): i32;}; type S: struct.[I] {ext i32 n;ext fn m(void): i32 {ret this.n;}}; type T: struct {ext i32 n;}; type H: struct {ext S value;ext i32 flag;}; fn consume(stack I i): i32 {ret i.m();} ";
int main(int argc,char **argv) {
    CHECK(argc==2&&IFACE_FACTORY_RESULT_PROMOTION==FACTORY_UNIT_EXPECT_PROMOTION);
    const char *bodies[]={
        "int fn make(void): H {stack S s=.{.n=17};stack H h=.{.value=s,.flag=5};ret h;} fn main(void): i32 {stack H h=make();ret h.flag;}",
        "int fn make(void): H {stack S s=.{.n=17};stack H h=.{.value=s,.flag=5};ret h;} fn main(void): i32 {stack H h=make();stack let s=h.value;ret consume(s);}",
        "int fn make(void): H {stack T t=.{.n=29};stack S s=t;stack H h=.{.value=s,.flag=5};ret h;} fn main(void): i32 {stack H h=make();ret 0;stack let s=h.value;ret consume(s);}",
        "int fn make(void): H {stack T t=.{.n=29};stack S s=t;stack H h=.{.value=s,.flag=5};ret h;} fn main(void): i32 {stack H h=make();stack let s=h.value;ret consume(s);}",
        "int fn make(void): H {ret;} fn main(void): i32 {ret 0;stack H h=make();stack let s=h.value;ret consume(s);}",
        "fn main(void): i32 {ret 0;}"
    };
    const boolean_t accepted[]={True,FACTORY_UNIT_EXPECT_PROMOTION,True,False,True,True};
    char source[32768],artifact[4096],pre[4096];snprintf(artifact,sizeof(artifact),"%s",argv[1]);snprintf(pre,sizeof(pre),"%s.pre.ll",argv[1]);
    for(unsigned round=0;round<4;round++)for(unsigned j=0;j<sizeof(bodies)/sizeof(*bodies);j++){
        unsigned i=round&1?(unsigned)(sizeof(bodies)/sizeof(*bodies)-1-j):j;
        snprintf(source,sizeof(source),"%s%s",prefix,bodies[i]);
        if(i==5){size_t len=strlen(source);for(unsigned k=0;k<FactoryMaxPlans+1;k++)len+=(size_t)snprintf(source+len,sizeof(source)-len,"fn unused%u(void): i32 {ret %u;} ",k,k);}
        restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);
        diag_register_source("factory-repeat.sts",source);diag_set_file("factory-repeat.sts");diag_set_source(source);
        node_t *ast=parse(source);CHECK(ast&&!get_error_count());remove(artifact);remove(pre);CHECK(!setenv("STS_DUMP_IR_PRE",pre,1));
        result_t result=codegen(ast,artifact,False,"x86_64-unknown-linux-gnu","factory-repeat.sts",False,round,True);
        CHECK((result==Ok)==accepted[i]);CHECK((get_error_count()==0)==accepted[i]);
        CHECK((access(artifact,F_OK)==0)==accepted[i]);CHECK((access(pre,F_OK)==0)==accepted[i]);
        ast_free_all();
    }
    unsetenv("STS_DUMP_IR_PRE");remove(artifact);remove(pre);
    printf("PASS: %u sanitizer checks, 24 repeated full-codegen calls, zero fixture executions\n",checks);return 0;
}
