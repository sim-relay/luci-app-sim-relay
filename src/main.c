#include "relay.h"
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

static int valid_arguments(const char *method, J *args, const char *schema) {
    J *all=cJSON_Parse(schema), *spec=cJSON_GetObjectItemCaseSensitive(all,method), *item;
    int ok=cJSON_IsObject(spec);
    cJSON_ArrayForEach(item,args) {
        J *field=cJSON_GetObjectItemCaseSensitive(spec,item->string);
        /* rpcd forwards browser session metadata alongside method arguments.
           Authorization belongs to rpcd/ACL; this field is never application data. */
        if (!strcmp(item->string,"ubus_rpc_session")) {
            if (!cJSON_IsString(item) || strlen(item->valuestring)>128) ok=0;
        } else if (!field || (cJSON_IsString(field) && !cJSON_IsString(item)) ||
            (cJSON_IsNumber(field) && (!cJSON_IsNumber(item) || item->valuedouble!=item->valueint))) ok=0;
        J *other;
        for (other=item->next;other;other=other->next)
            if (!strcmp(item->string,other->string)) ok=0;
    }
    cJSON_Delete(all); return ok;
}
static int contains_nul_escape(const char *input, size_t n) {
    for (size_t k=0;k<n;k++) {
        if (input[k]!='\\') continue;
        if (k+5<n && !memcmp(input+k,"\\u0000",6)) return 1;
        k++; /* An escaped backslash must not reinterpret a following literal u0000. */
    }
    return 0;
}

static volatile sig_atomic_t running=1;
static void stop(int sig) { (void)sig; running=0; }
static const char *methods=
    "{\"status\":{},\"devices\":{},\"discover\":{},"
    "\"channels\":{},\"clear_channel_credentials\":{\"id\":0,\"version\":0},\"save_channel\":{\"id\":0,\"name\":\"\",\"kind\":\"\",\"config\":\"\",\"secrets\":\"\",\"enabled\":0,\"version\":0},"
    "\"bind_channels\":{\"device_id\":0,\"ids\":\"\"},\"device_channels\":{\"device_id\":0},"
    "\"remove_channel\":{\"id\":0,\"version\":0},\"test_channel\":{\"device_id\":0,\"channel_id\":0,\"key\":\"\"},\"push_history\":{\"device_id\":0},"
    "\"sim_info\":{\"device_id\":0},\"save_pin\":{\"device_id\":0,\"iccid\":\"\",\"pin\":\"\",\"auto_enabled\":0,\"forget\":0},"
    "\"unlock_pin\":{\"device_id\":0,\"iccid\":\"\",\"pin\":\"\",\"key\":\"\",\"force\":0},"
    "\"delete_messages\":{\"device_id\":0,\"ids\":\"\",\"scope\":\"\",\"key\":\"\"},"
    "\"operation_history\":{\"device_id\":0},"
    "\"remove_device\":{\"device_id\":0},"
    "\"send\":{\"device_id\":0,\"key\":\"\",\"number\":\"\",\"text\":\"\"},"
    "\"send_history\":{\"device_id\":0},"
    "\"save_device\":{\"id\":0,\"name\":\"\",\"adapter\":\"\",\"base_url\":\"\",\"modem\":\"\",\"poll_interval\":0,\"enabled\":0},"
    "\"messages\":{\"device_id\":0,\"limit\":0,\"offset\":0,\"after_id\":0,\"query\":\"\",\"direction\":\"\"},"
    "\"sync\":{\"device_id\":0}}";
int main(int argc, char **argv) {
    const char *path=getenv("SIM_RELAY_STATE");
    if (!path || !*path) path="/etc/sim-relay";
    int i=1;
    if (argc>3 && !strcmp(argv[1],"--state-dir")) { path=argv[2]; i=3; }
    if (i>=argc) { fprintf(stderr,"Usage: sim-relayd [--state-dir PATH] list | call METHOD | daemon | poll\n"); return 2; }
    if (!strcmp(argv[i],"list")) { puts(methods); return 0; }
    int daemon=!strcmp(argv[i],"daemon"), once=!strcmp(argv[i],"poll");
    if (!daemon && !once && (strcmp(argv[i],"call") || i+1>=argc)) return 2;
    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
    Relay r={.lock=-1}; J *out=NULL; int exit_code=0;
    if (!open_store(&r,path)) { out=failure(r.error); exit_code=1; }
    else if (daemon) {
        signal(SIGTERM,stop); signal(SIGINT,stop);
        /* Clear volatile UI state; persistent cursors still recover newer SMS. */
        reset_runtime(&r);
        while (running) {
            poll_devices(&r); close_store(&r);
            if (!running) break;
            sleep(1); if (!running) break;
            if (!open_store(&r,path)) { fprintf(stderr,"%s\n",r.error); exit_code=1; break; }
        }
    } else if (once) poll_devices(&r);
    else {
        char input[16386]; size_t n=fread(input,1,sizeof(input)-1,stdin); input[n]=0;
        J *args=n ? cJSON_ParseWithLengthOpts(input,n+1,NULL,1) : cJSON_CreateObject();
        if (n>16384 || memchr(input,0,n) || contains_nul_escape(input,n) || !cJSON_IsObject(args)) out=failure("Expected a JSON object of at most 16 KiB, without NUL");
        else if (!valid_arguments(argv[i+1],args,methods)) out=failure("Unknown method, duplicate field, or invalid argument type");
        else {
            J *session=cJSON_GetObjectItemCaseSensitive(args,"ubus_rpc_session");
            if (cJSON_IsString(session)) erase_memory(session->valuestring,strlen(session->valuestring));
            cJSON_DeleteItemFromObjectCaseSensitive(args,"ubus_rpc_session");
            out=dispatch(&r,argv[i+1],args);
        }
        erase_memory(input,sizeof(input)); delete_secret_json(args);
        if (!out) out=failure(*r.error?r.error:"Operation failed");
        if (cJSON_IsArray(out)) {
            J *envelope=cJSON_CreateObject(); cJSON_AddItemToObject(envelope,"items",out); out=envelope;
        }
    }
    if (out) {
        char *value=cJSON_PrintUnformatted(out);
        if (daemon) fprintf(stderr,"%s\n",value?value:"Startup failed");
        else puts(value?value:"{\"error\":\"Out of memory\"}");
        cJSON_free(value); cJSON_Delete(out);
    }
    close_store(&r); curl_global_cleanup(); return exit_code;
}
