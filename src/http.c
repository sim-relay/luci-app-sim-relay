#include "relay.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESPONSE_LIMIT (2U * 1024U * 1024U)
typedef struct { char *data; size_t size; } Buffer;
static size_t receive(void *ptr, size_t size, size_t nmemb, void *arg) {
    Buffer *b=arg;
    if (size && nmemb>RESPONSE_LIMIT/size) return 0;
    size_t n=size*nmemb;
    if (n>RESPONSE_LIMIT-b->size) return 0;
    char *p=realloc(b->data,b->size+n+1);
    if (!p) return 0;
    b->data=p; memcpy(p+b->size,ptr,n); b->size+=n; p[b->size]=0;
    return n;
}
double relay_monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
char *device_request(const char *base, const char *path, const char *params,
                     int write, double deadline, char *error, size_t size) {
    if (!valid_url(base) || strlen(path)>256 || (params && strlen(params)>8192)) {
        snprintf(error,size,"Invalid device request"); return NULL;
    }
    long remaining=(long)((deadline-relay_monotonic())*1000);
    if (remaining<=0) { snprintf(error,size,"Device operation deadline exceeded"); return NULL; }
    char url[9216],referer[600]; size_t n=strlen(base);
    if (n && base[n-1]=='/') n--;
    snprintf(url,sizeof(url),"%.*s%s%s%s",(int)n,base,path,!write && params?"?":"",!write && params?params:"");
    snprintf(referer,sizeof(referer),"%.*s/index.html",(int)n,base);
    CURL *c=curl_easy_init();
    if (!c) return NULL;
    Buffer b={0};
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"X-Requested-With: XMLHttpRequest");
    headers=curl_slist_append(headers,"Cache-Control: no-cache");
    if (write) headers=curl_slist_append(headers,"Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_REFERER,referer);
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_PROXY,"");
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,remaining<10000?remaining:10000L);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT_MS,3000L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    if (write) curl_easy_setopt(c,CURLOPT_POSTFIELDS,params);
    /* No transport-level retry, including for POST. */
    CURLcode rc=curl_easy_perform(c);
    long status=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(headers); curl_easy_cleanup(c);
    if (rc!=CURLE_OK || status!=200 || !b.data || memchr(b.data,0,b.size)) {
        free(b.data); snprintf(error,size,"Device request failed, redirected, oversized or invalid response"); return NULL;
    }
    return b.data;
}
