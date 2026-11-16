#include "relay.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

char *base64_text(const unsigned char *text, size_t size) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out=malloc((size+2)/3*4+1); if (!out) return NULL;
    size_t n=0;
    for (size_t i=0;i<size;i+=3) {
        unsigned v=(unsigned)text[i]<<16;
        if (i+1<size) v|=(unsigned)text[i+1]<<8;
        if (i+2<size) v|=text[i+2];
        out[n++]=alphabet[v>>18]; out[n++]=alphabet[(v>>12)&63];
        out[n++]=i+1<size?alphabet[(v>>6)&63]:'='; out[n++]=i+2<size?alphabet[v&63]:'=';
    }
    out[n]=0; return out;
}
static int allowed(const char *s, const char *chars, size_t min, size_t max) {
    size_t n=strlen(s); return n>=min && n<=max && strspn(s,chars)==n;
}
static int email(const char *address) {
    const char *at=strchr(address,'@');
    return at && at!=address && !strchr(at+1,'@') && strchr(at+1,'.') && strlen(address)<255 &&
        strspn(address,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") == strlen(address);
}
static int webhook(const char *kind, const char *url) {
    CURLU *u=curl_url(); char *scheme=NULL,*host=NULL,*path=NULL,*query=NULL,*v=NULL; int ok=0;
    if (!u || curl_url_set(u,CURLUPART_URL,url,0)!=CURLUE_OK) goto end;
    if (curl_url_get(u,CURLUPART_SCHEME,&scheme,0) || strcmp(scheme,"https") ||
        curl_url_get(u,CURLUPART_HOST,&host,0) || curl_url_get(u,CURLUPART_PATH,&path,0)) goto end;
    CURLUPart forbidden[]={CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_FRAGMENT};
    for (size_t i=0;i<3;i++) if (curl_url_get(u,forbidden[i],&v,0)==CURLUE_OK) { curl_free(v); v=NULL; goto end; }
    if (curl_url_get(u,CURLUPART_PORT,&v,0)==CURLUE_OK) { int port=atoi(v); curl_free(v); v=NULL; if (port!=443) goto end; }
    const char *token_chars="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    if (!strcmp(kind,"wecom")) {
        if (strcmp(host,"qyapi.weixin.qq.com") || strcmp(path,"/cgi-bin/webhook/send") ||
            curl_url_get(u,CURLUPART_QUERY,&query,0) || strncmp(query,"key=",4) || !allowed(query+4,token_chars,8,200)) goto end;
    } else {
        if (strcmp(host,"open.feishu.cn") || strncmp(path,"/open-apis/bot/v2/hook/",23) ||
            !allowed(path+23,token_chars,8,200) || curl_url_get(u,CURLUPART_QUERY,&query,0)==CURLUE_OK) goto end;
    }
    ok=1;
end:
    curl_free(scheme); curl_free(host); curl_free(path); curl_free(query); if (u) curl_url_cleanup(u); return ok;
}
int push_validate(const char *kind, J *c, J *s) {
    const char *fields=!strcmp(kind,"sms")?"|number|prefix|":!strcmp(kind,"telegram")?"|chat_id|prefix|":
        !strcmp(kind,"feishu")?"|prefix|signing_enabled|":!strcmp(kind,"wecom")?"|prefix|":
        !strcmp(kind,"smtp")?"|host|port|from|to|username|tls_mode|prefix|":"";
    const char *secret_fields=!strcmp(kind,"telegram")?"|token|":!strcmp(kind,"feishu")?"|webhook_url|signing_secret|":
        !strcmp(kind,"wecom")?"|webhook_url|":!strcmp(kind,"smtp")?"|password|":"|";
    J *item;
    cJSON_ArrayForEach(item,c) {
        char field[128];
        if (!item->string || strlen(item->string)>100) return 0;
        snprintf(field,sizeof(field),"|%s|",item->string); if (!strstr(fields,field) && strcmp(item->string,"retry_count")) return 0;
        if (!strcmp(item->string,"retry_count")) {
            if (!cJSON_IsNumber(item) || item->valuedouble!=item->valueint || item->valueint<0 || item->valueint>10) return 0;
        }
        else if (!strcmp(item->string,"port")) { if (!cJSON_IsNumber(item) || item->valueint!=item->valuedouble) return 0; }
        else if (!strcmp(item->string,"signing_enabled")) {
            if (!cJSON_IsBool(item) && (!cJSON_IsNumber(item) || (item->valuedouble!=0 && item->valuedouble!=1))) return 0;
        } else if (!cJSON_IsString(item)) return 0;
    }
    cJSON_ArrayForEach(item,s) {
        char field[128];
        if (!item->string || strlen(item->string)>100) return 0;
        snprintf(field,sizeof(field),"|%s|",item->string); if (!strstr(secret_fields,field)) return 0;
        if (!cJSON_IsString(item)) return 0;
    }
    if (strlen(str(c,"prefix"))>320 || strpbrk(str(c,"prefix"),"\r\n")) return 0;
    const char *alnum="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    if (!strcmp(kind,"sms")) {
        const char *n=str(c,"number"); if (*n=='+') n++;
        return allowed(n,"0123456789",3,20);
    }
    if (!strcmp(kind,"telegram")) {
        const char *token=str(s,"token"),*colon=strchr(token,':'),*chat=str(c,"chat_id");
        if (!colon || colon-token<5 || colon-token>20 || strspn(token,"0123456789")!=(size_t)(colon-token) || !allowed(colon+1,alnum,20,200)) return 0;
        if (*chat=='@') return allowed(chat+1,alnum,5,32);
        if (*chat=='-') chat++;
        return allowed(chat,"0123456789",1,20);
    }
    if (!strcmp(kind,"wecom") || !strcmp(kind,"feishu")) {
        return webhook(kind,str(s,"webhook_url")) && strlen(str(s,"signing_secret"))<=2048 &&
            (!(integer(c,"signing_enabled",0) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"signing_enabled"))) || *str(s,"signing_secret"));
    }
    if (!strcmp(kind,"smtp")) {
        int port=integer(c,"port",587);
        return allowed(str(c,"host"),"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:",1,253) && port>0 && port<=65535 &&
            email(str(c,"from")) && email(str(c,"to")) && *str(c,"username") && strlen(str(c,"username"))<=255 &&
            !strpbrk(str(c,"username"),"\r\n") && *str(s,"password") && strlen(str(s,"password"))<=2048 && !strpbrk(str(s,"password"),"\r\n") &&
            (!strcmp(str(c,"tls_mode"),"starttls") || !strcmp(str(c,"tls_mode"),"tls"));
    }
    return 0;
}
