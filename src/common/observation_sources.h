#ifndef STASHA_OBSERVATION_SOURCES_H
#define STASHA_OBSERVATION_SOURCES_H

/* Observed-only ownership and identity. This is not the ordinary resolver. */
#include "observation_options.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

static int sts_observation_graph_error(sts_observation_graph *g, char *error, size_t n, const char *message) {
    if (g) {g->failed=1;g->frozen=0;g->input_graph_id[0]=0;}
    if(n) snprintf(error,n,"%s",message);
    return -1;
}
static void sts_observation_source_free(sts_observation_source *f) {
    free(f->lookup_path);free(f->canonical_path);free(f->requested_module);free(f->declared_module);free(f->bytes);
    memset(f,0,sizeof(*f));
}
static void sts_observation_graph_free(sts_observation_graph *g) {
    if(!g)return;
    for(uint32_t i=0;i<g->file_count;++i)sts_observation_source_free(&g->files[i]);
    for(uint32_t i=0;i<g->edge_count;++i){free(g->edges[i].requested_module);free(g->edges[i].lookup_path);}
    free(g->invocation_directory);free(g->original_root_spelling);free(g->entry_directory);
    memset(g,0,sizeof(*g));
}
static char *sts_observation_copy_bytes(const char *p,size_t n) {
    if(n==SIZE_MAX)return NULL;
    char *s=malloc(n+1); if(s){memcpy(s,p,n);s[n]=0;}return s;
}
static int sts_observation_module_valid(const char *p) {
    if(!p||!*p||strlen(p)>STS_OBS_MODULE_LIMIT)return 0;
    size_t component=0;
    for(const unsigned char *s=(const unsigned char *)p;*s;++s){
        if(*s=='.'){if(!component)return 0;component=0;continue;}
        if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||*s=='_'||(component&&*s>='0'&&*s<='9')))return 0;
        if(++component>STS_OBS_IDENTIFIER_LIMIT)return 0;
    }return component!=0;
}
/* Joining does not normalize '.', '..', or symlinks. */
static char *sts_observation_anchor_path(const char *directory,const char *path) {
    size_t n=strlen(path),d=strlen(directory);
    if(path[0]=='/')return n<=STS_OBS_PATH_LIMIT?sts_observation_copy_bytes(path,n):NULL;
    if(d>STS_OBS_PATH_LIMIT||n>STS_OBS_PATH_LIMIT-d||d+n>=STS_OBS_PATH_LIMIT)return NULL;
    char *out=malloc(d+n+2);if(!out)return NULL;
    memcpy(out,directory,d);out[d]='/';memcpy(out+d+1,path,n+1);return out;
}
#if defined(__linux__)
static int sts_observation_snapshot(const char *lookup,sts_observation_source *out,size_t byte_limit,
                                    uint32_t line_limit,char *error,size_t error_size) {
    sts_observation_source f={0}; int fd=-1,bad=0; struct stat before,after;
    f.lookup_path=sts_observation_copy_bytes(lookup,strlen(lookup));
    f.canonical_path=realpath(lookup,NULL);
    if(!f.lookup_path||!f.canonical_path||strlen(f.canonical_path)>STS_OBS_PATH_LIMIT)goto failed;
    fd=open(f.canonical_path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0||fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<0||
       (uint64_t)before.st_size>byte_limit)goto failed;
    f.byte_length=(size_t)before.st_size;f.bytes=malloc(f.byte_length+1);if(!f.bytes)goto failed;
    size_t off=0;
    while(off<f.byte_length){ssize_t n=read(fd,f.bytes+off,f.byte_length-off);if(n<0&&errno==EINTR)continue;if(n<=0)goto failed;off+=(size_t)n;}
    char extra;ssize_t n;do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);
    if(n!=0||fstat(fd,&after)||before.st_size!=after.st_size||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||
       before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||
       before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec)goto failed;
    bad=close(fd);fd=-1;if(bad||memchr(f.bytes,0,f.byte_length))goto failed;
    f.bytes[f.byte_length]=0;f.physical_line_count=1;
    for(size_t i=0;i<f.byte_length;++i)if(f.bytes[i]=='\n')++f.physical_line_count;
    if(f.physical_line_count>line_limit)goto failed;
    f.snapshot_device=(uint64_t)before.st_dev;f.snapshot_inode=(uint64_t)before.st_ino;
    sts_observation_digest(f.bytes,f.byte_length,f.content_id);*out=f;return 0;
