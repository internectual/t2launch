/* Plays a tone through /dev/dsp (t2_oss.so) for N seconds and reports the slowest write. Used with
   tests/fake_pulse.c to check that the game never blocks while the sound server is away. */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 12;
    int fd = open("/dev/dsp", O_WRONLY); if (fd < 0) { perror("open"); return 1; }
    int arg = AFMT_S16_LE; ioctl(fd, SNDCTL_DSP_SETFMT, &arg);
    arg = 2; ioctl(fd, SNDCTL_DSP_CHANNELS, &arg);
    arg = 22050; ioctl(fd, SNDCTL_DSP_SPEED, &arg);
    short tone[1024];
    for (int i = 0; i < 512; i++) tone[2*i] = tone[2*i+1] = (short)(3000 * sin(i * 2 * M_PI * 440 / 22050));
    double start = now(), worst = 0; long writes = 0;
    while (now() - start < seconds) {
        double t = now();
        if (write(fd, tone, sizeof tone) < 0) { perror("write"); return 2; }
        double dt = now() - t; if (dt > worst) worst = dt; writes++;
    }
    printf("writes=%ld worst_write=%.3fs\n", writes, worst);
    close(fd);
    return 0;
}
