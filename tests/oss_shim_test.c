/* Exercises t2_oss.so through the same libc calls Loki's OpenAL makes.
   Run with LD_PRELOAD=<t2_oss.so>; needs a PulseAudio-compatible server. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv) {
    int capture = argc > 1 && !strcmp(argv[1], "capture");
    int fd = open("/dev/dsp", O_WRONLY);
    CHECK(fd >= 0);
    int arg = 0x0001000a; CHECK(ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &arg) == 0);
    CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0);
    arg = AFMT_S16_LE; CHECK(ioctl(fd, SNDCTL_DSP_SETFMT, &arg) == 0 && arg == AFMT_S16_LE);
    arg = 2; CHECK(ioctl(fd, SNDCTL_DSP_CHANNELS, &arg) == 0 && arg == 2);
    arg = 22050; CHECK(ioctl(fd, SNDCTL_DSP_SPEED, &arg) == 0 && arg == 22050);
    audio_buf_info info; CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0);
    CHECK(info.fragsize > 0 && info.bytes > 0 && info.fragments * info.fragsize <= info.bytes);
    CHECK(fcntl(fd, F_GETFL) >= 0);
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w); struct timeval tv = {1, 0};
    CHECK(select(fd + 1, NULL, &w, NULL, &tv) == 1 && FD_ISSET(fd, &w));

    enum { SECONDS_TENTHS = 5 };
    size_t frames = 22050 * SECONDS_TENTHS / 10;
    short *tone = malloc(frames * 4);
    for (size_t i = 0; i < frames; i++) { short v = (short)(3000 * sin(i * 2 * M_PI * 440 / 22050)); tone[2 * i] = tone[2 * i + 1] = v; }
    double start = now();
    size_t total = frames * 4, done = 0;
    while (done < total) { ssize_t n = write(fd, (char *)tone + done, total - done > 2048 ? 2048 : total - done); CHECK(n > 0); done += (size_t)n; }
    CHECK(ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0);
    double elapsed = now() - start;
    printf("wrote %zu bytes in %.2fs (wanted ~%.1fs)\n", total, elapsed, SECONDS_TENTHS / 10.0);
    CHECK(elapsed > 0.25 && elapsed < 2.0);  /* paced by the device, not instant */
    CHECK(close(fd) == 0);

    int mixer = open("/dev/mixer", O_RDWR); CHECK(mixer >= 0);
    int volume = 0x2020; CHECK(ioctl(mixer, MIXER_WRITE(SOUND_MIXER_PCM), &volume) == 0);
    volume = 0; CHECK(ioctl(mixer, MIXER_READ(SOUND_MIXER_PCM), &volume) == 0 && volume == 0x2020);
    CHECK(close(mixer) == 0);

    CHECK(open("/dev/null", O_RDONLY) >= 0);  /* unrelated paths pass through */
    if (capture) {
        int in = open("/dev/dsp", O_RDONLY); CHECK(in >= 0);
        arg = AFMT_S16_LE; ioctl(in, SNDCTL_DSP_SETFMT, &arg);
        arg = 1; ioctl(in, SNDCTL_DSP_CHANNELS, &arg);
        arg = 8000; ioctl(in, SNDCTL_DSP_SPEED, &arg);
        unsigned char buf[1024]; size_t got = 0; start = now();
        while (now() - start < 0.4) { ssize_t n = read(in, buf, sizeof buf); CHECK(n > 0); got += (size_t)n; }
        printf("captured %zu bytes of 8 kHz mono in 0.4s (expect ~6400)\n", got);
        CHECK(got > 2000 && got < 12000);
        close(in);
    }
    puts("OK");
    return 0;
}
