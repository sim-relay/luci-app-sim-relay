/* Test actual platform signing and formatting without opening any sockets. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
static void check_webhook(const char *method, const char *body, const char *expect_path,
                          const char *expect_body, const char *message, int status, int expected) {
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(fd,(struct sockaddr *)&addr,sizeof(addr))==0 && listen(fd,1)==0);
    socklen_t len=sizeof(addr); assert(getsockname(fd,(struct sockaddr *)&addr,&len)==0);
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        alarm(5); int client=accept(fd,NULL,NULL); assert(client>=0); char request[16384]={0}; size_t used=0;
        while (used<sizeof(request)-1) {
            ssize_t n=read(client,request+used,sizeof(request)-1-used); assert(n>0); used+=(size_t)n; request[used]=0;
            char *end=strstr(request,"\r\n\r\n");
            if (end) {
                char *length=strstr(request,"Content-Length:"); size_t content=length?(size_t)strtoul(length+15,NULL,10):0;
                if (used>=(size_t)(end+4-request)+content) break;
            }
        }
        char first[512]; snprintf(first,sizeof(first),"%s %s HTTP/",method,expect_path);
        assert(!strncmp(request,first,strlen(first)) && strstr(request,"X-Test: value"));
        char *actual=strstr(request,"\r\n\r\n"); assert(actual); actual+=4;
        assert(!strcmp(actual,expect_body));
        char response[128]; int n=snprintf(response,sizeof(response),"HTTP/1.1 %d Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",status);
        assert(write(client,response,(size_t)n)==n); close(client); close(fd); _exit(0);
    }
    char url[256]; snprintf(url,sizeof(url),"http://127.0.0.1:%u/receive?q={{message}}",(unsigned)ntohs(addr.sin_port));
    J *c=cJSON_CreateObject(),*s=cJSON_CreateObject();
    cJSON_AddStringToObject(c,"method",method); cJSON_AddStringToObject(c,"body",body);
    cJSON_AddStringToObject(s,"url",url); cJSON_AddStringToObject(s,"headers","X-Test: value\nContent-Type: application/json");
    int retry=0; char error[128]={0};
    assert(push_deliver("webhook",c,s,message,&retry,error,sizeof(error))==expected);
    int result=0; assert(waitpid(child,&result,0)==child && WIFEXITED(result) && WEXITSTATUS(result)==0);
    close(fd); cJSON_Delete(c); delete_secret_json(s);
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
    config=cJSON_Parse("{\"method\":\"POST\",\"body\":\"{{message_json}}\"}");
    secrets=cJSON_Parse("{\"url\":\"http://127.0.0.1:8000/?text={{message}}\",\"headers\":\"X-Test: yes\\nBad Header: nope\"}");
    assert(!push_validate("webhook",config,secrets));
    cJSON_ReplaceItemInObjectCaseSensitive(secrets,"headers",cJSON_CreateString("X-Test: yes\rInjected: nope"));
    assert(!push_validate("webhook",config,secrets));
    cJSON_ReplaceItemInObjectCaseSensitive(secrets,"headers",cJSON_CreateString(""));
    assert(push_validate("webhook",config,secrets));
    cJSON_ReplaceItemInObjectCaseSensitive(secrets,"url",cJSON_CreateString("file:///etc/passwd"));
    assert(!push_validate("webhook",config,secrets));
    cJSON_Delete(config); delete_secret_json(secrets);
    check_webhook("GET","ignored","/receive?q=a%22b%0A","","a\"b\n",204,3);
    check_webhook("GET","","/receive?q=%E4%BD%A0%E5%A5%BD","","你好",200,3);
    check_webhook("POST","{\"text\":{{message_json}}}","/receive?q=a%22b%0A","{\"text\":\"a\\\"b\\n\"}","a\"b\n",200,3);
    check_webhook("PUT","value={{message}}","/receive?q=a%22b%0A","value=a\"b\n","a\"b\n",500,1);
    check_webhook("POST","{{message}}","/receive?q=a%22b%0A","a\"b\n","a\"b\n",400,2);
    puts("Push encoding, signing and validation tests passed"); return 0;
}
