/* A stand-in for libpulse-simple.so.0 used to test t2_oss.so's behaviour when the sound server goes away
 * and comes back. The "server" is down while the file named by FAKE_PULSE_DOWN exists. Every event is
 * appended to FAKE_PULSE_LOG: "open-ok", "open-fail", "write-fail", "played <bytes>" (once a second). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct pa_simple { int direction; unsigned rate, channels, frame; } pa_simple;
typedef struct { unsigned format; unsigned rate; unsigned char channels; } sample_spec;

static void note(const char *text) {
    const char *path = getenv("FAKE_PULSE_LOG");
    FILE *log = path ? fopen(path, "a") : NULL;
    if (log) { fputs(text, log); fputc('\n', log); fclose(log); }
}
static int server_down(void) { const char *path = getenv("FAKE_PULSE_DOWN"); return path && access(path, F_OK) == 0; }

pa_simple *pa_simple_new(const char *server, const char *name, int direction, const char *device,
                         const char *stream_name, const sample_spec *spec, const void *map, const void *attr, int *error) {
    (void)server; (void)name; (void)device; (void)stream_name; (void)map; (void)attr;
    if (server_down()) { note("open-fail"); if (error) *error = 6; errno = ECONNREFUSED; return NULL; }
    pa_simple *stream = calloc(1, sizeof *stream);
    stream->direction = direction;
    stream->rate = spec->rate;
    stream->channels = spec->channels;
    stream->frame = (spec->format == 0 ? 1 : 2) * spec->channels;   /* PA_SAMPLE_U8 = 0, else 16-bit */
    note("open-ok");
    return stream;
}
void pa_simple_free(pa_simple *stream) { free(stream); }
int pa_simple_write(pa_simple *stream, const void *data, size_t bytes, int *error) {
    (void)data;
    static size_t total; static time_t last;
    if (server_down()) { note("write-fail"); if (error) *error = 32; return -1; }
    struct timespec nap = {0, (long)(1e9 * (double)bytes / ((double)stream->rate * stream->frame))};
    nanosleep(&nap, NULL);           /* the device drains in real time */
    total += bytes;
    if (time(NULL) != last) { char line[64]; snprintf(line, sizeof line, "played %zu", total); note(line); last = time(NULL); total = 0; }
    return 0;
}
int pa_simple_read(pa_simple *stream, void *data, size_t bytes, int *error) {
    (void)error;
    if (server_down()) { if (error) *error = 32; return -1; }
    struct timespec nap = {0, (long)(1e9 * (double)bytes / ((double)stream->rate * stream->frame))};
    nanosleep(&nap, NULL);
    memset(data, 0, bytes);
    return 0;
}
int pa_simple_drain(pa_simple *stream, int *error) { (void)stream; (void)error; return 0; }
int pa_simple_flush(pa_simple *stream, int *error) { (void)stream; (void)error; return 0; }
const char *pa_strerror(int code) { return code == 6 ? "Connection refused" : "Connection terminated"; }
