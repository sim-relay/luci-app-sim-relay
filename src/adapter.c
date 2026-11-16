#include "relay.h"
#include <curl/curl.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>

int valid_url(const char *url) {
    if (strlen(url)>512 || strpbrk(url," \r\n\t")) return 0;
    CURLU *u=curl_url();
    if (!u) return 0;
    char *scheme=NULL,*host=NULL,*path=NULL,*v=NULL;
    int ok=curl_url_set(u,CURLUPART_URL,url,0)==CURLUE_OK;
    ok=ok && curl_url_get(u,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK &&
        (!strcmp(scheme,"http") || !strcmp(scheme,"https"));
    ok=ok && curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK && *host;
    ok=ok && curl_url_get(u,CURLUPART_PATH,&path,0)==CURLUE_OK && !strcmp(path,"/");
    CURLUPart forbidden[]={CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT};
    for (size_t i=0;i<sizeof(forbidden)/sizeof(forbidden[0]);i++) {
        if (curl_url_get(u,forbidden[i],&v,0)==CURLUE_OK) { ok=0; curl_free(v); v=NULL; }
    }
    curl_free(scheme); curl_free(host); curl_free(path); curl_url_cleanup(u);
    return ok;
}
int same_device_url(const char *a,const char *b) {
    CURLU *left=curl_url(),*right=curl_url();
    char *ls=NULL,*rs=NULL,*lh=NULL,*rh=NULL,*lp=NULL,*rp=NULL; int same=0;
    if (left && right && curl_url_set(left,CURLUPART_URL,a,0)==CURLUE_OK && curl_url_set(right,CURLUPART_URL,b,0)==CURLUE_OK &&
        curl_url_get(left,CURLUPART_SCHEME,&ls,0)==CURLUE_OK && curl_url_get(right,CURLUPART_SCHEME,&rs,0)==CURLUE_OK &&
        curl_url_get(left,CURLUPART_HOST,&lh,0)==CURLUE_OK && curl_url_get(right,CURLUPART_HOST,&rh,0)==CURLUE_OK &&
        curl_url_get(left,CURLUPART_PORT,&lp,CURLU_DEFAULT_PORT)==CURLUE_OK && curl_url_get(right,CURLUPART_PORT,&rp,CURLU_DEFAULT_PORT)==CURLUE_OK)
        same=!strcasecmp(ls,rs) && !strcasecmp(lh,rh) && !strcmp(lp,rp);
    curl_free(ls); curl_free(rs); curl_free(lh); curl_free(rh); curl_free(lp); curl_free(rp);
    if (left) curl_url_cleanup(left);
    if (right) curl_url_cleanup(right);
    return same;
}
static J *get(const char *base, const char *params, double deadline, char *error, size_t size) {
    char query[2048]; snprintf(query,sizeof(query),"isTest=false&%s",params);
    char *raw=device_request(base,"/goform/goform_get_cmd_process",query,0,deadline,error,size);
    J *j=raw?cJSON_ParseWithLengthOpts(raw,strlen(raw)+1,NULL,1):NULL;
    free(raw);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); snprintf(error,size,"Invalid device JSON response"); return NULL; }
    return j;
}
static int digits(const char *s, size_t min, size_t max) {
    size_t n=strlen(s);
    if (n<min || n>max) return 0;
    for (size_t i=0;i<n;i++) if (s[i]<'0' || s[i]>'9') return 0;
    return 1;
}
int normalize_iccid(const char *input, char output[23]) {
    size_t n=strlen(input); output[0]=0;
    if (n<10 || n>22) return 0;
    for (size_t i=0;i<n;i++) {
        unsigned char ch=(unsigned char)input[i];
        if ((i<6 && (ch<'0' || ch>'9')) || !strchr("0123456789abcdefABCDEF",ch)) { output[0]=0; return 0; }
        output[i]=(char)toupper(ch);
    }
    output[n]=0; return 1;
}
static int nibble(char ch) {
    if (ch>='0' && ch<='9') return ch-'0';
    if (ch>='a' && ch<='f') return ch-'a'+10;
    if (ch>='A' && ch<='F') return ch-'A'+10;
    return -1;
}
static char *decode(const char *hex) {
    size_t n=strlen(hex);
    if (n%4 || n>65536) return NULL;
    char *bytes=malloc(n/2+1), *text=calloc(1,n+1);
    if (!bytes || !text) { free(bytes); free(text); return NULL; }
    for (size_t i=0;i<n;i+=2) {
        int a=nibble(hex[i]),b=nibble(hex[i+1]);
        if (a<0 || b<0) { free(bytes); free(text); return NULL; }
        bytes[i/2]=(char)(a*16+b);
    }
    iconv_t cd=iconv_open("UTF-8","UTF-16BE");
    char *in=bytes,*out=text; size_t left=n/2,available=n;
    int ok=cd!=(iconv_t)-1 && iconv(cd,&in,&left,&out,&available)!=(size_t)-1 && left==0;
    if (cd!=(iconv_t)-1) iconv_close(cd);
    free(bytes);
    if (!ok || memchr(text,0,(size_t)(out-text))) { free(text); return NULL; }
    return text;
}
void message_date(const char *raw, char out[40]) {
    out[0]=0;
    if (!*raw) return;
    if (strlen(raw)>=20 && raw[4]=='-' && raw[10]=='T') { snprintf(out,40,"%s",raw); return; }
    char date[128]; snprintf(date,sizeof(date),"%s",raw);
    for (char *p=date;*p;p++) if (*p==';') *p=',';
    int year,month,day,hour,minute,second;
    if (sscanf(date,"%d,%d,%d,%d,%d,%d",&year,&month,&day,&hour,&minute,&second)!=6) return;
    if (year>=0 && year<100) year+=2000;
    int days[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (year%4==0 && (year%100!=0 || year%400==0)) days[2]=29;
    if (year<1970 || year>9999 || month<1 || month>12 || day<1 || day>days[month] || hour<0 || hour>23 ||
        minute<0 || minute>59 || second<0 || second>59) return;
    snprintf(out,40,"%04d-%02d-%02dT%02d:%02d:%02d+08:00",year,month,day,hour,minute,second);
}
static J *message(J *raw, int store, const char *iccid) {
    if (!cJSON_IsObject(raw)) return NULL;
    const char *tag=str(raw,"tag");
    char tag_number[16],index[32];
    J *t=cJSON_GetObjectItemCaseSensitive(raw,"tag"), *i=cJSON_GetObjectItemCaseSensitive(raw,"id");
    if (cJSON_IsNumber(t)) {
        if (t->valuedouble!=t->valueint) return NULL;
        snprintf(tag_number,sizeof(tag_number),"%d",t->valueint); tag=tag_number;
    }
    if (cJSON_IsString(i)) snprintf(index,sizeof(index),"%s",i->valuestring);
    else if (cJSON_IsNumber(i) && i->valuedouble==i->valueint) snprintf(index,sizeof(index),"%d",i->valueint);
    else return NULL;
    if (!digits(index,1,20) || strlen(tag)!=1 || !strchr("0123",tag[0]) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(raw,"number")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(raw,"content")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(raw,"date"))) return NULL;
    char *text=decode(str(raw,"content"));
    if (!text) return NULL;
    const char *number=str(raw,"number"), *date=str(raw,"date");
    if (strlen(number)>128 || strlen(date)>128) { free(text); return NULL; }
    J *identity=cJSON_CreateArray();
    cJSON_AddItemToArray(identity,cJSON_CreateNumber(store));
    cJSON_AddItemToArray(identity,cJSON_CreateString(store==0?iccid:""));
    cJSON_AddItemToArray(identity,cJSON_CreateString(index));
    cJSON_AddItemToArray(identity,cJSON_CreateString(number));
    cJSON_AddItemToArray(identity,cJSON_CreateString(text));
    cJSON_AddItemToArray(identity,cJSON_CreateString(date));
    char *key=cJSON_PrintUnformatted(identity); cJSON_Delete(identity);
    char identity_key[69];
    if (!key || !message_identity(key,identity_key)) { free(key); free(text); return NULL; }
    J *m=cJSON_CreateObject();
    cJSON_AddStringToObject(m,"external_id",identity_key);
    cJSON_AddStringToObject(m,"_zte_id",index);
    cJSON_AddNumberToObject(m,"_zte_store",store);
    cJSON_AddStringToObject(m,"number",number); cJSON_AddStringToObject(m,"text",text);
    char normalized[40]; message_date(date,normalized);
    cJSON_AddStringToObject(m,"timestamp",normalized);
    cJSON_AddStringToObject(m,"direction",tag[0]<'2'?"inbound":"outbound");
    cJSON_AddStringToObject(m,"state",tag[0]<'2'?"received":tag[0]=='2'?"sent":"failed");
    cJSON_free(key); free(text); return m;
}
J *adapter_snapshot(J *device, double deadline, char *error, size_t size) {
    if (!strcmp(str(device,"adapter"),"modemmanager")) return mm_snapshot(device,deadline,error,size);
    J *result=cJSON_CreateObject(), *messages=cJSON_AddArrayToObject(result,"messages");
    if (!strcmp(str(device,"adapter"),"mock")) {
        cJSON_AddStringToObject(result,"hardware_id","mock");
        J *m=cJSON_CreateObject();
        cJSON_AddStringToObject(m,"external_id","mock-welcome-1");
        cJSON_AddStringToObject(m,"number","10086");
        cJSON_AddStringToObject(m,"text","SIM Relay test SMS. Unicode ✓");
        cJSON_AddStringToObject(m,"direction","inbound"); cJSON_AddStringToObject(m,"state","received");
        cJSON_AddStringToObject(m,"timestamp","2026-01-01T00:00:00Z");
        cJSON_AddItemToArray(messages,m); return result;
    }
    if (strcmp(str(device,"adapter"),"zte_mf833u1") || !valid_url(str(device,"base_url"))) goto fail;
    const char *base=str(device,"base_url");
    J *status=get(base,"cmd=imei,hardware_version,wa_inner_version,loginfo,iccid,modem_main_state&multi_data=1",deadline,error,size);
    if (!status) goto fail;
    char card[23]; normalize_iccid(str(status,"iccid"),card);
    const char *imei=str(status,"imei"), *iccid=card, *state=str(status,"modem_main_state");
    char identity[64]; snprintf(identity,sizeof(identity),"zte-mf833u1:imei:%s",imei);
    if (!digits(imei,15,15) || (!strstr(str(status,"hardware_version"),"MF833U1") && !strstr(str(status,"wa_inner_version"),"MF833U1")) ||
        (*str(status,"loginfo") && strcmp(str(status,"loginfo"),"ok")) ||
        (*str(device,"hardware_id") && strcmp(str(device,"hardware_id"),identity))) {
        snprintf(error,size,"Unsupported firmware, authentication required, or hardware identity changed"); goto status_fail;
    }
    if (strcmp(state,"modem_init_complete")) { snprintf(error,size,"SIM not ready; no PIN submitted"); goto status_fail; }
    cJSON_AddStringToObject(result,"hardware_id",identity);
    cJSON_AddStringToObject(result,"iccid",iccid);
    size_t text_bytes=0;
    for (int store=1;store>=0;store--) {
        if (store==0 && !*iccid) { snprintf(error,size,"Cannot identify SIM ICCID"); goto status_fail; }
        char *previous=NULL;
        for (int page=0;page<100;page++) {
            char params[256];
            snprintf(params,sizeof(params),"cmd=sms_data_total&page=%d&data_per_page=100&mem_store=%d&tags=10&order_by=order%%20by%%20id%%20desc",page,store);
            J *response=get(base,params,deadline,error,size);
            J *rows=cJSON_GetObjectItemCaseSensitive(response,"messages");
            char *signature=rows?cJSON_PrintUnformatted(rows):NULL;
            int ok=cJSON_IsArray(rows) && !(previous && signature && !strcmp(previous,signature));
            free(previous); previous=signature;
            if (!ok) { cJSON_Delete(response); free(previous); snprintf(error,size,"Invalid or repeated SMS page"); goto status_fail; }
            int count=cJSON_GetArraySize(rows); J *raw;
            cJSON_ArrayForEach(raw,rows) {
                J *tag=cJSON_GetObjectItemCaseSensitive(raw,"tag");
                if (!strcmp(str(raw,"tag"),"4") || (cJSON_IsNumber(tag) && tag->valuedouble==4)) continue;
                if (!strcmp(str(raw,"received_all_concat_sms"),"0") ||
                    integer(raw,"received_all_concat_sms",1)==0) continue;
                J *m=message(raw,store,iccid);
                if (!m) { cJSON_Delete(response); free(previous); snprintf(error,size,"Invalid SMS fields or UTF-16 encoding"); goto status_fail; }
                text_bytes+=strlen(str(m,"text"));
                if (cJSON_GetArraySize(messages)>=4096 || text_bytes>1024U*1024U) {
                    cJSON_Delete(m); cJSON_Delete(response); free(previous);
                    snprintf(error,size,"SMS snapshot exceeds memory budget; snapshot discarded"); goto status_fail;
                }
                cJSON_AddItemToArray(messages,m);
            }
            cJSON_Delete(response);
            if (count<100) break;
            if (page==99) { free(previous); snprintf(error,size,"SMS page limit reached; snapshot discarded"); goto status_fail; }
        }
        free(previous);
    }
    /* Check identity again: never commit a snapshot across a device/SIM change. */
    J *fresh=get(base,"cmd=imei,iccid,modem_main_state&multi_data=1",deadline,error,size);
    char fresh_card[23]; normalize_iccid(str(fresh,"iccid"),fresh_card);
    int same=fresh && !strcmp(str(fresh,"imei"),imei) && !strcmp(fresh_card,iccid) &&
        !strcmp(str(fresh,"modem_main_state"),"modem_init_complete");
    cJSON_Delete(fresh);
    if (!same) { snprintf(error,size,"Device or SIM changed during sync; snapshot discarded"); goto status_fail; }
    cJSON_Delete(status); return result;
status_fail:
    cJSON_Delete(status);
fail:
    if (!*error) snprintf(error,size,"Invalid adapter configuration");
    cJSON_Delete(result); return NULL;
}

