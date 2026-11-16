#include "relay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int same(J *info, J *job) {
    return info && !strcmp(str(info,"hardware_id"),str(job,"hardware_id")) &&
        *str(job,"iccid") && !strcmp(str(info,"iccid"),str(job,"iccid"));
}
static J *target_message(J *snapshot, const char *key) {
    J *m,*target=NULL;
    cJSON_ArrayForEach(m,cJSON_GetObjectItemCaseSensitive(snapshot,"messages"))
        if (!strcmp(str(m,"external_id"),key)) { if (target) return NULL; target=m; }
    if (!target || !*str(target,"_zte_id")) return NULL;
    int count=0;
    cJSON_ArrayForEach(m,cJSON_GetObjectItemCaseSensitive(snapshot,"messages"))
        if (!strcmp(str(m,"_zte_id"),str(target,"_zte_id"))) count++;
    return count==1?target:NULL;
}
int adapter_operation(J *device, J *job, const char *pin, int (*commit)(void *), void *ctx,
                      char *error, size_t size) {
    if (!strcmp(str(device,"adapter"),"modemmanager")) return mm_operation(device,job,pin,commit,ctx,error,size);
    int is_pin=!strcmp(str(job,"kind"),"pin"),outcome=0;
    if (!strcmp(str(device,"adapter"),"mock")) {
        if (is_pin) { snprintf(error,size,"Mock device does not require a PIN"); return 0; }
        return commit(ctx)?3:0;
    }
    double deadline=relay_monotonic()+120; const char *base=str(device,"base_url");
    J *first=adapter_sim(device,deadline,error,size),*before=NULL,*guard=NULL;
    char ad[33]="",fields[256],body[512];
    if (!same(first,job)) { snprintf(error,size,"Device or SIM changed"); goto end; }
    if (is_pin) {
        int retries=integer(first,"pin_retries",-1);
        if (!pin || strlen(pin)<4 || strlen(pin)>8 || strspn(pin,"0123456789")!=strlen(pin) ||
            strcmp(str(first,"unlock_required"),"sim-pin") || retries<=0 || (!integer(job,"force",0) && retries!=3)) {
            snprintf(error,size,"PIN lock or retry guard rejected operation"); goto end;
        }
        snprintf(fields,sizeof(fields),"goformId=ENTER_PIN&PinNumber=%s",pin);
    } else {
        before=adapter_snapshot(device,deadline,error,size);
        J *target=target_message(before,str(job,"target"));
        if (!same(before,job) || !target) { snprintf(error,size,"SMS target missing or slot ambiguous"); goto end; }
        snprintf(fields,sizeof(fields),"goformId=DELETE_SMS&notCallback=true&msg_id=%s%%3B",str(target,"_zte_id"));
    }
    if (!zte_write_config(base,deadline,ad,error,size)) goto end;
    guard=is_pin?adapter_sim(device,deadline,error,size):adapter_snapshot(device,deadline,error,size);
    if (!same(guard,job)) { snprintf(error,size,"Device or SIM changed before submission"); goto end; }
    if (is_pin) {
        int retries=integer(guard,"pin_retries",-1);
        if (strcmp(str(guard,"unlock_required"),"sim-pin") || retries<=0 || (!integer(job,"force",0) && retries!=3)) {
            snprintf(error,size,"PIN retry guard changed before submission"); goto end;
        }
    } else {
        J *old=target_message(before,str(job,"target")),*latest=target_message(guard,str(job,"target"));
        if (!old || !latest || strcmp(str(old,"_zte_id"),str(latest,"_zte_id"))) {
            snprintf(error,size,"SMS slot changed before deletion"); goto end;
        }
    }
    snprintf(body,sizeof(body),"isTest=false&%s%s%s",fields,*ad?"&AD=":"",ad);
    if (relay_monotonic()>=deadline || !commit(ctx)) { snprintf(error,size,"Operation cancelled before submission"); goto end; }
    outcome=1;
    char *raw=device_request(base,"/goform/goform_set_cmd_process",body,1,deadline,error,size);
    /* Erase the only local form buffers containing a plaintext PIN. */
    erase_memory(fields,sizeof(fields)); erase_memory(body,sizeof(body));
    J *report=raw?cJSON_ParseWithLengthOpts(raw,strlen(raw)+1,NULL,1):NULL; free(raw);
    int accepted=!strcmp(str(report,"result"),"success");
    if (!strcmp(str(report,"result"),"failure")) outcome=2;
    cJSON_Delete(report);
    if (!accepted) { snprintf(error,size,"Operation not confirmed; never retried"); goto end; }
    while (relay_monotonic()<deadline) {
        if (is_pin) {
            J *fresh=adapter_sim(device,deadline,error,size);
            if (!same(fresh,job)) { cJSON_Delete(fresh); break; }
            int unlocked=!strcmp(str(fresh,"unlock_required"),"none"), retries=integer(fresh,"pin_retries",-1);
            int failed=!strcmp(str(fresh,"unlock_required"),"sim-puk") || (retries>=0 && retries<integer(first,"pin_retries",-1));
            cJSON_Delete(fresh);
            if (unlocked) { outcome=3; break; }
            if (failed) { outcome=2; break; }
        } else {
            report=zte_json(base,"isTest=false&cmd=sms_cmd_status_info&sms_cmd=6",deadline,error,size);
            int failed=!strcmp(str(report,"sms_cmd_status_result"),"2"),complete=!strcmp(str(report,"sms_cmd_status_result"),"3");
            cJSON_Delete(report);
            if (failed) { outcome=2; break; }
            if (complete) {
                J *fresh=adapter_snapshot(device,deadline,error,size),*m; int found=0,valid=same(fresh,job);
                cJSON_ArrayForEach(m,cJSON_GetObjectItemCaseSensitive(fresh,"messages")) if (!strcmp(str(m,"external_id"),str(job,"target"))) found=1;
                cJSON_Delete(fresh);
                if (!valid) break;
                if (!found) { outcome=3; break; }
            }
        }
        struct timespec delay={0,500000000}; nanosleep(&delay,NULL);
    }
    if (outcome!=3) snprintf(error,size,is_pin?"PIN failed or unconfirmed; automatic unlock blocked":"Delete failed or unconfirmed; not retried");
end:
    erase_memory(fields,sizeof(fields)); erase_memory(body,sizeof(body));
    cJSON_Delete(first); cJSON_Delete(before); cJSON_Delete(guard); return outcome;
}
