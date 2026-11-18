#ifndef RELAY_H
#define RELAY_H
#include <cjson/cJSON.h>
#include <stddef.h>
typedef cJSON J;
typedef struct { const char *path; const char *runtime; J *config; int lock; char error[256]; } Relay;
const char *str(J *,const char *);
int integer(J *,const char *,int);
J *failure(const char *);
int open_store(Relay *,const char *);
void close_store(Relay *);
J *dispatch(Relay *,const char *,J *);
int api_serve(Relay *);
int api_reserve_send(Relay *);
void poll_devices(Relay *);
void reset_runtime(Relay *);
double relay_monotonic(void);
char *device_request(const char *,const char *,const char *,int,double,char *,size_t);
int valid_url(const char *);
int same_device_url(const char *,const char *);
J *adapter_read(J *,char *,size_t);
J *adapter_snapshot(J *,double,char *,size_t);
J *adapter_sim(J *,double,char *,size_t);
int encode_sms(const char *,const char *,char **,const char **);
void protocol_md5(const char *,char [33]);
J *adapter_send(J *,J *,int (*)(void *),void *,int *,char *,size_t);
int adapter_operation(J *,J *,const char *,int (*)(void *),void *,char *,size_t);
int zte_write_config(const char *,double,char [33],char *,size_t);
J *zte_json(const char *,const char *,double,char *,size_t);
void message_date(const char *,char [40]);
int normalize_iccid(const char *,char [23]);
int message_identity(const char *,char [69]);
char *seal_secret(Relay *,const char *,const char *);
char *open_secret(Relay *,const char *,const char *);
void wipe_secret(char *);
void erase_memory(void *,size_t);
void delete_secret_json(J *);
char *base64_text(const unsigned char *,size_t);
int push_validate(const char *,J *,J *);
int push_deliver(const char *,J *,J *,const char *,int *,char *,size_t);
J *mm_discover(char *,size_t);
J *mm_sim(J *,double,char *,size_t);
J *mm_snapshot(J *,double,char *,size_t);
J *mm_send(J *,J *,int (*)(void *),void *,int *,char *,size_t);
int mm_operation(J *,J *,const char *,int (*)(void *),void *,char *,size_t);
int valid_sms(J *,const char *,const char *);
#endif
