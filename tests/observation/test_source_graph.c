#define _GNU_SOURCE
#include "../../src/common/observation_sources.h"
#include <assert.h>

/* Pure filesystem/encoding controls. No compiler or generated image launch. */
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);}while(0)
static char error[256];
static void put(const char *path,const void *data,size_t n) {
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(data,1,n,f)==n);CHECK(!fclose(f));
}
static void put_text(const char *path,const char *text){put(path,text,strlen(text));}
static void begin(sts_observation_graph *g,const char *root){CHECK(!sts_observation_graph_begin(g,root,error,sizeof(error)));CHECK(!sts_observation_graph_declare(g,0,"root",error,sizeof(error)));}
static uint32_t add(sts_observation_graph *g,const char *name){uint32_t id=UINT32_MAX;CHECK(!sts_observation_graph_add(g,name,1,1,&id,error,sizeof(error)));CHECK(!sts_observation_graph_declare(g,id,name,error,sizeof(error)));return id;}

static void encoding_vectors(void) {
    sts_observation_encoder e;unsigned char capture[128];
    sts_observation_encode_init(&e,128);e.capture=capture;e.capture_capacity=128;
    sts_observation_encode_u32(&e,0x01020304);sts_observation_encode_u64(&e,UINT64_C(0x05060708090a0b0c));
    sts_observation_encode_bytes(&e,"\xff\0",2);sts_observation_encode_optional_u32(&e,0,123);sts_observation_encode_optional_u32(&e,1,7);
    static const unsigned char expected[]={1,2,3,4,5,6,7,8,9,10,11,12,0,0,0,0,0,0,0,2,255,0,0,0,0,0,0,0,0,1,0,0,0,7};
    CHECK(!e.failed&&e.size==sizeof(expected)&&!memcmp(capture,expected,sizeof(expected)));
    sts_observation_encode_init(&e,3);sts_observation_encode_u32(&e,1);CHECK(e.failed&&e.size==0);
    sts_observation_encode_init(&e,SIZE_MAX);e.size=SIZE_MAX;sts_observation_encode_u32(&e,1);CHECK(e.failed);
    sts_observation_encode_init(&e,64);sts_observation_encode_digest(&e,"ABC");CHECK(e.failed);

    /* Independent review vector, frozen literal bytes/identities. */
    sts_observation_graph g={.invocation_directory="/invoke",.original_root_spelling="entry/root.sts",.file_count=2,.edge_count=1};
    g.files[0]=(sts_observation_source){.file_id=0,.role=0,.lookup_path="/invoke/entry/root.sts",.canonical_path="/target/root.sts",.declared_module="root",.byte_length=189,.physical_line_count=11,.content_id="d96b7a0bcd97e1e90a2420e768d5348fa4b063cde8e35be3a5603f0bbc2db61d"};
    g.files[1]=(sts_observation_source){.file_id=1,.role=1,.lookup_path="/invoke/entry/raw\xff.sts",.canonical_path="/target/raw\xff.sts",.requested_module="raw",.declared_module="raw",.byte_length=9,.physical_line_count=2,.content_id="22bc4a57004bf5e6f2139832a8c037573189d82916d5f69ca90ad2ff7011dd28"};
    g.edges[0]=(sts_observation_edge){.child_file_id=1,.line=2,.col=1,.requested_module="raw",.lookup_path="/invoke/entry/raw\xff.sts"};
    sts_observation_encode_init(&e,4096);CHECK(!sts_observation_graph_encode(&g,&e));char digest[129];sts_observation_digest_finish(&e.hash,digest);
    CHECK(e.size==544);CHECK(!strcmp(digest,"6134956c38d24a31674d40d1d65308619b177cb3168a23bfa059d097267d126a"));
    g.files[1].declared_module=NULL;CHECK(sts_observation_graph_freeze(&g,error,sizeof(error))!=0);CHECK(g.failed&&!g.frozen&&!g.input_graph_id[0]);

    sts_observation_config kat={.warn_memleak=1,.has_max_bytes=1,.max_bytes=8,.has_min_coverage=1,.min_coverage=90,.enabled_mask=3,.compiler_id="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
    CHECK(!sts_observation_config_digest(&kat,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","x86_64-unknown-linux-gnu",2,0));
    CHECK(!strcmp(kat.config_id,"bac9c0710c372aa6ccf949ce3def394ca04953638c22b915b0772279c4873e53"));
    CHECK(sts_observation_config_digest(&kat,"bad","x86_64-unknown-linux-gnu",2,0)!=0);
    CHECK(sts_observation_config_digest(&kat,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","x86_64-unknown-linux-gnu",4,0)!=0);
    sts_observation_config c={.input_graph_id="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",.config_id="202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"};
    uint32_t masks[]={0,5,4},lines[]={1},line_files[]={0};
    sts_observation_file_manifest files[]={{.physical_line_count=2,.content_id="a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5",.reason_masks=masks}};
    sts_observation_declaration decls[]={{.kind=7,.line=1,.col=2,.end_line=1,.end_col=20,.module_name="root",.name="f",.source_linkage=1,.selected=1}};
    sts_observation_test tests[]={{.line=1,.col=2,.name="repeat"}};
    sts_observation_anchor anchors[]={
        {.line=1,.col=2,.end_line=1,.end_col=20,.kind=1,.has_coverage_id=1},
        {.anchor_id=1,.line=2,.col=1,.end_line=2,.end_col=9,.kind=3,.exclusion_reason=2}};
    sts_observation_site sites[]={
        {.line=1,.col=3,.end_line=1,.end_col=8,.kind=1,.owner_test=UINT32_MAX},
        {.line=2,.col=1,.end_line=2,.end_col=9,.kind=3,.owner_test=0}};
    sts_observation_manifest m={.file_count=1,.files=files,.decl_count=1,.declarations=decls,.test_count=1,.tests=tests,
        .anchor_count=2,.anchors=anchors,.site_count=2,.sites=sites,.line_count=1,.lines=lines,.line_file_ids=line_files};
    CHECK(!sts_observation_manifest_digest(&c,&m));CHECK(m.serialized_size==464);
    CHECK(!strcmp(m.map_id,"3cd86e33227d7932d56031c1fdd1a4c2974d130c92a985506dd83cf8e0ea59d1"));
    m.file_count=STS_OBS_FILE_LIMIT+1;CHECK(sts_observation_manifest_digest(&c,&m)!=0);
}
static void module_bounds(void) {
    CHECK(sts_observation_module_valid("a.b_9"));CHECK(!sts_observation_module_valid("a..b"));CHECK(!sts_observation_module_valid("a."));
    CHECK(!sts_observation_module_valid("9a"));CHECK(!sts_observation_module_valid("a.\xff"));
    char module[257];memset(module,'a',sizeof(module));module[127]='.';module[255]=0;CHECK(sts_observation_module_valid(module));
    module[255]='a';module[256]=0;CHECK(!sts_observation_module_valid(module));module[127]='a';module[128]=0;CHECK(!sts_observation_module_valid(module));
    char directory[4096];memset(directory,'a',4095);directory[0]='/';directory[4095]=0;CHECK(!sts_observation_anchor_path(directory,"a"));
    char *p=sts_observation_anchor_path("/a","../b");CHECK(p&&!strcmp(p,"/a/../b"));free(p);
}
static void filesystem_controls(void) {
    char cwd[4096];CHECK(getcwd(cwd,sizeof(cwd)));char temp[]="/tmp/stasha-source-graph-unit-XXXXXX";CHECK(mkdtemp(temp));CHECK(!chdir(temp));
    CHECK(!mkdir("requested",0700));CHECK(!mkdir("target",0700));CHECK(!mkdir("requested/util",0700));CHECK(!mkdir("target/util",0700));
    put_text("target/root.sts","mod root;\n");put_text("target/other.sts","mod root;\n");put_text("requested/util/calc.sts","mod util.calc;\n");put_text("target/util/calc.sts","WRONG TARGET\n");
    CHECK(!symlink("../target/root.sts","requested/root.sts"));sts_observation_graph g;
    begin(&g,"requested/root.sts");CHECK(add(&g,"util.calc")==1);CHECK(g.file_count==2&&g.edge_count==1);
    CHECK(strstr(g.files[0].canonical_path,"/target/root.sts"));CHECK(strstr(g.files[1].lookup_path,"/requested/util/calc.sts"));
    CHECK(!strcmp(g.files[1].bytes,"mod util.calc;\n"));CHECK(g.physical_line_count==4);CHECK(!sts_observation_graph_freeze(&g,error,sizeof(error)));
    CHECK(!sts_observation_graph_revalidate(&g,error,sizeof(error)));
    CHECK(!unlink("requested/root.sts"));CHECK(!symlink("../target/other.sts","requested/root.sts"));
    CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);CHECK(!unlink("requested/root.sts"));CHECK(!symlink("../target/root.sts","requested/root.sts"));
    put_text("requested/util/calc.sts","changed bytes\n");CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);
    put_text("requested/util/calc.sts","mod util.calc;\n");CHECK(!sts_observation_graph_revalidate(&g,error,sizeof(error)));
    put_text("requested/util/replacement.sts","mod util.calc;\n");CHECK(!rename("requested/util/replacement.sts","requested/util/calc.sts"));
    CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    uint32_t id;
    begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"util.calc",1,1,&id,error,sizeof(error))!=0);CHECK(g.failed&&!g.frozen);CHECK(sts_observation_graph_freeze(&g,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(!symlink("util/calc.sts","requested/same.sts"));begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"same",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"canonical"));sts_observation_graph_free(&g);
    CHECK(!symlink("../target/root.sts","requested/self.sts"));begin(&g,"requested/root.sts");CHECK(sts_observation_graph_add(&g,"self",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"self-import"));sts_observation_graph_free(&g);
    begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"other.calc",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"alias"));sts_observation_graph_free(&g);
    begin(&g,"requested/root.sts");CHECK(!sts_observation_graph_add(&g,"util.calc",1,1,&id,error,sizeof(error)));CHECK(sts_observation_graph_declare(&g,id,"wrong",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    /* Distinct canonical hardlink paths are not coalesced by inode or digest. */
    CHECK(!link("requested/util/calc.sts","requested/hard.sts"));begin(&g,"requested/root.sts");add(&g,"util.calc");add(&g,"hard");CHECK(g.file_count==3);CHECK(!strcmp(g.files[1].content_id,g.files[2].content_id));sts_observation_graph_free(&g);
    /* Linux literal-backslash entry uses the ordinary original-spelling rule. */
    CHECK(!mkdir("back",0700));put_text("back\\root.sts","mod root;");put_text("back/leaf.sts","mod leaf;");
    begin(&g,"back\\root.sts");add(&g,"leaf");CHECK(strstr(g.files[1].lookup_path,"/back/leaf.sts"));sts_observation_graph_free(&g);
    /* One LF adds one physical line, including the final empty line. */
    put_text("empty.sts","");begin(&g,"empty.sts");CHECK(g.files[0].physical_line_count==1&&g.files[0].byte_length==0);sts_observation_graph_free(&g);
    put("nul.sts","a\0b",3);CHECK(sts_observation_graph_begin(&g,"nul.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(!mkfifo("pipe.sts",0600));CHECK(sts_observation_graph_begin(&g,"pipe.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(sts_observation_graph_begin(&g,"requested",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    char long_root[513];memset(long_root,'a',512);long_root[512]=0;CHECK(sts_observation_graph_begin(&g,long_root,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    begin(&g,"empty.sts");g.file_count=STS_OBS_FILE_LIMIT;CHECK(sts_observation_graph_add(&g,"leaf",1,1,&id,error,sizeof(error))!=0);g.file_count=1;sts_observation_graph_free(&g);
    /* Exact-read capacity checks include bytes/lines before any parser access. */
    char *large=malloc(STS_OBS_SOURCE_LIMIT+1);CHECK(large);memset(large,'x',STS_OBS_SOURCE_LIMIT+1);put("large.sts",large,STS_OBS_SOURCE_LIMIT+1);
    CHECK(sts_observation_graph_begin(&g,"large.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    put("large.sts",large,STS_OBS_SOURCE_LIMIT);begin(&g,"large.sts");CHECK(g.byte_length==STS_OBS_SOURCE_LIMIT);CHECK(sts_observation_graph_add(&g,"empty",1,1,&id,error,sizeof(error))==0);CHECK(g.file_count==2);sts_observation_graph_free(&g);
    memset(large,'\n',STS_OBS_SOURCE_LINES);put("large.sts",large,STS_OBS_SOURCE_LINES);CHECK(sts_observation_graph_begin(&g,"large.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    put("large.sts",large,STS_OBS_SOURCE_LINES-1);begin(&g,"large.sts");CHECK(g.physical_line_count==STS_OBS_SOURCE_LINES);CHECK(sts_observation_graph_add(&g,"empty",1,1,&id,error,sizeof(error))!=0);sts_observation_graph_free(&g);free(large);
    const char *paths[]={"target/root.sts","target/other.sts","requested/root.sts","requested/util/calc.sts","target/util/calc.sts","requested/same.sts","requested/self.sts","requested/hard.sts","back\\root.sts","back/leaf.sts","empty.sts","nul.sts","pipe.sts","large.sts"};
    for(size_t i=0;i<sizeof(paths)/sizeof(paths[0]);++i)CHECK(!unlink(paths[i]));
    CHECK(!rmdir("requested/util"));CHECK(!rmdir("target/util"));CHECK(!rmdir("requested"));CHECK(!rmdir("target"));CHECK(!rmdir("back"));CHECK(!chdir(cwd));CHECK(!rmdir(temp));
}
int main(void){encoding_vectors();module_bounds();filesystem_controls();printf("observation source graph: %u pure checks passed; no compiler or generated image executed\n",checks);return 0;}
