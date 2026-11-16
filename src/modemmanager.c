/* ModemManager integration through bounded mmcli calls; no shell or modem drivers. */
#include "relay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>

#define MMCLI "/usr/bin/mmcli"
#define PIN_HELPER "/usr/libexec/sim-relay-mm-pin"
static int object_path(const char *path,const char *kind) {
    char prefix[96]; snprintf(prefix,sizeof(prefix),"/org/freedesktop/ModemManager1/%s/",kind);
    size_t n=strlen(prefix); return !strncmp(path,prefix,n) && path[n] && strlen(path+n)<=12 && strspn(path+n,"0123456789")==strlen(path+n);
}
static char *execute(const char *binary,const char *const args[],const char *input,double deadline,int *success,char *error,size_t size) {
    *success=0; int out[2],in[2]; if (pipe(out)<0) return NULL;
    if (pipe(in)<0) { close(out[0]); close(out[1]); return NULL; }
    pid_t pid=fork();
    if (pid<0) { close(out[0]); close(out[1]); close(in[0]); close(in[1]); return NULL; }
    if (!pid) {
        setpgid(0,0); close(out[0]); close(in[1]); dup2(out[1],STDOUT_FILENO); dup2(in[0],STDIN_FILENO); close(out[1]); close(in[0]);
        int null=open("/dev/null",O_WRONLY); if (null>=0) { dup2(null,STDERR_FILENO); close(null); }
        execv(binary,(char *const *)args); _exit(127);
    }
    close(out[1]); close(in[0]);
    /* Only the PIN helper reads stdin: <=8 digits, safely within pipe capacity. */
    if (input) { struct sigaction ignore={0},old; ignore.sa_handler=SIG_IGN; sigaction(SIGPIPE,&ignore,&old); ssize_t n=write(in[1],input,strlen(input)); (void)n; sigaction(SIGPIPE,&old,NULL); }
    close(in[1]); fcntl(out[0],F_SETFL,O_NONBLOCK);
    char *raw=calloc(1,1); size_t used=0; int status=0,finished=0,valid=raw!=NULL;
    while (!finished && valid && relay_monotonic()<deadline) {
        char buf[4096]; ssize_t n;
        while ((n=read(out[0],buf,sizeof(buf)))>0) {
            if ((size_t)n>2*1024*1024-used) { valid=0; break; }
            char *more=realloc(raw,used+(size_t)n+1); if (!more) { valid=0; break; }
            raw=more; memcpy(raw+used,buf,(size_t)n); used+=(size_t)n; raw[used]=0;
        }
        pid_t result=waitpid(pid,&status,WNOHANG);
        if (result==pid) { finished=1; /* Drain the final output after exit. */
            while (valid && (n=read(out[0],buf,sizeof(buf)))>0) {
                if ((size_t)n>2*1024*1024-used) { valid=0; break; }
                char *more=realloc(raw,used+(size_t)n+1); if (!more) { valid=0; break; }
                raw=more; memcpy(raw+used,buf,(size_t)n); used+=(size_t)n; raw[used]=0;
            }
        } else if (result<0 && errno!=EINTR) break;
        else { struct pollfd p={out[0],POLLIN,0}; poll(&p,1,20); }
    }
    if (!finished) { kill(-pid,SIGKILL); kill(pid,SIGKILL); while (waitpid(pid,&status,0)<0 && errno==EINTR) {} }
    close(out[0]);
    *success=valid && finished && WIFEXITED(status) && WEXITSTATUS(status)==0;
    if (!*success) { free(raw); snprintf(error,size,"ModemManager command failed, unavailable or timed out"); return NULL; }
    return raw;
}
static const char *program(int pin) {
#ifdef RELAY_TESTING
    const char *fixture=getenv(pin?"SIM_RELAY_TEST_PIN_HELPER":"SIM_RELAY_TEST_MMCLI"); if (fixture && *fixture) return fixture;
#endif
    return pin?PIN_HELPER:MMCLI;
}
static J *command(const char *option,const char *target,const char *action,const char *extra,double deadline,char *error,size_t size) {
    const char *args[10]={program(0),"--output-json",option,target,NULL}; int n=option?4:2;
    if (action) args[n++]=action;
    if (extra) args[n++]=extra;
    args[n]=NULL; int success; char *raw=execute(program(0),args,NULL,deadline,&success,error,size);
    if (!success) return NULL;
    J *out=*raw?cJSON_Parse(raw):cJSON_CreateObject(); free(raw);
    if (!out) snprintf(error,size,"Invalid mmcli JSON output");
    return out;
}
static int action(const char *option,const char *target,const char *verb,const char *extra,const char *pin,double deadline,char *error,size_t size) {
    const char *args[8]={program(pin!=NULL),"--output-json",option,target,verb,extra,NULL};
    const char *pin_args[]={program(1),target,NULL}; int success;
    char *raw=execute(program(pin!=NULL),pin?pin_args:args,pin,deadline,&success,error,size); free(raw); return success;
}
static J *field(J *j,const char *name) { return cJSON_GetObjectItemCaseSensitive(j,name); }
static const char *value(J *j,const char *name) { const char *s=str(j,name); return !strcmp(s,"--")?"":s; }
static J *describe(J *data) {
    J *generic=field(data,"generic"); const char *equipment=value(generic,"equipment-identifier");
    if (!*equipment || strlen(equipment)>100 || strspn(equipment,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_:")!=strlen(equipment)) return NULL;
    J *out=cJSON_CreateObject(); char identity[128]; snprintf(identity,sizeof(identity),"mm:%s",equipment);
    cJSON_AddStringToObject(out,"hardware_id",identity); cJSON_AddStringToObject(out,"modem",str(data,"dbus-path"));
    cJSON_AddStringToObject(out,"name",value(generic,"model")); cJSON_AddStringToObject(out,"manufacturer",value(generic,"manufacturer"));
    cJSON_AddStringToObject(out,"state",value(generic,"state")); return out;
}
static J *discover(double deadline,char *error,size_t size) {
    J *list=command(NULL,NULL,"-L",NULL,deadline,error,size); if (!list) return NULL;
    J *paths=field(list,"modem-list"),*p,*out=cJSON_CreateArray();
    if (!cJSON_IsArray(paths)) { cJSON_Delete(out); cJSON_Delete(list); snprintf(error,size,"Invalid modem list"); return NULL; }
    int count=0;
    cJSON_ArrayForEach(p,paths) {
        if (++count>32) break;
        if (!cJSON_IsString(p) || !object_path(p->valuestring,"Modem")) continue;
        J *raw=command("-m",p->valuestring,NULL,NULL,deadline,error,size),*d=describe(field(raw,"modem"));
        if (d) { cJSON_DeleteItemFromObjectCaseSensitive(d,"modem"); cJSON_AddStringToObject(d,"modem",p->valuestring); cJSON_AddItemToArray(out,d); }
        cJSON_Delete(raw);
    }
    cJSON_Delete(list); return out;
}
J *mm_discover(char *error,size_t size) {
    if (access(program(0),X_OK)!=0) { snprintf(error,size,"ModemManager is not installed. Install the modemmanager package before adding a local cellular modem."); return NULL; }
    J *out=discover(relay_monotonic()+45,error,size);
    if (!out) snprintf(error,size,"Cannot access ModemManager. Make sure the modemmanager package is installed and running.");
    return out;
}
static J *resolve(J *device,double deadline,char *error,size_t size) {
    J *all=discover(deadline,error,size),*d,*match=NULL; int count=0;
    const char *identity=str(device,"hardware_id"),*selector=str(device,"modem");
    cJSON_ArrayForEach(d,all) if (*identity?!strcmp(identity,str(d,"hardware_id")):(!*selector || !strcmp(selector,"any") || !strcmp(selector,str(d,"modem")))) { match=d; count++; }
    J *out=count==1?cJSON_Duplicate(match,1):NULL; cJSON_Delete(all);
    if (!out) snprintf(error,size,"Selected modem absent or ambiguous");
    return out;
}
J *mm_sim(J *device,double deadline,char *error,size_t size) {
    J *d=resolve(device,deadline,error,size); if (!d) return NULL;
    J *raw=command("-m",str(d,"modem"),NULL,NULL,deadline,error,size),*modem=field(raw,"modem"),*g=field(modem,"generic");
    J *checked=describe(modem);
    if (!checked || strcmp(str(checked,"hardware_id"),str(d,"hardware_id"))) { cJSON_Delete(checked); cJSON_Delete(raw); cJSON_Delete(d); snprintf(error,size,"Modem identity changed"); return NULL; }
    cJSON_Delete(checked); const char *sim=value(g,"sim"); J *info=object_path(sim,"SIM")?command("-i",sim,NULL,NULL,deadline,error,size):NULL;
    char iccid[23]; normalize_iccid(value(field(field(info,"sim"),"properties"),"iccid"),iccid);
    cJSON_AddStringToObject(d,"iccid",iccid); cJSON_AddStringToObject(d,"sim_path",sim); cJSON_AddBoolToObject(d,"present",object_path(sim,"SIM"));
    const char *lock=value(g,"unlock-required"),*state=value(g,"state");
    /* mmcli prints -- for no pending lock, including disabled modems after SendPin.
       An absent value while locked/initializing is not proof of successful unlock. */
    if (!*lock) {
        const char *ready[]={"disabled","enabling","enabled","searching","registered","disconnecting","connecting","connected"};
        lock="unknown";
        for (size_t i=0;i<sizeof(ready)/sizeof(ready[0]);i++)
            if (*iccid && !strcmp(state,ready[i])) { lock="none"; break; }
    }
    cJSON_AddStringToObject(d,"unlock_required",lock); cJSON_AddStringToObject(d,"operator",value(field(modem,"3gpp"),"operator-name"));
    J *v; int retries=-1;
    cJSON_ArrayForEach(v,field(g,"unlock-retries")) if (cJSON_IsString(v)) { int n; char tail; if (sscanf(v->valuestring,"sim-pin (%d)%c",&n,&tail)==1 && n>=0 && n<=3) retries=n; }
    cJSON_AddBoolToObject(d,"pin_helper_available",access(program(1),X_OK)==0);
    cJSON_AddNumberToObject(d,"pin_retries",retries); cJSON_Delete(info); cJSON_Delete(raw); return d;
}
static J *read_sms(const char *path,double deadline,char *error,size_t size) {
    if (!object_path(path,"SMS")) return NULL;
    J *raw=command("-s",path,NULL,NULL,deadline,error,size),*sms=field(raw,"sms"),*content=field(sms,"content"),*props=field(sms,"properties");
    const char *state=value(props,"state"),*number=value(content,"number"),*text=value(content,"text"),*stamp=value(props,"timestamp");
    if (!sms || strlen(text)>32768 || strlen(number)>128) { cJSON_Delete(raw); return NULL; }
    const char *direction=(!strcmp(state,"received") || !strcmp(state,"receiving"))?"inbound":"outbound";
    /* Object paths can change after restart. Received timestamp/content define identity. */
    J *parts=cJSON_CreateArray(); cJSON_AddItemToArray(parts,cJSON_CreateString(number)); cJSON_AddItemToArray(parts,cJSON_CreateString(text));
    cJSON_AddItemToArray(parts,cJSON_CreateString(direction)); cJSON_AddItemToArray(parts,cJSON_CreateString(*stamp?stamp:path));
    char *serialized=cJSON_PrintUnformatted(parts),key[69]; int valid=serialized && message_identity(serialized,key); free(serialized); cJSON_Delete(parts);
    J *out=valid?cJSON_CreateObject():NULL;
    if (out) { cJSON_AddStringToObject(out,"external_id",key); cJSON_AddStringToObject(out,"_mm_path",path); cJSON_AddStringToObject(out,"number",number); cJSON_AddStringToObject(out,"text",text); cJSON_AddStringToObject(out,"timestamp",stamp); cJSON_AddStringToObject(out,"direction",direction); cJSON_AddStringToObject(out,"state",state); }
    cJSON_Delete(raw); return out;
}
J *mm_snapshot(J *device,double deadline,char *error,size_t size) {
    J *sim=mm_sim(device,deadline,error,size); if (!sim) return NULL;
    if (strcmp(str(sim,"unlock_required"),"none") || !*str(sim,"iccid")) { cJSON_Delete(sim); snprintf(error,size,"SIM absent or locked"); return NULL; }
    if (!strcmp(str(sim,"state"),"disabled")) {
        if (!action("-m",str(sim,"modem"),"--enable",NULL,NULL,deadline,error,size)) {
            cJSON_Delete(sim); snprintf(error,size,"Unable to enable modem in ModemManager"); return NULL;
        }
        J *fresh=mm_sim(device,deadline,error,size);
        int valid=fresh && !strcmp(str(fresh,"hardware_id"),str(sim,"hardware_id")) &&
            !strcmp(str(fresh,"iccid"),str(sim,"iccid")) && !strcmp(str(fresh,"unlock_required"),"none");
        cJSON_Delete(sim); sim=fresh;
        if (!valid) { cJSON_Delete(sim); snprintf(error,size,"Modem or SIM changed while enabling"); return NULL; }
    }
    J *raw=command("-m",str(sim,"modem"),"--messaging-list-sms",NULL,deadline,error,size),*paths=field(raw,"modem.messaging.sms"),*p;
    if (!cJSON_IsArray(paths) || cJSON_GetArraySize(paths)>4096) { cJSON_Delete(raw); cJSON_Delete(sim); snprintf(error,size,"Invalid SMS list"); return NULL; }
    J *messages=cJSON_AddArrayToObject(sim,"messages");
    cJSON_ArrayForEach(p,paths) {
        J *m=cJSON_IsString(p)?read_sms(p->valuestring,deadline,error,size):NULL;
        if (!m) { cJSON_Delete(raw); cJSON_Delete(sim); return NULL; }
        /* Only complete received messages may enter the forwarding cursor. */
        if (!strcmp(str(m,"state"),"receiving")) { cJSON_Delete(m); cJSON_Delete(raw); cJSON_Delete(sim); snprintf(error,size,"Waiting for a complete multipart SMS"); return NULL; }
        cJSON_AddItemToArray(messages,m);
    }
    cJSON_Delete(raw); return sim;
}
int valid_sms(J *device,const char *number,const char *text) {
    if (!strcmp(str(device,"adapter"),"modemmanager")) { const char *n=*number=='+'?number+1:number; return strlen(n)>=3 && strlen(n)<=20 && strspn(n,"0123456789")==strlen(n) && *text && strlen(text)<=8192; }
    char *hex=NULL; const char *encoding=NULL; int valid=encode_sms(number,text,&hex,&encoding); free(hex); return valid;
}
static int same(J *sim,J *job) { return sim && !strcmp(str(sim,"hardware_id"),str(job,"hardware_id")) && *str(job,"iccid") && !strcmp(str(sim,"iccid"),str(job,"iccid")); }
J *mm_send(J *device,J *job,int (*commit)(void *),void *ctx,int *outcome,char *error,size_t size) {
    *outcome=0; double deadline=relay_monotonic()+120; J *sim=mm_sim(device,deadline,error,size),*out=NULL;
    if (!same(sim,job) || !valid_sms(device,str(job,"number"),str(job,"text"))) { snprintf(error,size,"SIM changed or invalid SMS"); goto end; }
    char path[]="/tmp/sim-relay-sms.XXXXXX"; int fd=mkstemp(path); if (fd<0) goto end;
    size_t n=strlen(str(job,"text")); int written=write(fd,str(job,"text"),n)==(ssize_t)n; close(fd);
    char number[80],file[256]; snprintf(number,sizeof(number),"--messaging-create-sms=number=%s",str(job,"number")); snprintf(file,sizeof(file),"--messaging-create-sms-with-text=%s",path);
    J *created=written?command("-m",str(sim,"modem"),number,file,deadline,error,size):NULL; unlink(path);
    const char *sms=str(field(field(created,"modem"),"messaging"),"created-sms");
    if (!object_path(sms,"SMS")) { cJSON_Delete(created); goto end; }
    J *guard=mm_sim(device,deadline,error,size); int ready=same(guard,job) && !strcmp(str(guard,"modem"),str(sim,"modem")); cJSON_Delete(guard);
    if (ready && commit(ctx)) { *outcome=1; if (action("-s",sms,"--send",NULL,NULL,deadline,error,size)) { out=read_sms(sms,deadline,error,size); if (out && !strcmp(str(out,"state"),"sent")) *outcome=3; else { cJSON_Delete(out); out=NULL; } } }
    cJSON_Delete(created);
end:
    cJSON_Delete(sim); return out;
}
int mm_operation(J *device,J *job,const char *pin,int (*commit)(void *),void *ctx,char *error,size_t size) {
    double deadline=relay_monotonic()+120; J *sim=mm_sim(device,deadline,error,size); int outcome=0;
    if (!same(sim,job)) { snprintf(error,size,"Modem or SIM changed"); goto end; }
    if (!strcmp(str(job,"kind"),"pin")) {
        int tries=integer(sim,"pin_retries",-1);
        if (!pin || strlen(pin)<4 || strlen(pin)>8 || strspn(pin,"0123456789")!=strlen(pin) || strcmp(str(sim,"unlock_required"),"sim-pin") || tries<=0 || (!integer(job,"force",0) && tries!=3)) { snprintf(error,size,"PIN retry guard rejected operation"); goto end; }
        if (access(program(1),X_OK)!=0) { snprintf(error,size,"Install the optional sim-relay-modemmanager package to enable PIN unlock."); goto end; }
        if (!commit(ctx)) goto end;
        outcome=1;
        if (action(NULL,str(sim,"sim_path"),NULL,NULL,pin,deadline,error,size)) {
            J *after=mm_sim(device,deadline,error,size);
            if (same(after,job) && !strcmp(str(after,"unlock_required"),"none")) outcome=3;
            else snprintf(error,size,"PIN result not yet confirmed by ModemManager; do not retry automatically");
            cJSON_Delete(after);
        }
    } else {
        J *snapshot=mm_snapshot(device,deadline,error,size),*m,*target=NULL; int count=0;
        cJSON_ArrayForEach(m,field(snapshot,"messages")) if (!strcmp(str(m,"external_id"),str(job,"target"))) { target=m; count++; }
        if (same(snapshot,job) && count==1) {
            J *guard=read_sms(str(target,"_mm_path"),deadline,error,size);
            if (guard && !strcmp(str(guard,"external_id"),str(job,"target")) && commit(ctx)) {
                char option[160]; snprintf(option,sizeof(option),"--messaging-delete-sms=%s",str(target,"_mm_path")); outcome=1;
                if (action("-m",str(snapshot,"modem"),option,NULL,NULL,deadline,error,size)) outcome=3;
            }
            cJSON_Delete(guard);
        } else snprintf(error,size,"SMS target missing or ambiguous");
        cJSON_Delete(snapshot);
    }
end:
    cJSON_Delete(sim); return outcome;
}
