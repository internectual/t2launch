/* OSS emulation for the Loki OpenAL bundled with Tribes 2.
 *
 * Loki's OpenAL talks only to /dev/dsp and /dev/mixer through open, close,
 * fcntl, ioctl, read, write and select. /dev/dsp does not exist on a modern
 * desktop, and padsp (libpulsedsp), which normally fakes it, aborts inside
 * libpulse when a session changes servers. This preloaded library answers
 * those calls itself and drives libpulse-simple from its own threads, so the
 * game thread only ever touches a ring buffer.
 *
 *   T2LAUNCH_AUDIO_OUTPUT  PulseAudio sink name (default: the server default)
 *   T2LAUNCH_AUDIO_INPUT   PulseAudio source name (default: the server default)
 *   T2LAUNCH_MIC_GAIN      linear microphone gain, 0..8 (default 1.0)
 *   T2LAUNCH_OSS_DEBUG     1 = trace calls on stderr
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>

#define OSS_MAX_FDS 1024
#define OSS_FRAGMENT 2048
#define OSS_RING 16384
#define OSS_CAPTURE_IDLE_MS 1500

enum oss_kind { OSS_NONE, OSS_DSP, OSS_MIXER };

struct oss_pulse {
    pa_simple *(*new_stream)(const char *, const char *, pa_stream_direction_t, const char *,
                             const char *, const pa_sample_spec *, const pa_channel_map *,
                             const pa_buffer_attr *, int *);
    void (*free_stream)(pa_simple *);
    int (*write)(pa_simple *, const void *, size_t, int *);
    int (*read)(pa_simple *, void *, size_t, int *);
    int (*drain)(pa_simple *, int *);
    int (*flush)(pa_simple *, int *);
    const char *(*strerror)(int);
};

struct oss_fd {
    enum oss_kind kind;
    int flags;                 /* open flags */
    int format;                /* AFMT_* */
    int channels, speed;
    int dirty;                 /* spec changed since the stream was made */
    int mixer_volume[SOUND_MIXER_NRDEVICES];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned char ring[OSS_RING];
    size_t head, count;        /* ring read position and fill */
    pa_simple *stream;
    pthread_t thread;
    int thread_running, stop, flush_pending;
    int out;                   /* 1 playback, 0 capture */
    uint64_t last_read_ms;
};

static struct oss_fd *oss_table[OSS_MAX_FDS];
static pthread_mutex_t oss_table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct oss_pulse pulse;
static int pulse_state;  /* 0 untried, 1 ready, -1 unavailable */
static int oss_debug = -1;

static int (*real_open)(const char *, int, ...);
static int (*real_close)(int);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_fcntl)(int, int, ...);
static int (*real_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *);

#define TRACE(...) do { if (oss_debug > 0) { fprintf(stderr, "[t2_oss] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void oss_init_once(void) {
    real_open = dlsym(RTLD_NEXT, "open");
    real_close = dlsym(RTLD_NEXT, "close");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");
    real_select = dlsym(RTLD_NEXT, "select");
    const char *debug = getenv("T2LAUNCH_OSS_DEBUG");
    oss_debug = debug && *debug == '1';
}
static void oss_init_real(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, oss_init_once);
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)(t.tv_nsec / 1000000);
}

static int pulse_ready(void) {
    if (pulse_state) return pulse_state > 0;
    void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (lib) {
        pulse.new_stream = dlsym(lib, "pa_simple_new");
        pulse.free_stream = dlsym(lib, "pa_simple_free");
        pulse.write = dlsym(lib, "pa_simple_write");
        pulse.read = dlsym(lib, "pa_simple_read");
        pulse.drain = dlsym(lib, "pa_simple_drain");
        pulse.flush = dlsym(lib, "pa_simple_flush");
        void *base = dlopen("libpulse.so.0", RTLD_NOW | RTLD_GLOBAL);
        pulse.strerror = base ? dlsym(base, "pa_strerror") : NULL;
    }
    pulse_state = (lib && pulse.new_stream && pulse.free_stream && pulse.write && pulse.read &&
                   pulse.drain && pulse.flush) ? 1 : -1;
    if (pulse_state < 0) fprintf(stderr, "[t2_oss] libpulse-simple is unavailable; the game will have no sound\n");
    return pulse_state > 0;
}

