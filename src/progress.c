#include "progress.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int line_len;
static int remote_started;
static Buf remote_line;

static struct timespec recv_start;
static double last_recv_draw;
static double last_delta_draw;
static double last_checkout_draw;

static int ui_on(void)
{
    return !cgc_quiet;
}

static int ui_tty(void)
{
    return isatty(STDERR_FILENO);
}

int progress_live_enabled(void)
{
    return ui_on() && ui_tty();
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fmt_amount(double value, char *out, size_t n)
{
    static const char *units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(out, n, "%.0f %s", value, units[unit]);
    else
        snprintf(out, n, "%.2f %s", value, units[unit]);
}

static void draw(const char *fmt, ...)
{
    if (!ui_on() || !ui_tty())
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int n = (int)strlen(buf);
    fprintf(stderr, "\r%s", buf);
    if (line_len > n)
        fprintf(stderr, "%*s", line_len - n, "");
    line_len = n;
    fflush(stderr);
}

void progress_clear(void)
{
    if (line_len > 0 && ui_tty()) {
        fprintf(stderr, "\r%*s\r", line_len, "");
        fflush(stderr);
    }
    line_len = 0;
}

void progress_remote(const void *data, size_t len)
{
    if (!ui_on())
        return;
    if (!remote_started) {
        buf_init(&remote_line);
        remote_started = 1;
    }

    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            progress_clear();
            fputs("remote: ", stderr);
            if (remote_line.len)
                fwrite(remote_line.data, 1, remote_line.len, stderr);
            fputc('\n', stderr);
            remote_line.len = 0;
            fflush(stderr);
        } else {
            buf_putc(&remote_line, p[i]);
        }
    }
}

void progress_remote_end(void)
{
    if (!ui_on() || !remote_started || remote_line.len == 0)
        return;
    progress_clear();
    fputs("remote: ", stderr);
    fwrite(remote_line.data, 1, remote_line.len, stderr);
    fputc('\n', stderr);
    remote_line.len = 0;
    fflush(stderr);
}

void progress_receive_begin(void)
{
    clock_gettime(CLOCK_MONOTONIC, &recv_start);
    last_recv_draw = 0;
}

void progress_receive_update(size_t bytes)
{
    if (!ui_on() || !ui_tty())
        return;
    double t = now_seconds();
    if (t - last_recv_draw < 0.1)
        return;
    last_recv_draw = t;

    double elapsed = t - ((double)recv_start.tv_sec +
                          (double)recv_start.tv_nsec / 1e9);
    char size[32], rate[32];
    fmt_amount((double)bytes, size, sizeof(size));
    fmt_amount(elapsed > 0 ? (double)bytes / elapsed : 0, rate, sizeof(rate));
    draw("Receiving objects: %s | %s/s", size, rate);
}

void progress_receive_end(uint32_t objects, size_t bytes)
{
    progress_clear();
    if (!ui_on())
        return;

    double elapsed = now_seconds() - ((double)recv_start.tv_sec +
                                      (double)recv_start.tv_nsec / 1e9);
    char size[32], rate[32];
    fmt_amount((double)bytes, size, sizeof(size));
    fmt_amount(elapsed > 0 ? (double)bytes / elapsed : 0, rate, sizeof(rate));

    if (objects)
        fprintf(stderr, "Receiving objects: 100%% (%u/%u), %s | %s/s, done.\n",
                objects, objects, size, rate);
    else
        fprintf(stderr, "Receiving objects: %s | %s/s, done.\n", size, rate);
    fflush(stderr);
}

void progress_deltas_update(uint32_t done, uint32_t total)
{
    if (!ui_on() || !ui_tty())
        return;
    double t = now_seconds();
    if (t - last_delta_draw < 0.1 && done < total)
        return;
    last_delta_draw = t;

    unsigned pct = total ? (unsigned)((uint64_t)done * 100 / total) : 100;
    draw("Resolving deltas: %3u%% (%u/%u)", pct, done, total);
}

void progress_deltas_end(uint32_t total)
{
    progress_clear();
    if (!ui_on())
        return;
    fprintf(stderr, "Resolving deltas: 100%% (%u/%u), done.\n", total, total);
    fflush(stderr);
}

void progress_checkout_begin(size_t total)
{
    (void)total;
    last_checkout_draw = 0;
}

void progress_checkout_update(size_t done, size_t total)
{
    if (!ui_on() || !ui_tty() || total == 0)
        return;
    double t = now_seconds();
    if (t - last_checkout_draw < 0.1 && done < total)
        return;
    last_checkout_draw = t;

    unsigned pct = (unsigned)((uint64_t)done * 100 / total);
    draw("Updating files: %3u%% (%zu/%zu)", pct, done, total);
}

void progress_checkout_end(size_t total)
{
    progress_clear();
    if (!ui_on() || total == 0)
        return;
    fprintf(stderr, "Updating files: 100%% (%zu/%zu), done.\n", total, total);
    fflush(stderr);
}
