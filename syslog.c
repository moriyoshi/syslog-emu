#include <sys/time.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "syslog.h"

#define SIG_CRIT_SECTION \
    sigset_t sigs, old_sigs, *p = NULL; \
    for (sigemptyset(&sigs), sigfillset(&sigs), \
            pthread_sigmask(SIG_SETMASK, &sigs, &old_sigs); \
            !p; p = &old_sigs, pthread_sigmask(SIG_SETMASK, &old_sigs, NULL))

#define LOG_PRIO_ALL (LOG_EMERG | LOG_ALERT | LOG_CRIT | LOG_ERR | LOG_WARNING | LOG_NOTICE | LOG_INFO | LOG_DEBUG)

typedef struct _str_buf {
    char *p;
    size_t alloc_sz;
    size_t sz;
} str_buf;

static void str_buf_destroy(str_buf *buf)
{
    if (buf->p) {
        free(buf->p);
        buf->alloc_sz = 0;
        buf->p = NULL;
    }
    buf->sz = 0;
}

static int str_buf_alloc_buffer(str_buf *buf, size_t required_sz)
{
    const size_t required_alloc_sz = required_sz + 1;
    if (required_alloc_sz < required_sz)
        return ENOMEM;

    if (required_alloc_sz > buf->alloc_sz) {
        char *new_buf;
        size_t new_alloc_sz = buf->alloc_sz > 0 ? buf->alloc_sz: 1;

        while (new_alloc_sz < required_alloc_sz) {
            new_alloc_sz <<= 1;
            if (new_alloc_sz == 0)
                return ENOMEM;
        }

        new_buf = realloc(buf->p, required_alloc_sz);
        if (new_buf == NULL)
            return ENOMEM;
        buf->p = new_buf;
        buf->alloc_sz = required_alloc_sz;
    }

    return 0;
}

static int str_buf_grow_buffer(str_buf *buf, size_t delta)
{
    size_t required_buf_sz = 0;

    required_buf_sz = buf->sz + delta;
    if (required_buf_sz < buf->sz || required_buf_sz < delta)
        return ENOMEM;

    return str_buf_alloc_buffer(buf, required_buf_sz);
}

static int str_buf_appendn(str_buf *buf, const char *str, size_t str_sz)
{
    int err;

    err = str_buf_grow_buffer(buf, str_sz);
    if (err)
        return err;

    memmove(buf->p + buf->sz, str, str_sz);
    buf->sz += str_sz;
    buf->p[buf->sz] = '\0';

    return 0;
}

static int str_buf_append(str_buf *buf, const char *str)
{
    return str_buf_appendn(buf, str, strlen(str));
}

static int str_buf_vappendf(str_buf *buf, const char *format, va_list ap)
{
    int err;
    char dummy[0];
    size_t delta = vsnprintf(dummy, 0, format, ap);
    err = str_buf_grow_buffer(buf, delta);
    if (err)
        return err;
    buf->sz += vsnprintf(
            buf->p + buf->sz, buf->alloc_sz - buf->sz, format, ap);
    buf->p[buf->sz] = '\0';
    return 0;
}

static int str_buf_appendf(str_buf *buf, const char *format, ...)
{
    va_list ap;
    int err;
    va_start(ap, format);
    err = str_buf_vappendf(buf, format, ap);
    va_end(ap);
    return err;
}

static void str_buf_clear(str_buf *buf)
{
    buf->sz = 0;
}

static int str_buf_init(str_buf *buf, size_t initial_alloc_sz)
{
    buf->p = (void *)0;
    buf->alloc_sz = 0;
    buf->sz = 0;

    if (initial_alloc_sz > 0) {
        int err = str_buf_alloc_buffer(buf, initial_alloc_sz);
        if (err)
            return err;
    }

    return 0;
}

typedef struct _datetime_t
{
    int year;
    int mon;
    int mday;
    int hour;
    int min;
    int sec;
} datetime_t;

static void datetime_init_from_timeval(datetime_t *dt, const struct timeval *tv)
{
    static const int dom_tbl[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    static const int ey = 1600;
    static const int doy = 365;
    static const int qyd = 365 * 4 + 1;
    static const int ro = 92 * (365 * 4 + 1) + 366 + 365;
    static const int dncqy = (2000 - 1600) / 4;

    int q, qy;
    dt->sec  = tv->tv_sec % 60, q = tv->tv_sec / 60;
    dt->min  = q % 60, q = q / 60;
    dt->hour = q % 24, q = q / 24;
    q += ro;
    qy = q / qyd, q = q % qyd;
    q += (qy - dncqy + 100) / 100 - (qy - dncqy + 25) / 25;
    if (q >= qyd) {
        qy += 1;
        q -= qyd;
    }
    dt->year = (qy * 4 + q / doy) + ey;
    if (q >= doy + 1) {
        int m = 0;
        q = (q - (doy + 1)) % doy;
        while (q > 0) {
            const int dom = dom_tbl[m];
            q -= dom;
            if (q < 0) {
                q += dom;
                break;
            }
            m++;
        }
        dt->mon = m;
    } else {
        int m = 0;
        while (q > 0) {
            const int dom = dom_tbl[m] + (m == 1 ? 1: 0);
            q -= dom;
            if (q < 0) {
                q += dom;
                break;
            }
            m++;
        }
        dt->mon = m;
    }
    dt->mday = q;
}

typedef struct _TSD
{
    str_buf line_buf;
} TSD;

struct syslog_data
{
    pthread_key_t tls;
    char *ident;
    int opts;
    int facility;
    int mask;
};

static TSD *TSD_get(struct syslog_data *sld)
{
    int err = 0;
    TSD *tsd = (TSD*)pthread_getspecific(sld->tls);
    if (tsd) {
        return tsd;
    }

    tsd = malloc(sizeof(TSD));
    if (!tsd) {
        err = ENOMEM;
        goto failure;
    }

    err = str_buf_init(&tsd->line_buf, 1024);
    if (err) {
        free(tsd);
        goto failure;
    }

    err = pthread_setspecific(sld->tls, tsd);
    if (err) {
        free(tsd);
        goto failure;
    }

    return tsd;
failure:
    errno = err;
    return NULL;
}

static void TSD_destroy(TSD *data)
{
    str_buf_destroy(&data->line_buf);
}

static int fd_log_file;

static int get_log_file_fd(void)
{
    const char *filename;
    int fd;

    if (fd_log_file)
        return fd_log_file;

    filename = getenv("SYSLOG_FILE");
    if (!filename) {
        fd = 2;
    } else {
        fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            abort();
        }
    }
    fd_log_file = fd;
    return fd;
}

