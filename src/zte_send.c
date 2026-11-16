#include "relay.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

J *zte_json(const char *base, const char *params, double deadline, char *error, size_t size) {
    char *raw=device_request(base,"/goform/goform_get_cmd_process",params,0,deadline,error,size);
    J *j=raw?cJSON_ParseWithLengthOpts(raw,strlen(raw)+1,NULL,1):NULL;
    free(raw);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    return j;
}
/* Fail closed on absent, duplicate or unsupported JS flag syntax. Never execute JS. */
static int flag(const char *js, const char *name) {
    const char *p=js,*found=NULL; size_t n=strlen(name);
    while ((p=strstr(p,name))) {
        if ((p==js || !(isalnum((unsigned char)p[-1]) || p[-1]=='_')) &&
            !(isalnum((unsigned char)p[n]) || p[n]=='_')) {
            if (found) return -2;
            found=p+n;
        }
        p+=n;
    }
    if (!found) return -1;
    while (isspace((unsigned char)*found)) found++;
    if (*found++!=':') return -2;
    while (isspace((unsigned char)*found)) found++;
    const char *values[]={"!0","!1","true","false","1","0"};
    const int truth[]={1,0,1,0,1,0};
    for (unsigned i=0;i<6;i++) {
        size_t len=strlen(values[i]);
        if (strncmp(found,values[i],len)) continue;
        const char *end=found+len; while (isspace((unsigned char)*end)) end++;
        if (*end==',' || *end=='}') return truth[i];
    }
    return -2;
}
int zte_write_config(const char *base, double deadline, char ad_value[33], char *error, size_t size) {
    char *base_js=device_request(base,"/js/config/config.js",NULL,0,deadline,error,size);
    char *specific=device_request(base,"/js/config/datacard/config.js",NULL,0,deadline,error,size);
    int login=base_js?flag(base_js,"HAS_LOGIN"):-1;
    int specific_login=specific?flag(specific,"HAS_LOGIN"):-1;
    int ad=specific?flag(specific,"ACCESSIBLE_ID_SUPPORT"):-1;
    if (ad==-1 && base_js) ad=flag(base_js,"ACCESSIBLE_ID_SUPPORT");
    free(base_js); free(specific);
    if (login<0 || specific_login!=0 || ad<0) {
        snprintf(error,size,"Firmware write configuration unsupported or authentication required"); return 0;
    }
    ad_value[0]=0;
    if (ad) {
        J *challenge=zte_json(base,"isTest=false&cmd=wa_inner_version,cr_version,RD&multi_data=1",deadline,error,size);
        const char *fw=str(challenge,"wa_inner_version"),*cr=str(challenge,"cr_version"),*rd=str(challenge,"RD");
        if (!*fw || !*cr || !*rd || strlen(fw)+strlen(cr)>2048 || strlen(rd)>2048) {
            cJSON_Delete(challenge); snprintf(error,size,"Missing firmware AD challenge"); return 0;
        }
        char input[4096],digest[33]; snprintf(input,sizeof(input),"%s%s",fw,cr); protocol_md5(input,digest);
        if (!*digest) { cJSON_Delete(challenge); return 0; }
        snprintf(input,sizeof(input),"%s%s",digest,rd); protocol_md5(input,ad_value);
        cJSON_Delete(challenge);
        if (!*ad_value) return 0;
    }
    return 1;
}
static int same_card(J *snapshot, J *job) {
    return snapshot && !strcmp(str(snapshot,"hardware_id"),str(job,"hardware_id")) &&
        !strcmp(str(snapshot,"iccid"),str(job,"iccid"));
}
J *adapter_send(J *device, J *job, int (*commit)(void *), void *ctx,
                int *outcome, char *error, size_t size) {
    if (!strcmp(str(device,"adapter"),"modemmanager")) return mm_send(device,job,commit,ctx,outcome,error,size);
    *outcome=0; /* 0: no submission, 1: unknown, 2: explicit rejection, 3: confirmed. */
    char *hex=NULL; const char *encoding=NULL;
    if (!encode_sms(str(job,"number"),str(job,"text"),&hex,&encoding)) {
        snprintf(error,size,"Invalid recipient, encoding or message length"); return NULL;
    }
    if (!strcmp(str(device,"adapter"),"mock")) {
        free(hex);
        if (!commit(ctx)) return NULL;
        J *m=cJSON_CreateObject();
        char key[180]; snprintf(key,sizeof(key),"mock-send:%s",str(job,"key"));
        cJSON_AddStringToObject(m,"external_id",key);
        cJSON_AddStringToObject(m,"number",str(job,"number")); cJSON_AddStringToObject(m,"text",str(job,"text"));
        cJSON_AddStringToObject(m,"direction","outbound"); cJSON_AddStringToObject(m,"state","sent");
        time_t timestamp=time(NULL); struct tm utc; char formatted[40]; gmtime_r(&timestamp,&utc);
        strftime(formatted,sizeof(formatted),"%Y-%m-%dT%H:%M:%SZ",&utc);
        cJSON_AddStringToObject(m,"timestamp",formatted); *outcome=3; return m;
    }
    const char *base=str(device,"base_url"); double deadline=relay_monotonic()+120;
    J *before=adapter_snapshot(device,deadline,error,size), *result=NULL;
    if (!same_card(before,job)) { snprintf(error,size,"Device or SIM changed before sending"); goto end; }
    char ad_value[33]="";
    if (!zte_write_config(base,deadline,ad_value,error,size)) goto end;
    J *guard=zte_json(base,"isTest=false&cmd=imei,iccid,modem_main_state&multi_data=1",deadline,error,size);
    char guard_iccid[23]; normalize_iccid(str(guard,"iccid"),guard_iccid);
    char identity[80]; snprintf(identity,sizeof(identity),"zte-mf833u1:imei:%s",str(guard,"imei"));
    int same=guard && !strcmp(identity,str(job,"hardware_id")) && !strcmp(guard_iccid,str(job,"iccid")) &&
        !strcmp(str(guard,"modem_main_state"),"modem_init_complete");
    cJSON_Delete(guard);
    if (!same) { snprintf(error,size,"Device or SIM changed immediately before sending"); goto end; }
    time_t now=time(NULL)+8*3600; struct tm tm; gmtime_r(&now,&tm);
    char stamp[80],date[64]; strftime(date,sizeof(date),"%y;%m;%d;%H;%M;%S;+8",&tm);
    char *number=curl_easy_escape(NULL,str(job,"number"),0),*time_encoded=curl_easy_escape(NULL,date,0);
    if (!number || !time_encoded) {
        curl_free(number); curl_free(time_encoded); snprintf(error,size,"Cannot allocate send request"); goto end;
    }
    snprintf(stamp,sizeof(stamp),"%s",time_encoded?time_encoded:""); curl_free(time_encoded);
    char body[4096];
    int length=snprintf(body,sizeof(body),"isTest=false&goformId=SEND_SMS&notCallback=true&Number=%s&sms_time=%s&MessageBody=%s&ID=-1&encode_type=%s%s%s",
        number?number:"",stamp,hex,encoding,*ad_value?"&AD=":"",ad_value);
    curl_free(number);
    if (length<0 || (size_t)length>=sizeof(body) || relay_monotonic()>=deadline) goto end;
    /* The caller can cancel immediately before the single POST. */
    if (!commit(ctx)) { snprintf(error,size,"Send cancelled before submission"); goto end; }
    *outcome=1;
    char *raw=device_request(base,"/goform/goform_set_cmd_process",body,1,deadline,error,size);
    J *response=raw?cJSON_ParseWithLengthOpts(raw,strlen(raw)+1,NULL,1):NULL; free(raw);
    int accepted=!strcmp(str(response,"result"),"success");
    if (!strcmp(str(response,"result"),"failure")) *outcome=2;
    cJSON_Delete(response);
    if (!accepted) { snprintf(error,size,"Device did not confirm acceptance"); goto end; }
    while (relay_monotonic()<deadline) {
        J *report=zte_json(base,"isTest=false&cmd=sms_cmd_status_info&sms_cmd=4",deadline,error,size);
        int failed=!strcmp(str(report,"sms_cmd_status_result"),"2");
        int sent=!strcmp(str(report,"sms_cmd_status_result"),"3");
        cJSON_Delete(report);
        if (failed) { *outcome=2; snprintf(error,size,"Device reported sending failed"); break; }
        if (sent) {
            J *after=adapter_snapshot(device,deadline,error,size), *m,*match=NULL;
            int count=0;
            if (same_card(after,job)) {
                cJSON_ArrayForEach(m,cJSON_GetObjectItemCaseSensitive(after,"messages")) {
                    if (strcmp(str(m,"direction"),"outbound") || strcmp(str(m,"state"),"sent") ||
                        strcmp(str(m,"number"),str(job,"number")) || strcmp(str(m,"text"),str(job,"text"))) continue;
                    int existed=0; J *old;
                    cJSON_ArrayForEach(old,cJSON_GetObjectItemCaseSensitive(before,"messages"))
                        if (!strcmp(str(old,"external_id"),str(m,"external_id"))) existed=1;
                    if (!existed) { count++; match=m; }
                }
            } else count=-1;
            if (count==1) { result=cJSON_Duplicate(match,1); *outcome=3; }
            cJSON_Delete(after);
            if (result || count>1 || count<0) break;
        }
        /* This is only a read-side wait; the POST is never repeated. */
        struct timespec delay={0,500000000}; nanosleep(&delay,NULL);
    }
    if (!result && *outcome==1) snprintf(error,size,"Sending result unconfirmed");
end:
    free(hex); cJSON_Delete(before); return result;
}
