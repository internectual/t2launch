/* Test the actual patch code with read-only pages at the game's addresses. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <sys/stat.h>

/* A BitStream stand-in for the native handshake: bits in order, LSB first. */
struct fake_stream { unsigned char bits[16384]; int length, position; };
static char fake_write_flag(void *stream, int flag) {
    struct fake_stream *fake = stream;
    fake->bits[fake->length++] = (flag & 0xff) != 0;
    return (flag & 0xff) != 0;
}
static void fake_write_int(void *stream, int value, int bits) {
    struct fake_stream *fake = stream;
    for (int i = 0; i < bits; i++) fake->bits[fake->length++] = (value >> i) & 1;
}
static int fake_read_int(void *stream, int bits) {
    struct fake_stream *fake = stream;
    int value = 0;
    for (int i = 0; i < bits; i++)
        if (fake->position < fake->length && fake->bits[fake->position++]) value |= 1 << i;
    return value;
}
static int32_t call_target_offset(uintptr_t site) {
    int32_t offset;
    memcpy(&offset, (const void *)(site + 1), sizeof(offset));
    return offset;
}
/* A HUD bar's value function stand-in (vtable +0x118). */
static float fake_value;
static float fake_bar_value(void *control) { (void)control; return fake_value; }
#define TEST_RSA_MODULUS "8302c77f71653bd46d53472094c45171bbb1b2dc8408bab25ea4b016be4da0869737ea99ca887a4869a09176c43e73eab974ccdbf9a25ab8ccbb86fa343720f600c6cd6be3a4599a2796868790a2051e15607d06b470ad281b57e66fde832e576a760fb757c8ea3f6c82ed0381caeddfcbb6a0e467fc253b1e86d3f50341ea95"
#define TEST_RSA_PRIVATE "346782ffc6f54b21c55482d9d51b53c717e0adf1ce69e447590eacd5e5b8a69c3c7cc43d843697502a403a2f81b294c44a2eb857fd742449eb7e35fdae7c739506d703ccf3688366bd209480bc8e0a517d37ec89912132399cf3003298fef461bee2565b46084f4983a1f5accf6ead869d3d4c187b6ffa2f04c95250a26c130d"