static int syslog_data_init(struct syslog_data *sld,
                      const char *ident, int opts, int facility)
{
    int err;
    char *id;
    pthread_key_t tls;

    err = pthread_key_create(&tls, (void(*)(void *))TSD_destroy);
    if (err)
        return err;

    if (!ident) {
#ifdef __GLIBC__
        extern const char *__progname;
        ident = __progname;
#else
        ident = "???";
#endif
    }
    id = strdup(ident);
    if (!id) {
        pthread_key_delete(tls);
        return ENOMEM;
    }

    sld->tls = tls;
    sld->ident = id;
    sld->opts = opts;
    sld->facility = facility;
    sld->mask = LOG_PRIO_ALL;

    return 0;
}

void openlog_r(const char *ident, int opts, int facility,
               struct syslog_data *sld)
{
    int err;
    err = syslog_data_init(sld, ident, opts, facility);
    if (err) {
        fprintf(stderr, "Failed to create syslog data (errno=%d)\n", err);
        fflush(stderr);
        abort();
    }
}

void syslog_r(int prio, struct syslog_data *sld, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog_r(prio, sld, format, ap);
    va_end(ap);
}

static const char *prio_to_string(int prio)
{
    static const struct {
        int val;
        const char *str;
    } msgs[] = {
        { LOG_EMERG  , "EMERG"  },
        { LOG_ALERT  , "ALERT"  },
        { LOG_CRIT   , "CRIT"   },
        { LOG_ERR    , "ERROR"  },
        { LOG_WARNING, "WARN"   },
        { LOG_NOTICE , "NOTICE" },
        { LOG_INFO   , "INFO"   },
        { LOG_DEBUG  , "DEBUG"  },
        { -1         , NULL     }
    };
    size_t i;

    for (i = 0; msgs[i].str; i++) {
        if (msgs[i].val == prio)
            return msgs[i].str;
    }

    return "?";
}

void vsyslog_r(int prio, struct syslog_data *sld,
               const char *format, va_list ap)
{
    SIG_CRIT_SECTION {
        TSD *tsd = TSD_get(sld);
        datetime_t dt;
        struct timeval tv;
        char hostname[HOST_NAME_MAX + 1];

        if (!(prio & sld->mask))
            continue;

        gethostname(hostname, sizeof(hostname));
        gettimeofday(&tv, NULL);

        datetime_init_from_timeval(&dt, &tv);
        str_buf_clear(&tsd->line_buf);
        str_buf_appendf(&tsd->line_buf, "%04d-%02d-%02dT%02d:%02d:%02d ",
                dt.year, dt.mon + 1, dt.mday + 1,
                dt.hour, dt.min, dt.sec);
        str_buf_append(&tsd->line_buf, hostname);
        str_buf_appendn(&tsd->line_buf, " ", 1);

        if (sld->opts & LOG_PID) {
            str_buf_appendf(&tsd->line_buf, "%s[%d]", sld->ident, getpid());
        } else {
            str_buf_appendf(&tsd->line_buf, sld->ident);
        }

        str_buf_appendn(&tsd->line_buf, ": ", sizeof(": ") - 1);
        str_buf_appendf(&tsd->line_buf, "(%s) ",
                prio_to_string(prio & LOG_PRIO_ALL));
        str_buf_vappendf(&tsd->line_buf, format, ap);
        str_buf_appendn(&tsd->line_buf, "\n", 1);
        write(get_log_file_fd(), tsd->line_buf.p, tsd->line_buf.sz);
    }
}

void closelog_r(struct syslog_data *sld)
{
    SIG_CRIT_SECTION {
        pthread_key_delete(sld->tls);
        if (sld->ident)
            free(sld->ident);
    }
}

int setlogmask_r(int mask, struct syslog_data *sld)
{
    int prev_mask;
    SIG_CRIT_SECTION {
        prev_mask = sld->mask;
        sld->mask = mask;
    }
    return prev_mask;
}

static struct syslog_data globals;

void openlog(const char *ident, int opts, int facility)
{
    SIG_CRIT_SECTION {
        openlog_r(ident, opts, facility, &globals);
    }
}

void vsyslog(int prio, const char *format, va_list ap)
{
    vsyslog_r(prio, &globals, format, ap);
}

void syslog(int prio, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(prio, format, ap);
    va_end(ap);
}

void closelog(void)
{
    closelog_r(&globals);
}

int setlogmask(int mask)
{
    return setlogmask_r(mask, &globals);
}
