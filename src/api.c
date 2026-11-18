/* CGI endpoint for the separate, authenticated uhttpd instance. */
#include "relay.h"
#include <sodium.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <arpa/inet.h>

static const char *env(const char *name) { const char *s=getenv(name); return s?s:""; }
static int response(int code,const char *reason,J *body) {
    char *raw=body?cJSON_PrintUnformatted(body):NULL;
    printf("Status: %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n%s\n",code,reason,raw?raw:"{\"error\":\"Out of memory\"}");
    free(raw); cJSON_Delete(body); return 0;
}
static int error(int code,const char *reason,const char *message) { return response(code,reason,failure(message)); }
static int token_ok(J *api) {
    const char *provided=env("HTTP_AUTHORIZATION"),*stored=str(api,"token_hash");
    if (strncmp(provided,"Bearer ",7) || strlen(provided+7)!=64 || strlen(stored)!=64 || sodium_init()<0) return 0;
    unsigned char hash[crypto_hash_sha256_BYTES],expected[crypto_hash_sha256_BYTES]; size_t n=0;
    int valid=sodium_hex2bin(expected,sizeof(expected),stored,64,NULL,&n,NULL)==0 && n==sizeof(expected) &&
        crypto_hash_sha256(hash,(const unsigned char *)provided+7,64)==0 && sodium_memcmp(hash,expected,sizeof(hash))==0;
    sodium_memzero(hash,sizeof(hash)); sodium_memzero(expected,sizeof(expected)); return valid;
}
static int in_subnet(const char *remote,const char *cidr) {
    char network[128]; size_t len=strlen(cidr); if (!len || len>=sizeof(network)) return 0;
    memcpy(network,cidr,len+1); char *slash=strchr(network,'/'); if (!slash) return 0; *slash++=0;
    if (!*slash || strspn(slash,"0123456789")!=strlen(slash)) return 0;
    int family=strchr(network,':')?AF_INET6:AF_INET, bytes=family==AF_INET?4:16;
    long bits=strtol(slash,NULL,10); if (bits<0 || bits>bytes*8) return 0;
    unsigned char address[16],base[16];
    if (family==AF_INET && !strncmp(remote,"::ffff:",7)) remote+=7;
    if (inet_pton(family,remote,address)!=1 || inet_pton(family,network,base)!=1) return 0;
    for (int i=0;i<bytes;i++) {
        int remaining=(int)bits-i*8; unsigned mask=remaining>=8?255U:remaining<=0?0U:(255U<<(8-remaining))&255U;
        if ((address[i]&mask)!=(base[i]&mask)) return 0;
    }
    return 1;
}
static int lan_client(const char *root) {
    char path[4096]; int n=snprintf(path,sizeof(path),"%s/allowed-subnets",root);
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    FILE *file=fopen(path,"r"); if (!file) return 0;
    char line[128]; int allowed=0;
    while (fgets(line,sizeof(line),file)) { line[strcspn(line,"\r\n")]=0; if (in_subnet(env("REMOTE_ADDR"),line)) { allowed=1; break; } }
    fclose(file); return allowed;
}
static int key_ok(const char *key) {
    size_t n=strlen(key); return n>0 && n<=128 && strspn(key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==n;
}
static int contains_nul_escape(const char *input,size_t n) {
    for (size_t i=0;i<n;i++) if (input[i]=='\\') {
        if (i+5<n && !memcmp(input+i,"\\u0000",6)) return 1;
        i++;
    }
    return 0;
}
static J *body_json(void) {
    const char *type=env("CONTENT_TYPE"),*length=env("CONTENT_LENGTH");
    if (strncmp(type,"application/json",16) || (type[16] && type[16]!=';' && type[16]!=' ') || !*length || strspn(length,"0123456789")!=strlen(length)) return NULL;
    char *end; unsigned long n=strtoul(length,&end,10);
    if (*end || !n || n>16384) return NULL;
    char *raw=calloc(n+1,1); if (!raw) return NULL;
    size_t used=fread(raw,1,n,stdin);
    J *body=(used==n && !memchr(raw,0,n) && !contains_nul_escape(raw,n))?cJSON_ParseWithLengthOpts(raw,n+1,NULL,1):NULL;
    erase_memory(raw,n); free(raw); return body;
}
static int valid_body(J *body) {
    if (!cJSON_IsObject(body)) return 0;
    J *v,*other; cJSON_ArrayForEach(v,body) {
        if (!v->string || (strcmp(v->string,"device_id") && strcmp(v->string,"request_id") && strcmp(v->string,"number") && strcmp(v->string,"text"))) return 0;
        for (other=v->next;other;other=other->next) if (other->string && !strcmp(v->string,other->string)) return 0;
    }
    J *id=cJSON_GetObjectItemCaseSensitive(body,"device_id"),*key=cJSON_GetObjectItemCaseSensitive(body,"request_id");
    J *number=cJSON_GetObjectItemCaseSensitive(body,"number"),*text=cJSON_GetObjectItemCaseSensitive(body,"text");
    return cJSON_IsNumber(id) && id->valuedouble==id->valueint && id->valueint>0 && cJSON_IsString(key) && key_ok(key->valuestring) && cJSON_IsString(number) && cJSON_IsString(text);
}
static int query(const char *query,int *device,char key[129]) {
    if (strlen(query)>256) return 0;
    char buf[257]; snprintf(buf,sizeof(buf),"%s",query); char *save=NULL,*part=strtok_r(buf,"&",&save); int got_id=0,got_key=0;
    while (part) {
        if (!strncmp(part,"device_id=",10) && !got_id++) {
            const char *v=part+10; if (!*v || strspn(v,"0123456789")!=strlen(v)) return 0;
            char *end; long id=strtol(v,&end,10); if (*end || id<1 || id>INT_MAX) return 0; *device=(int)id;
        } else if (!strncmp(part,"request_id=",11) && !got_key++) {
            const char *v=part+11; if (!key_ok(v)) return 0; snprintf(key,129,"%s",v);
        } else return 0;
        part=strtok_r(NULL,"&",&save);
    }
    return got_id==1 && got_key==1;
}
static J *public_job(J *job,const char *key,int id) {
    J *out=cJSON_CreateObject();
    cJSON_AddStringToObject(out,"request_id",key); cJSON_AddNumberToObject(out,"device_id",id);
    cJSON_AddStringToObject(out,"status",str(job,"status"));
    if (*str(job,"error")) cJSON_AddStringToObject(out,"error",str(job,"error"));
    return out;
}
int api_serve(Relay *r) {
    J *api=cJSON_GetObjectItemCaseSensitive(r->config,"api");
    if (!integer(api,"enabled",0)) return error(404,"Not Found","API disabled");
    if (strcmp(str(api,"scope"),"lan") && strcmp(str(api,"scope"),"any")) return error(503,"Service Unavailable","Invalid API configuration");
    if (strcmp(str(api,"transport"),"http") && strcmp(str(api,"transport"),"https")) return error(503,"Service Unavailable","Invalid API configuration");
    char expected[4096]; int n=snprintf(expected,sizeof(expected),"%s/api-%d",r->runtime,integer(api,"revision",0));
    if (n<0 || (size_t)n>=sizeof(expected) || integer(api,"revision",0)<=0 || strcmp(env("DOCUMENT_ROOT"),expected))
        return error(503,"Service Unavailable","API configuration changed; service restarting");
    if (!strcmp(str(api,"scope"),"lan") && !lan_client(expected)) return error(403,"Forbidden","Client is outside the LAN");
    if (strcmp(str(api,"transport"),"http") && strcmp(env("HTTPS"),"on")) return error(403,"Forbidden","HTTPS required");
    if (!token_ok(api)) return error(401,"Unauthorized","Invalid API token");
    const char *method=env("REQUEST_METHOD"),*path=env("PATH_INFO");
    if (!strcmp(method,"GET") && !strcmp(path,"/v1/devices")) {
        J *items=cJSON_CreateArray(),*d;
        cJSON_ArrayForEach(d,cJSON_GetObjectItemCaseSensitive(r->config,"devices")) {
            J *row=cJSON_CreateObject(); cJSON_AddNumberToObject(row,"id",integer(d,"id",0));
            cJSON_AddStringToObject(row,"name",str(d,"name")); cJSON_AddBoolToObject(row,"enabled",integer(d,"enabled",1));
            cJSON_AddStringToObject(row,"adapter",str(d,"adapter")); cJSON_AddItemToArray(items,row);
        }
        J *out=cJSON_CreateObject(); cJSON_AddItemToObject(out,"items",items); return response(200,"OK",out);
    }
    if (!strcmp(method,"POST") && !strcmp(path,"/v1/messages")) {
        J *body=body_json(); if (!valid_body(body)) { delete_secret_json(body); return error(400,"Bad Request","Invalid JSON or message fields"); }
        if (!api_reserve_send(r)) { delete_secret_json(body); return error(429,"Too Many Requests","API rate or queue limit reached"); }
        int id=integer(body,"device_id",0); const char *key=str(body,"request_id");
        J *args=cJSON_CreateObject(); cJSON_AddNumberToObject(args,"device_id",id); cJSON_AddStringToObject(args,"key",key);
        cJSON_AddStringToObject(args,"number",str(body,"number")); cJSON_AddStringToObject(args,"text",str(body,"text"));
        J *job=dispatch(r,"send",args),*out=NULL; int failed=!job || !*str(job,"request");
        if (failed) out=failure(job?str(job,"error"):"Cannot queue message"); else out=public_job(job,key,id);
        cJSON_Delete(job); delete_secret_json(args); delete_secret_json(body);
        return response(failed?400:202,failed?"Bad Request":"Accepted",out);
    }
    if (!strcmp(method,"GET") && !strcmp(path,"/v1/messages")) {
        int id=0; char key[129],request[180]; if (!query(env("QUERY_STRING"),&id,key)) return error(400,"Bad Request","device_id and request_id required");
        J *args=cJSON_CreateObject(); cJSON_AddNumberToObject(args,"device_id",id);
        J *jobs=dispatch(r,"send_history",args),*job,*found=NULL; cJSON_Delete(args);
        snprintf(request,sizeof(request),"%d:send:%s",id,key);
        cJSON_ArrayForEach(job,jobs) if (!strcmp(str(job,"request"),request)) { found=job; break; }
        J *out=found?public_job(found,key,id):NULL; cJSON_Delete(jobs);
        return out?response(200,"OK",out):error(404,"Not Found","Request not found");
    }
    return error(404,"Not Found","Endpoint not found");
}
