/* Synthetic i386 mappings only: exercises the hook without launching a game.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/review_patch_test.c
 *     -o /tmp/opencode/review_patch_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static uintptr_t pages[16];
static size_t page_count;
static size_t page_size;
static int shortcut_definitions, shortcut_enables, shortcut_disables;
static const char *fake_evaluate(const char *code, unsigned char echo,
                                  const char *filename, unsigned char no_calls) {
    assert(!echo && !no_calls);
    if (code == review_shortcuts_script) {
        assert(filename && strcmp(filename, "t2launch/review-keys.cs") == 0);
        shortcut_definitions++;
    } else if (strcmp(code, "t2launch_reviewKeysSet(1);") == 0) {
        shortcut_enables++;
        return "range=bound global_beacons=bound ffi=bound no_cloak=bound autobeacon=bound";
    } else if (strcmp(code, "t2launch_reviewKeysSet(0);") == 0) {
        shortcut_disables++;
        return "range=released global_beacons=released ffi=released no_cloak=released autobeacon=released";
    } else assert(!"unexpected script evaluation in the review fixture");
    return "";
}

static void map_page(uintptr_t address) {
    uintptr_t page = address & ~(page_size - 1);
    for (size_t i = 0; i < page_count; i++)
        if (pages[i] == page) return;
    assert(page_count < sizeof pages / sizeof pages[0]);
    assert(mmap((void *)page, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)page);
    pages[page_count++] = page;
}

static void assert_originals(void) {
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++) {
        const struct code_patch *p = &review_patches_on[i];
        assert(memcmp((void *)p->address, p->expected, p->expected_length) == 0);
    }
}

static uint32_t loaded_float_bits(uintptr_t address) {
    float value = ((float (*)(void))address)();
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* The actual client classifier at 0x0819101d..0x0819105b. Keep its relative
   branches at their engine addresses so the two real patch sites are exercised.
   At 0x0819105c the fixture returns the computed local flag instead of running
   the rest of the game's HUD collection. No installed executable is run. */
#define FFI_CLASSIFIER_ADDRESS 0x0819101D
#define FFI_CLASSIFIER_RETURN 0x0819105C
static const unsigned char ffi_classifier[] = {
    0x8b, 0x48, 0x1c, 0x8a, 0x40, 0x3c, 0x85, 0xc9, 0x74, 0x35, 0xf6, 0xc5, 0x01,
    0x75, 0x2c, 0x31, 0xd2, 0xf6, 0xc1, 0x01, 0x74, 0x0d, 0x8b, 0x5d, 0x84, 0x80,
    0xbb, 0x04, 0x02, 0x00, 0x00, 0x00, 0x75, 0x19, 0x90, 0x42, 0x83, 0xfa, 0x07,
    0x77, 0x16, 0x0f, 0xa3, 0xd1, 0x73, 0xf5, 0x8b, 0x5d, 0x84, 0x80, 0xbc, 0x1a,
    0x04, 0x02, 0x00, 0x00, 0x00, 0x74, 0xe8, 0xc6, 0x45, 0x83, 0x01
};
static const unsigned char ffi_return_flag[] = {0x0f, 0xb6, 0x45, 0x83, 0xc3};

/* The fragment expects EAX=target-info and EBP=a caller frame containing
   object-info at -0x7c and a zero-initialized friendly flag at -0x7d. */
__attribute__((naked, noinline))
static int run_ffi_classifier(void *info, void *frame, uintptr_t address) {
    __asm__ volatile(
        "push %ebp\n\t"
        "push %ebx\n\t"
        "mov 12(%esp), %eax\n\t"
        "mov 16(%esp), %ebp\n\t"
        "call *20(%esp)\n\t"
        "pop %ebx\n\t"
        "pop %ebp\n\t"
        "ret\n\t");
}

