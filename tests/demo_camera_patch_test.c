/* review.demo_camera: the three script-visibility checks of build 25034 are removed together or not at all.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/demo_camera_patch_test.c -o demo_camera_patch_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static unsigned char *map_site(uintptr_t address, const unsigned char *original, size_t length, size_t *size_out) {
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t page = address & ~((uintptr_t)size - 1);
    unsigned char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    memcpy((void *)address, original, length);
    *size_out = size;
    return mapping;
}

static void protect(unsigned char *page, size_t size, int writable) {
    assert(mprotect(page, size, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) == 0);
}

int main(void) {
    size_t size;
    unsigned char *dump_page = map_site(DEMO_CAMERA_DUMP_ADDRESS, demo_camera_dump_original,
                                        sizeof demo_camera_dump_original, &size);
    unsigned char *get_page = map_site(DEMO_CAMERA_GETOBJECT_ADDRESS, demo_camera_getobject_original,
                                       sizeof demo_camera_getobject_original, &size);
    unsigned char *call_page = map_site(DEMO_CAMERA_CALL_ADDRESS, demo_camera_call_original,
                                        sizeof demo_camera_call_original, &size);
    unsigned char *dump_site = (unsigned char *)DEMO_CAMERA_DUMP_ADDRESS;
    unsigned char *get_site = (unsigned char *)DEMO_CAMERA_GETOBJECT_ADDRESS;
    unsigned char *call_site = (unsigned char *)DEMO_CAMERA_CALL_ADDRESS;

    /* One unexpected byte anywhere: nothing is written, not even to the sites that match. */
    get_site[0] = 0x75;
    protect(dump_page, size, 0); protect(get_page, size, 0); protect(call_page, size, 0);
    assert(!install_demo_camera());
    assert(memcmp(dump_site, demo_camera_dump_original, sizeof demo_camera_dump_original) == 0);
    assert(get_site[0] == 0x75 && get_site[1] == 0x07);
    assert(memcmp(call_site, demo_camera_call_original, sizeof demo_camera_call_original) == 0);

    protect(get_page, size, 1);
    get_site[0] = 0x74;
    protect(get_page, size, 0);
    assert(install_demo_camera());
    assert(memcmp(dump_site, "\x90\x90\x90\x90\x90\x90", 6) == 0);        /* the skip is gone */
    assert(memcmp(get_site, "\xeb\x07", 2) == 0);                          /* getObject always returns the id */
    assert(memcmp(call_site, "\xeb\x45", 2) == 0);                         /* a method call is always accepted */
    /* The short jumps keep the original branch targets. */
    assert(DEMO_CAMERA_GETOBJECT_ADDRESS + 2 + 0x07 == 0x080FCD93);
    assert(DEMO_CAMERA_CALL_ADDRESS + 2 + 0x45 == 0x080F0E65);

    /* The same bytes in the real client, when it is installed. */
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    FILE *client = fopen(path, "rb");
    int checked = 0;
    if (client) {
        static const struct { uintptr_t address; const unsigned char *bytes; size_t length; } sites[] = {
            {DEMO_CAMERA_DUMP_ADDRESS, demo_camera_dump_original, sizeof demo_camera_dump_original},
            {DEMO_CAMERA_GETOBJECT_ADDRESS, demo_camera_getobject_original, sizeof demo_camera_getobject_original},
            {DEMO_CAMERA_CALL_ADDRESS, demo_camera_call_original, sizeof demo_camera_call_original},
        };
        for (size_t i = 0; i < 3; i++) {
            unsigned char found[8] = {0};
            assert(fseek(client, (long)(sites[i].address - 0x08048000), SEEK_SET) == 0);
            assert(fread(found, 1, sites[i].length, client) == sites[i].length);
            assert(memcmp(found, sites[i].bytes, sites[i].length) == 0);
            checked++;
        }
        fclose(client);
    }
    printf("demo camera patches applied together or not at all, branch targets kept, %d real-client sites verified passed\n",
           checked);
    return 0;
}
