#include "relay.h"
#include <sodium.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int master_key(Relay *r, unsigned char key[crypto_secretbox_KEYBYTES]) {
    if (sodium_init()<0) return 0;
    char path[4096];
    int n=snprintf(path,sizeof(path),"%s/credentials.key",r->path);
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    /* The configuration lock serializes key creation across callers. */
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) {
        /* The config lock serializes creation. Never replace a lost credential key. */
        J *row;
        cJSON_ArrayForEach(row,cJSON_GetObjectItemCaseSensitive(r->config,"pins"))
            if (*str(row,"ciphertext")) goto fail;
        cJSON_ArrayForEach(row,cJSON_GetObjectItemCaseSensitive(r->config,"channels"))
            if (*str(row,"ciphertext")) goto fail;
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) goto fail;
        randombytes_buf(key,crypto_secretbox_KEYBYTES);
        int ok=write(fd,key,crypto_secretbox_KEYBYTES)==crypto_secretbox_KEYBYTES && fsync(fd)==0;
        close(fd);
        /* Persist the directory entry as well as the key before storing ciphertext. */
        char *slash=strrchr(path,'/');
        if (slash) *slash=0;
        int parent=open(slash?(*path?path:"/"):".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (parent<0 || fsync(parent)<0) ok=0;
        if (parent>=0) close(parent);
        if (!ok) goto fail;
        return 1;
    }
    struct stat st; unsigned char extra;
    int ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077) && st.st_nlink==1 &&
        read(fd,key,crypto_secretbox_KEYBYTES)==crypto_secretbox_KEYBYTES && read(fd,&extra,1)==0;
    close(fd);
    if (ok) return 1;
fail:
    sodium_memzero(key,crypto_secretbox_KEYBYTES);
    snprintf(r->error,sizeof(r->error),"Credential key missing, unsafe, or unavailable; restore the original key");
    return 0;
}
void wipe_secret(char *secret) {
    if (secret) { sodium_memzero(secret,strlen(secret)); free(secret); }
}
void erase_memory(void *memory, size_t size) { sodium_memzero(memory,size); }
static void erase_json(J *value) {
    if (cJSON_IsString(value) && value->valuestring) erase_memory(value->valuestring,strlen(value->valuestring));
    J *child;
    cJSON_ArrayForEach(child,value) erase_json(child);
}
void delete_secret_json(J *value) { erase_json(value); cJSON_Delete(value); }
char *seal_secret(Relay *r, const char *context, const char *secret) {
    if (strlen(context)>256 || strlen(secret)>8192 || strchr(context,'|')) return NULL;
    unsigned char key[crypto_secretbox_KEYBYTES];
    if (!master_key(r,key)) return NULL;
    size_t prefix=strlen(context)+1, n=prefix+strlen(secret), size=crypto_secretbox_NONCEBYTES+crypto_secretbox_MACBYTES+n;
    unsigned char *plain=malloc(n+1),*blob=malloc(size); char *hex=malloc(size*2+2);
    if (!plain || !blob || !hex) { free(plain); free(blob); free(hex); sodium_memzero(key,sizeof(key)); return NULL; }
    snprintf((char *)plain,n+1,"%s|%s",context,secret);
    randombytes_buf(blob,crypto_secretbox_NONCEBYTES);
    int ok=crypto_secretbox_easy(blob+crypto_secretbox_NONCEBYTES,plain,n,blob,key)==0;
    sodium_memzero(key,sizeof(key)); sodium_memzero(plain,n); free(plain);
    hex[0]='1';
    for (size_t i=0;i<size;i++) snprintf(hex+1+2*i,3,"%02x",blob[i]);
    free(blob);
    if (!ok) { free(hex); return NULL; }
    return hex;
}
char *open_secret(Relay *r, const char *context, const char *ciphertext) {
    size_t len=strlen(ciphertext), prefix=strlen(context)+1;
    if (len<1+2*(crypto_secretbox_NONCEBYTES+crypto_secretbox_MACBYTES) || len>18000 || ciphertext[0]!='1' || !(len%2)) return NULL;
    unsigned char key[crypto_secretbox_KEYBYTES];
    if (!master_key(r,key)) return NULL;
    size_t n=(len-1)/2, text=n-crypto_secretbox_NONCEBYTES-crypto_secretbox_MACBYTES;
    unsigned char *blob=malloc(n), *plain=calloc(1,text+1); char *secret=NULL;
    if (!blob || !plain) goto finish;
    for (size_t i=0;i<n;i++) {
        const char *digits="0123456789abcdef";
        const char *a=strchr(digits,ciphertext[1+2*i]),*b=strchr(digits,ciphertext[2+2*i]);
        if (!a || !b) goto finish;
        blob[i]=(unsigned char)((a-digits)*16+(b-digits));
    }
    if (crypto_secretbox_open_easy(plain,blob+crypto_secretbox_NONCEBYTES,n-crypto_secretbox_NONCEBYTES,blob,key)!=0 ||
        text<prefix || memcmp(plain,context,prefix-1) || plain[prefix-1]!='|' || memchr(plain,0,text)) goto finish;
    secret=strdup((char *)plain+prefix);
finish:
    sodium_memzero(key,sizeof(key));
    if (plain) { sodium_memzero(plain,text); free(plain); }
    free(blob);
    if (!secret) snprintf(r->error,sizeof(r->error),"Credential authentication failed");
    return secret;
}
