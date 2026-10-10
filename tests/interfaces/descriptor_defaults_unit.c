/* Production default emitter on fully initialized sentinel buffers. No dynamic
 * dispatch, invalid-pointer dereference or uninitialized source execution. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#include <unistd.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;} ++checks; } while(0)
static unsigned checks;
static LLVMTypeRef ordinary(cg_t *cg,const char *name,LLVMTypeRef *types,int *flags,unsigned count,int packed,int comptime) {
    LLVMTypeRef type=LLVMStructCreateNamed(cg->ctx,name);LLVMStructSetBody(type,types,count,packed);
    register_struct(cg,name,type,False);
    node_t *decl=make_node(NodeTypeDecl,1);decl->as.type_decl.name=(char *)name;decl->as.type_decl.decl_kind=TypeDeclStruct;
    struct_reg_t *sr=find_struct(cg,name);sr->layout_decl=decl;sr->layout_state=2;sr->is_packed=packed;
    if(comptime){node_t *f=make_node(NodeVarDecl,1);f->as.var_decl.name="metadata";f->as.var_decl.flags=VdeclComptimeField;node_list_push(&decl->as.type_decl.fields,f);}
    for(unsigned i=0;i<count;i++){
        char namebuf[32];snprintf(namebuf,sizeof(namebuf),"field%u",i);
        node_t *f=make_node(NodeVarDecl,1);f->as.var_decl.name=ast_strdup(namebuf,strlen(namebuf));f->as.var_decl.flags=flags?flags[i]:0;
        node_list_push(&decl->as.type_decl.fields,f);
        struct_add_field(sr,f->as.var_decl.name,NO_TYPE,i,LinkageNone,StorageDefault);sr->fields[i].llvm_type=types[i];
    }
    return type;
}
static int emit_sentinel(cg_t *cg,LLVMTypeRef type,int root_flags,const uint64_t *offsets,unsigned offset_count) {
    uint64_t size=LLVMABISizeOfType(cg->di_data_layout,type), total=size+32;
    if(total>8192)return 1;
    LLVMTypeRef byte=LLVMInt8TypeInContext(cg->ctx),index=LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef buffer=LLVMArrayType2(byte,total);LLVMValueRef storage=LLVMBuildAlloca(cg->builder,buffer,"sentinel.buffer");
    LLVMBuildMemSet(cg->builder,storage,LLVMConstInt(byte,0xa5,0),LLVMConstInt(index,total,0),1);
    LLVMValueRef prefix=LLVMConstInt(index,16,0);
    LLVMValueRef address=LLVMBuildGEP2(cg->builder,byte,storage,&prefix,1,"sentinel.object");
    node_t declaration={0};declaration.kind=NodeVarDecl;declaration.as.var_decl.flags=root_flags;
    iface_default_local(cg,&declaration,type,address);
    LLVMValueRef bad=LLVMConstInt(LLVMInt1TypeInContext(cg->ctx),0,0);
    for(uint64_t at=0;at<total;at++){
        unsigned expected=0xa5;
        for(unsigned j=0;j<offset_count;j++)if(at>=16+offsets[j]&&at<16+offsets[j]+16)expected=0;
        LLVMValueRef offset=LLVMConstInt(index,at,0),p=LLVMBuildGEP2(cg->builder,byte,storage,&offset,1,"sentinel.byte");
        LLVMValueRef got=LLVMBuildLoad2(cg->builder,byte,p,"sentinel.read");LLVMSetAlignment(got,1);
        LLVMValueRef neq=LLVMBuildICmp(cg->builder,LLVMIntNE,got,LLVMConstInt(byte,expected,0),"sentinel.changed");
        bad=LLVMBuildOr(cg->builder,bad,neq,"sentinel.any");
    }
    scalar_trap_if(cg,bad,99);return 0;
}
int main(int argc,char **argv){
    CHECK(argc==2);cg_t cg={0};cg.ctx=LLVMContextCreate();cg.module=LLVMModuleCreateWithNameInContext("descriptor.sentinel",cg.ctx);cg.builder=LLVMCreateBuilderInContext(cg.ctx);
    cg.di_data_layout=LLVMCreateTargetData("e-p:64:64-i64:64-n8:16:32:64-S128");LLVMSetDataLayout(cg.module,"e-p:64:64-i64:64-n8:16:32:64-S128");LLVMSetTarget(cg.module,"x86_64-unknown-linux-gnu");
    LLVMTypeRef ptr=LLVMPointerTypeInContext(cg.ctx,0),words[2]={ptr,ptr};LLVMTypeRef iface=LLVMStructCreateNamed(cg.ctx,"I");LLVMStructSetBody(iface,words,2,0);
    iface_reg_t interface={0};interface.fat_ptr_type=iface;cg.interfaces=&interface;cg.iface_count=1;
    LLVMTypeRef byte=LLVMInt8TypeInContext(cg.ctx),i32=LLVMInt32TypeInContext(cg.ctx),empty=LLVMStructTypeInContext(cg.ctx,NULL,0,0);
    LLVMTypeRef mixed_fields[]={byte,iface,ptr,empty,i32};LLVMTypeRef mixed=ordinary(&cg,"Mixed",mixed_fields,NULL,5,0,1);
    LLVMTypeRef packed=ordinary(&cg,"Packed",mixed_fields,NULL,5,1,0);
    LLVMTypeRef effect_fields[]={iface,iface,iface,iface};int effects[]={VdeclVolatile,0,VdeclAtomic,VdeclTls};LLVMTypeRef effect=ordinary(&cg,"Effects",effect_fields,effects,4,0,0);
    LLVMTypeRef union_type=LLVMStructCreateNamed(cg.ctx,"Union");LLVMStructSetBody(union_type,&iface,1,0);register_struct(&cg,"Union",union_type,True);
    LLVMTypeRef any_type=LLVMStructCreateNamed(cg.ctx,"Erased");LLVMStructSetBody(any_type,&iface,1,0);register_struct(&cg,"Erased",any_type,False);find_struct(&cg,"Erased")->is_any_type=True;
    LLVMTypeRef carriers_fields[]={union_type,any_type,iface};LLVMTypeRef carriers=ordinary(&cg,"Carriers",carriers_fields,NULL,3,0,0);
    LLVMTypeRef nested=LLVMArrayType2(LLVMArrayType2(packed,3),2),array=LLVMArrayType2(iface,4),zero=LLVMArrayType2(iface,0);
    LLVMTypeRef fn_type=LLVMFunctionType(i32,NULL,0,0);cg.current_fn=LLVMAddFunction(cg.module,"main",fn_type);LLVMPositionBuilderAtEnd(cg.builder,LLVMAppendBasicBlockInContext(cg.ctx,cg.current_fn,"entry"));
    uint64_t direct[]={0},mo[]={LLVMOffsetOfElement(cg.di_data_layout,mixed,1)},po[]={LLVMOffsetOfElement(cg.di_data_layout,packed,1)},eo[]={16},co[]={32},ao[]={0,16,32,48},no[6];
    for(unsigned j=0;j<6;j++)no[j]=j*LLVMABISizeOfType(cg.di_data_layout,packed)+po[0];
    CHECK(!emit_sentinel(&cg,iface,0,direct,1));CHECK(!emit_sentinel(&cg,mixed,0,mo,1));CHECK(!emit_sentinel(&cg,packed,0,po,1));CHECK(!emit_sentinel(&cg,effect,0,eo,1));CHECK(!emit_sentinel(&cg,carriers,0,co,1));CHECK(!emit_sentinel(&cg,nested,0,no,6));CHECK(!emit_sentinel(&cg,array,0,ao,4));CHECK(!emit_sentinel(&cg,zero,0,NULL,0));
    CHECK(!emit_sentinel(&cg,iface,VdeclAtomic,NULL,0));CHECK(!emit_sentinel(&cg,iface,VdeclVolatile,NULL,0));CHECK(!emit_sentinel(&cg,iface,VdeclTls,NULL,0));
    /* Repeat discovery/disposal, then force each fail-closed internal boundary.
     * None of these invalid metadata plans emits stores or runs generated code. */
    node_t local={0};local.kind=NodeVarDecl;
    for(unsigned i=0;i<100;i++){iface_default_plan_t p={.cg=&cg,.declaration=&local};iface_default_node_t *root=iface_default_discover(&p,nested,0);CHECK(root&&!p.failed&&root->kind==IfaceDefaultArray);iface_default_dispose(&p);CHECK(!p.nodes);}
    diag_set_render_enabled(False);restore_diagnostic_counts(0,0);
    struct_reg_t *sr=find_struct(&cg,"Mixed");sr->fields[1].index=0;iface_default_plan_t invalid={.cg=&cg,.declaration=&local};iface_default_discover(&invalid,mixed,0);CHECK(invalid.failed);iface_default_dispose(&invalid);sr->fields[1].index=1;restore_diagnostic_counts(0,0);
    iface_default_plan_t depth={.cg=&cg,.declaration=&local};iface_default_discover(&depth,iface,IfaceDefaultMaxDepth+1);CHECK(depth.failed);iface_default_dispose(&depth);restore_diagnostic_counts(0,0);
    iface_default_plan_t budget={.cg=&cg,.declaration=&local,.count=IfaceDefaultMaxNodes};iface_default_discover(&budget,iface,0);CHECK(budget.failed);iface_default_dispose(&budget);restore_diagnostic_counts(0,0);
    LLVMBuildRet(cg.builder,LLVMConstInt(i32,0,0));char *error=NULL;CHECK(!LLVMVerifyModule(cg.module,LLVMReturnStatusAction,&error));if(error)LLVMDisposeMessage(error);error=NULL;CHECK(!LLVMPrintModuleToFile(cg.module,argv[1],&error));if(error)LLVMDisposeMessage(error);
    for(usize_t i=0;i<cg.struct_count;i++)if(cg.structs[i].fields_heap.pointer)deallocate(cg.structs[i].fields_heap);if(cg.structs_heap.pointer)deallocate(cg.structs_heap);
    LLVMDisposeBuilder(cg.builder);LLVMDisposeModule(cg.module);LLVMDisposeTargetData(cg.di_data_layout);LLVMContextDispose(cg.ctx);ast_free_all();
    const char *sources[]={
        "mod repeated;type I:interface{m(void):i32;};fn check(void):i32{stack I value;ret value.m();}",
        "mod repeated;type I:interface{m(void):i32;};type H:struct{ext i32 other;ext I value;};fn check(void):i32{stack H h;ret h.value.m();}",
        "mod repeated;type I:interface{m(void):i32;};fn check(void):I{ret nil;}",
        "mod repeated;fn check(void):i32{stack i32 value;ret 0;}",
        "mod repeated;type I:interface{m(void):i32;};type H:@comptime[T] struct{ext T data;ext I value;};fn check(void):i32{stack H.[i32] h;stack I value=h.value;ret value.m();}"
    };
    char output[4096];snprintf(output,sizeof(output),"%s.repeated.ll",argv[1]);
    for(unsigned round=0;round<4;round++)for(unsigned j=0;j<5;j++){
        unsigned i=round&1?4-j:j;restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);
        diag_register_source("descriptor-repeat.sts",sources[i]);diag_set_file("descriptor-repeat.sts");diag_set_source(sources[i]);
        node_t *ast=parse(sources[i]);CHECK(ast&&!get_error_count());remove(output);
        result_t result=codegen(ast,output,False,"x86_64-unknown-linux-gnu","descriptor-repeat.sts",False,round,True);
        CHECK((result==Ok)==(i!=2));CHECK((get_error_count()==0)==(i!=2));CHECK((access(output,F_OK)==0)==(i!=2));ast_free_all();
    }
    const char *owned[]={
        "mod owned;type I:interface{m(void):i32;};fn bad(void):I{ret nil;}",
        "mod owned;type I:interface{m(void):i32;};fn @comptime[T] bad(stack T value):I{value;ret nil;}fn check(void):void{stack I _value=bad.[i32](1);}",
        "mod owned;type I:interface{m(void):i32;};fn check(void):void{stack let _function=lam.(void):I{ret nil;};}",
        "mod owned;type I:interface{m(void):i32;};test 'owned nil' {stack I _value=nil;}"
    };
    for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<4;i++){
        restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);
        char filename[64];snprintf(filename,sizeof(filename),"owned-provider-%u.sts",i);
        char stable[64];strcpy(stable,filename);diag_register_source(filename,owned[i]);diag_set_file(filename);
        pp_stream_t *stream=pp_process(owned[i],filename,NULL,0);CHECK(stream);node_t *ast=parse_from_stream(stream);CHECK(ast&&!get_error_count());pp_stream_free(stream);memset(filename,'x',strlen(filename));
        diag_register_source("primary-caller.sts","mod primary;\n");diag_set_file("primary-caller.sts");remove(output);
        result_t result=codegen(ast,output,i==3,"x86_64-unknown-linux-gnu","primary-caller.sts",False,pass?3:0,True);
        CHECK(result==Err&&get_error_count());CHECK(access(output,F_OK)!=0);CHECK(!strcmp(diag_get_file(),"primary-caller.sts"));
        unsigned found=0;for(usize_t j=0;j<diag_get_captured_count();j++){
            const captured_diag_t *d=diag_get_captured(j);if(!strstr(d->diag.message,"nil cannot initialize an interface descriptor"))continue;
            CHECK(!strcmp(d->filename,stable));CHECK(d->diag.label_count==1);CHECK(d->diag.labels[0].loc.line==1);CHECK(d->diag.labels[0].loc.col==(usize_t)(strstr(owned[i],"ret nil")?strstr(owned[i],"ret nil")+4-owned[i]+1:strstr(owned[i],"=nil")+1-owned[i]+1));CHECK(d->diag.labels[0].loc.len==3);++found;
        }CHECK(found==1);ast_free_all();diag_clear_sources();CHECK(!strcmp(diag_get_file(),"primary-caller.sts"));
    }
    remove(output);printf("PASS %u checks; 20 repeated compilations plus 8 owned diagnostic contexts; sentinel IR emitted; no descriptor dispatch executed\n",checks);return 0;
}
