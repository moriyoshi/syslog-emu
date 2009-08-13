#include <stdio.h>
#include "syslog.h"

int main()
{
    openlog("*IDENT*", LOG_PID | LOG_NOWAIT, LOG_AUTH);
    syslog(LOG_WARNING, "%s %d", "test", 123);
    closelog();
    return 0;
}

