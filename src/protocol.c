#include "relay.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <iconv.h>
#include <sodium.h>

int message_identity(const char *raw, char output[69]) {
    unsigned char hash[32];
    if (sodium_init()<0 || crypto_hash_sha256(hash,(const unsigned char *)raw,strlen(raw))) return 0;
    memcpy(output,"zte-",4);
    for (unsigned i=0;i<32;i++) snprintf(output+4+2*i,3,"%02x",hash[i]);
    return 1;
}

/* RFC 1321 MD5 for the modem's AD wire protocol only. Not credential encryption. */
void protocol_md5(const char *text, char out[33]) {
    static const uint32_t k[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const unsigned shifts[16]={7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    size_t len=strlen(text), padded=((len+8)/64+1)*64;
    unsigned char *p=calloc(1,padded);
    if (!p) { out[0]=0; return; }
    memcpy(p,text,len); p[len]=0x80;
    uint64_t bits=(uint64_t)len*8;
    for (unsigned i=0;i<8;i++) p[padded-8+i]=(unsigned char)(bits>>(8*i));
    uint32_t h[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    for (size_t off=0;off<padded;off+=64) {
        uint32_t w[16];
        for (unsigned i=0;i<16;i++) {
            const unsigned char *q=p+off+4*i;
            w[i]=(uint32_t)q[0]|(uint32_t)q[1]<<8|(uint32_t)q[2]<<16|(uint32_t)q[3]<<24;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3];
        for (unsigned i=0;i<64;i++) {
            uint32_t f; unsigned g;
            if (i<16) { f=(b&c)|(~b&d); g=i; }
            else if (i<32) { f=(d&b)|(~d&c); g=(5*i+1)%16; }
            else if (i<48) { f=b^c^d; g=(3*i+5)%16; }
            else { f=c^(b|~d); g=(7*i)%16; }
            uint32_t x=a+f+k[i]+w[g]; unsigned shift=shifts[(i/16)*4+i%4];
            a=d; d=c; c=b; b+=(x<<shift)|(x>>(32-shift));
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    }
    free(p);
    for (unsigned i=0;i<16;i++) snprintf(out+2*i,3,"%02x",(unsigned)((h[i/4]>>(8*(i%4)))&255));
}

int encode_sms(const char *number, const char *text, char **hex, const char **encoding) {
    *hex=NULL;
    const char *digits=number[0]=='+'?number+1:number;
    size_t n=strlen(digits), bytes=strlen(text);
    if (n<3 || n>20 || !bytes || bytes>3060) return 0;
    for (size_t i=0;i<n;i++) if (digits[i]<'0' || digits[i]>'9') return 0;
    char ucs[1532], *in=(char *)text,*out=ucs;
    size_t left=bytes,capacity=sizeof(ucs);
    iconv_t cd=iconv_open("UTF-16BE","UTF-8");
    if (cd==(iconv_t)-1) return 0;
    int ok=iconv(cd,&in,&left,&out,&capacity)!=(size_t)-1 && !left;
    iconv_close(cd);
    if (!ok) return 0;
    size_t units=(size_t)(out-ucs)/2, gsm_units=0;
    int gsm=1, nonspace=0;
    /* Complete GSM 03.38 default and extension alphabets, matched by code point. */
    const char *basic="@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà";
    const char *extended="\f^{}\\[~]|€";
    for (size_t i=0;i<units;i++) {
        unsigned ch=(unsigned char)ucs[2*i]*256+(unsigned char)ucs[2*i+1];
        if (!ch || (ch>=0xd800 && ch<=0xdfff)) return 0;
        if (ch!=32 && ch!=9 && ch!=10 && ch!=13 && ch!=0xa0 && ch!=0x3000) nonspace=1;
        char cp[4]={0};
        if (ch<128) cp[0]=(char)ch;
        else if (ch<2048) { cp[0]=(char)(0xc0|(ch>>6)); cp[1]=(char)(0x80|(ch&63)); }
        else { cp[0]=(char)(0xe0|(ch>>12)); cp[1]=(char)(0x80|((ch>>6)&63)); cp[2]=(char)(0x80|(ch&63)); }
        if (strstr(extended,cp)) gsm_units+=2;
        else if (strstr(basic,cp)) gsm_units++;
        else gsm=0;
    }
    if (!nonspace || (gsm?gsm_units>765:units>335)) return 0;
    *hex=malloc(units*4+1);
    if (!*hex) return 0;
    for (size_t i=0;i<units*2;i++) snprintf(*hex+2*i,3,"%02X",(unsigned char)ucs[i]);
    (*hex)[units*4]=0;
    *encoding=gsm?"GSM7_default":"UNICODE";
    return 1;
}