static const char *pulse_error(int code) { return pulse.strerror ? pulse.strerror(code) : "error"; }

static float mic_gain(void) {
    const char *text = getenv("T2LAUNCH_MIC_GAIN");
    float gain = text ? (float)atof(text) : 1.0f;
    return gain < 0.0f ? 0.0f : gain > 8.0f ? 8.0f : (text ? gain : 1.0f);
}

static struct oss_fd *oss_lookup(int fd) {
    if (fd < 0 || fd >= OSS_MAX_FDS) return NULL;
    pthread_mutex_lock(&oss_table_lock);
    struct oss_fd *entry = oss_table[fd];
    pthread_mutex_unlock(&oss_table_lock);
    return entry;
}

static void oss_scale(struct oss_fd *entry, unsigned char *data, size_t bytes, float gain) {
    if (gain == 1.0f) return;
    if (entry->format == AFMT_S16_LE) {
        short *samples = (short *)data;
        for (size_t i = 0; i < bytes / 2; i++) {
            float value = samples[i] * gain;
            samples[i] = (short)(value > 32767.0f ? 32767 : value < -32768.0f ? -32768 : value);
        }
    } else {
        for (size_t i = 0; i < bytes; i++) {
            float value = ((int)data[i] - 128) * gain + 128;
            data[i] = (unsigned char)(value > 255.0f ? 255 : value < 0.0f ? 0 : value);
        }
    }
}

static pa_simple *oss_open_stream(struct oss_fd *entry, int out) {
    pa_sample_spec spec = {
        .format = entry->format == AFMT_U8 ? PA_SAMPLE_U8 : PA_SAMPLE_S16LE,
        .rate = (uint32_t)entry->speed,
        .channels = (uint8_t)entry->channels,
    };
    pa_buffer_attr attr = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1};
    size_t frame = (entry->format == AFMT_U8 ? 1 : 2) * (size_t)entry->channels;
    attr.tlength = (uint32_t)(OSS_FRAGMENT * 3);
    attr.minreq = OSS_FRAGMENT / 2;
    attr.fragsize = (uint32_t)(frame * (size_t)entry->speed / 50);  /* 20 ms reads */
    attr.maxlength = (uint32_t)-1;
    attr.prebuf = (uint32_t)-1;
    const char *device = getenv(out ? "T2LAUNCH_AUDIO_OUTPUT" : "T2LAUNCH_AUDIO_INPUT");
    if (device && !*device) device = NULL;
    int error = 0;
    pa_simple *stream = pulse.new_stream(NULL, "Tribes 2", out ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD,
                                         device, out ? "game audio" : "voice", &spec, NULL, &attr, &error);
    if (!stream && device) {  /* the chosen device is unplugged: use the default rather than go silent */
        fprintf(stderr, "[t2_oss] %s device %s is unavailable (%s); using the default\n",
                out ? "output" : "input", device, pulse_error(error));
        device = NULL;
        stream = pulse.new_stream(NULL, "Tribes 2", out ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD, NULL,
                                  out ? "game audio" : "voice", &spec, NULL, &attr, &error);
    }
    if (!stream) fprintf(stderr, "[t2_oss] cannot open %s: %s\n", out ? "output" : "input", pulse_error(error));
    else TRACE("%s stream %uHz %uch fmt=%d device=%s", out ? "playback" : "capture", spec.rate,
               spec.channels, entry->format, device ? device : "default");
    return stream;
}

