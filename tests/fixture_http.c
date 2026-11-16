/* Offline substitute for the HTTP boundary. Not linked into production. */
#include "../src/relay.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double relay_monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static char *read_file(const char *path) {
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    if (n<0 || n>1024*1024) { fclose(f); return NULL; }
    char *s=calloc(1,(size_t)n+1);
    if (s && fread(s,1,(size_t)n,f)!=(size_t)n) { free(s); s=NULL; }
    fclose(f); return s;
}
static char *param(const char *body, const char *key) {
    size_t n=strlen(key); const char *p=body;
    while (p && *p) {
        if (!strncmp(p,key,n) && p[n]=='=') {
            const char *end=strchr(p+n+1,'&'); size_t len=end?(size_t)(end-p-n-1):strlen(p+n+1);
            char *decoded=curl_easy_unescape(NULL,p+n+1,(int)len,NULL);
            char *result=decoded?strdup(decoded):NULL; curl_free(decoded); return result;
        }
        p=strchr(p,'&'); if (p) p++;
    }
    return strdup("");
}
char *device_request(const char *base, const char *path, const char *params,
                     int write, double deadline, char *error, size_t size) {
    (void)base; (void)deadline;
    const char *file=getenv("SIM_RELAY_FIXTURE");
    if (!file) { snprintf(error,size,"Fixture file missing"); return NULL; }
    char *raw=read_file(file); J *f=raw?cJSON_Parse(raw):NULL; free(raw);
    if (!f) return NULL;
    J *requests=cJSON_GetObjectItemCaseSensitive(f,"requests");
    if (!requests) requests=cJSON_AddArrayToObject(f,"requests");
    J *request=cJSON_CreateObject(); cJSON_AddStringToObject(request,"path",path);
    cJSON_AddStringToObject(request,"params",params?params:""); cJSON_AddBoolToObject(request,"write",write);
    cJSON_AddItemToArray(requests,request);
    const char *mode=str(f,"mode"); J *out=NULL; char *answer=NULL;
    if (!strcmp(mode,"slow") && !write && params && strstr(params,"cmd=imei,hardware_version") && integer(f,"status_reads",0)==0) {
        struct timespec delay={2,0}; nanosleep(&delay,NULL);
    }
    if (write) {
        int posts=integer(f,"posts",0)+1;
        cJSON_DeleteItemFromObjectCaseSensitive(f,"posts"); cJSON_AddNumberToObject(f,"posts",posts);
        if (!strcmp(mode,"post_timeout") || !strcmp(mode,"post_crash")) goto finish;
        out=cJSON_CreateObject();
        cJSON_AddStringToObject(out,"result",!strcmp(mode,"reject")?"failure":"success");
        char *action=param(params,"goformId");
        if (strcmp(mode,"reject") && !strcmp(action,"ENTER_PIN")) {
            char *pin=param(params,"PinNumber");
            int correct=!strcmp(pin,*str(f,"expected_pin")?str(f,"expected_pin"):"73195628");
            cJSON_DeleteItemFromObjectCaseSensitive(f,"modem_state");
            cJSON_AddStringToObject(f,"modem_state",correct?"modem_init_complete":"modem_waitpin");
            cJSON_DeleteItemFromObjectCaseSensitive(f,"pin_retries");
            cJSON_AddNumberToObject(f,"pin_retries",correct?3:2); free(pin);
        } else if (strcmp(mode,"reject") && !strcmp(action,"DELETE_SMS")) {
            char *slot=param(params,"msg_id");
            J *messages=cJSON_GetObjectItemCaseSensitive(f,"messages");
            for (int index=cJSON_GetArraySize(messages)-1;index>=0;index--) {
                J *m=cJSON_GetArrayItem(messages,index),*id=cJSON_GetObjectItemCaseSensitive(m,"id");
                char candidate[80];
                if (cJSON_IsString(id)) snprintf(candidate,sizeof(candidate),"%s;",id->valuestring);
                else snprintf(candidate,sizeof(candidate),"%d;",integer(m,"id",-1));
                if (!strcmp(candidate,slot)) cJSON_DeleteItemFromArray(messages,index);
            }
            free(slot);
        } else if (strcmp(mode,"reject") && !strcmp(action,"SEND_SMS")) {
            J *messages=cJSON_GetObjectItemCaseSensitive(f,"messages");
            char *number=param(params,"Number"),*text=param(params,"MessageBody");
            for (int i=0;i<(!strcmp(mode,"duplicate")?2:1);i++) {
                J *m=cJSON_CreateObject(); cJSON_AddNumberToObject(m,"id",100+posts*2+i);
                cJSON_AddStringToObject(m,"tag","2"); cJSON_AddStringToObject(m,"number",number);
                cJSON_AddStringToObject(m,"content",text); cJSON_AddStringToObject(m,"date","26,11,14,12,00,00");
                cJSON_AddItemToArray(messages,m);
            }
            free(number); free(text);
        }
        free(action);
    } else if (!strcmp(path,"/js/config/config.js")) {
        answer=strdup(*str(f,"base_js")?str(f,"base_js"):"{HAS_LOGIN: false, ACCESSIBLE_ID_SUPPORT: true}");
    } else if (!strcmp(path,"/js/config/datacard/config.js")) {
        answer=strdup(*str(f,"specific_js")?str(f,"specific_js"):"{HAS_LOGIN: false, ACCESSIBLE_ID_SUPPORT: true}");
    } else if (params && strstr(params,"cmd=sms_data_total")) {
        int reads=integer(f,"sms_reads",0)+(strstr(params,"mem_store=1")?1:0);
        cJSON_DeleteItemFromObjectCaseSensitive(f,"sms_reads"); cJSON_AddNumberToObject(f,"sms_reads",reads);
        if (!strcmp(mode,"delete_slot_reuse") && reads>=2) {
            J *m=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(f,"messages"),0);
            if (m) { cJSON_DeleteItemFromObjectCaseSensitive(m,"content"); cJSON_AddStringToObject(m,"content","65B0"); }
        }
        out=cJSON_CreateObject();
        if (!strcmp(mode,"invalid_sim") && strstr(params,"mem_store=0")) cJSON_AddStringToObject(out,"messages","invalid");
        else cJSON_AddItemToObject(out,"messages",strstr(params,"mem_store=1")?
            cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(f,"messages"),1):cJSON_CreateArray());
    } else if (params && strstr(params,"cmd=sms_cmd_status_info")) {
        out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"sms_cmd_status_result",!strcmp(mode,"report_failure")?"2":"3");
    } else if (params && strstr(params,"cmd=wa_inner_version")) {
        out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"wa_inner_version","MF833U1-v1");
        cJSON_AddStringToObject(out,"cr_version","CR-1");
        if (strcmp(mode,"missing_ad")) cJSON_AddStringToObject(out,"RD","challenge");
    } else {
        int reads=integer(f,"status_reads",0)+1;
        cJSON_DeleteItemFromObjectCaseSensitive(f,"status_reads"); cJSON_AddNumberToObject(f,"status_reads",reads);
        out=cJSON_CreateObject();
        cJSON_AddStringToObject(out,"imei",*str(f,"imei")?str(f,"imei"):"123456789012345");
        cJSON_AddStringToObject(out,"iccid",!strcmp(mode,"swap_guard") && reads>=3?"8986001234567890999":
            *str(f,"iccid")?str(f,"iccid"):"8986001234567890123");
        cJSON_AddStringToObject(out,"hardware_version","MF833U1");
        cJSON_AddStringToObject(out,"wa_inner_version","MF833U1-v1");
        cJSON_AddStringToObject(out,"loginfo","ok"); cJSON_AddStringToObject(out,"modem_main_state",*str(f,"modem_state")?str(f,"modem_state"):"modem_init_complete");
        char retries[8]; snprintf(retries,sizeof(retries),"%d",integer(f,"pin_retries",3));
        cJSON_AddStringToObject(out,"pinnumber",retries);
    }
finish:
    raw=cJSON_PrintUnformatted(f); FILE *fp=fopen(file,"wb");
    if (fp) { fputs(raw,fp); fclose(fp); }
    cJSON_free(raw);
    int crash=write && !strcmp(mode,"post_crash"); cJSON_Delete(f);
    if (crash) exit(75);
    if (!answer && out) answer=cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!answer) snprintf(error,size,"Fixture transport timeout");
    return answer;
}
