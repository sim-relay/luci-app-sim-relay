/* Test the actual adapter parsing functions without any network operation. */
#include <assert.h>
#include "../src/adapter.c"

int main(void) {
    char *s=decode("4E2D6587D83DDE00");
    assert(s && !strcmp(s,"中文😀")); free(s);
    const char *invalid[]={"D800","DC00","0000","XYZW","123","D8000041"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) assert(decode(invalid[i])==NULL);
    J *raw=cJSON_Parse("{\"id\":1,\"tag\":1,\"number\":\"10086\",\"content\":\"4E2D6587\",\"date\":\"26,11,14,00,00,00\"}");
    J *m=message(raw,1,"");
    assert(m && !strcmp(str(m,"text"),"中文") && !strcmp(str(m,"direction"),"inbound"));
    char *first=strdup(str(m,"external_id")); cJSON_Delete(m);
    cJSON_ReplaceItemInObjectCaseSensitive(raw,"content",cJSON_CreateString("65B0"));
    m=message(raw,1,"");
    assert(m && strcmp(first,str(m,"external_id")));
    free(first); cJSON_Delete(m);
    cJSON_ReplaceItemInObjectCaseSensitive(raw,"tag",cJSON_CreateString("9"));
    assert(message(raw,1,"")==NULL); cJSON_Delete(raw);
    assert(valid_url("http://192.168.101.1"));
    assert(valid_url("https://[::1]:8443/"));
    assert(!valid_url("http://user:pass@localhost/"));
    assert(!valid_url("file:///etc/passwd"));
    const char *md5_inputs[]={"","a","abc","message digest","abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890"};
    const char *digests[]={"d41d8cd98f00b204e9800998ecf8427e","0cc175b9c0f1b6a831c399e269772661",
        "900150983cd24fb0d6963f7d28e17f72","f96b697d7cb7938d525a2f31aaf161d0",
        "c3fcd3d76192e4007dfb496cca67e13b","d174ab98d277d9f5a5611c2c9f419d9f",
        "57edf4a22be3c955ac49da2e2107b67a"};
    for (size_t i=0;i<7;i++) { char hash[33]; protocol_md5(md5_inputs[i],hash); assert(!strcmp(hash,digests[i])); }
    const char *encoding; char *hex;
    assert(encode_sms("+8613800000000","你好",&hex,&encoding));
    assert(!strcmp(hex,"4F60597D") && !strcmp(encoding,"UNICODE")); free(hex);
    assert(encode_sms("123","hello ^ €",&hex,&encoding));
    assert(!strcmp(encoding,"GSM7_default")); free(hex);
    assert(!encode_sms("123","😀",&hex,&encoding));
    assert(!encode_sms("123","   \n",&hex,&encoding));
    assert(!encode_sms("1;bad","hello",&hex,&encoding));
    char extended[384]; memset(extended,'^',383); extended[383]=0;
    assert(!encode_sms("123",extended,&hex,&encoding));
    extended[382]=0; assert(encode_sms("123",extended,&hex,&encoding)); free(hex);
    char basic_limit[767]; memset(basic_limit,'A',766); basic_limit[766]=0;
    assert(!encode_sms("123",basic_limit,&hex,&encoding));
    basic_limit[765]=0; assert(encode_sms("123",basic_limit,&hex,&encoding)); free(hex);
    char unicode_limit[1009];
    for (int i=0;i<336;i++) memcpy(unicode_limit+3*i,"中",3);
    unicode_limit[1008]=0; assert(!encode_sms("123",unicode_limit,&hex,&encoding));
    unicode_limit[1005]=0; assert(encode_sms("123",unicode_limit,&hex,&encoding)); free(hex);
    assert(!encode_sms("123","\xc0\xaf",&hex,&encoding));
    char card[23],date[40];
    assert(normalize_iccid("89860012abcd1234567f",card));
    assert(!strcmp(card,"89860012ABCD1234567F"));
    assert(!normalize_iccid("ABCDE0123456",card));
    message_date("24,2,29,1,2,3",date); assert(!strcmp(date,"2024-02-29T01:02:03+08:00"));
    message_date("23,2,29,1,2,3",date); assert(!*date);
    puts("Adapter parsing and UTF-16 tests passed");
    return 0;
}