static void *oss_playback_thread(void *argument) {
    struct oss_fd *entry = argument;
    unsigned char chunk[OSS_FRAGMENT];
    pthread_mutex_lock(&entry->lock);
    while (!entry->stop) {
        if (entry->flush_pending) {
            entry->flush_pending = 0;
            int error;
            if (entry->stream) pulse.flush(entry->stream, &error);
        }
        if (!entry->count) {
            pthread_cond_broadcast(&entry->cond);  /* wake SYNC waiters */
            pthread_cond_wait(&entry->cond, &entry->lock);
            continue;
        }
        if (entry->dirty && entry->stream) {
            int error;
            pulse.free_stream(entry->stream);
            entry->stream = NULL;
            entry->dirty = 0;
            (void)error;
        }
        if (!entry->stream) {
            entry->dirty = 0;
            pthread_mutex_unlock(&entry->lock);
            pa_simple *stream = oss_open_stream(entry, 1);
            pthread_mutex_lock(&entry->lock);
            entry->stream = stream;
            if (!stream) {  /* no sound: discard so the game never blocks */
                entry->count = 0;
                pthread_cond_broadcast(&entry->cond);
                struct timespec nap = {0, 20000000};
                pthread_mutex_unlock(&entry->lock);
                nanosleep(&nap, NULL);
                pthread_mutex_lock(&entry->lock);
                continue;
            }
        }
        size_t amount = entry->count < sizeof chunk ? entry->count : sizeof chunk;
        for (size_t i = 0; i < amount; i++) chunk[i] = entry->ring[(entry->head + i) % OSS_RING];
        entry->head = (entry->head + amount) % OSS_RING;
        entry->count -= amount;
        pthread_cond_broadcast(&entry->cond);
        pa_simple *stream = entry->stream;
        pthread_mutex_unlock(&entry->lock);
        int error = 0;
        if (pulse.write(stream, chunk, amount, &error) < 0) {
            fprintf(stderr, "[t2_oss] playback write failed: %s\n", pulse_error(error));
            pthread_mutex_lock(&entry->lock);
            pulse.free_stream(entry->stream);
            entry->stream = NULL;
            continue;
        }
        pthread_mutex_lock(&entry->lock);
    }
    if (entry->stream) { pulse.free_stream(entry->stream); entry->stream = NULL; }
    pthread_mutex_unlock(&entry->lock);
    return NULL;
}

static void *oss_capture_thread(void *argument) {
    struct oss_fd *entry = argument;
    unsigned char chunk[512];
    float gain = mic_gain();
    pthread_mutex_lock(&entry->lock);
    while (!entry->stop) {
        int active = now_ms() - entry->last_read_ms < OSS_CAPTURE_IDLE_MS;
        if (!active || entry->dirty) {
            if (entry->stream) {  /* release the microphone while nobody reads */
                pulse.free_stream(entry->stream);
                entry->stream = NULL;
                entry->dirty = 0;
                TRACE("capture stream released");
            }
            if (!active) {
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 100000000;
                if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&entry->cond, &entry->lock, &until);
                continue;
            }
        }
        if (!entry->stream) {
            entry->dirty = 0;
            pthread_mutex_unlock(&entry->lock);
            pa_simple *stream = oss_open_stream(entry, 0);
            pthread_mutex_lock(&entry->lock);
            entry->stream = stream;
            if (!stream) {
                entry->last_read_ms = 0;  /* retry only after the next read */
                continue;
            }
        }
        pa_simple *stream = entry->stream;
        size_t frame = (entry->format == AFMT_U8 ? 1 : 2) * (size_t)entry->channels;
        size_t want = sizeof chunk - sizeof chunk % frame;
        pthread_mutex_unlock(&entry->lock);
        int error = 0;
        int result = pulse.read(stream, chunk, want, &error);
        pthread_mutex_lock(&entry->lock);
        if (result < 0) {
            fprintf(stderr, "[t2_oss] capture read failed: %s\n", pulse_error(error));
            if (entry->stream == stream) { pulse.free_stream(stream); entry->stream = NULL; }
            entry->last_read_ms = 0;
            continue;
        }
        oss_scale(entry, chunk, want, gain);
        for (size_t i = 0; i < want; i++) {
            if (entry->count == OSS_RING) { entry->head = (entry->head + 1) % OSS_RING; entry->count--; }
            entry->ring[(entry->head + entry->count++) % OSS_RING] = chunk[i];
        }
        pthread_cond_broadcast(&entry->cond);
    }
    if (entry->stream) { pulse.free_stream(entry->stream); entry->stream = NULL; }
    pthread_mutex_unlock(&entry->lock);
    return NULL;
}

