/* Offline boundary only: production never links this file. */
#include "../src/relay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int push_deliver(const char *kind, J *config, J *secrets, const char *text,
                 int *retry_after, char *error, size_t size) {
    if (!push_validate(kind,config,secrets)) return 0;
    const char *path=getenv("SIM_RELAY_FIXTURE");
    FILE *file=path?fopen(path,"rb"):NULL;
    if (!file) return 0;
    fseek(file,0,SEEK_END); long n=ftell(file); rewind(file);
    if (n<0 || n>1024*1024) { fclose(file); return 0; }
    char *raw=calloc(1,(size_t)n+1);
    if (!raw) { fclose(file); return 0; }
    size_t read=fread(raw,1,(size_t)n,file); fclose(file);
    J *state=read==(size_t)n?cJSON_Parse(raw):NULL; free(raw); if (!state) return 0;
    int attempts=integer(state,"push_attempts",0)+1;
    cJSON_DeleteItemFromObjectCaseSensitive(state,"push_attempts"); cJSON_AddNumberToObject(state,"push_attempts",attempts);
    cJSON_DeleteItemFromObjectCaseSensitive(state,"last_push_text"); cJSON_AddStringToObject(state,"last_push_text",text);
    const char *mode=str(state,"push_mode");
    int crash=!strcmp(mode,"crash"),outcome=!strcmp(mode,"timeout")?1:!strcmp(mode,"reject")?2:3;
    *retry_after=integer(state,"retry_after",0); raw=cJSON_PrintUnformatted(state);
    file=fopen(path,"wb"); if (file) { fputs(raw,file); fclose(file); }
    free(raw); cJSON_Delete(state);
    if (crash) exit(76);
    if (outcome!=3) snprintf(error,size,"fixture_delivery_%s",outcome==2?"rejected":"unconfirmed");
    return outcome;
}
