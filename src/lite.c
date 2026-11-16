/* Small file-backed service: UCI configuration, volatile snapshots, pending forwards only. */
#include "relay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#ifndef RELAY_UCI
#define RELAY_UCI "/sbin/uci"
#endif
#define MAX_FILE (4*1024*1024)
const char *str(J *j,const char *key) { J *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v)?v->valuestring:""; }
int integer(J *j,const char *key,int fallback) { J *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsNumber(v)?v->valueint:fallback; }
J *failure(const char *message) { J *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"error",message); return j; }
static void text(J *j,const char *k,const char *v) { cJSON_DeleteItemFromObjectCaseSensitive(j,k); cJSON_AddStringToObject(j,k,v); }
static void number(J *j,const char *k,int v) { cJSON_DeleteItemFromObjectCaseSensitive(j,k); cJSON_AddNumberToObject(j,k,v); }
static void item(J *j,const char *k,J *v) { cJSON_DeleteItemFromObjectCaseSensitive(j,k); cJSON_AddItemToObject(j,k,v); }
static J *array(J *j,const char *k) { J *a=cJSON_GetObjectItemCaseSensitive(j,k); return a?a:cJSON_AddArrayToObject(j,k); }
static J *find(J *list,int id) { J *v; cJSON_ArrayForEach(v,list) if (integer(v,"id",0)==id) return v; return NULL; }
static void remove_id(J *list,int id) { for (int i=cJSON_GetArraySize(list)-1;i>=0;i--) if (integer(cJSON_GetArrayItem(list,i),"id",0)==id) cJSON_DeleteItemFromArray(list,i); }
static int allocate_id(J *config,const char *key,J *list) {
    int id=integer(config,key,1); J *v; cJSON_ArrayForEach(v,list) if (integer(v,"id",0)>=id) id=integer(v,"id",0)+1;
    number(config,key,id+1); return id;
}
static J *ok(void) { J *j=cJSON_CreateObject(); cJSON_AddBoolToObject(j,"ok",1); return j; }
static int path_join(char out[4096],const char *dir,const char *name) { int n=snprintf(out,4096,"%s/%s",dir,name); return n>=0 && n<4096; }
static int directory(const char *path,int repair) {
    if (mkdir(path,0700)<0 && errno!=EEXIST) return 0;
    struct stat s;
    if (lstat(path,&s)<0 || !S_ISDIR(s.st_mode) || s.st_uid!=geteuid()) return 0;
    if (!(s.st_mode&0077)) return 1;
    /* Restore tools may recreate intermediate directories with their own umask. */
    return repair && chmod(path,0700)==0;
}
static J *read_json(const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if (fd<0) return NULL;
    struct stat st; if (fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>MAX_FILE) { close(fd); return NULL; }
    size_t n=(size_t)st.st_size,used=0; char *raw=calloc(n+1,1); if (!raw) { close(fd); return NULL; }
    while (used<n) { ssize_t got=read(fd,raw+used,n-used); if (got<=0) break; used+=(size_t)got; }
    close(fd); J *j=used==n?cJSON_ParseWithLengthOpts(raw,n+1,NULL,1):NULL; erase_memory(raw,n); free(raw); return j;
}
static int atomic_json(const char *path,J *j,int durable) {
    char tmp[4096]; int n=snprintf(tmp,sizeof(tmp),"%s.XXXXXX",path); if (n<0 || n>=(int)sizeof(tmp)) return 0;
    char *raw=cJSON_PrintUnformatted(j); if (!raw) return 0;
    int fd=mkstemp(tmp),success=fd>=0; size_t len=strlen(raw),offset=0;
    while (success && offset<len) { ssize_t wrote=write(fd,raw+offset,len-offset); if (wrote<=0) success=0; else offset+=(size_t)wrote; }
    if (success && durable && fsync(fd)<0) success=0;
    if (fd>=0 && close(fd)<0) success=0;
    if (success && rename(tmp,path)<0) success=0;
    if (!success) unlink(tmp);
    if (success && durable) {
        char parent[4096]; snprintf(parent,sizeof(parent),"%s",path); char *slash=strrchr(parent,'/');
        if (slash) *slash=0;
        int p=open(slash?parent:".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); success=p>=0 && fsync(p)==0; if (p>=0) close(p);
    }
    erase_memory(raw,len); free(raw); return success;
}
/* UCI sees only public configuration and encrypted credential strings. No shell parsing. */
static char *uci(const char *command,const char *argument,int *success) {
    int fds[2]; *success=0; if (pipe(fds)<0) return NULL;
    pid_t pid=fork(); if (pid<0) { close(fds[0]); close(fds[1]); return NULL; }
    if (!pid) {
        close(fds[0]); dup2(fds[1],STDOUT_FILENO); close(fds[1]);
        int null=open("/dev/null",O_RDWR); if (null>=0) { dup2(null,STDERR_FILENO); dup2(null,STDIN_FILENO); close(null); }
        const char *executable=RELAY_UCI;
#ifdef RELAY_TESTING
        const char *fixture=getenv("SIM_RELAY_TEST_UCI"); if (fixture && *fixture) executable=fixture;
#endif
        execl(executable,"uci","-q",command,argument,(char *)NULL); _exit(127);
    }
    close(fds[1]); char *raw=calloc(65537,1); size_t n=0; char buf[4096]; ssize_t got;
    while ((got=read(fds[0],buf,sizeof(buf)))>0) if (raw && n+(size_t)got<=65536) { memcpy(raw+n,buf,(size_t)got); n+=(size_t)got; } else { free(raw); raw=NULL; }
    close(fds[0]); int status=0; while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
    *success=WIFEXITED(status) && WEXITSTATUS(status)==0 && raw; return raw;
}
static int save_config(Relay *r) {
    const char *local=getenv("SIM_RELAY_CONFIG");
    if (local && *local) return atomic_json(local,r->config,1);
    char *raw=cJSON_PrintUnformatted(r->config); if (!raw) return 0;
    size_t n=strlen(raw); if (n>60000) { free(raw); return 0; }
    char *arg=malloc(n+21); if (!arg) { free(raw); return 0; }
    snprintf(arg,n+21,"sim-relay.main.data=%s",raw); int success;
    char *out=uci("set",arg,&success); free(out); free(arg); free(raw); if (!success) return 0;
    out=uci("commit","sim-relay",&success); free(out); return success;
}
int open_store(Relay *r,const char *path) {
    memset(r,0,sizeof(*r)); r->lock=-1; r->path=path; r->runtime=getenv("SIM_RELAY_RUNTIME"); if (!r->runtime || !*r->runtime) r->runtime="/tmp/sim-relay";
    umask(0077); char p[4096];
    if (!directory(path,0) || !directory(r->runtime,0)) goto fail;
    if (!path_join(p,path,"pending") || !directory(p,1)) goto fail;
    if (!path_join(p,r->runtime,"actions") || !directory(p,1)) goto fail;
    if (!path_join(p,r->runtime,"config.lock")) goto fail;
    r->lock=open(p,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600); if (r->lock<0 || flock(r->lock,LOCK_EX)<0) goto fail;
    const char *local=getenv("SIM_RELAY_CONFIG");
    if (local && *local) {
        r->config=read_json(local); if (!r->config && access(local,F_OK)==0) goto fail;
    } else {
        int success; char *raw=uci("get","sim-relay.main.data",&success);
        if (!success) { free(raw); goto fail; }
        r->config=cJSON_Parse(raw); free(raw); if (!r->config) goto fail;
    }
    if (!r->config) r->config=cJSON_CreateObject();
    if (!cJSON_IsObject(r->config)) goto fail;
    const char *keys[]={"devices","channels","pins"};
    for (size_t i=0;i<3;i++) if (!cJSON_IsArray(array(r->config,keys[i]))) goto fail;
    return 1;
fail:
    snprintf(r->error,sizeof(r->error),"Cannot open private state directory or configuration (UCI main.data must exist)"); return 0;
}
void close_store(Relay *r) { if (r->lock>=0) close(r->lock); r->lock=-1; delete_secret_json(r->config); r->config=NULL; }
static J *runtime(Relay *r) { char p[4096]; path_join(p,r->runtime,"state.json"); J *s=read_json(p); return s?s:cJSON_CreateObject(); }
void reset_runtime(Relay *r) { char p[4096]; path_join(p,r->runtime,"state.json"); unlink(p); }
static J *state_device(J *state,int id) { J *list=array(state,"devices"),*d=find(list,id); if (!d) { d=cJSON_CreateObject(); number(d,"id",id); cJSON_AddItemToArray(list,d); } return d; }
static J *pin_for(Relay *r,const char *iccid) { J *p; cJSON_ArrayForEach(p,array(r->config,"pins")) if (!strcmp(str(p,"iccid"),iccid)) return p; return NULL; }
static int commit(void *ctx) { (void)ctx; return 1; }
static void history(J *state,const char *key,J *job) { J *a=array(state,key); cJSON_AddItemToArray(a,cJSON_Duplicate(job,1)); while (cJSON_GetArraySize(a)>50) cJSON_DeleteItemFromArray(a,0); }
static int pending_path(Relay *r,const char *name,char p[4096]) { char dir[4096]; return path_join(dir,r->path,"pending") && path_join(p,dir,name); }
static int json_name(const char *name) { size_t n=strlen(name); return n>5 && !strcmp(name+n-5,".json"); }
static J *pending_list(Relay *r,int device) {
    J *out=cJSON_CreateArray(); char p[4096]; pending_path(r,"",p); DIR *dir=opendir(p); struct dirent *e;
    if (!dir) return out;
    while ((e=readdir(dir))) if (json_name(e->d_name)) { pending_path(r,e->d_name,p); J *j=read_json(p); if (j && (!device || integer(j,"device_id",0)==device)) cJSON_AddItemToArray(out,j); else cJSON_Delete(j); }
    closedir(dir); return out;
}
static int purge_device_jobs(const char *directory,int device) {
    DIR *dir=opendir(directory); if (!dir) return errno==ENOENT;
    struct dirent *e; int ok=1; char p[4096];
    while ((e=readdir(dir))) if (json_name(e->d_name) && path_join(p,directory,e->d_name)) {
        J *job=read_json(p);
        if (job && integer(job,"device_id",0)==device && unlink(p)<0 && errno!=ENOENT) ok=0;
        cJSON_Delete(job);
    }
    closedir(dir); return ok;
}
static int enqueue_forward(Relay *r,J *d,J *m,J *targets,const char *iccid) {
    char source[256],key[69],name[80],p[4096];
    snprintf(source,sizeof(source),"%d:%s:%s",integer(d,"id",0),iccid,str(m,"external_id"));
    if (!message_identity(source,key)) return 0;
    snprintf(name,sizeof(name),"%s.json",key); pending_path(r,name,p);
    if (access(p,F_OK)==0) return 1;
    J *all=pending_list(r,0); int full=cJSON_GetArraySize(all)>=1024; cJSON_Delete(all); if (full) return 0;
    J *job=cJSON_CreateObject(); number(job,"device_id",integer(d,"id",0)); text(job,"device_name",str(d,"name"));
    text(job,"iccid",iccid); text(job,"key",key); item(job,"message",cJSON_Duplicate(m,1)); item(job,"targets",cJSON_Duplicate(targets,1));
    int success=atomic_json(p,job,1); cJSON_Delete(job); return success;
}
static int contains(J *list,const char *key) { J *v; cJSON_ArrayForEach(v,list) if (!strcmp(str(v,"external_id"),key)) return 1; return 0; }
static int bound(J *d,int id) { J *n; cJSON_ArrayForEach(n,cJSON_GetObjectItemCaseSensitive(d,"channels")) if (cJSON_IsNumber(n) && n->valueint==id) return 1; return 0; }
static int bound_version(J *list,int id,int version) { J *v=find(list,id); return v && integer(v,"version",0)==version; }
static J *channel_secrets(Relay *r,J *c) {
    if (!*str(c,"ciphertext")) return cJSON_CreateObject();
    char context[40]; snprintf(context,sizeof(context),"channel-%d",integer(c,"id",0));
    char *raw=open_secret(r,context,str(c,"ciphertext")); J *out=raw?cJSON_Parse(raw):NULL; wipe_secret(raw); return out;
}
static void process_pending(Relay *r,J *state) {
    char dirpath[4096],p[4096]; pending_path(r,"",dirpath); DIR *dir=opendir(dirpath); if (!dir) return;
    struct dirent *e;
    while ((e=readdir(dir))) if (json_name(e->d_name)) {
        pending_path(r,e->d_name,p); J *job=read_json(p); if (!job) { text(state,"error","Unreadable pending file; retained for inspection"); continue; }
        int id=integer(job,"device_id",0); J *d=find(array(r->config,"devices"),id),*targets=array(job,"targets");
        if (!d) { unlink(p); cJSON_Delete(job); continue; }
        if (!integer(d,"enabled",1)) { cJSON_Delete(job); continue; }
        for (int i=cJSON_GetArraySize(targets)-1;i>=0;i--) {
            J *t=cJSON_GetArrayItem(targets,i),*c=find(array(r->config,"channels"),integer(t,"id",0));
            if (!c || !integer(c,"enabled",1) || !bound(d,integer(c,"id",0)) || integer(c,"version",0)!=integer(t,"version",0)) {
                cJSON_DeleteItemFromArray(targets,i); if (!atomic_json(p,job,1)) break; continue;
            }
            if (integer(t,"next_try",0)>(int)time(NULL)) continue;
            char error[256]="",body[8192]; J *m=cJSON_GetObjectItemCaseSensitive(job,"message"),*config=cJSON_GetObjectItemCaseSensitive(c,"config");
            snprintf(body,sizeof(body),"%s%s[%s] %s\n%s\n%s",str(config,"prefix"),*str(config,"prefix")?"\n":"",str(job,"device_name"),str(m,"number"),str(m,"text"),str(m,"timestamp"));
            int result=0,retry=0;
            if (!strcmp(str(c,"kind"),"sms")) {
                J *sim=adapter_sim(d,relay_monotonic()+30,error,sizeof(error));
                if (sim && !strcmp(str(sim,"iccid"),str(job,"iccid")) && strcmp(str(m,"number"),str(config,"number"))) {
                    J *send=cJSON_Duplicate(sim,1); text(send,"number",str(config,"number")); text(send,"text",body); text(send,"key",str(job,"key"));
                    J *sent=adapter_send(d,send,commit,NULL,&result,error,sizeof(error)); cJSON_Delete(sent); cJSON_Delete(send);
                } else snprintf(error,sizeof(error),"SIM changed, unavailable, or forwarding loop");
                cJSON_Delete(sim);
            } else {
                J *secrets=channel_secrets(r,c);
                if (secrets) result=push_deliver(str(c,"kind"),config,secrets,body,&retry,error,sizeof(error));
                else snprintf(error,sizeof(error),"Cannot decrypt channel credentials");
                delete_secret_json(secrets);
            }
            J *event=cJSON_CreateObject(); number(event,"device_id",id); text(event,"channel",str(c,"name")); text(event,"status",result==3?"sent":"queued");
            number(event,"attempts",integer(t,"attempts",0)+1); text(event,"error",error); history(state,"push_history",event); cJSON_Delete(event);
            if (result==3) cJSON_DeleteItemFromArray(targets,i);
            else { number(t,"attempts",integer(t,"attempts",0)+1); number(t,"next_try",(int)time(NULL)+(retry>60 && retry<=3600?retry:60)); text(t,"error",error); }
            /* One checkpoint per channel; successful targets are never kept intentionally. */
            if (!atomic_json(p,job,1)) { text(state,"error","Cannot update pending file"); break; }
        }
        if (!cJSON_GetArraySize(targets)) unlink(p);
        cJSON_Delete(job);
    }
    closedir(dir);
}
/* UTC seconds, independent of the host timezone. Device timestamps are ISO 8601. */
static double message_time(const char *stamp) {
    int y,m,d,h,min,sec,zh=0,zm=0; char zone=0;
    if (sscanf(stamp,"%d-%d-%dT%d:%d:%d%c%d:%d",&y,&m,&d,&h,&min,&sec,&zone,&zh,&zm)<7 ||
        y<1970 || y>2100 || m<1 || m>12 || d<1 || d>31 || h<0 || h>23 || min<0 || min>59 || sec<0 || sec>59 ||
        (zone!='Z' && zone!='+' && zone!='-') || zh<0 || zh>23 || zm<0 || zm>59) return -1;
    y-=m<=2; int era=y/400,yoe=y-era*400;
    int doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    int days=era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
    return (double)days*86400+h*3600+min*60+sec-(zone=='-'?-1:1)*(zh*3600+zm*60);
}
static int key_in(J *keys,const char *key) { J *v; cJSON_ArrayForEach(v,keys) if (cJSON_IsString(v) && !strcmp(v->valuestring,key)) return 1; return 0; }
static int snapshot_update(Relay *r,J *state,J *d,J *sd,J *snapshot) {
    int id=integer(d,"id",0); char name[40],path[4096]; snprintf(name,sizeof(name),"cursor-%d.json",id); path_join(path,r->path,name);
    J *cursor=read_json(path);
    if (!cursor && access(path,F_OK)==0) { text(sd,"last_error","Unreadable progress marker; not reset automatically"); return 0; }
    int initialized=cursor && !strcmp(str(cursor,"iccid"),str(snapshot,"iccid"));
    J *old=cJSON_GetObjectItemCaseSensitive(sd,"messages"),*fresh=cJSON_GetObjectItemCaseSensitive(snapshot,"messages"),*bindings=cJSON_CreateArray();
    J *epoch=initialized?cJSON_GetObjectItemCaseSensitive(cursor,"time"):NULL;
    double previous=cJSON_IsNumber(epoch)?epoch->valuedouble:-1,latest=previous;
    J *keys=initialized?cJSON_Duplicate(array(cursor,"keys"),1):cJSON_CreateArray(); J *c,*m;
    cJSON_ArrayForEach(c,array(r->config,"channels")) if (integer(c,"enabled",1) && bound(d,integer(c,"id",0))) {
        J *b=cJSON_CreateObject(); number(b,"id",integer(c,"id",0)); number(b,"version",integer(c,"version",0)); cJSON_AddItemToArray(bindings,b);
    }
    cJSON_ArrayForEach(m,fresh) {
        J *previous_message=NULL,*v; cJSON_ArrayForEach(v,old) if (!strcmp(str(v,"external_id"),str(m,"external_id"))) { previous_message=v; break; }
        int mid=previous_message?integer(previous_message,"id",0):integer(state,"next_message",1);
        if (!previous_message) number(state,"next_message",mid+1);
        number(m,"id",mid);
        if (strcmp(str(m,"direction"),"inbound")) continue;
        double received=message_time(str(m,"timestamp"));
        int newer=received>previous || (received==previous && !key_in(cJSON_GetObjectItemCaseSensitive(cursor,"keys"),str(m,"external_id")));
        /* Unknown dates can be recognized while running, but cannot advance the time cursor. */
        if (received<0) newer=cJSON_IsArray(old) && !contains(old,str(m,"external_id"));
        if (initialized && newer) {
            J *targets=cJSON_CreateArray(),*b;
            cJSON_ArrayForEach(b,bindings) if (bound_version(cJSON_GetObjectItemCaseSensitive(cursor,"bindings"),integer(b,"id",0),integer(b,"version",0))) cJSON_AddItemToArray(targets,cJSON_Duplicate(b,1));
            int success=!cJSON_GetArraySize(targets) || enqueue_forward(r,d,m,targets,str(snapshot,"iccid")); cJSON_Delete(targets);
            if (!success) { cJSON_Delete(keys); cJSON_Delete(cursor); cJSON_Delete(bindings); text(sd,"last_error","Cannot persist pending forward; progress not advanced"); return 0; }
        }
        if (received>latest) { latest=received; cJSON_Delete(keys); keys=cJSON_CreateArray(); }
        if (received>=0 && received==latest && !key_in(keys,str(m,"external_id"))) cJSON_AddItemToArray(keys,cJSON_CreateString(str(m,"external_id")));
    }
    J *next=cJSON_CreateObject(); text(next,"iccid",str(snapshot,"iccid")); cJSON_AddNumberToObject(next,"time",latest); item(next,"keys",keys); item(next,"bindings",cJSON_Duplicate(bindings,1));
    /* No flash write during idle polls. Mark progress only after all new pending files exist. */
    int success=(cursor && cJSON_Compare(cursor,next,1)) || atomic_json(path,next,1); cJSON_Delete(next); cJSON_Delete(cursor);
    if (!success) { cJSON_Delete(bindings); text(sd,"last_error","Cannot persist progress marker"); return 0; }
    item(sd,"messages",cJSON_Duplicate(fresh,1)); item(sd,"bindings",bindings); text(sd,"iccid",str(snapshot,"iccid")); text(sd,"hardware_id",str(snapshot,"hardware_id"));
    char timestamp[40]; time_t now=time(NULL); struct tm tm; gmtime_r(&now,&tm); strftime(timestamp,sizeof(timestamp),"%Y-%m-%dT%H:%M:%SZ",&tm);
    text(sd,"last_sync",timestamp); text(sd,"last_error",""); return 1;
}
static int action_path(Relay *r,const char *key,char out[4096]) { char hash[69],name[80],dir[4096]; if (!message_identity(key,hash)) return 0; snprintf(name,sizeof(name),"%s.json",hash); return path_join(dir,r->runtime,"actions") && path_join(out,dir,name); }
static int same_request(J *old,J *args,const char *kind) {
    if (!strcmp(kind,"send")) return !strcmp(str(old,"number"),str(args,"number")) && !strcmp(str(old,"text"),str(args,"text"));
    return 1;
}
static J *enqueue_action(Relay *r,J *a,const char *kind) {
    int id=integer(a,"device_id",0); J *d=find(array(r->config,"devices"),id);
    if (!d || !integer(d,"enabled",1)) return failure("Device missing or disabled");
    const char *key=str(a,"key"); if (!*key || strlen(key)>128) return failure("Request key required (1..128 bytes)");
    char scoped[180],p[4096]; snprintf(scoped,sizeof(scoped),"%d:%s:%s",id,kind,key); if (!action_path(r,scoped,p)) return NULL;
    J *existing=read_json(p); if (existing) {
        if (!same_request(existing,a,kind)) { cJSON_Delete(existing); return failure("Request key already used with different SMS content"); }
        cJSON_DeleteItemFromObjectCaseSensitive(existing,"ciphertext"); return existing;
    }
    J *s=runtime(r),*h; const char *list=!strcmp(kind,"send")?"send_history":"operation_history";
    cJSON_ArrayForEach(h,cJSON_GetObjectItemCaseSensitive(s,list)) if (!strcmp(str(h,"request"),scoped)) { J *out=same_request(h,a,kind)?cJSON_Duplicate(h,1):failure("Request key already used with different SMS content"); cJSON_Delete(s); return out; }
    J *job=cJSON_Duplicate(a,1); text(job,"kind",kind); text(job,"request",scoped); text(job,"status","queued"); number(job,"id",(int)(time(NULL)%1000000000));
    if (!strcmp(kind,"pin")) {
        J *sd=state_device(s,id),*sim=cJSON_GetObjectItemCaseSensitive(sd,"sim");
        if (strcmp(str(sim,"iccid"),str(a,"iccid"))) { cJSON_Delete(s); cJSON_Delete(job); return failure("Refresh SIM before unlocking"); }
        J *saved=pin_for(r,str(a,"iccid")); char context[40]; snprintf(context,sizeof(context),"pin-%s",str(a,"iccid"));
        char *pin=*str(a,"pin")?strdup(str(a,"pin")):open_secret(r,context,str(saved,"ciphertext"));
        if (!pin || strlen(pin)<4 || strlen(pin)>8 || strspn(pin,"0123456789")!=strlen(pin)) { wipe_secret(pin); cJSON_Delete(s); delete_secret_json(job); return failure("A saved or valid PIN is required"); }
        char *encrypted=seal_secret(r,context,pin); wipe_secret(pin); cJSON_DeleteItemFromObjectCaseSensitive(job,"pin");
        if (!encrypted) { cJSON_Delete(s); cJSON_Delete(job); return NULL; }
        text(job,"ciphertext",encrypted); free(encrypted);
    }
    cJSON_Delete(s);
    if (!atomic_json(p,job,0)) { delete_secret_json(job); return failure("Cannot queue temporary action"); }
    cJSON_DeleteItemFromObjectCaseSensitive(job,"ciphertext"); return job;
}
static void process_actions(Relay *r,J *state) {
    char dirpath[4096],p[4096]; path_join(dirpath,r->runtime,"actions"); DIR *dir=opendir(dirpath); if (!dir) return;
    struct dirent *e;
    while ((e=readdir(dir))) if (json_name(e->d_name)) {
        path_join(p,dirpath,e->d_name); J *job=read_json(p); if (!job) continue;
        int id=integer(job,"device_id",0); J *d=find(array(r->config,"devices"),id); char error[256]=""; int outcome=0;
        int send=!strcmp(str(job,"kind"),"send");
        if (d && integer(d,"enabled",1)) {
            J *sim=adapter_sim(d,relay_monotonic()+30,error,sizeof(error));
            if (sim) {
                text(job,"hardware_id",str(sim,"hardware_id"));
                if (send) {
                    text(job,"iccid",str(sim,"iccid")); J *result=adapter_send(d,job,commit,NULL,&outcome,error,sizeof(error)); cJSON_Delete(result);
                } else if (!strcmp(str(job,"kind"),"pin")) {
                    char context[40]; snprintf(context,sizeof(context),"pin-%s",str(job,"iccid")); char *pin=open_secret(r,context,str(job,"ciphertext"));
                    if (pin) outcome=adapter_operation(d,job,pin,commit,NULL,error,sizeof(error));
                    wipe_secret(pin);
                } else {
                    J *targets=cJSON_GetObjectItemCaseSensitive(job,"targets"),*target; outcome=3;
                    cJSON_ArrayForEach(target,targets) {
                        J *op=cJSON_Duplicate(sim,1); text(op,"kind","delete"); text(op,"target",target->valuestring);
                        int result=adapter_operation(d,op,NULL,commit,NULL,error,sizeof(error)); cJSON_Delete(op); if (result!=3) { outcome=result; break; }
                    }
                }
            }
            cJSON_Delete(sim);
        } else snprintf(error,sizeof(error),"Device missing or disabled");
        cJSON_DeleteItemFromObjectCaseSensitive(job,"ciphertext"); text(job,"status",outcome==3?(send?"sent":"succeeded"):outcome==1?"unconfirmed":"failed"); text(job,"error",error);
        history(state,send?"send_history":"operation_history",job); unlink(p); cJSON_Delete(job);
    }
    closedir(dir);
}
void poll_devices(Relay *r) {
    /* Configuration lock protects edits/key creation only; network calls never hold it. */
    if (r->lock>=0) { close(r->lock); r->lock=-1; }
    char p[4096]; path_join(p,r->runtime,"worker.lock"); int fd=open(p,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0 || flock(fd,LOCK_EX|LOCK_NB)<0) { if (fd>=0) close(fd); return; }
    J *state=runtime(r),*d; text(state,"error","");
    process_actions(r,state);
    cJSON_ArrayForEach(d,array(r->config,"devices")) if (integer(d,"enabled",1)) {
        int id=integer(d,"id",0); J *sd=state_device(state,id);
        char wake[4096],name[40]; snprintf(name,sizeof(name),"sync-%d",id); path_join(wake,r->runtime,name);
        int forced=unlink(wake)==0;
        if (!forced && integer(sd,"next_poll",0)>(int)time(NULL)) continue;
        char error[256]=""; J *sim=adapter_sim(d,relay_monotonic()+30,error,sizeof(error));
        if (sim) {
            item(sd,"sim",cJSON_Duplicate(sim,1)); J *pin=pin_for(r,str(sim,"iccid"));
            if (!strcmp(str(sim,"unlock_required"),"none")) number(sd,"auto_attempted",0);
            if (pin && integer(pin,"auto_enabled",0) && !integer(sd,"auto_attempted",0) && !strcmp(str(sim,"unlock_required"),"sim-pin") && integer(sim,"pin_retries",-1)==3) {
                number(sd,"auto_attempted",1); char context[40]; snprintf(context,sizeof(context),"pin-%s",str(sim,"iccid"));
                char *secret=open_secret(r,context,str(pin,"ciphertext")); J *op=cJSON_Duplicate(sim,1); text(op,"kind","pin");
                int outcome=secret?adapter_operation(d,op,secret,commit,NULL,error,sizeof(error)):0;
                number(op,"device_id",id); text(op,"status",outcome==3?"succeeded":outcome==1?"unconfirmed":"failed"); text(op,"error",error);
                history(state,"operation_history",op); wipe_secret(secret); cJSON_Delete(op);
            }
            J *snapshot=adapter_read(d,error,sizeof(error)); if (snapshot) { snapshot_update(r,state,d,sd,snapshot); cJSON_Delete(snapshot); } else text(sd,"last_error",*error?error:"Cannot read device SMS");
            cJSON_Delete(sim);
        } else text(sd,"last_error",*error?error:"Cannot read SIM state");
        number(sd,"next_poll",(int)time(NULL)+integer(d,"poll_interval",30));
    }
    /* Files left by a previous boot are the only messages eligible for recovery. */
    process_pending(r,state);
    path_join(p,r->runtime,"state.json"); if (!atomic_json(p,state,0)) snprintf(r->error,sizeof(r->error),"Cannot save runtime snapshot");
    cJSON_Delete(state); close(fd);
}
static J *save_result(Relay *r,int id) { if (!save_config(r)) return failure("Cannot commit UCI configuration"); J *out=ok(); number(out,"id",id); return out; }
static J *list_history(J *state,const char *key,int id) { J *out=cJSON_CreateArray(),*v; cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(state,key)) if (integer(v,"device_id",0)==id) cJSON_AddItemToArray(out,cJSON_Duplicate(v,1)); return out; }
J *dispatch(Relay *r,const char *method,J *a) {
    J *devices=array(r->config,"devices"),*channels=array(r->config,"channels"); int id=integer(a,"device_id",0); J *d=find(devices,id);
    if (!strcmp(method,"status")) { J *out=ok(); text(out,"version","0.6.0"); text(out,"storage","uci+pending-files"); cJSON_AddBoolToObject(out,"smtp_available",access("/usr/bin/msmtp",X_OK)==0); return out; }
    if (!strcmp(method,"discover")) { char error[256]=""; J *out=mm_discover(error,sizeof(error)); return out?out:failure(error); }
    if (!strcmp(method,"save_device")) {
        int did=integer(a,"id",0),interval=integer(a,"poll_interval",30); const char *adapter=str(a,"adapter"); J *old=find(devices,did);
        if (!*str(a,"name") || strlen(str(a,"name"))>128 || interval<10 || interval>86400 || (strcmp(adapter,"mock") && strcmp(adapter,"zte_mf833u1") && strcmp(adapter,"modemmanager")) || (!strcmp(adapter,"zte_mf833u1") && !valid_url(str(a,"base_url"))) || (did && !old)) return failure("Invalid device configuration");
        if (old && (strcmp(str(old,"adapter"),adapter) || strcmp(str(old,"base_url"),str(a,"base_url")))) return failure("Remove and re-add a device to change its adapter/address");
        if (!old && !strcmp(adapter,"zte_mf833u1")) {
            J *existing;
            cJSON_ArrayForEach(existing,devices)
                if (!strcmp(str(existing,"adapter"),adapter) && same_device_url(str(existing,"base_url"),str(a,"base_url")))
                    return failure("This ZTE device address has already been added.");
        }
        if (!old && cJSON_GetArraySize(devices)>=16) return failure("Maximum 16 devices");
        J *n=cJSON_Duplicate(a,1); if (!did) did=allocate_id(r->config,"next_device",devices); number(n,"id",did);
        if (!strcmp(adapter,"modemmanager")) {
            if (old) { text(n,"modem",str(old,"modem")); text(n,"hardware_id",str(old,"hardware_id")); }
            else {
                char error[256]=""; J *all=mm_discover(error,sizeof(error)),*v,*selected=NULL; int count=0;
                if (!all) { cJSON_Delete(n); return failure(error); }
                cJSON_ArrayForEach(v,all) if (!*str(a,"modem") || !strcmp(str(a,"modem"),"any") || !strcmp(str(v,"modem"),str(a,"modem"))) { selected=v; count++; }
                if (count!=1) { cJSON_Delete(all); cJSON_Delete(n); return failure("Select one of the discovered local cellular modems."); }
                J *existing; int duplicate=0;
                cJSON_ArrayForEach(existing,devices) if (!strcmp(str(existing,"adapter"),"modemmanager") && !strcmp(str(existing,"hardware_id"),str(selected,"hardware_id"))) duplicate=1;
                if (duplicate) { cJSON_Delete(all); cJSON_Delete(n); return failure("This local cellular modem has already been added."); }
                text(n,"modem",str(selected,"modem")); text(n,"hardware_id",str(selected,"hardware_id")); cJSON_Delete(all);
            }
        }
        if (old) item(n,"channels",cJSON_Duplicate(array(old,"channels"),1));
        remove_id(devices,did); cJSON_AddItemToArray(devices,n); return save_result(r,did);
    }
    if (!strcmp(method,"devices")) {
        J *out=cJSON_Duplicate(devices,1),*s=runtime(r),*v; cJSON_ArrayForEach(v,out) {
            J *sd=find(cJSON_GetObjectItemCaseSensitive(s,"devices"),integer(v,"id",0));
            const char *keys[]={"last_error","last_sync","iccid"}; for (size_t i=0;i<3;i++) text(v,keys[i],str(sd,keys[i]));
            if (*str(sd,"hardware_id")) text(v,"hardware_id",str(sd,"hardware_id"));
            if (*str(s,"error")) text(v,"last_error",str(s,"error"));
        } cJSON_Delete(s); return out;
    }
    if (!strcmp(method,"remove_device")) {
        if (!d) return failure("Device not found");
        char p[4096],name[40],iccid[23]="";
        snprintf(name,sizeof(name),"cursor-%d.json",id); path_join(p,r->path,name);
        J *state=runtime(r),*sd=find(cJSON_GetObjectItemCaseSensitive(state,"devices"),id);
        const char *current=str(cJSON_GetObjectItemCaseSensitive(sd,"sim"),"iccid");
        if (!*current) current=str(sd,"iccid");
        J *cursor=read_json(p);
        if (!*current) current=str(cursor,"iccid");
        normalize_iccid(current,iccid);
        J *pins=array(r->config,"pins");
        for (int i=cJSON_GetArraySize(pins)-1;i>=0;i--) {
            J *pin=cJSON_GetArrayItem(pins,i);
            if (integer(pin,"device_id",0)==id || (*iccid && !strcmp(str(pin,"iccid"),iccid))) cJSON_DeleteItemFromArray(pins,i);
        }
        cJSON_Delete(cursor); cJSON_Delete(state);
        remove_id(devices,id); J *result=save_result(r,id);
        if (!*str(result,"error")) {
            int cursor_ok=unlink(p)==0 || errno==ENOENT;
            char pending[4096],actions[4096];
            int pending_ok=path_join(pending,r->path,"pending") && purge_device_jobs(pending,id);
            int actions_ok=path_join(actions,r->runtime,"actions") && purge_device_jobs(actions,id);
            if (!cursor_ok || !pending_ok || !actions_ok)
                cJSON_AddStringToObject(result,"warning","Some device data could not be removed");
        }
        return result;
    }
    if (!strcmp(method,"channels")) {
        J *out=cJSON_Duplicate(channels,1),*v; cJSON_ArrayForEach(v,out) { cJSON_AddBoolToObject(v,"has_credentials",*str(v,"ciphertext")!=0); cJSON_DeleteItemFromObjectCaseSensitive(v,"ciphertext"); char *raw=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(v,"config")); text(v,"config_json",raw?raw:"{}"); free(raw); } return out;
    }
    if (!strcmp(method,"save_channel")) {
        int cid=integer(a,"id",0); J *old=find(channels,cid);
        if (!*str(a,"name") || strlen(str(a,"name"))>128 || (cid && (!old || integer(old,"version",0)!=integer(a,"version",0) || strcmp(str(old,"kind"),str(a,"kind"))))) return failure("Invalid or stale channel configuration");
        if (!cid && cJSON_GetArraySize(channels)>=32) return failure("Maximum 32 channels");
        J *config=cJSON_Parse(str(a,"config")),*input=cJSON_Parse(str(a,"secrets")),*secrets=old?channel_secrets(r,old):cJSON_CreateObject(),*v;
        if (!cJSON_IsObject(config) || !cJSON_IsObject(input) || !cJSON_IsObject(secrets)) { cJSON_Delete(config); delete_secret_json(input); delete_secret_json(secrets); return failure("Invalid channel JSON or credentials"); }
        cJSON_ArrayForEach(v,input) { item(secrets,v->string,cJSON_Duplicate(v,1)); }
        delete_secret_json(input);
        if (!push_validate(str(a,"kind"),config,secrets)) { cJSON_Delete(config); delete_secret_json(secrets); return failure("Invalid channel fields"); }
        if (!cid) cid=allocate_id(r->config,"next_channel",channels);
        char context[40]; snprintf(context,sizeof(context),"channel-%d",cid);
        char *raw=cJSON_PrintUnformatted(secrets),*encrypted=raw?seal_secret(r,context,raw):NULL; wipe_secret(raw); delete_secret_json(secrets);
        if (!encrypted) { cJSON_Delete(config); return failure(r->error); }
        J *n=cJSON_CreateObject(); number(n,"id",cid); number(n,"version",old?integer(old,"version",0)+1:1); number(n,"enabled",integer(a,"enabled",1)); text(n,"name",str(a,"name")); text(n,"kind",str(a,"kind")); text(n,"ciphertext",encrypted); free(encrypted); item(n,"config",config);
        remove_id(channels,cid); cJSON_AddItemToArray(channels,n); return save_result(r,cid);
    }
    if (!strcmp(method,"remove_channel") || !strcmp(method,"clear_channel_credentials")) {
        int cid=integer(a,"id",0); J *c=find(channels,cid); if (!c || integer(c,"version",0)!=integer(a,"version",0)) return failure("Channel missing or changed");
        if (!strcmp(method,"remove_channel")) remove_id(channels,cid); else { text(c,"ciphertext",""); number(c,"enabled",0); number(c,"version",integer(c,"version",0)+1); } return save_result(r,cid);
    }
    if (!d) return failure("Device not found");
    if (!strcmp(method,"bind_channels")) {
        J *ids=cJSON_Parse(str(a,"ids")),*v; int valid=cJSON_IsArray(ids) && cJSON_GetArraySize(ids)<=32;
        cJSON_ArrayForEach(v,ids) if (!cJSON_IsNumber(v) || !find(channels,v->valueint)) valid=0;
        if (!valid) { cJSON_Delete(ids); return failure("Invalid channel selection"); }
        item(d,"channels",ids); return save_result(r,id);
    }
    if (!strcmp(method,"device_channels")) { J *out=cJSON_CreateArray(),*v; cJSON_ArrayForEach(v,array(d,"channels")) { J *b=cJSON_CreateObject(); number(b,"channel_id",v->valueint); cJSON_AddItemToArray(out,b); } return out; }
    J *s=runtime(r),*sd=state_device(s,id),*out=NULL;
    if (!strcmp(method,"sim_info")) {
        J *sim=cJSON_GetObjectItemCaseSensitive(sd,"sim"); out=cJSON_CreateObject(); item(out,"sim",sim?cJSON_Duplicate(sim,1):cJSON_CreateObject()); text(out,"iccid",str(sim,"iccid")); J *pin=pin_for(r,str(sim,"iccid"));
        cJSON_AddBoolToObject(out,"saved",pin && *str(pin,"ciphertext")); number(out,"auto_enabled",integer(pin,"auto_enabled",0));
    } else if (!strcmp(method,"messages")) {
        out=cJSON_CreateArray(); J *m; int offset=integer(a,"offset",0),limit=integer(a,"limit",50),count=0;
        if (offset<0 || limit<1 || limit>200 || integer(a,"after_id",0)<0 || strlen(str(a,"query"))>256 ||
            (*str(a,"direction") && strcmp(str(a,"direction"),"inbound") && strcmp(str(a,"direction"),"outbound"))) { cJSON_Delete(out); out=failure("Invalid pagination"); }
        else cJSON_ArrayForEach(m,cJSON_GetObjectItemCaseSensitive(sd,"messages")) {
            if (integer(m,"id",0)<=integer(a,"after_id",0)) continue;
            if ((*str(a,"direction") && strcmp(str(a,"direction"),str(m,"direction"))) || (!strstr(str(m,"text"),str(a,"query")) && !strstr(str(m,"number"),str(a,"query")))) continue;
            if (count++<offset || cJSON_GetArraySize(out)>=limit) continue;
            cJSON_AddItemToArray(out,cJSON_Duplicate(m,1));
        }
    } else if (!strcmp(method,"sync")) {
        /* An RPC only creates a wakeup; the daemon alone owns runtime snapshots. */
        char p[4096],name[40]; snprintf(name,sizeof(name),"sync-%d",id); path_join(p,r->runtime,name); int fd=open(p,O_CREAT|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600); if (fd>=0) close(fd); out=fd>=0?ok():failure("Cannot request refresh");
    } else if (!strcmp(method,"save_pin")) {
        char iccid[23]; normalize_iccid(str(a,"iccid"),iccid);
        if (!*iccid || strcmp(iccid,str(cJSON_GetObjectItemCaseSensitive(sd,"sim"),"iccid"))) out=failure("SIM changed; refresh before saving PIN");
        else {
            J *pins=array(r->config,"pins"),*pin=pin_for(r,iccid); int index=0;
            if (integer(a,"forget",0)) { J *v; cJSON_ArrayForEach(v,pins) { if (v==pin) { cJSON_DeleteItemFromArray(pins,index); break; } index++; } out=save_result(r,id); }
            else if (!*str(a,"pin") && pin) { number(pin,"device_id",id); number(pin,"auto_enabled",integer(a,"auto_enabled",0)); out=save_result(r,id); }
            else if (strlen(str(a,"pin"))<4 || strlen(str(a,"pin"))>8 || strspn(str(a,"pin"),"0123456789")!=strlen(str(a,"pin"))) out=failure("PIN must contain 4..8 digits");
            else {
                char context[40]; snprintf(context,sizeof(context),"pin-%s",iccid); char *encrypted=seal_secret(r,context,str(a,"pin"));
                if (!encrypted) out=failure(r->error); else { if (!pin) { pin=cJSON_CreateObject(); text(pin,"iccid",iccid); cJSON_AddItemToArray(pins,pin); } text(pin,"ciphertext",encrypted); free(encrypted); number(pin,"device_id",id); number(pin,"auto_enabled",integer(a,"auto_enabled",0)); out=save_result(r,id); }
            }
        }
    } else if (!strcmp(method,"send")) {
        int valid=valid_sms(d,str(a,"number"),str(a,"text")); out=valid?enqueue_action(r,a,"send"):failure("Invalid SMS number or text");
    } else if (!strcmp(method,"unlock_pin")) out=enqueue_action(r,a,"pin");
    else if (!strcmp(method,"delete_messages")) {
        J *ids=cJSON_Parse(str(a,"ids")),*v,*targets=cJSON_CreateArray(); int valid=cJSON_IsArray(ids) && cJSON_GetArraySize(ids)>0 && cJSON_GetArraySize(ids)<=100;
        cJSON_ArrayForEach(v,ids) { J *m=cJSON_IsNumber(v)?find(cJSON_GetObjectItemCaseSensitive(sd,"messages"),v->valueint):NULL; if (!m) valid=0; else cJSON_AddItemToArray(targets,cJSON_CreateString(str(m,"external_id"))); }
        if (strcmp(str(a,"scope"),"device")) valid=0;
        if (valid) { J *job=cJSON_Duplicate(a,1); item(job,"targets",targets); targets=NULL; out=enqueue_action(r,job,"delete"); cJSON_Delete(job); } else out=failure("Select 1..100 current device messages (scope=device)");
        cJSON_Delete(ids); cJSON_Delete(targets);
    } else if (!strcmp(method,"test_channel")) {
        J *c=find(channels,integer(a,"channel_id",0));
        if (!c || !integer(c,"enabled",1) || !bound(d,integer(c,"id",0))) out=failure("Bind an enabled channel to this device before testing");
        else if (!*str(a,"key") || strlen(str(a,"key"))>128) out=failure("Test key required");
        else { J *targets=cJSON_CreateArray(),*t=cJSON_CreateObject(),*m=cJSON_CreateObject(); number(t,"id",integer(c,"id",0)); number(t,"version",integer(c,"version",0)); cJSON_AddItemToArray(targets,t); text(m,"external_id",str(a,"key")); text(m,"number","SIM Relay"); text(m,"text","Test message"); out=enqueue_forward(r,d,m,targets,str(sd,"iccid"))?ok():failure("Cannot persist test message"); cJSON_Delete(m); cJSON_Delete(targets); }
    } else if (!strcmp(method,"send_history") || !strcmp(method,"operation_history") || !strcmp(method,"push_history")) {
        out=list_history(s,method,id);
        if (strcmp(method,"push_history")) {
            char path[4096],dirpath[4096]; path_join(dirpath,r->runtime,"actions"); DIR *dir=opendir(dirpath); struct dirent *e;
            if (dir) {
                while ((e=readdir(dir))) if (json_name(e->d_name)) {
                    path_join(path,dirpath,e->d_name); J *job=read_json(path);
                    if (job && integer(job,"device_id",0)==id && ((!strcmp(method,"send_history"))==(!strcmp(str(job,"kind"),"send")))) {
                        cJSON_DeleteItemFromObjectCaseSensitive(job,"ciphertext"); cJSON_AddItemToArray(out,job);
                    } else cJSON_Delete(job);
                }
                closedir(dir);
            }
        }
        if (!strcmp(method,"push_history")) {
            J *pending=pending_list(r,id),*j,*t; cJSON_ArrayForEach(j,pending) cJSON_ArrayForEach(t,cJSON_GetObjectItemCaseSensitive(j,"targets")) {
                J *v=cJSON_Duplicate(t,1),*c=find(channels,integer(t,"id",0)); text(v,"channel",str(c,"name")); text(v,"status","queued"); cJSON_AddItemToArray(out,v);
            } cJSON_Delete(pending);
        }
    } else out=failure("Method unavailable in lightweight mode");
    cJSON_Delete(s); return out;
}