static void oss_start_thread(struct oss_fd *entry, int out) {
    if (entry->thread_running) return;
    entry->out = out;
    entry->stop = 0;
    if (pthread_create(&entry->thread, NULL, out ? oss_playback_thread : oss_capture_thread, entry) == 0)
        entry->thread_running = 1;
}

static void oss_destroy(struct oss_fd *entry) {
    pthread_mutex_lock(&entry->lock);
    entry->stop = 1;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->lock);
    if (entry->thread_running) pthread_join(entry->thread, NULL);
    pthread_mutex_destroy(&entry->lock);
    pthread_cond_destroy(&entry->cond);
    free(entry);
}

static int oss_is_device(const char *path, enum oss_kind *kind) {
    if (!path) return 0;
    if (!strcmp(path, "/dev/dsp") || !strcmp(path, "/dev/dsp0") || !strcmp(path, "/dev/audio")) { *kind = OSS_DSP; return 1; }
    if (!strcmp(path, "/dev/mixer") || !strcmp(path, "/dev/mixer0")) { *kind = OSS_MIXER; return 1; }
    return 0;
}

static int oss_open(const char *path, int flags) {
    enum oss_kind kind;
    if (!oss_is_device(path, &kind)) return -2;
    oss_init_real();
    if (kind == OSS_DSP && !pulse_ready()) { errno = ENODEV; return -1; }
    int fd = real_open("/dev/null", O_RDWR);  /* a real descriptor, so fcntl/close/dup behave */
    if (fd < 0 || fd >= OSS_MAX_FDS) { if (fd >= 0) real_close(fd); errno = EMFILE; return -1; }
    struct oss_fd *entry = calloc(1, sizeof *entry);
    if (!entry) { real_close(fd); errno = ENOMEM; return -1; }
    entry->kind = kind;
    entry->flags = flags;
    entry->format = AFMT_U8;
    entry->channels = 1;
    entry->speed = 8000;
    for (int i = 0; i < SOUND_MIXER_NRDEVICES; i++) entry->mixer_volume[i] = 100 | (100 << 8);
    pthread_mutex_init(&entry->lock, NULL);
    pthread_cond_init(&entry->cond, NULL);
    pthread_mutex_lock(&oss_table_lock);
    oss_table[fd] = entry;
    pthread_mutex_unlock(&oss_table_lock);
    TRACE("open %s flags=%#x -> fd %d", path, flags, fd);
    return fd;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
    int fd = oss_open(path, flags);
    if (fd != -2) return fd;
    oss_init_real();
    return real_open(path, flags, mode);
}
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
    int fd = oss_open(path, flags);
    if (fd != -2) return fd;
    oss_init_real();
    return real_open(path, flags | O_LARGEFILE, mode);
}

int close(int fd) {
    oss_init_real();
    struct oss_fd *entry = NULL;
    if (fd >= 0 && fd < OSS_MAX_FDS) {
        pthread_mutex_lock(&oss_table_lock);
        entry = oss_table[fd];
        oss_table[fd] = NULL;
        pthread_mutex_unlock(&oss_table_lock);
    }
    if (entry) { TRACE("close fd %d", fd); oss_destroy(entry); }
    return real_close(fd);
}

static int oss_wants_output(const struct oss_fd *entry) { return (entry->flags & O_ACCMODE) != O_RDONLY; }

