/* Optional tiny D-Bus helper: PIN arrives on stdin, never on the command line. */
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv) {
    const char *prefix="/org/freedesktop/ModemManager1/SIM/";
    if (argc!=2 || strncmp(argv[1],prefix,strlen(prefix)) || !argv[1][strlen(prefix)] || strspn(argv[1]+strlen(prefix),"0123456789")!=strlen(argv[1]+strlen(prefix))) return 2;
    char pin[10]={0}; ssize_t n=read(STDIN_FILENO,pin,sizeof(pin));
    if (n<4 || n>8 || strspn(pin,"0123456789")!=(size_t)n) return 2;
    DBusConnection *bus=dbus_bus_get_private(DBUS_BUS_SYSTEM,NULL); if (!bus) return 1;
    dbus_connection_set_exit_on_disconnect(bus,FALSE);
    DBusMessage *request=dbus_message_new_method_call("org.freedesktop.ModemManager1",argv[1],"org.freedesktop.ModemManager1.Sim","SendPin"),*reply=NULL;
    const char *value=pin;
    if (request && dbus_message_append_args(request,DBUS_TYPE_STRING,&value,DBUS_TYPE_INVALID)) reply=dbus_connection_send_with_reply_and_block(bus,request,45000,NULL);
    volatile char *wipe=pin; for (size_t i=0;i<sizeof(pin);i++) wipe[i]=0;
    int success=reply!=NULL; if (reply) dbus_message_unref(reply); if (request) dbus_message_unref(request);
    dbus_connection_close(bus); dbus_connection_unref(bus); return success?0:1;
}
