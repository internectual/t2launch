/* Test the actual patch code with read-only pages at the game's addresses. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

int main(void) {
    /* AuthInfo comes from the emulator's Auth2 certificate; the game's own
       account check and warning must run unmodified. */
    const char *scripts[] = {core_runtime_script, tribesnext_runtime_script,
                             player_pane_button_click_override};
    for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
        assert(strstr(scripts[i], "function checkNamesAndAliases") == NULL);
        assert(strstr(scripts[i], "function LaunchGui::onWake") == NULL);
        assert(strstr(scripts[i], "function MessageBoxOK") == NULL);
        assert(strstr(scripts[i], "CachedAuthInfo") == NULL);
    }

    /* Hook evaluations must be statements; a bare expression is a syntax error. */
    assert(strstr(core_runtime_script, "function t2launch_audioState()") != NULL);
    assert(strstr(core_runtime_script, "function t2launch_launchGuiReady()") != NULL);
    /* The hook evaluates these helpers even when the TribesNext script is off. */
    assert(strstr(tribesnext_runtime_script, "function t2launch_audioState()") == NULL);
    assert(strstr(tribesnext_runtime_script, "function t2launch_launchGuiReady()") == NULL);

    /* Options: the config file wins, then a legacy environment variable, then
       the default; unknown keys are kept for hook_option(). */
    char config_path[] = "/tmp/t2_hook_options_XXXXXX";
    int config_fd = mkstemp(config_path);
    assert(config_fd >= 0);
    const char config[] = "# comment\n video.full_redraw = 0 \nfuture.datablock=1.5\nnot a pair\n";
    assert(write(config_fd, config, sizeof(config) - 1) == (ssize_t)(sizeof(config) - 1));
    close(config_fd);
    setenv("T2LAUNCH_HOOK_CONFIG", config_path, 1);
    setenv("T2LAUNCH_FULL_REDRAW", "1", 1);
    setenv("T2LAUNCH_DISABLE_HOOK_COMMANDS", "1", 1);
    assert(!hook_option_bool("video.full_redraw"));
    assert(!hook_option_bool("hook.console_commands"));
    assert(hook_option_bool("hook.tribesnext"));
    assert(strcmp(hook_option_str("future.datablock"), "1.5") == 0);
    assert(hook_option_str("missing.key") == NULL);
    const char *option_args[] = {"hook_option", "future.datablock"};
    assert(strcmp(hook_option_cb(NULL, 2, option_args), "1.5") == 0);
    unlink(config_path);

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
    char path[64];
    setenv("T2LAUNCH_GAME_DIR", "/games/t2", 1);
    assert(strcmp(game_path(path, sizeof(path), "base/public.store"),
                  "/games/t2/base/public.store") == 0);
    char tiny[8];
    assert(game_path(tiny, sizeof(tiny), "rsa_helper")[0] == 0);

    char slots[3][WON_ENDPOINT_SIZE];
    assert(parse_won_endpoints(NULL, NULL, slots) && strcmp(slots[2], "127.0.0.1:15104") == 0);
    assert(parse_won_endpoints("", "16000", slots) && strcmp(slots[0], "127.0.0.1:16000") == 0);
    assert(parse_won_endpoints("won.example.net:15104", NULL, slots));
    assert(strcmp(slots[0], "won.example.net:15104") == 0 && strcmp(slots[2], slots[0]) == 0);
    assert(parse_won_endpoints("10.0.0.2:15104,10.0.0.3:15200", NULL, slots));
    assert(strcmp(slots[1], "10.0.0.3:15200") == 0 && strcmp(slots[2], "10.0.0.3:15200") == 0);
    assert(!parse_won_endpoints("a-very-long-hostname.example:15104", NULL, slots));  /* 34 bytes */
    assert(!parse_won_endpoints("10.0.0.2", NULL, slots));
    assert(!parse_won_endpoints("10.0.0.2:0", NULL, slots));
    assert(!parse_won_endpoints("bad host:1", NULL, slots));
    assert(!parse_won_endpoints("a:1,b:2,c:3,d:4", NULL, slots));
    assert(!parse_won_endpoints("a:1,,b:2", NULL, slots));
    assert(!parse_won_endpoints("", "70000", slots));

    puts("Hook mode enumeration and WON patch: host modes preserved, replacement and permissions passed");
    return 0;
}
