#ifndef _SYSLOG_EMU_H
#define _SYSLOG_EMU_H

#include <syslog.h>
#undef openlog
#undef syslog
#undef vsyslog
#undef closelog

#include <stdarg.h>

#define syslog_data  __emu_syslog_data
#define openlog_r    __emu_openlog_r
#define syslog_r     __emu_syslog_r
#define vsyslog_r    __emu_vsyslog_r
#define closelog_r   __emu_closelog_r
#define setlogmask_r __emu_setlogmask_r
#define openlog      __emu_openlog
#define syslog       __emu_syslog
#define vsyslog      __emu_vsyslog
#define closelog     __emu_closelog
#define setlogmask   __emu_setlogmask

struct syslog_data;

void openlog_r(const char *ident, int opts, int facility,
               struct syslog_data *sld);
void syslog_r(int prio, struct syslog_data *sld, const char *format, ...);
void vsyslog_r(int prio, struct syslog_data *sld,
               const char *format, va_list ap);
void closelog_r(struct syslog_data *sld);
int setlogmask_r(int mask, struct syslog_data *sld);

void openlog(const char *ident, int opts, int facility);
void syslog(int prio, const char *format, ...);
void vsyslog(int prio, const char *format, va_list ap);
void closelog(void);
int setlogmask(int mask);

#endif /* _SYSLOG_EMU_H */
