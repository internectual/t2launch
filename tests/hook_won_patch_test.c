/* Test the actual patch code with read-only pages at the game's addresses. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

int main(void) {
    /* AuthInfo comes from the emulator's Auth2 certificate; the game's own
       account check and warning must run unmodified. */
    const char *scripts[] = {tribesnext_runtime_script, player_pane_button_click_override};
    for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
        assert(strstr(scripts[i], "function checkNamesAndAliases") == NULL);
        assert(strstr(scripts[i], "function LaunchGui::onWake") == NULL);
        assert(strstr(scripts[i], "function MessageBoxOK") == NULL);
        assert(strstr(scripts[i], "CachedAuthInfo") == NULL);
    }

    /* Hook evaluations must be statements; a bare expression is a syntax error. */
    assert(strstr(tribesnext_runtime_script, "function t2launch_audioState()") != NULL);

    sdl_rect_t mode_16_9 = {0, 0, 1920, 1080};
    sdl_rect_t mode_16_10 = {0, 0, 1920, 1200};
    sdl_rect_t *host_modes[] = {&mode_16_9, &mode_16_10, NULL};
    sdl_rect_t **windowed_any = (sdl_rect_t **)(intptr_t)-1;
    sdl_rect_t *windowed_modes[] = {&mode_16_9, NULL};
    assert(prefer_concrete_host_modes(windowed_any, host_modes) == host_modes);
    assert(prefer_concrete_host_modes(windowed_modes, host_modes) == windowed_modes);
    assert(prefer_concrete_host_modes(windowed_any, NULL) == windowed_any);
    assert(sdl_mode_count(host_modes) == 2);

    char browser_ip[64] = "IP:185.66.108.39:28000";
    strcpy(browser_ip, browser_ip + 3);
    assert(strcmp(browser_ip, "185.66.108.39:28000") == 0);

    long size = sysconf(_SC_PAGESIZE);
    uintptr_t page = 0x084846B1 & ~((uintptr_t)size - 1);
    char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    char *a = (char *)0x084846B1;
    char *b = (char *)0x084846CD;
    char *c = (char *)0x084846E9;
    strcpy(a, "tribes2.m1.sierra.com:15104");
    strcpy(b, "unsupported");
    strcpy(c, "tribes2.m3.sierra.com:15104");
    assert(mprotect(mapping, size, PROT_READ) == 0);
    assert(!redirect_won_bootstrap());
    assert(strcmp(a, "tribes2.m1.sierra.com:15104") == 0);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    strcpy(b, "tribes2.m2.sierra.com:15104");
    assert(mprotect(mapping, size, PROT_READ) == 0);
    assert(redirect_won_bootstrap());
    assert(strcmp(a, "127.0.0.1:15104") == 0);
    assert(strcmp(b, "127.0.0.1:15104") == 0);
    assert(strcmp(c, "127.0.0.1:15104") == 0);
    int protection;
    assert(readable_range((uintptr_t)a, 15, &protection));
    assert(protection == PROT_READ);
    munmap(mapping, size);
    puts("Hook mode enumeration and WON patch: host modes preserved, replacement and permissions passed");
    return 0;
}