ssize_t write(int fd, const void *data, size_t bytes) {
    oss_init_real();
    struct oss_fd *entry = oss_lookup(fd);
    if (!entry) return real_write(fd, data, bytes);
    if (entry->kind != OSS_DSP || !oss_wants_output(entry)) { errno = EBADF; return -1; }
    int nonblocking = (real_fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
    const unsigned char *input = data;
    size_t done = 0;
    pthread_mutex_lock(&entry->lock);
    oss_start_thread(entry, 1);
    while (done < bytes) {
        while (entry->count == OSS_RING && !entry->stop) {
            if (nonblocking) {
                pthread_mutex_unlock(&entry->lock);
                if (done) return (ssize_t)done;
                errno = EAGAIN;
                return -1;
            }
            pthread_cond_wait(&entry->cond, &entry->lock);
        }
        size_t room = OSS_RING - entry->count, amount = bytes - done < room ? bytes - done : room;
        for (size_t i = 0; i < amount; i++)
            entry->ring[(entry->head + entry->count + i) % OSS_RING] = input[done + i];
        entry->count += amount;
        done += amount;
        pthread_cond_broadcast(&entry->cond);
    }
    pthread_mutex_unlock(&entry->lock);
    return (ssize_t)done;
}

ssize_t read(int fd, void *data, size_t bytes) {
    oss_init_real();
    struct oss_fd *entry = oss_lookup(fd);
    if (!entry) return real_read(fd, data, bytes);
    if (entry->kind != OSS_DSP || (entry->flags & O_ACCMODE) == O_WRONLY) { errno = EBADF; return -1; }
    int nonblocking = (real_fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
    unsigned char *output = data;
    pthread_mutex_lock(&entry->lock);
    entry->last_read_ms = now_ms();
    oss_start_thread(entry, 0);
    pthread_cond_broadcast(&entry->cond);
    while (!entry->count && !entry->stop) {
        if (nonblocking) { pthread_mutex_unlock(&entry->lock); errno = EAGAIN; return -1; }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&entry->cond, &entry->lock, &until);
        entry->last_read_ms = now_ms();
    }
    size_t amount = entry->count < bytes ? entry->count : bytes;
    for (size_t i = 0; i < amount; i++) output[i] = entry->ring[(entry->head + i) % OSS_RING];
    entry->head = (entry->head + amount) % OSS_RING;
    entry->count -= amount;
    pthread_mutex_unlock(&entry->lock);
    return (ssize_t)amount;
}

static void oss_mark_dirty(struct oss_fd *entry) {
    pthread_mutex_lock(&entry->lock);
    entry->dirty = 1;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->lock);
}

