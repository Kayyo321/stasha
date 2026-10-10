/* Direct production contract tests. Synthetic invalid declarations never run. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n", __LINE__, #x); return 1; } checks++; } while (0)
static unsigned checks;
static node_t *source(const char *text) {
    restore_diagnostic_counts(0,0); diag_clear_captured(); diag_set_render_enabled(False);
    diag_register_source("binding-unit.sts",text); diag_set_file("binding-unit.sts"); diag_set_source(text);
    return parse(text);
}
static void init(cg_t *cg, node_t *ast) {
    memset(cg,0,sizeof(*cg)); cg->ast=cg->root_ast=ast;
    sts_target_init(&cg->target_spec,"x86_64-unknown-linux-gnu");
    validate_interface_declarations(cg);
}
static void cleanup(cg_t *cg) {
    iface_free_contracts(cg); free_interface_registry(cg);
    if (cg->vtables_heap.pointer) deallocate(cg->vtables_heap);
    if (cg->module) LLVMDisposeModule(cg->module);
    if (cg->ctx) LLVMContextDispose(cg->ctx);
    ast_free_all();
}
int main(void) {
    cg_t cg;
    const char *plain="mod binding; type I: interface { m(stack i32 *r *rw p): i32; };";
    node_t *ast=source(plain); CHECK(ast && !get_error_count()); init(&cg,ast); CHECK(!get_error_count());
    iface_preflight(&cg); CHECK(!get_error_count());
    iface_signature_t *signature=cg.interfaces[0].groups[0].signature;
    CHECK(signature && signature->valid && signature->count==1);
    CHECK(signature->params[0].type.ptr_depth==2);
    CHECK(signature->params[0].type.ptr_perms[0]==PtrReadWrite && signature->params[0].type.ptr_perms[1]==PtrRead);
    iface_type_t copy=signature->params[0]; CHECK(iface_types_equal(&copy,&signature->params[0]));
    copy.type.ptr_perms[1]|=PtrArith; CHECK(!iface_types_equal(&copy,&signature->params[0]));
    copy=signature->params[0];copy.type.nullable=True;CHECK(!iface_types_equal(&copy,&signature->params[0]));
    copy=signature->params[0];copy.domain=StorageHeap;CHECK(!iface_types_equal(&copy,&signature->params[0]));
    cleanup(&cg);
    for (unsigned bit=0;bit<10;bit++) {
        if ((1u<<bit)==VdeclConst || (1u<<bit)==VdeclFinal) continue;
        ast=source(plain);CHECK(ast&&!get_error_count());init(&cg,ast);CHECK(!get_error_count());
        cg.interfaces[0].groups[0].origins[0]->as.fn_decl.params.items[0]->as.var_decl.flags=(1u<<bit);
        iface_preflight(&cg);CHECK(get_error_count()>0);CHECK(!cg.interfaces[0].groups[0].signature->valid);cleanup(&cg);
    }
    /* Bound/cycle invalid metadata returns invalid, never equality. */
    ast=source(plain);init(&cg,ast);CHECK(!get_error_count());
    node_t *seen[IfaceMaxMetadataDepth];iface_type_t result;
    type_info_t type={.base=TypeI32,.is_pointer=True,.ptr_depth=8,.ptr_perm=PtrRead,.source_loc=iface_node_loc(ast)};
    for(int i=0;i<8;i++)type.ptr_perms[i]=PtrRead;
    CHECK(iface_canonical_type(&cg,type,False,False,seen,0,&result));
    type.ptr_depth=9;CHECK(!iface_canonical_type(&cg,type,False,False,seen,0,&result));
    restore_diagnostic_counts(0,0);type.ptr_depth=8;
    CHECK(!iface_canonical_type(&cg,type,False,False,seen,IfaceMaxMetadataDepth+1,&result));cleanup(&cg);
    const char *binding="mod binding; type I: interface { a(stack i32 p): i32; b(void): i32; };"
        "type S: struct.[I] { ext fn a(stack i32 p): i32 { ret p; } ext fn b(void): i32 { ret 1; } };";
    /* Every failure mode is checked before publication, including a bad last slot. */
    for(unsigned mode=0;mode<8;mode++) {
        ast=source(binding);CHECK(ast&&!get_error_count());init(&cg,ast);iface_preflight(&cg);CHECK(!get_error_count());
        iface_binding_t *b=cg.iface_bindings;CHECK(b&&b->state==IfaceBindingSourceReady);
        cg.ctx=LLVMContextCreate();cg.module=LLVMModuleCreateWithNameInContext("binding",cg.ctx);
        for(usize_t i=0;i<2;i++) {
            LLVMTypeRef ft=iface_native_type(&cg,cg.interfaces[0].groups[i].signature);
            if(mode==1 && i==1) { LLVMTypeRef ptr=LLVMPointerTypeInContext(cg.ctx,0);ft=LLVMFunctionType(LLVMInt64TypeInContext(cg.ctx),&ptr,1,0); }
            LLVMValueRef fn=LLVMAddFunction(cg.module,i?"selected_b":"selected_a",ft);
            if(mode==2 && i==1)LLVMSetFunctionCallConv(fn,LLVMFastCallConv);
            if(mode==3 && i==1) { unsigned kind=LLVMGetEnumAttributeKindForName("noalias",7);LLVMAddAttributeAtIndex(fn,1,LLVMCreateEnumAttribute(cg.ctx,kind,0)); }
            if(mode==6 && i==1) { unsigned kind=LLVMGetEnumAttributeKindForName("byref",5);LLVMAddAttributeAtIndex(fn,1,LLVMCreateTypeAttribute(cg.ctx,kind,LLVMInt32TypeInContext(cg.ctx))); }
            if(mode==7 && i==1) { unsigned kind=LLVMGetEnumAttributeKindForName("swiftasync",10);LLVMAddAttributeAtIndex(fn,1,LLVMCreateEnumAttribute(cg.ctx,kind,0)); }
            if(mode==4 && i==1)continue;
            iface_register_native(&cg,b->methods[i]->decl,mode==5&&i==1?Null:b->owner,fn,True,False);
        }
        iface_finish_bindings(&cg);
        if(!mode) { CHECK(!get_error_count());CHECK(cg.vtable_count==1);CHECK(b->state==IfaceBindingTableReady);CHECK(LLVMGetFirstGlobal(cg.module)); }
        else { CHECK(get_error_count()>0);CHECK(!cg.vtable_count);CHECK(b->state==IfaceBindingInvalid);CHECK(!LLVMGetFirstGlobal(cg.module)); }
        cleanup(&cg);
    }
    /* Context-local caches cannot retain a previous declaration or result. */
    for(unsigned repeat=0;repeat<4;repeat++) {
        ast=source(repeat%2?"mod x; type I: interface { m(void): u64; };":"mod x; type I: interface { m(void): i8; };");
        init(&cg,ast);iface_preflight(&cg);CHECK(!get_error_count());CHECK(cg.interfaces[0].groups[0].signature->result.type.base==(repeat%2?TypeU64:TypeI8));cleanup(&cg);
    }
    printf("PASS: %u canonical/native atomic-publication checks\n",checks);return 0;
}
