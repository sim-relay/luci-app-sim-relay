/* Test actual platform signing and formatting without opening any sockets. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#define access relay_test_access
#define execl relay_test_execl
#include "../src/push_transport.c"
#undef access
#undef execl
static char helper_output[128];
static int helper_available=1,helper_exit=0;
int relay_test_access(const char *path, int mode) {
    (void)mode; assert(!strcmp(path,"/usr/bin/msmtp")); return helper_available?0:-1;
}
int relay_test_execl(const char *path, const char *arg0, ...) {
    assert(!strcmp(path,"/usr/bin/msmtp") && !strcmp(arg0,"msmtp"));
    va_list args; va_start(args,arg0);
    assert(!strcmp(va_arg(args,const char *),"--file")); const char *source=va_arg(args,const char *);
    assert(!strncmp(source,"/proc/self/fd/",14));
    assert(!strcmp(va_arg(args,const char *),"--account=default"));
    assert(!strcmp(va_arg(args,const char *),"--"));
    assert(!strcmp(va_arg(args,const char *),"to@example.com")); assert(!va_arg(args,const char *)); va_end(args);
    FILE *config=fopen(source,"r"),*output=fopen(helper_output,"w");
    assert(config && output); struct stat st; assert(fstat(fileno(config),&st)==0 && st.st_nlink==0 && !(st.st_mode&0077));
    char buffer[4096]; size_t n;
    while ((n=fread(buffer,1,sizeof(buffer),config))) assert(fwrite(buffer,1,n,output)==n);
    fclose(config);
    while ((n=fread(buffer,1,sizeof(buffer),stdin))) assert(fwrite(buffer,1,n,output)==n);
    fclose(output); _exit(helper_exit);
}
int main(void) {
    assert(sodium_init()>=0);
    char signature[45];
    assert(feishu_sign("1700000000\nsecret",signature));
    assert(!strcmp(signature,"fiWS2+gh28DOydAv7hzONH/mDn9+b1Y4Y5ivXWXy8vA="));
    char *encoded=base64_text((const unsigned char *)"hello",5);
    assert(encoded && !strcmp(encoded,"aGVsbG8=")); free(encoded);
    char *quoted=quote_config("a\"b\\c");
    assert(quoted && !strcmp(quoted,"\"a\\\"b\\\\c\"")); free(quoted);
    J *config=cJSON_Parse("{\"prefix\":\"测试\",\"signing_enabled\":true,\"retry_count\":3}");
    J *secrets=cJSON_Parse("{\"webhook_url\":\"https://open.feishu.cn/open-apis/bot/v2/hook/abcdefghijk\",\"signing_secret\":\"secret\"}");
    assert(push_validate("feishu",config,secrets));
    cJSON_DeleteItemFromObjectCaseSensitive(secrets,"signing_secret");
    assert(!push_validate("feishu",config,secrets));
    cJSON_Delete(config); cJSON_Delete(secrets);
    char temporary[]="/tmp/sim-relay-smtp-test.XXXXXX"; int fd=mkstemp(temporary); assert(fd>=0); close(fd);
    snprintf(helper_output,sizeof(helper_output),"%s",temporary);
    config=cJSON_Parse("{\"host\":\"smtp.example.com\",\"port\":587,\"from\":\"from@example.com\",\"to\":\"to@example.com\",\"username\":\"from@example.com\",\"tls_mode\":\"starttls\"}");
    secrets=cJSON_Parse("{\"password\":\"smtp-test-only-password\"}");
    int delay=0; char error[128]="";
    assert(push_deliver("smtp",config,secrets,"中文测试",&delay,error,sizeof(error))==3);
    FILE *result=fopen(temporary,"r"); assert(result); char data[8192]={0}; size_t count=fread(data,1,sizeof(data)-1,result); fclose(result);
    assert(count>0 && strstr(data,"tls_starttls on") && strstr(data,"Content-Transfer-Encoding: base64"));
    char *expected=base64_text((const unsigned char *)"中文测试",strlen("中文测试")); assert(expected && strstr(data,expected)); free(expected);
    helper_exit=75; assert(push_deliver("smtp",config,secrets,"retry guard",&delay,error,sizeof(error))==1);
    helper_available=0; assert(push_deliver("smtp",config,secrets,"missing helper",&delay,error,sizeof(error))==0);
    unlink(temporary); cJSON_Delete(config); delete_secret_json(secrets);
    puts("Push encoding, signing and validation tests passed"); return 0;
}