int main(void) {
    /* AuthInfo comes from the emulator's Auth2 certificate; the game's own
       account check and warning must run unmodified. */
    const char *scripts[] = {core_runtime_script, tribesnext_runtime_script,
                             player_pane_button_click_override, depot_downloads_script,
                             discord_presence_script};
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
    /* The game clock jumps to the CLOCK_MONOTONIC replacement only over the
       expected getRealMilliseconds prologue, and restores the protection. */
    page = REAL_MILLISECONDS_ADDRESS & ~((uintptr_t)size - 1);
    mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    unsigned char *timer = (unsigned char *)REAL_MILLISECONDS_ADDRESS;
    memcpy(timer, real_milliseconds_original, sizeof(real_milliseconds_original));
    timer[7] = 0x10;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(!install_monotonic_clock());
    assert(timer[0] == 0x0f);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    timer[7] = 0x14;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(install_monotonic_clock());
    int32_t offset;
    memcpy(&offset, timer + 1, sizeof(offset));
    assert(timer[0] == 0xe9);
    assert(REAL_MILLISECONDS_ADDRESS + 5 + (uintptr_t)offset == (uintptr_t)monotonic_real_milliseconds);
    assert(memcmp(timer + 5, real_milliseconds_original + 5, 3) == 0);
    assert(readable_range(REAL_MILLISECONDS_ADDRESS, 5, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    uint32_t (*patched_clock)(void) = (uint32_t (*)(void))REAL_MILLISECONDS_ADDRESS;
    uint32_t before = patched_clock();
    struct timespec pause = {0, 20 * 1000000};
    nanosleep(&pause, NULL);
    uint32_t after = patched_clock();
    assert(after >= before + 19 && after < before + 1000);
    munmap(mapping, size);

    /* The browser's default bot limit changes only over the expected code. */
    page = BROWSER_BOT_LIMIT_ADDRESS & ~((uintptr_t)size - 1);
    mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    unsigned char *bot_limit = (unsigned char *)BROWSER_BOT_LIMIT_ADDRESS;
    memcpy(bot_limit, browser_bot_limit_original, sizeof(browser_bot_limit_original));
    bot_limit[9] = 0x07;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(!install_browser_bot_limit());
    assert(bot_limit[1] == 0x10);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    bot_limit[9] = 0x06;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(install_browser_bot_limit());
    assert(bot_limit[0] == 0xb8 && bot_limit[1] == 0xff);
    assert(memcmp(bot_limit + 2, browser_bot_limit_original + 2, sizeof(browser_bot_limit_original) - 2) == 0);
    assert(readable_range(BROWSER_BOT_LIMIT_ADDRESS, 2, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    munmap(mapping, size);
    assert(strstr(filter_max_bots_override, "function FilterEditDlg::setMaxBots(%this)") != NULL);
    assert(strstr(filter_max_bots_override, "> 255") != NULL);

    /* HUD bars: the layout matches the stock fill, stores the value and fills
       vertically when asked; the patch applies only over the stock bytes. */
    {
        static unsigned char control[0x100];
        static void *vtable[0x120 / 4];
        vtable[0x118 / 4] = (void *)fake_bar_value;
        memset(control, 0, sizeof(control));
        *(void **)control = vtable;
        int32_t bounds[4] = {10, 20, 100, 8};
        memcpy(control + 0xC4, bounds, sizeof(bounds));
        int32_t offset[2] = {5, 7}, frame[6];
        fake_value = 0.5f;
        hud_bar_store_value = 0; hud_bar_vertical_fill = 1;
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 && frame[3] == 27 && frame[4] == 50 && frame[5] == 8);
        assert(*(float *)(control + HUD_BAR_VALUE_OFFSET) == 0.0f);
        hud_bar_store_value = 1;
        control[HUD_BAR_VERTICAL_OFFSET] = 1;
        int32_t tall[2] = {10, 40};
        memcpy(control + 0xCC, tall, sizeof(tall));
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 && frame[3] == 27 + 20 && frame[4] == 10 && frame[5] == 20);
        assert(*(float *)(control + HUD_BAR_VALUE_OFFSET) == 0.5f);
        fake_value = 1.0f / 40.0f;  /* a one-pixel fill is drawn as two, as stock */
        hud_bar_layout(frame, control, offset);
        assert(frame[5] == 2 && frame[3] == 27 + 38);
        hud_bar_vertical_fill = 0;
        hud_bar_layout(frame, control, offset);
        assert(frame[4] == 0 && frame[5] == 40);

        uintptr_t hud_page = HUD_BAR_LAYOUT_ADDRESS & ~((uintptr_t)size - 1);
        assert(HUD_BAR_LAYOUT_ADDRESS + sizeof(hud_bar_layout_original) <= hud_page + (uintptr_t)size);
        unsigned char *hud = mmap((void *)hud_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(hud == (void *)hud_page);
        unsigned char *site = (unsigned char *)HUD_BAR_LAYOUT_ADDRESS;
        memcpy(site, hud_bar_layout_original, sizeof(hud_bar_layout_original));
        site[10] = 0x02;
        assert(mprotect(hud, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_hud_bars(1, 1));
        assert(mprotect(hud, size, PROT_READ | PROT_WRITE) == 0);
        site[10] = 0x00;
        assert(mprotect(hud, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_hud_bars(1, 0));
        assert(hud_bar_store_value == 1 && hud_bar_vertical_fill == 0);
        assert(memcmp(site, "\x53\x56\x8d\x45\xe8\x50\xe8", 7) == 0);
        assert(HUD_BAR_LAYOUT_ADDRESS + 11 + (uintptr_t)call_target_offset(HUD_BAR_LAYOUT_ADDRESS + 6) ==
               (uintptr_t)hud_bar_layout);
        assert(memcmp(site + 11, "\x83\xc4\x0c\xe9", 4) == 0);
        assert(HUD_BAR_LAYOUT_ADDRESS + 19 + (uintptr_t)call_target_offset(HUD_BAR_LAYOUT_ADDRESS + 14) ==
               HUD_BAR_LAYOUT_RESUME);
        assert(site[19] == 0x90 && site[sizeof(hud_bar_layout_original) - 1] == 0x90);
        munmap(hud, size);

        uintptr_t iff_page = OBSERVER_IFF_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *iff = mmap((void *)iff_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(iff == (void *)iff_page);
        memcpy((void *)OBSERVER_IFF_ADDRESS, observer_iff_original, sizeof(observer_iff_original));
        assert(mprotect(iff, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_observer_iff());
        assert(memcmp((void *)OBSERVER_IFF_ADDRESS, "\xeb\x4e\x8b\x88", 4) == 0);
        munmap(iff, size);

        /* setFov's limits: the max load points at 360.0, fld1 becomes fldz. */
        uintptr_t fov_page = FOV_MAX_LOAD & ~((uintptr_t)size - 1);
        unsigned char *fov = mmap((void *)fov_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(fov == (void *)fov_page);
        memcpy((void *)FOV_MAX_LOAD, fov_max_load_original, sizeof(fov_max_load_original));
        memcpy((void *)FOV_MIN_LOAD, fov_min_load_original, sizeof(fov_min_load_original));
        assert(mprotect(fov, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_fov_range());
        uint32_t max_address;
        memcpy(&max_address, (void *)(FOV_MAX_LOAD + 2), 4);
        assert(*(const float *)(uintptr_t)max_address == 360.0f);
        assert(memcmp((void *)FOV_MIN_LOAD, "\xd9\xee\xd8\xd1", 4) == 0);
        munmap(fov, size);

        /* Client datablocks: two je become jmp, the jne becomes nops. */
        uintptr_t data_page = CLIENT_SET_DATABLOCK_CHECK & ~((uintptr_t)size - 1);
        assert(mmap((void *)data_page, 2 * size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)data_page);
        assert(CLIENT_GET_DATABLOCK_CHECK + 5 <= data_page + 2 * (uintptr_t)size);
        memcpy((void *)CLIENT_GET_DATABLOCK_CHECK, client_get_datablock_original, 5);
        memcpy((void *)CLIENT_SET_DATABLOCK_CHECK, client_set_datablock_original, 5);
        memcpy((void *)CLIENT_SET_TARGET_CHECK, client_set_target_original, 6);
        assert(mprotect((void *)data_page, 2 * size, PROT_READ | PROT_EXEC) == 0);
        assert(install_client_datablocks());
        assert(((unsigned char *)CLIENT_GET_DATABLOCK_CHECK)[0] == 0xeb);
        assert(((unsigned char *)CLIENT_SET_DATABLOCK_CHECK)[0] == 0xeb);
        assert(memcmp((void *)CLIENT_SET_TARGET_CHECK, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        munmap((void *)data_page, 2 * size);
    }

    /* Network packet limits: twelve sites over two pages, all or nothing, at
       each executable's own sites and variables. */
    for (size_t t = 0; t < GAME_TARGET_COUNT; t++) {
        target = &game_targets[t];
        const uintptr_t *sites = target->net_rate_compare;
        uintptr_t pages[2] = {sites[0] & ~((uintptr_t)size - 1), sites[1] & ~((uintptr_t)size - 1)};
        assert(sites[2] + NET_SIZE_STORE_OFFSET + 10 <= pages[1] + (uintptr_t)size);
        for (int i = 0; i < 2; i++)
            assert(mmap((void *)pages[i], size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)pages[i]);
        uint32_t rate_variable = (uint32_t)target->net_rate_variable;
        uint32_t size_variable = (uint32_t)target->net_size_variable;
        for (int i = 0; i < 3; i++) {
            unsigned char *site = (unsigned char *)sites[i];
            memcpy(site, "\x83\x3d", 2); memcpy(site + 2, &rate_variable, 4); site[6] = 0x20;
            unsigned char *store = site + NET_RATE_STORE_OFFSET;
            memcpy(store, "\xc7\x05", 2); memcpy(store + 2, &rate_variable, 4);
            memcpy(store + 6, "\x20\x00\x00\x00", 4);
            unsigned char *size_compare = site + NET_SIZE_COMPARE_OFFSET;
            memcpy(size_compare, "\x81\x3d", 2); memcpy(size_compare + 2, &size_variable, 4);
            memcpy(size_compare + 6, "\xc2\x01\x00\x00", 4);
            unsigned char *size_store = site + NET_SIZE_STORE_OFFSET;
            memcpy(size_store, "\xc7\x05", 2); memcpy(size_store + 2, &size_variable, 4);
            memcpy(size_store + 6, "\xc2\x01\x00\x00", 4);
        }
        unsigned char *last = (unsigned char *)sites[2] + NET_SIZE_STORE_OFFSET;
        last[6] = 0xc3;
        for (int i = 0; i < 2; i++) assert(mprotect((void *)pages[i], size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_net_packet_limits());
        assert(((unsigned char *)sites[0])[6] == 0x20);
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_WRITE) == 0);
        last[6] = 0xc2;
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_EXEC) == 0);
        assert(install_net_packet_limits());
        for (int i = 0; i < 3; i++) {
            const unsigned char *site = (const unsigned char *)sites[i];
            uint32_t value;
            assert(site[6] == 64);
            memcpy(&value, site + NET_RATE_STORE_OFFSET + 6, 4);
            assert(value == 64);
            memcpy(&value, site + NET_SIZE_COMPARE_OFFSET + 6, 4);
            assert(value == 1000);
            memcpy(&value, site + NET_SIZE_STORE_OFFSET + 6, 4);
            assert(value == 1000);
            memcpy(&value, site + NET_SIZE_STORE_OFFSET + 2, 4);
            assert(value == size_variable);
        }
        assert(readable_range(sites[0], 7, &protection) && protection == (PROT_READ | PROT_EXEC));
        for (int i = 0; i < 2; i++) munmap((void *)pages[i], size);
    }
    target = &game_targets[0];
    assert(strstr(net_options_override, "OP_PacketRateSlider.range = \"2 64\"") != NULL);
    assert(strstr(net_options_override, "\\t64\\t32\\t1000") != NULL);

    /* The dedicated server's WON strings and clock live at its own addresses. */
    {
        target = &game_targets[1];
        assert(target->dedicated && strcmp(target->executable, "tribes2d.dynamic") == 0);
        uintptr_t won_page = target->won_bootstrap[0] & ~((uintptr_t)size - 1);
        char *won = mmap((void *)won_page, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(won == (void *)won_page);
        strcpy((char *)target->won_bootstrap[0], "tribes2.m1.sierra.com:15104");
        strcpy((char *)target->won_bootstrap[1], "tribes2.m2.sierra.com:15104");
        strcpy((char *)target->won_bootstrap[2], "tribes2.m3.sierra.com:15104");
        assert(mprotect(won, size, PROT_READ) == 0);
        assert(redirect_won_bootstrap());
        assert(strcmp((char *)target->won_bootstrap[2], "127.0.0.1:15104") == 0);
        munmap(won, size);
        uintptr_t clock_page = target->real_milliseconds & ~((uintptr_t)size - 1);
        unsigned char *clock = mmap((void *)clock_page, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(clock == (void *)clock_page);
        memcpy((void *)target->real_milliseconds, real_milliseconds_original, sizeof(real_milliseconds_original));
        assert(mprotect(clock, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_monotonic_clock());
        assert(((unsigned char *)target->real_milliseconds)[0] == 0xe9);
        munmap(clock, size);

        /* Clients without a WON certificate: both server patches, all or nothing. */
        uintptr_t auth_page = SERVER_REPLY_CERT_FLAG & ~((uintptr_t)size - 1);
        assert((SERVER_LAN_CHECK & ~((uintptr_t)size - 1)) == auth_page);
        unsigned char *auth = mmap((void *)auth_page, size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(auth == (void *)auth_page);
        memcpy((void *)SERVER_REPLY_CERT_FLAG, server_reply_cert_flag_original,
               sizeof(server_reply_cert_flag_original));
        memcpy((void *)SERVER_LAN_CHECK, server_lan_check_original, sizeof(server_lan_check_original));
        ((unsigned char *)SERVER_LAN_CHECK)[9] = 0x43;
        /* The ping and info skips are on two adjacent pages. */
        uintptr_t query_page = SERVER_PING_QUERY_SKIP & ~((uintptr_t)size - 1);
        assert((SERVER_INFO_QUERY_SKIP & ~((uintptr_t)size - 1)) == query_page + (uintptr_t)size);
        assert(mmap((void *)query_page, 2 * size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)query_page);
        memcpy((void *)SERVER_PING_QUERY_SKIP, server_ping_query_skip_original, 6);
        memcpy((void *)SERVER_INFO_QUERY_SKIP, server_info_query_skip_original, 6);
        assert(mprotect((void *)query_page, 2 * size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(auth, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_server_tribesnext_auth());
        assert(memcmp((void *)SERVER_REPLY_CERT_FLAG, server_reply_cert_flag_original, 10) == 0);
        assert(mprotect(auth, size, PROT_READ | PROT_WRITE) == 0);
        ((unsigned char *)SERVER_LAN_CHECK)[9] = 0x42;
        assert(mprotect(auth, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_server_tribesnext_auth());
        const unsigned char *flag = (const unsigned char *)SERVER_REPLY_CERT_FLAG;
        assert(memcmp(flag, "\x83\xbe\x54\x21\x00\x00\x00\x0f\x95\xc0", 10) == 0);
        const unsigned char *lan = (const unsigned char *)SERVER_LAN_CHECK;
        assert(lan[0] == 0xe9 && SERVER_LAN_CHECK + 5 + (uintptr_t)call_target_offset(SERVER_LAN_CHECK) ==
               0x0822BFD4);
        munmap(auth, size);
        /* Browser queries from clients without a certificate: both skips became nops. */
        assert(memcmp((void *)SERVER_PING_QUERY_SKIP, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        assert(memcmp((void *)SERVER_INFO_QUERY_SKIP, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        munmap((void *)query_page, 2 * size);
        target = &game_targets[0];
    }

    /* T2CSRI server helpers: hex addresses both ways, and certificates signed
       by the TribesNext root key (a real one from the game's public.store
       when there is one). */
    {
        const char *hex_args[] = {"hook_tn_hex_ipv4", "7f000001"};
        assert(strcmp(hook_tn_hex_ipv4_cb(NULL, 2, hex_args), "127.0.0.1") == 0);
        const char *bad_hex[] = {"hook_tn_hex_ipv4", "7f0000zz"};
        assert(strcmp(hook_tn_hex_ipv4_cb(NULL, 2, bad_hex), "") == 0);
        const char *dotted[] = {"hook_tn_ipv4_hex", "10.0.0.7"};
        assert(strcmp(hook_tn_ipv4_hex_cb(NULL, 2, dotted), "0a000007") == 0);
        const char *malformed[] = {"hook_tn_verify_cert", "name\t12\t3\tabc"};
        assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, malformed), "0") == 0);
        const char *bad_guid[] = {"hook_tn_verify_cert", "name\t1x\t3\tabc\tdef"};
        assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, bad_guid), "0") == 0);
        setenv("T2LAUNCH_RSA_HELPER", getenv("T2LAUNCH_TEST_RSA_HELPER") ?
               getenv("T2LAUNCH_TEST_RSA_HELPER") : "games/tribes2/rsa_helper", 1);
        char store_path[4096];
        snprintf(store_path, sizeof(store_path), "%s/t2-linux/base/public.store", getenv("HOME"));
        FILE *store = fopen(store_path, "r");
        static char line[20001];
        if (store && fgets(line, sizeof(line), store)) {
            line[strcspn(line, "\r\n")] = 0;
            const char *cert[] = {"hook_tn_verify_cert", line};
            assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, cert), "1") == 0);
            line[0] ^= 0x20;  /* another name: the signature no longer matches */
            assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, cert), "0") == 0);
        }
        if (store) fclose(store);
    }
    assert(strstr(server_tribesnext_script, "package T2LaunchTribesNextServer") != NULL);
    /* Without a public address the check falls back to the LAN, never to any address. */
    assert(strstr(server_tribesnext_script, " return t2launch_tnSameLan(%source, %destination);\n") != NULL);
    assert(strstr(server_tribesnext_script, "return %public $= \"\"") == NULL);
    assert(strstr(server_tribesnext_script, "function serverCmdt2csri_challengeResponse(%client, %serverChallenge)") != NULL);

    /* Native TribesNext handshake: blob layout, nonce search, the three
       BitStream hooks against a real RSA round trip, and the code patches. */
    {
        unsigned char bytes[8];
        assert(native_auth_hex_bytes("000a1b", 6, bytes, sizeof(bytes)) == 2 &&
               bytes[0] == 0x0a && bytes[1] == 0x1b);
        assert(native_auth_hex_bytes("5", 1, bytes, sizeof(bytes)) == 1 && bytes[0] == 5);
        assert(native_auth_hex_bytes("00", 2, bytes, sizeof(bytes)) == 1 && bytes[0] == 0);
        assert(native_auth_hex_bytes("xyz", 3, bytes, sizeof(bytes)) == -1);
        assert(native_auth_hex_bytes("112233445566778899", 18, bytes, sizeof(bytes)) == -1);

        static char certificate[2048], signature[1022];
        memset(signature, '0', sizeof(signature) - 1);
        signature[0] = '1';                    /* 1021 digits: 511 bytes, padded to 512 */
        signature[sizeof(signature) - 1] = 0;
        snprintf(certificate, sizeof(certificate), "Player\t0004321\t05\t%s\t%s", TEST_RSA_MODULUS, signature);
        unsigned char blob[NATIVE_AUTH_BLOB_MAX];
        int length = native_auth_blob(certificate, blob, sizeof(blob));
        const char *header = "Player\t4321\t1\t129\t";
        size_t header_length = strlen(header);
        assert(length == (int)(header_length + 1 + 1 + 128 + 512));
        assert(memcmp(blob, header, header_length) == 0);
        assert(blob[header_length] == 5 && blob[header_length + 1] == 0);
        assert(blob[header_length + 2] == 0x83 && blob[header_length + 129] == 0x95);
        assert(blob[header_length + 130] == 0 && blob[header_length + 131] == 0x01);
        assert(native_auth_blob("Player\t1\t05\tab", blob, sizeof(blob)) == -1);
        snprintf(certificate, sizeof(certificate), "Player\tguid\t05\t%s\t%s", TEST_RSA_MODULUS, signature);
        assert(native_auth_blob(certificate, blob, sizeof(blob)) == -1);
        snprintf(certificate, sizeof(certificate), "Player\t0004321\t05\t%s\t%s", TEST_RSA_MODULUS, signature);

        unsigned char plain[128] = {0}, nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8}, block[64];
        memcpy(plain + 20, nonce, 8);
        plain[60] = 0xaa;
        assert(native_auth_find_block(plain, 128, nonce, block) && block[40] == 0xaa);
        memset(plain, 0, sizeof(plain));
        memcpy(plain + 70, nonce, 8);
        assert(!native_auth_find_block(plain, 128, nonce, block));

        /* The pending connection's address (an IP NetAddress) and a real key. */
        uintptr_t address_page = NATIVE_AUTH_SERVER_ADDRESS & ~((uintptr_t)size - 1);
        assert(mmap((void *)address_page, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)address_page);
        int32_t ip_type = 1;
        memcpy((void *)NATIVE_AUTH_SERVER_ADDRESS, &ip_type, 4);
        memcpy((void *)(NATIVE_AUTH_SERVER_ADDRESS + 4), "\x0a\x00\x00\x07", 4);
        bitstream_write_flag = fake_write_flag;
        bitstream_write_int = fake_write_int;
        bitstream_read_int = fake_read_int;
        static struct fake_stream stream;

        /* Without unlocked credentials the stock WON flag goes through. */
        tribesnext_certificate[0] = 0;
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_challenge_flag(&stream, 0x7701) == 1 && stream.length == 1 && stream.bits[0]);

        snprintf(tribesnext_certificate, sizeof(tribesnext_certificate), "%s", certificate);
        snprintf(tribesnext_modulus, sizeof(tribesnext_modulus), "%s", TEST_RSA_MODULUS);
        snprintf(tribesnext_private_exponent, sizeof(tribesnext_private_exponent), "%s", TEST_RSA_PRIVATE);
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_challenge_flag(&stream, 1) == 0 && native_auth.sent);
        assert(fake_read_int(&stream, 1) == 0 && fake_read_int(&stream, 1) == 1);
        assert(fake_read_int(&stream, 8) == NATIVE_AUTH_MARKER);
        for (int i = 0; i < 8; i++) assert(fake_read_int(&stream, 8) == native_auth.nonce[i]);
        assert(fake_read_int(&stream, 8) == 10 && fake_read_int(&stream, 8) == 0 &&
               fake_read_int(&stream, 8) == 0 && fake_read_int(&stream, 8) == 7);
        assert(fake_read_int(&stream, 12) == length);
        for (int i = 0; i < length; i++) assert(fake_read_int(&stream, 8) == blob[i]);
        assert(stream.position == stream.length);

        /* The server's challenge: the nonce and 56 more bytes, encrypted to the account key. */
        char helper[4096];
        snprintf(helper, sizeof(helper), "%s", getenv("T2LAUNCH_TEST_RSA_HELPER") ?
                 getenv("T2LAUNCH_TEST_RSA_HELPER") : "games/tribes2/rsa_helper");
        assert(access(helper, X_OK) == 0);
        setenv("T2LAUNCH_RSA_HELPER", helper, 1);
        memset(plain, 0, sizeof(plain));
        plain[1] = 0x42;  /* nonzero bytes before the nonce are skipped */
        memcpy(plain + 5, native_auth.nonce, 8);
        for (int i = 13; i < 69; i++) plain[i] = (unsigned char)(i * 7);
        char plain_hex[257], cipher_hex[2048];
        bytes_to_hex(plain, sizeof(plain), plain_hex);
        assert(run_rsa_helper(plain_hex, "5", TEST_RSA_MODULUS, cipher_hex, sizeof(cipher_hex)));
        unsigned char cipher[128];
        int cipher_length = native_auth_hex_bytes(cipher_hex, strlen(cipher_hex), cipher, sizeof(cipher));
        assert(cipher_length >= 64 && cipher_length <= 128);
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 1);
        fake_write_int(&stream, cipher_length, 9);
        for (int i = 0; i < cipher_length; i++) fake_write_int(&stream, cipher[i], 8);
        assert(native_auth_reply_flag(&stream) == 0 && native_auth.verified && !native_auth.sent);
        assert(memcmp(native_auth.block, plain + 5, 64) == 0);

        memset(&stream, 0, sizeof(stream));
        assert(native_auth_connect_flag(&stream, 0) == 0);
        assert(fake_read_int(&stream, 1) == 1);
        for (int i = 12; i < 52; i++) assert(fake_read_int(&stream, 8) == plain[5 + i]);
        assert(stream.position == stream.length && !native_auth.verified);
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_connect_flag(&stream, 1) == 1 && stream.length == 1);

        /* A stock server's WON flag reaches the stock code unchanged. */
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 1);
        assert(native_auth_reply_flag(&stream) == 1);
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 0);
        assert(native_auth_reply_flag(&stream) == 0);
        munmap((void *)address_page, size);
        tribesnext_certificate[0] = tribesnext_modulus[0] = tribesnext_private_exponent[0] = 0;

        /* The patches: two calls retargeted and the inline flag read replaced. */
        uintptr_t pages[2] = {0x08236000, 0x08237000};
        for (int i = 0; i < 2; i++)
            assert(mmap((void *)pages[i], size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)pages[i]);
        memcpy((void *)NATIVE_AUTH_CHALLENGE_FLAG_CALL, native_auth_challenge_call_original, 5);
        memcpy((void *)NATIVE_AUTH_CONNECT_FLAG_CALL, native_auth_connect_call_original, 5);
        memcpy((void *)NATIVE_AUTH_REPLY_FLAG_READ, native_auth_reply_read_original,
               sizeof(native_auth_reply_read_original));
        ((unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ)[13] = 0x01;
        for (int i = 0; i < 2; i++) assert(mprotect((void *)pages[i], size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_native_auth());
        assert(memcmp((void *)NATIVE_AUTH_CHALLENGE_FLAG_CALL, native_auth_challenge_call_original, 5) == 0);
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_WRITE) == 0);
        ((unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ)[13] = 0x00;
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_EXEC) == 0);
        assert(NATIVE_AUTH_CHALLENGE_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CHALLENGE_FLAG_CALL) ==
               BITSTREAM_WRITE_FLAG);
        assert(NATIVE_AUTH_CONNECT_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CONNECT_FLAG_CALL) ==
               BITSTREAM_WRITE_FLAG);
        assert(install_native_auth());
        assert(NATIVE_AUTH_CHALLENGE_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CHALLENGE_FLAG_CALL) ==
               (uintptr_t)native_auth_challenge_flag);
        assert(NATIVE_AUTH_CONNECT_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CONNECT_FLAG_CALL) ==
               (uintptr_t)native_auth_connect_flag);
        const unsigned char *reply = (const unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ;
        assert(reply[0] == 0x57 && reply[1] == 0xe8 && reply[6] == 0x83 && reply[7] == 0xc4 &&
               reply[8] == 0x04 && reply[9] == 0xe9);
        assert(NATIVE_AUTH_REPLY_FLAG_READ + 6 + call_target_offset(NATIVE_AUTH_REPLY_FLAG_READ + 1) ==
               (uintptr_t)native_auth_reply_flag);
        assert(NATIVE_AUTH_REPLY_FLAG_READ + 14 + call_target_offset(NATIVE_AUTH_REPLY_FLAG_READ + 9) ==
               NATIVE_AUTH_REPLY_FLAG_STORE);
        for (int i = 0; i < 2; i++) munmap((void *)pages[i], size);
    }

    /* Locked arrays: only the exact extension token is removed. */
    {
        char out[256];
        remove_extension("GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_compiled_vertex_array_x "
                         "GL_EXT_texture_env_add", LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "GL_ARB_multitexture GL_EXT_compiled_vertex_array_x GL_EXT_texture_env_add") == 0);
        remove_extension(LOCKED_ARRAYS_EXTENSION, LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "") == 0);
        remove_extension("  GL_A  " LOCKED_ARRAYS_EXTENSION " GL_B ", LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "GL_A GL_B") == 0);
    }

    /* Frame cap: steady frames sleep to the deadline; a late frame restarts it. */
    {
        struct timespec deadline = {0, 0}, now = {100, 0};
        const long period = 1000000000L / 100;
        assert(frame_cap_delay_ns(&deadline, &now, period) == 0);
        assert(deadline.tv_sec == 100 && deadline.tv_nsec == period);
        now.tv_nsec = 4000000;
        assert(frame_cap_delay_ns(&deadline, &now, period) == period - 4000000);
        assert(deadline.tv_nsec == 2 * period);
        now.tv_sec = 101;
        assert(frame_cap_delay_ns(&deadline, &now, period) == 0);
        assert(deadline.tv_sec == 101 && deadline.tv_nsec == 4000000 + period);
    }

    /* Web links: only the marker with a plain http(s)/ftp URL is opened. */
    assert(strcmp(web_link_url(WEB_LINK_MARKER "https://www.tribesnext.com/forum"),
                  "https://www.tribesnext.com/forum") == 0);
    assert(web_link_url(WEB_LINK_MARKER "ftp://example.net/map.vl2") != NULL);
    assert(web_link_url("netscape https://www.tribesnext.com") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "file:///etc/passwd") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/';touch /tmp/pwned;'") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/a b") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/$(id)`id`") == NULL);
    assert(system(WEB_LINK_MARKER "javascript:alert(1)") == -1);
    assert(system("exit 3") != -1);
    /* The URL reaches xdg-open as one argument, without the game's loader settings. */
    char bin_dir[] = "/tmp/t2_hook_xdg_XXXXXX";
    assert(mkdtemp(bin_dir));
    char fake_open[96], opened[96], saved_path[4096];
    snprintf(fake_open, sizeof(fake_open), "%s/xdg-open", bin_dir);
    snprintf(opened, sizeof(opened), "%s/opened", bin_dir);
    FILE *script = fopen(fake_open, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\nprintf '%%s|%%s|%%s' \"$#\" \"$1\" \"${LD_PRELOAD-unset}\" > '%s'\n", opened);
    fclose(script);
    assert(chmod(fake_open, 0700) == 0);
    snprintf(saved_path, sizeof(saved_path), "%s", getenv("PATH"));
    setenv("PATH", bin_dir, 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(system(WEB_LINK_MARKER "https://www.tribesnext.com/?a=1&b=$HOME") == 0);
    setenv("PATH", saved_path, 1);
    unsetenv("LD_PRELOAD");
    char result[256] = "";
    for (int tries = 0; tries < 100 && !result[0]; tries++) {
        FILE *seen = fopen(opened, "r");
        if (seen) { if (!fgets(result, sizeof(result), seen)) result[0] = 0; fclose(seen); }
        if (!result[0]) usleep(20000);
    }
    assert(strcmp(result, "1|https://www.tribesnext.com/?a=1&b=$HOME|unset") == 0);
    unlink(opened); unlink(fake_open); rmdir(bin_dir);

    /* Depot downloads: the helper gets the file name after "--", runs without
       the game's loader settings, and its one output line is the status. */
    assert(strcmp(hook_depot_status_cb(NULL, 1, NULL), "error no download is running") == 0);
    char depot_dir[] = "/tmp/t2_hook_depot_XXXXXX";
    assert(mkdtemp(depot_dir));
    char fake_depot[96];
    snprintf(fake_depot, sizeof(fake_depot), "%s/depot.py", depot_dir);
    script = fopen(fake_depot, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\n[ \"$#|$1|$3|${LD_PRELOAD-unset}\" = \"4|--dest|--|unset\" ] || exit 2\n"
                    "[ \"$4\" = 'missing map.ter' ] || { echo \"error not in the depot\"; exit 1; }\n"
                    "echo \"ok $2/terrains/$4\"\n");
    fclose(script);
    assert(chmod(fake_depot, 0700) == 0);
    const char *depot_args[] = {"hook_depot_fetch", "missing map.ter"};
    unsetenv("T2LAUNCH_DEPOT_HELPER");
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "0") == 0);
    setenv("T2LAUNCH_DEPOT_HELPER", fake_depot, 1);
    setenv("HOME", "/home/player", 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "1") == 0);
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "0") == 0);  /* one at a time */
    unsetenv("LD_PRELOAD");
    const char *depot_status = "running";
    for (int tries = 0; tries < 250 && strcmp(depot_status, "running") == 0; tries++) {
        usleep(20000);
        depot_status = hook_depot_status_cb(NULL, 1, NULL);
    }
    assert(strcmp(depot_status, "ok /home/player/.loki/tribes2/base/terrains/missing map.ter") == 0);
    depot_args[1] = "other.dif";
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "1") == 0);
    depot_status = "running";
    for (int tries = 0; tries < 250 && strcmp(depot_status, "running") == 0; tries++) {
        usleep(20000);
        depot_status = hook_depot_status_cb(NULL, 1, NULL);
    }
    assert(strcmp(depot_status, "error not in the depot") == 0);
    unlink(fake_depot); rmdir(depot_dir);

    /* Discord presence: the helper reads one line per state change on stdin,
       runs without the game's loader settings, and sees EOF when the game
       closes its end. */
    char discord_dir[] = "/tmp/t2_hook_discord_XXXXXX";
    assert(mkdtemp(discord_dir));
    char fake_discord[96], discord_seen[96];
    snprintf(fake_discord, sizeof(fake_discord), "%s/discord.py", discord_dir);
    snprintf(discord_seen, sizeof(discord_seen), "%s/seen", discord_dir);
    script = fopen(fake_discord, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\necho \"$#|$1|$2|$3|$4|${LD_PRELOAD-unset}\" > '%s'\ncat >> '%s'\n",
            discord_seen, discord_seen);
    fclose(script);
    assert(chmod(fake_discord, 0700) == 0);
    unsetenv("T2LAUNCH_DISCORD_HELPER");
    assert(!start_discord_presence());
    setenv("T2LAUNCH_DISCORD_HELPER", fake_discord, 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(start_discord_presence());
    unsetenv("LD_PRELOAD");
    send_discord_presence("menu");
    send_discord_presence("menu");  /* unchanged: not sent again */
    send_discord_presence("game\tServer\tKatabatic\tCTF\t12\r\nextra");
    send_discord_presence(NULL);
    close(discord_socket);
    discord_socket = -1;
    assert(waitpid(discord_child, NULL, 0) == discord_child);
    char discord_expected[256], discord_result[256] = "";
    snprintf(discord_expected, sizeof(discord_expected),
             "4|--app-id|1563045749846056970|--pid|%d|unset\nmenu\ngame\tServer\tKatabatic\tCTF\t12\n",
             (int)getpid());
    FILE *discord_file = fopen(discord_seen, "r");
    assert(discord_file);
    size_t discord_length = fread(discord_result, 1, sizeof(discord_result) - 1, discord_file);
    discord_result[discord_length] = 0;
    fclose(discord_file);
    assert(strcmp(discord_result, discord_expected) == 0);
    send_discord_presence("menu");  /* no helper: ignored */
    unlink(fake_discord); unlink(discord_seen); rmdir(discord_dir);

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

    puts("Hook mode enumeration, WON patch, game clock, and browser bot limit: host modes preserved, replacement and permissions passed");
    return 0;
}