J *adapter_read(J *device, char *error, size_t size) {
    return adapter_snapshot(device,relay_monotonic()+60,error,size);
}

J *adapter_sim(J *device, double deadline, char *error, size_t size) {
    if (!strcmp(str(device,"adapter"),"modemmanager")) return mm_sim(device,deadline,error,size);
    if (!strcmp(str(device,"adapter"),"mock")) {
        J *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"hardware_id","mock");
        cJSON_AddStringToObject(j,"iccid",""); cJSON_AddStringToObject(j,"unlock_required","none");
        cJSON_AddBoolToObject(j,"present",1); return j;
    }
    if (strcmp(str(device,"adapter"),"zte_mf833u1") || !valid_url(str(device,"base_url"))) return NULL;
    J *data=get(str(device,"base_url"),"cmd=imei,hardware_version,wa_inner_version,loginfo,iccid,modem_main_state,pinnumber,network_provider&multi_data=1",deadline,error,size);
    if (!data) return NULL;
    char card[23]; normalize_iccid(str(data,"iccid"),card);
    const char *imei=str(data,"imei"),*iccid=card,*state=str(data,"modem_main_state");
    char identity[64]; snprintf(identity,sizeof(identity),"zte-mf833u1:imei:%s",imei);
    if (!digits(imei,15,15) || (!strstr(str(data,"hardware_version"),"MF833U1") && !strstr(str(data,"wa_inner_version"),"MF833U1")) ||
        (*str(data,"loginfo") && strcmp(str(data,"loginfo"),"ok")) ||
        (*str(device,"hardware_id") && strcmp(str(device,"hardware_id"),identity))) {
        cJSON_Delete(data); snprintf(error,size,"Hardware identity or firmware mismatch"); return NULL;
    }
    J *out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"hardware_id",identity);
    cJSON_AddStringToObject(out,"iccid",iccid);
    cJSON_AddBoolToObject(out,"present",strcmp(state,"modem_sim_undetected") && strcmp(state,"modem_undetected") && *state);
    cJSON_AddStringToObject(out,"unlock_required",!strcmp(state,"modem_waitpin")?"sim-pin":
        !strcmp(state,"modem_waitpuk")?"sim-puk":!strcmp(state,"modem_init_complete")?"none":"unknown");
    const char *retries=str(data,"pinnumber"); int n=-1;
    if (digits(retries,1,1) && retries[0]<='3') n=retries[0]-'0';
    else { int value=integer(data,"pinnumber",-1); if (value>=0 && value<=3) n=value; }
    cJSON_AddNumberToObject(out,"pin_retries",n);
    cJSON_AddStringToObject(out,"operator",str(data,"network_provider"));
    cJSON_Delete(data); return out;
}
