/* demo.playback_fix: the one-byte change of Tribes2DemoPlayback.exe (je -> jmp) at its Linux site.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/demo_playback_fix_test.c -o demo_playback_fix_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

int main(void) {
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t page = DEMO_PLAYBACK_FIX_ADDRESS & ~((uintptr_t)size - 1);
    unsigned char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    unsigned char *site = (unsigned char *)DEMO_PLAYBACK_FIX_ADDRESS;
    memcpy(site, demo_playback_fix_original, sizeof demo_playback_fix_original);

    /* A different branch opcode: refused, nothing written. */
    site[0] = 0x75;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(!install_demo_playback_fix());
    assert(site[0] == 0x75 && site[1] == 0x2a);

    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    site[0] = 0x74;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(install_demo_playback_fix());
    assert(memcmp(site, "\xeb\x2a", 2) == 0);                           /* je -> jmp, same displacement */
    assert(DEMO_PLAYBACK_FIX_ADDRESS + 2 + 0x2a == 0x0827CA86);          /* the generic path */

    /* The same bytes in the real client (test al,al; je), when it is installed. */
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    FILE *client = fopen(path, "rb");
    int checked = 0;
    if (client) {
        unsigned char found[4] = {0};
        assert(fseek(client, (long)(DEMO_PLAYBACK_FIX_ADDRESS - 2 - 0x08048000), SEEK_SET) == 0);
        assert(fread(found, 1, 4, client) == 4);
        assert(found[0] == 0x84 && found[1] == 0xc0);                    /* test %al,%al */
        assert(memcmp(found + 2, demo_playback_fix_original, 2) == 0);
        fclose(client);
        checked = 1;
    }
    printf("demo playback fix applies only to the expected je, keeps its target, %d real-client site verified passed\n",
           checked);
    return 0;
}
