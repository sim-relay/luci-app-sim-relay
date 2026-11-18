#include <assert.h>
#define curl_slist_append audit_append
#define curl_slist_free_all audit_free
#include "../src/http.c"
#undef curl_slist_append
#undef curl_slist_free_all

extern struct curl_slist *curl_slist_append(struct curl_slist *,const char *);
extern void curl_slist_free_all(struct curl_slist *);
static struct curl_slist *first,*freed;
static int calls;
struct curl_slist *audit_append(struct curl_slist *list,const char *value) {
    if (++calls==2) return NULL;
    first=curl_slist_append(list,value);
    return first;
}
void audit_free(struct curl_slist *list) { freed=list; curl_slist_free_all(list); }
int valid_url(const char *url) { (void)url; return 1; }
int main(void) {
    assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    char error[128]={0};
    char *result=device_request("http://127.0.0.1:1","/",NULL,0,relay_monotonic()+1,error,sizeof(error));
    assert(!result && calls==2 && first && freed==first);
    assert(strstr(error,"allocate request headers"));
    curl_global_cleanup(); return 0;
}