static void check_ffi_classifier(int patched) {
    unsigned char info[0x40] = {0}, object_info[0x20c] = {0}, frame[0x84] = {0};
    void *object_pointer = object_info;
    memcpy(frame + 4, &object_pointer, sizeof object_pointer);
    /* Bits 0..7 choose visibility bytes, bit 8 is always-friendly, and bit 9
       must remain irrelevant. Cover every visibility-byte and mask pattern. */
    for (uint32_t visible = 0; visible < 256; visible++) {
        for (int bit = 0; bit < 8; bit++) object_info[0x204 + bit] = (visible >> bit) & 1;
        for (uint32_t mask = 0; mask < 1024; mask++) {
            memcpy(info + 0x1c, &mask, sizeof mask);
            frame[3] = 0;
            int expected = (mask & 0x100) != 0 || (mask & (patched ? 0xff : visible)) != 0;
            assert(run_ffi_classifier(info, frame + 0x80, FFI_CLASSIFIER_ADDRESS) == expected);
        }
    }
}

/* Execute either the patched absolute load or the stock [eax+0xe2c] load. */
__attribute__((naked, noinline))
static float run_far_load(void *scene, uintptr_t address) {
    __asm__ volatile("mov 4(%esp), %eax\n\tcall *8(%esp)\n\tret\n\t");
}
static uint32_t far_load_bits(void *scene, uintptr_t address) {
    float value = run_far_load(scene, address);
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
static void check_ffi_boundaries(int patched) {
    unsigned char info[0x40] = {0}, object_info[0x20c] = {0}, frame[0x84] = {0};
    void *object_pointer = object_info;
    memcpy(frame + 4, &object_pointer, sizeof object_pointer);
    const uint32_t masks[] = {0, 1, 0x20, 0x100, 0x200};
    for (size_t i = 0; i < sizeof masks / sizeof masks[0]; i++) {
        memcpy(info + 0x1c, &masks[i], 4);
        frame[3] = 0;
        int expected = (masks[i] & 0x100) != 0 || (patched && (masks[i] & 0xff) != 0);
        assert(run_ffi_classifier(info, frame + 0x80, FFI_CLASSIFIER_ADDRESS) == expected);
    }
}
static void check_feature_combination(int range, int global_beacons, void *stock_scene, uint32_t stock_bits) {
    assert(review_patches_applied);
    int ffi = hook_option_bool("review.ffi"), no_cloak = hook_option_bool("review.no_cloak");
    uint32_t expected = review_group_mask(REVIEW_CORE);
    if (range) expected |= review_group_mask(REVIEW_RANGE);
    if (global_beacons) expected |= review_group_mask(REVIEW_GLOBAL_BEACONS);
    if (ffi) expected |= review_group_mask(REVIEW_FFI);
    if (no_cloak) expected |= review_group_mask(REVIEW_NO_CLOAK);
    assert(review_applied_mask == expected);
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++) {
        const struct code_patch *p = &review_patches_on[i];
        const void *bytes = expected & (1u << i) ? p->replacement : p->expected;
        assert(memcmp((void *)p->address, bytes, p->expected_length) == 0);
    }
    assert(loaded_float_bits(RV_ALPHA_ADDR) == (no_cloak ? 0x3f666666 : 0x3e000000));
    check_ffi_boundaries(ffi);
    assert(far_load_bits(stock_scene, RV_CAMERA_FAR_ADDR) == (range ? 0x469c4000 : stock_bits));
    assert(far_load_bits(stock_scene, RV_HUD_FAR_ADDR) == (range ? 0x469c4000 : stock_bits));
    /* Native global-beacon compare/skip: equal and differing sensor groups.
       The same ABI shim supplies EAX=group and EBP-0x68=local group. */
    unsigned char frame[0x84] = {0};
    uint32_t local_group = 1;
    memcpy(frame + 0x18, &local_group, 4);
    assert(run_ffi_classifier((void *)1, frame + 0x80, RV_GBEACONS_ADDR - 3) == 1);
    assert(run_ffi_classifier((void *)2, frame + 0x80, RV_GBEACONS_ADDR - 3) == global_beacons);
}