failed:
    if(fd>=0)close(fd);
    sts_observation_source_free(&f);
    if(error_size)snprintf(error,error_size,"cannot snapshot bounded regular source '%s' (missing, changed, NUL, unreadable or capacity exceeded)",lookup);
    return -1;
}
static int sts_observation_graph_begin(sts_observation_graph *g,const char *root,char *error,size_t error_size) {
    memset(g,0,sizeof(*g));
    if(!root||!*root||strlen(root)>STS_OBS_ROOT_PATH_LIMIT)return sts_observation_graph_error(g,error,error_size,"requested observation root exceeds 511 bytes");
    char cwd[STS_OBS_PATH_LIMIT+1];if(!getcwd(cwd,sizeof(cwd)))return sts_observation_graph_error(g,error,error_size,"cannot capture bounded invocation directory");
    g->invocation_directory=sts_observation_copy_bytes(cwd,strlen(cwd));
    g->original_root_spelling=sts_observation_copy_bytes(root,strlen(root));
    const char *sep=strrchr(root,'/');if(!sep)sep=strrchr(root,'\\');
    g->entry_directory=sep?sts_observation_copy_bytes(root,(size_t)(sep-root)):sts_observation_copy_bytes(".",1);
    if(!g->invocation_directory||!g->original_root_spelling||!g->entry_directory)return sts_observation_graph_error(g,error,error_size,"cannot allocate observation lookup identity");
    char *lookup=sts_observation_anchor_path(cwd,root);
    if(!lookup)return sts_observation_graph_error(g,error,error_size,"observation root lookup path exceeds capacity");
    int status=sts_observation_snapshot(lookup,&g->files[0],STS_OBS_SOURCE_LIMIT,STS_OBS_SOURCE_LINES,error,error_size);free(lookup);
    if(status){g->failed=1;return -1;}g->file_count=1;g->files[0].file_id=0;g->files[0].role=STS_OBS_ROOT;
    g->byte_length=g->files[0].byte_length;g->physical_line_count=g->files[0].physical_line_count;return 0;
}
static int sts_observation_graph_add(sts_observation_graph *g,const char *module,uint32_t line,uint32_t col,
                                     uint32_t *file_id,char *error,size_t error_size) {
    if(!g||g->failed||g->frozen||!g->file_count||!line||!col||line>g->files[0].physical_line_count||
       !sts_observation_module_valid(module))return sts_observation_graph_error(g,error,error_size,"invalid observed root import or graph state");
    if(g->file_count>=STS_OBS_FILE_LIMIT||g->edge_count>=STS_OBS_EDGE_LIMIT)return sts_observation_graph_error(g,error,error_size,"observation graph exceeds 256 files or 255 imports");
    const char *alias=strrchr(module,'.');alias=alias?alias+1:module;
    for(uint32_t i=1;i<g->file_count;++i){
        const char *existing=g->files[i].requested_module,*other=strrchr(existing,'.');other=other?other+1:existing;
        if(!strcmp(module,existing))return sts_observation_graph_error(g,error,error_size,"repeated requested observation import module");
        if(!strcmp(alias,other))return sts_observation_graph_error(g,error,error_size,"duplicate final-segment observation import alias");
    }
    size_t d=strlen(g->entry_directory),m=strlen(module);
    if(d>STS_OBS_IMPORT_PATH_LIMIT||m+5>STS_OBS_IMPORT_PATH_LIMIT-d)return sts_observation_graph_error(g,error,error_size,"ordinary-form import lookup exceeds 1023 bytes");
    char ordinary[STS_OBS_IMPORT_PATH_LIMIT+1];memcpy(ordinary,g->entry_directory,d);ordinary[d]='/';
    for(size_t i=0;i<m;++i)ordinary[d+1+i]=module[i]=='.'?'/':module[i];
    memcpy(ordinary+d+1+m,".sts",5);
    char *lookup=sts_observation_anchor_path(g->invocation_directory,ordinary);
    if(!lookup)return sts_observation_graph_error(g,error,error_size,"absolute import lookup exceeds 4095 bytes");
    sts_observation_source f={0};
    int status=sts_observation_snapshot(lookup,&f,STS_OBS_SOURCE_LIMIT-g->byte_length,STS_OBS_SOURCE_LINES-g->physical_line_count,error,error_size);free(lookup);
    if(status){g->failed=1;return -1;}
    for(uint32_t i=0;i<g->file_count;++i)if(!strcmp(f.canonical_path,g->files[i].canonical_path)){
        sts_observation_source_free(&f);return sts_observation_graph_error(g,error,error_size,i?"repeated canonical observation import source":"observed root self-import");
    }
    f.requested_module=sts_observation_copy_bytes(module,m);f.file_id=g->file_count;f.role=STS_OBS_IMPORT;
    sts_observation_edge e={.parent_file_id=0,.child_file_id=f.file_id,.line=line,.col=col,.ordinal=g->edge_count};
    e.requested_module=sts_observation_copy_bytes(module,m);e.lookup_path=sts_observation_copy_bytes(f.lookup_path,strlen(f.lookup_path));
    if(!f.requested_module||!e.requested_module||!e.lookup_path){sts_observation_source_free(&f);free(e.requested_module);free(e.lookup_path);return sts_observation_graph_error(g,error,error_size,"cannot own observed import identity");}
    g->files[g->file_count++]=f;g->edges[g->edge_count++]=e;g->byte_length+=f.byte_length;g->physical_line_count+=f.physical_line_count;
    *file_id=f.file_id;return 0;
}
static int sts_observation_graph_revalidate(const sts_observation_graph *g,char *error,size_t error_size) {
    if(!g||!g->frozen||g->failed)return sts_observation_graph_error(NULL,error,error_size,"observation input graph is not complete and frozen");
    for(uint32_t i=0;i<g->file_count;++i){
        const sts_observation_source *f=&g->files[i];sts_observation_source now={0};
        if(sts_observation_snapshot(f->lookup_path,&now,STS_OBS_SOURCE_LIMIT,STS_OBS_SOURCE_LINES,error,error_size))return -1;
        int bad=strcmp(f->canonical_path,now.canonical_path)||strcmp(f->content_id,now.content_id)||f->byte_length!=now.byte_length||f->snapshot_device!=now.snapshot_device||f->snapshot_inode!=now.snapshot_inode;
        sts_observation_source_free(&now);if(bad){if(error_size)snprintf(error,error_size,"observation input file %u changed or its requested lookup rebound",i);return -1;}
    }return 0;
}
#endif
static int sts_observation_graph_declare(sts_observation_graph *g,uint32_t id,const char *module,char *error,size_t error_size) {
    if(!g||g->failed||g->frozen||id>=g->file_count||!sts_observation_module_valid(module)||g->files[id].declared_module)
        return sts_observation_graph_error(g,error,error_size,"missing, repeated or invalid observed module declaration");
    if(id&&strcmp(module,g->files[id].requested_module))return sts_observation_graph_error(g,error,error_size,"observed imported declared module does not match requested module");
    g->files[id].declared_module=sts_observation_copy_bytes(module,strlen(module));
    return g->files[id].declared_module?0:sts_observation_graph_error(g,error,error_size,"cannot own declared module identity");
}
static int sts_observation_graph_encode(const sts_observation_graph *g,sts_observation_encoder *e) {
    if(!g||g->failed||!g->file_count||g->file_count>STS_OBS_FILE_LIMIT||g->edge_count!=g->file_count-1)return -1;
#define OBS_GU(v) sts_observation_encode_u32(e,(v))
    sts_observation_encode_text(e,"stasha-observation-input-graph-v2");OBS_GU(2);
    sts_observation_encode_text(e,STS_OBS_GRAPH_PROFILE_V2);sts_observation_encode_text(e,STS_OBS_LOOKUP_POLICY);
    sts_observation_encode_text(e,STS_OBS_DUPLICATE_POLICY);sts_observation_encode_text(e,STS_OBS_ALIAS_POLICY);
    sts_observation_encode_text(e,g->invocation_directory);sts_observation_encode_text(e,g->original_root_spelling);OBS_GU(0);OBS_GU(g->file_count);
    for(uint32_t i=0;i<g->file_count;++i){const sts_observation_source *f=&g->files[i];
        OBS_GU(f->file_id);OBS_GU(f->role);sts_observation_encode_text(e,f->lookup_path);sts_observation_encode_text(e,f->canonical_path);
        sts_observation_encode_optional_text(e,f->requested_module);sts_observation_encode_text(e,f->declared_module);
        sts_observation_encode_u64(e,f->byte_length);OBS_GU(f->physical_line_count);sts_observation_encode_digest(e,f->content_id);
    }
    OBS_GU(g->edge_count);
    for(uint32_t i=0;i<g->edge_count;++i){const sts_observation_edge *a=&g->edges[i];
        OBS_GU(a->parent_file_id);OBS_GU(a->child_file_id);OBS_GU(a->line);OBS_GU(a->col);OBS_GU(a->ordinal);
        sts_observation_encode_text(e,a->requested_module);sts_observation_encode_text(e,a->lookup_path);
    }
#undef OBS_GU
    return e->failed?-1:0;
}
static int sts_observation_graph_freeze(sts_observation_graph *g,char *error,size_t error_size) {
    if(!g||g->frozen)return sts_observation_graph_error(g,error,error_size,"observation graph already frozen or absent");
    sts_observation_encoder e;sts_observation_encode_init(&e,STS_OBS_METADATA_LIMIT);
    if(sts_observation_graph_encode(g,&e))return sts_observation_graph_error(g,error,error_size,"incomplete or oversized observation graph");
    sts_observation_digest_finish(&e.hash,g->input_graph_id);g->frozen=1;return 0;
}
#endif
