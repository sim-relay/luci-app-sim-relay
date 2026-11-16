#include "relay.h"
#include <curl/curl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

static int feishu_sign(const char *secret, char output[45]) {
    unsigned char key[64]={0},inner[64],digest[32],outer[96];
    size_t n=strlen(secret);
    if (n>64) { if (crypto_hash_sha256(key,(const unsigned char *)secret,n)) return 0; }
    else memcpy(key,secret,n);
    for (int i=0;i<64;i++) { inner[i]=key[i]^0x36; outer[i]=key[i]^0x5c; }
    if (crypto_hash_sha256(digest,inner,sizeof(inner))) return 0;
    memcpy(outer+64,digest,32); if (crypto_hash_sha256(digest,outer,sizeof(outer))) return 0;
    char *encoded=base64_text(digest,sizeof(digest));
    erase_memory(key,sizeof(key)); erase_memory(inner,sizeof(inner)); erase_memory(outer,sizeof(outer)); erase_memory(digest,sizeof(digest));
    if (!encoded) return 0;
    snprintf(output,45,"%s",encoded); free(encoded); return 1;
}
typedef struct { char *data; size_t size; } Response;
static size_t receive(void *p, size_t size, size_t nmemb, void *ctx) {
    Response *r=ctx;
    if (size && nmemb>262144/size) return 0;
    size_t n=size*nmemb; if (n>262144-r->size) return 0;
    char *data=realloc(r->data,r->size+n+1); if (!data) return 0;
    r->data=data; memcpy(data+r->size,p,n); r->size+=n; data[r->size]=0; return n;
}
static char *quote_config(const char *text) {
    size_t n=strlen(text); char *out=malloc(2*n+3); if (!out) return NULL;
    char *p=out; *p++='"';
    for (;*text;text++) { if (*text=='"' || *text=='\\') *p++='\\'; *p++=*text; }
    *p++='"'; *p=0; return out;
}
static int send_mail(J *c, J *s, const char *text, char *error, size_t size) {
    if (access("/usr/bin/msmtp",X_OK)!=0) { snprintf(error,size,"smtp_helper_missing"); return 0; }
    char *encoded=base64_text((const unsigned char *)text,strlen(text));
    if (!encoded) return 0;
    char mail[16384];
    int length=snprintf(mail,sizeof(mail),"From: <%s>\r\nTo: <%s>\r\nSubject: SIM Relay SMS\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n",str(c,"from"),str(c,"to"));
    int valid=length>=0 && (size_t)length<sizeof(mail);
    for (size_t i=0,n=strlen(encoded);valid && i<n;i+=76) {
        int wrote=snprintf(mail+length,sizeof(mail)-(size_t)length,"%.*s\r\n",(int)(n-i<76?n-i:76),encoded+i);
        if (wrote<0 || (size_t)wrote>=sizeof(mail)-(size_t)length) valid=0;
        else length+=wrote;
    }
    free(encoded); if (!valid) { snprintf(error,size,"smtp_message_too_large"); return 0; }
    char config_path[]="/tmp/sim-relay-smtp.XXXXXX";
    int fd=mkstemp(config_path); if (fd<0) return 0;
    char *user=quote_config(str(c,"username")),*password=quote_config(str(s,"password"));
    if (!user || !password) { free(user); wipe_secret(password); close(fd); unlink(config_path); return 0; }
    FILE *file=fdopen(fd,"w");
    if (!file) { free(user); wipe_secret(password); close(fd); unlink(config_path); return 0; }
    fprintf(file,"account default\nhost %s\nport %d\ntimeout 20\nauth on\nuser %s\npassword %s\nfrom %s\ntls on\ntls_starttls %s\ntls_trust_file /etc/ssl/certs/ca-certificates.crt\n",
        str(c,"host"),integer(c,"port",587),user,password,str(c,"from"),!strcmp(str(c,"tls_mode"),"starttls")?"on":"off");
    free(user); wipe_secret(password);
    int ok=!ferror(file) && fflush(file)==0 && fsync(fd)==0; fclose(file);
    int config_fd=ok?open(config_path,O_RDONLY|O_NOFOLLOW):-1;
    unlink(config_path); /* Only an inherited descriptor retains the plaintext configuration. */
    if (config_fd<0) return 0;
    int pipefd[2]; if (pipe(pipefd)<0) { close(config_fd); return 0; }
    pid_t pid=fork();
    if (pid==0) {
        close(pipefd[1]); dup2(pipefd[0],STDIN_FILENO); close(pipefd[0]);
        int null=open("/dev/null",O_WRONLY); if (null>=0) { dup2(null,STDOUT_FILENO); dup2(null,STDERR_FILENO); close(null); }
        char source[64]; snprintf(source,sizeof(source),"/proc/self/fd/%d",config_fd);
        execl("/usr/bin/msmtp","msmtp","--file",source,"--account=default","--",str(c,"to"),(char *)NULL); _exit(127);
    }
    close(config_fd); close(pipefd[0]);
    if (pid<0) { close(pipefd[1]); return 0; }
    fcntl(pipefd[1],F_SETFL,O_NONBLOCK);
    struct sigaction old,ignore={0}; ignore.sa_handler=SIG_IGN; sigemptyset(&ignore.sa_mask); sigaction(SIGPIPE,&ignore,&old);
    double deadline=relay_monotonic()+30; int status=0,finished=0; size_t sent=0;
    while (relay_monotonic()<deadline && sent<(size_t)length) {
        ssize_t n=write(pipefd[1],mail+sent,(size_t)length-sent);
        if (n>0) sent+=(size_t)n;
        else if (n<0 && errno!=EINTR && errno!=EAGAIN) break;
        if (waitpid(pid,&status,WNOHANG)==pid) { finished=1; break; }
        if (n<=0) { struct timespec delay={0,100000000}; nanosleep(&delay,NULL); }
    }
    close(pipefd[1]); sigaction(SIGPIPE,&old,NULL); erase_memory(mail,sizeof(mail));
    while (!finished && relay_monotonic()<deadline) {
        if (waitpid(pid,&status,WNOHANG)==pid) { finished=1; break; }
        struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    }
    if (!finished) { kill(pid,SIGKILL); waitpid(pid,&status,0); }
    if (sent==(size_t)length && finished && WIFEXITED(status) && WEXITSTATUS(status)==0) return 3;
    snprintf(error,size,"smtp_result_unconfirmed"); return 1;
}
int push_deliver(const char *kind, J *c, J *s, const char *text, int *retry_after, char *error, size_t size) {
    if (!push_validate(kind,c,s)) { snprintf(error,size,"invalid_push_configuration"); return 0; }
    if (!strcmp(kind,"smtp")) return send_mail(c,s,text,error,size);
    char url[2300]; J *body=cJSON_CreateObject();
    if (!strcmp(kind,"telegram")) {
        if (strlen(text)>4096) { cJSON_Delete(body); snprintf(error,size,"telegram_text_too_long"); return 0; }
        snprintf(url,sizeof(url),"https://api.telegram.org/bot%s/sendMessage",str(s,"token"));
        cJSON_AddStringToObject(body,"chat_id",str(c,"chat_id")); cJSON_AddStringToObject(body,"text",text);
    } else {
        snprintf(url,sizeof(url),"%s",str(s,"webhook_url"));
        if (!strcmp(kind,"wecom") && strlen(text)>2048) { cJSON_Delete(body); snprintf(error,size,"wecom_text_too_long"); return 0; }
        cJSON_AddStringToObject(body,!strcmp(kind,"wecom")?"msgtype":"msg_type","text");
        J *content=cJSON_AddObjectToObject(body,!strcmp(kind,"wecom")?"text":"content");
        cJSON_AddStringToObject(content,!strcmp(kind,"wecom")?"content":"text",text);
        if (!strcmp(kind,"feishu") && (integer(c,"signing_enabled",0) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"signing_enabled")))) {
            char stamp[40],key[2200],signature[45]; snprintf(stamp,sizeof(stamp),"%lld",(long long)time(NULL));
            snprintf(key,sizeof(key),"%s\n%s",stamp,str(s,"signing_secret"));
            if (!feishu_sign(key,signature)) { cJSON_Delete(body); erase_memory(key,sizeof(key)); return 0; }
            erase_memory(key,sizeof(key)); cJSON_AddStringToObject(body,"timestamp",stamp); cJSON_AddStringToObject(body,"sign",signature);
        }
    }
    char *encoded=cJSON_PrintUnformatted(body); cJSON_Delete(body);
    CURL *curl=curl_easy_init(); if (!curl || !encoded) { if (curl) curl_easy_cleanup(curl); free(encoded); return 0; }
    Response response={0}; struct curl_slist *headers=curl_slist_append(NULL,"Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl,CURLOPT_URL,url); curl_easy_setopt(curl,CURLOPT_POSTFIELDS,encoded);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers); curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(curl,CURLOPT_TIMEOUT,20L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L); curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
    CURLcode rc=curl_easy_perform(curl); long http=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
    curl_slist_free_all(headers); curl_easy_cleanup(curl); free(encoded);
    J *result=response.data && !memchr(response.data,0,response.size)?cJSON_ParseWithLengthOpts(response.data,response.size+1,NULL,1):NULL; free(response.data);
    int outcome=1;
    if (rc==CURLE_OK && http>=400 && http<500 && http!=408) outcome=2;
    if (rc==CURLE_OK && http==200 && result) {
        if (!strcmp(kind,"telegram")) {
            J *ok=cJSON_GetObjectItemCaseSensitive(result,"ok");
            if (cJSON_IsTrue(ok) && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(result,"result"))) outcome=3;
            else if (cJSON_IsFalse(ok) && integer(result,"error_code",0)>0) outcome=2;
            J *parameters=cJSON_GetObjectItemCaseSensitive(result,"parameters");
            int delay=integer(parameters,"retry_after",0); if (delay>0) *retry_after=delay>3600?3600:delay;
        } else {
            J *code=cJSON_GetObjectItemCaseSensitive(result,!strcmp(kind,"wecom")?"errcode":"code");
            if (cJSON_IsNumber(code)) outcome=code->valueint==0?3:2;
        }
    }
    cJSON_Delete(result);
    if (outcome!=3) snprintf(error,size,"%s_delivery_%s",kind,outcome==2?"rejected":"unconfirmed");
    return outcome;
}