int main(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++) {
        const struct code_patch *p = &review_patches_on[i];
        map_page(p->address);
        memcpy((void *)p->address, p->expected, p->expected_length);
        /* Allow patched fld instructions to be called as float-return stubs. */
        *(unsigned char *)(p->address + p->expected_length) = 0xc3;
    }
    memcpy((void *)FFI_CLASSIFIER_ADDRESS, ffi_classifier, sizeof ffi_classifier);
    memcpy((void *)FFI_CLASSIFIER_RETURN, ffi_return_flag, sizeof ffi_return_flag);
    static const unsigned char group_compare[] = {0x3b, 0x45, 0x98};
    static const unsigned char return_yes[] = {0xb8, 1, 0, 0, 0, 0xc3};
    static const unsigned char return_no[] = {0x31, 0xc0, 0xc3};
    memcpy((void *)(RV_GBEACONS_ADDR - 3), group_compare, sizeof group_compare);
    memcpy((void *)(RV_GBEACONS_ADDR + 2), return_yes, sizeof return_yes);
    memcpy((void *)(RV_GBEACONS_ADDR + 2 + 0x43), return_no, sizeof return_no);
    map_page(RV_DEMO_CALLBACK_ADDR);
    memcpy((void *)RV_DEMO_CALLBACK_ADDR, rv_demo_callback_orig, sizeof rv_demo_callback_orig);
    map_page(RV_BEACON_RENDER_ADDR);
    memcpy((void *)RV_BEACON_RENDER_ADDR, rv_beacon_render_orig, sizeof rv_beacon_render_orig);
    map_page(0x08495cdc);
    map_page(0x086bf894);
    *(float *)0x086bf894 = 0.125f;  /* native shape alpha, distinct from forced 0.9 */
    for (size_t i = 0; i < page_count; i++) {
        int protection = pages[i] == (0x08495cdc & ~(page_size - 1)) ||
                         pages[i] == (0x086bf894 & ~(page_size - 1))
            ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC;
        assert(mprotect((void *)pages[i], page_size, protection) == 0);
    }

    /* Neither an absent connection nor a live (non-demo) connection activates
       patches, even with the review option enabled. No script result is used. */
    store_hook_option("review.cheat_view", "1");
    Con__evaluate = fake_evaluate;
    registered = 1;
    game_main_thread = pthread_self(); game_main_thread_known = 1;
    unsigned char *connection = calloc(1, 0x8224);
    assert(connection);
    update_review_patches();
    assert(!review_patches_applied);
    *(void **)0x08495cdc = connection;
    update_review_patches();
    assert(!review_patches_applied);
    assert_originals();
    assert(!shortcut_definitions && !shortcut_enables && !shortcut_disables);
    check_ffi_classifier(0);

    *(uint32_t *)(connection + 0x8220) = 1;
    update_review_patches();
    assert(review_patches_applied);
    assert(shortcut_definitions == 1 && shortcut_enables == 1 && review_shortcuts_active);
    int32_t call_offset;
    memcpy(&call_offset, (void *)(RV_BEACON_CALL_ADDR + 1), sizeof call_offset);
    assert((uintptr_t)(RV_BEACON_CALL_ADDR + 5 + call_offset) == (uintptr_t)review_beacon_render);
    check_ffi_classifier(1);

    /* Execute the replacement loads repeatedly: each must return exactly one
       x87 value, with no accumulated FPU stack overflow or changed constant. */
    for (int i = 0; i < 32; i++) {
        assert(loaded_float_bits(RV_ALPHA_ADDR) == 0x3f666666);
        assert(loaded_float_bits(RV_CAMERA_FAR_ADDR) == 0x469c4000);
        assert(loaded_float_bits(RV_HUD_FAR_ADDR) == 0x469c4000);
    }
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++) {
        int protection;
        assert(readable_range(review_patches_on[i].address, 1, &protection));
        assert(protection == (PROT_READ | PROT_EXEC));
    }

    /* Both optional groups can be A/B tested without undoing FFI, no-cloak or
       the autobeacon CALL. Stock instructions really load a different range. */
    unsigned char *stock_scene = calloc(1, 0xe30), *graph = calloc(1, 0xe8);
    assert(stock_scene && graph);
    float stock_far = 750.0f;
    uint32_t stock_bits;
    memcpy(&stock_bits, &stock_far, 4);
    memcpy(stock_scene + 0xe2c, &stock_far, 4);
    memcpy(graph + 0xe4, &stock_scene, 4);
    *(void **)0x08495184 = graph;
    game_main_thread = pthread_self(); game_main_thread_known = 1;
    const char *range_off[] = {"hook_review_range", "0"}, *range_on[] = {"hook_review_range", "1"};
    const char *global_off[] = {"hook_review_global_beacons", "0"};
    const char *global_on[] = {"hook_review_global_beacons", "1"};
    const char *query[] = {"hook_review_range"}, *invalid[] = {"hook_review_range", "yes"};
    const char *ffi_off[] = {"hook_review_ffi", "0"}, *ffi_on[] = {"hook_review_ffi", "1"};
    const char *cloak_off[] = {"hook_review_no_cloak", "0"}, *cloak_on[] = {"hook_review_no_cloak", "1"};
    const char *auto_off[] = {"hook_review_autobeacon", "0"}, *auto_on[] = {"hook_review_autobeacon", "1"};
    const char *auto_query[] = {"hook_review_autobeacon"};
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(shortcut_definitions == 1 && shortcut_enables == 1 && !shortcut_disables);
    assert(strstr(hook_review_range_cb(NULL, 2, range_off), "configured=0 applied=0 stock_far=750.0") != NULL);
    check_feature_combination(0, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_global_beacons_cb(NULL, 2, global_off), "configured=0 applied=0") != NULL);
    check_feature_combination(0, 0, stock_scene, stock_bits);
    assert(strstr(hook_review_range_cb(NULL, 2, range_on), "configured=1 applied=1") != NULL);
    check_feature_combination(1, 0, stock_scene, stock_bits);
    assert(strstr(hook_review_global_beacons_cb(NULL, 2, global_on), "configured=1 applied=1") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_range_cb(NULL, 2, invalid), "expected 0 or 1") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_ffi_cb(NULL, 2, ffi_off), "configured=0 applied=0") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_no_cloak_cb(NULL, 2, cloak_off), "configured=0 applied=0") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_ffi_cb(NULL, 2, ffi_on), "configured=1 applied=1") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    assert(strstr(hook_review_no_cloak_cb(NULL, 2, cloak_on), "configured=1 applied=1") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    uint32_t dispatcher_mask = review_applied_mask;
    assert(strstr(hook_review_autobeacon_cb(NULL, 2, auto_off), "configured=0 applied=0") != NULL);
    assert(review_applied_mask == dispatcher_mask);
    assert(strstr(hook_review_autobeacon_cb(NULL, 2, auto_on), "configured=1 applied=1") != NULL);
    assert(review_applied_mask == dispatcher_mask);

    /* Exercise every combination, including only the dispatcher CALL active:
       shortcut access remains available with all visual groups turned off. */
    for (unsigned int combination = 0; combination < 32; combination++) {
        store_hook_option("review.extended_range", combination & 1 ? "1" : "0");
        store_hook_option("review.global_beacons", combination & 2 ? "1" : "0");
        store_hook_option("review.ffi", combination & 4 ? "1" : "0");
        store_hook_option("review.no_cloak", combination & 8 ? "1" : "0");
        store_hook_option("review.autobeacon", combination & 16 ? "1" : "0");
        update_review_patches();
        check_feature_combination((combination & 1) != 0, (combination & 2) != 0, stock_scene, stock_bits);
        assert(review_shortcuts_active && shortcut_definitions == 1 && shortcut_enables == 1);
        assert(strstr(hook_review_autobeacon_cb(NULL, 1, auto_query), combination & 16
                      ? "configured=1 applied=1" : "configured=0 applied=0") != NULL);
    }
    /* An off-thread request updates the request only, not executable pages. */
    game_main_thread_known = 0;
    assert(strstr(hook_review_range_cb(NULL, 2, range_off), "configured=0 applied=1") != NULL);
    check_feature_combination(1, 1, stock_scene, stock_bits);
    game_main_thread_known = 1;
    assert(strstr(hook_review_range_cb(NULL, 1, query), "configured=0 applied=0") != NULL);
    check_feature_combination(0, 1, stock_scene, stock_bits);

    /* End playback with IFF/alpha already restored; don't try to restore them
       twice. New setters also remain inert outside playback. */
    hook_review_ffi_cb(NULL, 2, ffi_off); hook_review_no_cloak_cb(NULL, 2, cloak_off);
    check_feature_combination(0, 1, stock_scene, stock_bits);
    *(uint32_t *)(connection + 0x8220) = 0;
    update_review_patches();
    assert(!review_patches_applied);
    assert_originals();
    check_ffi_classifier(0);
    assert(!review_shortcuts_active && shortcut_disables == 1);
    assert(strstr(hook_review_range_cb(NULL, 2, range_on), "configured=1 applied=0") != NULL);
    assert(strstr(hook_review_global_beacons_cb(NULL, 2, global_on), "configured=1 applied=0") != NULL);
    assert(strstr(hook_review_ffi_cb(NULL, 2, ffi_on), "configured=1 applied=0") != NULL);
    assert(strstr(hook_review_no_cloak_cb(NULL, 2, cloak_on), "configured=1 applied=0") != NULL);
    assert(strstr(hook_review_autobeacon_cb(NULL, 2, auto_on), "configured=1 applied=0") != NULL);
    assert_originals();
    *(uint32_t *)(connection + 0x8220) = 1;
    update_review_patches();
    assert(review_patches_applied);
    assert(review_shortcuts_active && shortcut_definitions == 1 && shortcut_enables == 2);
    /* Shortcut preference/command support can release the keys without
       changing any rendering patches, and re-enable without redefining code. */
    uint32_t before_shortcuts = review_applied_mask;
    store_hook_option("review.shortcuts", "0"); update_review_patches();
    assert(!review_shortcuts_active && shortcut_disables == 2 && review_applied_mask == before_shortcuts);
    store_hook_option("review.shortcuts", "1"); update_review_patches();
    assert(review_shortcuts_active && shortcut_enables == 3 && shortcut_definitions == 1);
    store_hook_option("hook.console_commands", "0"); update_review_patches();
    assert(!review_shortcuts_active && shortcut_disables == 3 && review_applied_mask == before_shortcuts);
    store_hook_option("hook.console_commands", "1"); update_review_patches();
    assert(review_shortcuts_active && shortcut_enables == 4 && shortcut_definitions == 1);
    store_hook_option("review.cheat_view", "0");
    update_review_patches();
    assert(!review_patches_applied);
    assert_originals();
    assert(!review_shortcuts_active && shortcut_disables == 4);
    assert(strstr(hook_review_range_cb(NULL, 2, range_on), "configured=1 applied=0") != NULL);
    assert_originals();

    store_hook_option("review.cheat_view", "1");
    target = &game_targets[1];
    assert(strcmp(hook_review_range_cb(NULL, 2, range_off), "client only") == 0);
    assert(strcmp(hook_review_autobeacon_cb(NULL, 2, auto_off), "client only") == 0);
    assert(hook_option_bool("review.autobeacon"));
    assert(hook_option_bool("review.extended_range"));
    update_review_patches();
    assert(!review_patches_applied);
    assert_originals();
    target = &game_targets[0];

    /* A mismatch in the LAST site must prevent writes to every earlier site. */
    uintptr_t last = review_patches_on[REVIEW_PATCH_COUNT - 1].address;
    uintptr_t page = last & ~(page_size - 1);
    assert(mprotect((void *)page, page_size, PROT_READ | PROT_WRITE) == 0);
    *(unsigned char *)last ^= 1;
    assert(mprotect((void *)page, page_size, PROT_READ | PROT_EXEC) == 0);
    assert(!apply_code_patches("Review mismatch fixture", review_patches_on, REVIEW_PATCH_COUNT));
    for (size_t i = 0; i + 1 < REVIEW_PATCH_COUNT; i++) {
        const struct code_patch *p = &review_patches_on[i];
        assert(memcmp((void *)p->address, p->expected, p->expected_length) == 0);
    }

    for (size_t i = 0; i < page_count; i++)
        assert(munmap((void *)pages[i], page_size) == 0);
    free(connection);
    free(graph); free(stock_scene);
    puts("Review patches: exhaustive FFI, 32 independent feature combinations, live gates, stock loads and restoration passed");
    return 0;
}
