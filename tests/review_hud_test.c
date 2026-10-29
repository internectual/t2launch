/* Reference beacon presentation before native wake; no game or GL is run. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <sys/stat.h>

static unsigned char hud[0x300], original[sizeof hud];
static int expect_reference, parent_calls;
static unsigned char parent_result = 1;

static void initialize_hud(void) {
    memset(hud, 0xa5, sizeof hud);
    uint32_t base = (uint32_t)(uintptr_t)"gui/beacon_base";
    uint32_t target_name = (uint32_t)(uintptr_t)"gui/crosshairs";
    memcpy(hud + 0x1fc, &base, 4); memcpy(hud + 0x200, &target_name, 4);
    memcpy(original, hud, sizeof hud);
}
static void check_reference(void) {
    assert(strcmp(rv_pointer(hud, 0x1fc), "gui/happy_beaconbase") == 0);
    assert(strcmp(rv_pointer(hud, 0x200), "gui/happy_beacontarget") == 0);
    assert(rv_word(hud, 0x204) == 4000 && hud[0x208] == 1);
    const float expected[3][4] = {{0, 1, 1, .8f}, {1, 1, 0, .8f}, {1, 1, 1, 1}};
    for (unsigned int color = 0; color < 6; color++)
        for (unsigned int channel = 0; channel < 4; channel++)
            assert(rv_float(hud, 0x20c + color * 16 + channel * 4) == expected[color / 2][channel]);
    assert(rv_float(hud, 0x26c) == 1 && rv_word(hud, 0x270) == 14000 && hud[0x274] == 0);
    for (size_t i = 0; i < sizeof hud; i++)
        if (!(i >= 0x1fc && i < 0x209) && !(i >= 0x20c && i < 0x275)) assert(hud[i] == original[i]);
}
static unsigned char fake_parent_wake(void *object) {
    assert(object == hud);
    parent_calls++;
    if (expect_reference) check_reference();
    else assert(memcmp(hud, original, sizeof hud) == 0);
    return parent_result;
}

int main(void) {
    rv_native_hud_wake = fake_parent_wake;
    for (unsigned int mask = 0; mask < 4; mask++) {
        store_hook_option("review.cheat_view", mask & 1 ? "1" : "0");
        store_hook_option("review.replace_models", mask & 2 ? "1" : "0");
        initialize_hud();
        assert(rv_apply_review_hud(hud, 1) == (mask == 3));
        if (mask == 3) check_reference();
        else assert(memcmp(hud, original, sizeof hud) == 0);
    }
    initialize_hud();
    assert(!rv_apply_review_hud(hud, 0) && memcmp(hud, original, sizeof hud) == 0);
    assert(!rv_apply_review_hud(NULL, 1));
    target = &game_targets[1];
    assert(!rv_apply_review_hud(hud, 1) && memcmp(hud, original, sizeof hud) == 0);
    target = &game_targets[0];

    /* Availability checks use a disposable directory, never an installed game.
       Empty files are enough here: the fixture tests gating, not PNG decoding. */
    char root[4096], path[4096];
    snprintf(root, sizeof root, "%s/t2-review-hud-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(root));
    assert(setenv("T2LAUNCH_GAME_DIR", root, 1) == 0);
    expect_reference = 0;
    assert(review_hud_wake(hud) == 1 && parent_calls == 1);
    const char *directories[] = {"base", "base/textures", "base/textures/gui"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", root, directories[i]);
        assert(mkdir(path, 0700) == 0);
    }
    const char *images[] = {"happy_beaconbase.png", "happy_beacontarget.png"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/base/textures/gui/%s", root, images[i]);
        FILE *file = fopen(path, "wb"); assert(file && fclose(file) == 0);
        if (i == 0) assert(review_hud_wake(hud) == 1 && parent_calls == 2);
    }
    expect_reference = 1;
    assert(review_hud_wake(hud) == 1 && parent_calls == 3);
    initialize_hud(); parent_result = 0;
    assert(review_hud_wake(hud) == 0 && parent_calls == 4); /* native failure propagates */
    initialize_hud(); expect_reference = 0; parent_result = 1;
    store_hook_option("review.replace_models", "0");
    assert(review_hud_wake(hud) == 1 && parent_calls == 5);
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/base/textures/gui/%s", root, images[i]);
        assert(unlink(path) == 0);
    }
    for (int i = 2; i >= 0; i--) {
        snprintf(path, sizeof path, "%s/%s", root, directories[i]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    puts("review HUD: reference fields precede native wake and fallback gates passed");
    return 0;
}