static int oss_dsp_ioctl(struct oss_fd *entry, unsigned long request, void *argument) {
    int *value = argument;
    switch (request) {
    case SNDCTL_DSP_RESET:
        pthread_mutex_lock(&entry->lock);
        entry->head = entry->count = 0;
        entry->flush_pending = 1;
        if (!entry->out) entry->last_read_ms = 0;
        pthread_cond_broadcast(&entry->cond);
        pthread_mutex_unlock(&entry->lock);
        return 0;
    case SNDCTL_DSP_SYNC: {
        pthread_mutex_lock(&entry->lock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (entry->count && entry->thread_running && entry->out)
            if (pthread_cond_timedwait(&entry->cond, &entry->lock, &until) != 0) break;
        pthread_mutex_unlock(&entry->lock);
        return 0;
    }
    case SNDCTL_DSP_POST:
        return 0;
    case SNDCTL_DSP_SETFMT:
        if (*value != AFMT_QUERY) {
            int format = *value == AFMT_U8 ? AFMT_U8 : AFMT_S16_LE;
            if (entry->format != format) { entry->format = format; oss_mark_dirty(entry); }
        }
        *value = entry->format;
        return 0;
    case SNDCTL_DSP_CHANNELS:
    case SNDCTL_DSP_STEREO: {
        int channels = request == SNDCTL_DSP_STEREO ? (*value ? 2 : 1) : *value;
        if (channels < 1) channels = 1;
        if (channels > 2) channels = 2;
        if (entry->channels != channels) { entry->channels = channels; oss_mark_dirty(entry); }
        *value = request == SNDCTL_DSP_STEREO ? channels == 2 : channels;
        return 0;
    }
    case SNDCTL_DSP_SPEED:
        if (*value < 4000) *value = 4000;
        if (*value > 96000) *value = 96000;
        if (entry->speed != *value) { entry->speed = *value; oss_mark_dirty(entry); }
        return 0;
    case SOUND_PCM_READ_RATE: *value = entry->speed; return 0;
    case SOUND_PCM_READ_CHANNELS: *value = entry->channels; return 0;
    case SOUND_PCM_READ_BITS: *value = entry->format == AFMT_U8 ? 8 : 16; return 0;
    case SNDCTL_DSP_SETFRAGMENT:
    case SNDCTL_DSP_SETDUPLEX:
    case SNDCTL_DSP_NONBLOCK:
        return 0;
    case SNDCTL_DSP_GETFMTS: *value = AFMT_U8 | AFMT_S16_LE; return 0;
    case SNDCTL_DSP_GETCAPS: *value = DSP_CAP_REALTIME | DSP_CAP_DUPLEX; return 0;
    case SNDCTL_DSP_GETBLKSIZE: *value = OSS_FRAGMENT; return 0;
    case SNDCTL_DSP_GETOSPACE: {
        audio_buf_info *info = argument;
        pthread_mutex_lock(&entry->lock);
        info->fragsize = OSS_FRAGMENT;
        info->fragstotal = OSS_RING / OSS_FRAGMENT;
        info->bytes = (int)(OSS_RING - entry->count);
        info->fragments = info->bytes / OSS_FRAGMENT;
        pthread_mutex_unlock(&entry->lock);
        return 0;
    }
    case SNDCTL_DSP_GETISPACE: {
        audio_buf_info *info = argument;
        pthread_mutex_lock(&entry->lock);
        info->fragsize = OSS_FRAGMENT;
        info->fragstotal = OSS_RING / OSS_FRAGMENT;
        info->bytes = (int)entry->count;
        info->fragments = info->bytes / OSS_FRAGMENT;
        pthread_mutex_unlock(&entry->lock);
        return 0;
    }
    case SNDCTL_DSP_GETODELAY: {
        pthread_mutex_lock(&entry->lock);
        *value = (int)entry->count;
        pthread_mutex_unlock(&entry->lock);
        return 0;
    }
    default:
        TRACE("unhandled dsp ioctl %#lx", request);
        errno = EINVAL;
        return -1;
    }
}

static int oss_mixer_ioctl(struct oss_fd *entry, unsigned long request, void *argument) {
    int *value = argument;
    int channel = (int)(request & 0xff);
    unsigned long direction = request & ~0xffUL;
    if (direction == (MIXER_READ(0) & ~0xffUL) && channel < SOUND_MIXER_NRDEVICES) {
        switch (channel) {
        case SOUND_MIXER_DEVMASK: case SOUND_MIXER_STEREODEVS:
            *value = (1 << SOUND_MIXER_VOLUME) | (1 << SOUND_MIXER_PCM) | (1 << SOUND_MIXER_MIC);
            return 0;
        case SOUND_MIXER_RECMASK: *value = 1 << SOUND_MIXER_MIC; return 0;
        case SOUND_MIXER_RECSRC: *value = 1 << SOUND_MIXER_MIC; return 0;
        case SOUND_MIXER_CAPS: *value = 0; return 0;
        default: *value = entry->mixer_volume[channel]; return 0;
        }
    }
    if (direction == (MIXER_WRITE(0) & ~0xffUL) && channel < SOUND_MIXER_NRDEVICES) {
        entry->mixer_volume[channel] = *value;
        return 0;
    }
    TRACE("unhandled mixer ioctl %#lx", request);
    errno = EINVAL;
    return -1;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    oss_init_real();
    struct oss_fd *entry = oss_lookup(fd);
    if (!entry) return real_ioctl(fd, request, argument);
    return entry->kind == OSS_MIXER ? oss_mixer_ioctl(entry, request, argument)
                                    : oss_dsp_ioctl(entry, request, argument);
}

int fcntl(int fd, int command, ...) {
    va_list args;
    va_start(args, command);
    void *argument = va_arg(args, void *);
    va_end(args);
    oss_init_real();
    return real_fcntl(fd, command, argument);
}

/* The placeholder descriptor is /dev/null (always ready), so select() has to
   answer for emulated descriptors itself: writable while the ring has room,
   readable while capture data is buffered. */
int select(int count, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    oss_init_real();
    int emulated = 0;
    for (int fd = 0; fd < count && fd < OSS_MAX_FDS; fd++)
        if (oss_table[fd] && ((readfds && FD_ISSET(fd, readfds)) || (writefds && FD_ISSET(fd, writefds)))) emulated = 1;
    if (!emulated) return real_select(count, readfds, writefds, exceptfds, timeout);
    fd_set want_read, want_write;
    FD_ZERO(&want_read); FD_ZERO(&want_write);
    if (readfds) want_read = *readfds;
    if (writefds) want_write = *writefds;
    uint64_t deadline = timeout ? now_ms() + (uint64_t)timeout->tv_sec * 1000 + (uint64_t)timeout->tv_usec / 1000 : 0;
    for (;;) {
        int ready = 0;
        fd_set got_read, got_write;
        FD_ZERO(&got_read); FD_ZERO(&got_write);
        for (int fd = 0; fd < count && fd < OSS_MAX_FDS; fd++) {
            struct oss_fd *entry = oss_lookup(fd);
            if (!entry) continue;
            pthread_mutex_lock(&entry->lock);
            if (FD_ISSET(fd, &want_write) && entry->count < OSS_RING) { FD_SET(fd, &got_write); ready++; }
            if (FD_ISSET(fd, &want_read)) {
                entry->last_read_ms = now_ms();
                if (entry->count) { FD_SET(fd, &got_read); ready++; }
                else if (!entry->thread_running) oss_start_thread(entry, 0);
            }
            pthread_mutex_unlock(&entry->lock);
        }
        if (ready) {
            if (readfds) { for (int fd = 0; fd < count; fd++) if (FD_ISSET(fd, &want_read) && !FD_ISSET(fd, &got_read)) FD_CLR(fd, readfds); }
            if (writefds) { for (int fd = 0; fd < count; fd++) if (FD_ISSET(fd, &want_write) && !FD_ISSET(fd, &got_write)) FD_CLR(fd, writefds); }
            return ready;
        }
        if (timeout && now_ms() >= deadline) {
            if (readfds) FD_ZERO(readfds);
            if (writefds) FD_ZERO(writefds);
            return 0;
        }
        struct timespec nap = {0, 5000000};
        nanosleep(&nap, NULL);
    }
}

/* Called by the hook after it changes T2LAUNCH_AUDIO_OUTPUT/INPUT: every open
   stream is reopened on the new device the next time its thread runs. */
void t2_oss_reopen(void) {
    pthread_mutex_lock(&oss_table_lock);
    for (int fd = 0; fd < OSS_MAX_FDS; fd++) {
        struct oss_fd *entry = oss_table[fd];
        if (!entry || entry->kind != OSS_DSP) continue;
        pthread_mutex_lock(&entry->lock);
        entry->dirty = 1;
        pthread_cond_broadcast(&entry->cond);
        pthread_mutex_unlock(&entry->lock);
    }
    pthread_mutex_unlock(&oss_table_lock);
}
