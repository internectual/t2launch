/* Synthetic engine registries and a mock native renderer; no game is run.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/autobeacon_test.c
 *     -o /tmp/opencode/autobeacon_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

struct fake_object { unsigned char bytes[0x88c]; };
static uint32_t dictionary[4096];
static unsigned char connection[0x8500], beacon_set[0x3c];
static struct fake_object self, enemy, friend, behind, deployable, ignored, beacon[3], waypoint;
static unsigned char player_info[0x1c], friend_info[0x1c], object_info[0x1c], data[0x48];
static uint32_t beacon_array[3];
static unsigned char class_player[0x1c], class_shape[0x1c], class_beacon[0x1c], class_waypoint[0x1c];
static void *vt_player[3], *vt_shape[3], *vt_beacon[3], *vt_waypoint[3];
static void *get_player(void *unused) { (void)unused; return class_player; }
static void *get_shape(void *unused) { (void)unused; return class_shape; }
static void *get_beacon(void *unused) { (void)unused; return class_beacon; }
static void *get_waypoint(void *unused) { (void)unused; return class_waypoint; }

static void put_word(void *object, size_t offset, uint32_t value) {
    memcpy((unsigned char *)object + offset, &value, 4);
}
static void put_pointer(void *object, size_t offset, const void *pointer) {
    put_word(object, offset, (uint32_t)(uintptr_t)pointer);
}
static void put_float(void *object, size_t offset, float value) {
    memcpy((unsigned char *)object + offset, &value, 4);
}
static void init_object(struct fake_object *object, void *vtable, uint32_t id,
                         float x, float y, float z, void *info) {
    memset(object, 0, sizeof *object);
    put_pointer(object, 0, vtable);
    put_word(object, 0x20, id);
    put_word(object, 0x40, 2);
    put_float(object, 0xa8, x); put_float(object, 0xb8, y); put_float(object, 0xc8, z);
    put_pointer(object, 0x26c, info);
    put_word(object, 0x10, dictionary[id & 0xfff]);
    dictionary[id & 0xfff] = (uint32_t)(uintptr_t)object;
}

static int calls, expected_count;
static float expected_xyz[3][3], copied_xyz[3][3];
static uint32_t expected_types[3];
static void fake_render(void *hud) {
    assert(hud == beacon_set);
    calls++;
    for (int i = 0; i < expected_count; i++) {
        for (int axis = 0; axis < 3; axis++) {
            float actual = rv_float(&beacon[i], 0xa8 + 16 * axis);
            assert(actual == expected_xyz[i][axis]);
            copied_xyz[i][axis] = actual;  /* the second HUD pass consumes copies */
        }
        assert(rv_word(&beacon[i], 0x888) == expected_types[i]);
    }
}
static void expect_position(int slot, const struct fake_object *object, int waypoint_position, int type) {
    for (int axis = 0; axis < 3; axis++)
        expected_xyz[slot][axis] = rv_float(object, waypoint_position ? 0x38 + 4 * axis : 0xa8 + 16 * axis);
    expected_types[slot] = type;
}
static void render_and_check_restore(void) {
    struct fake_object before[3];
    memcpy(before, beacon, sizeof before);
    int previous = calls;
    review_beacon_render(beacon_set);
    assert(calls == previous + 1);
    assert(memcmp(before, beacon, sizeof before) == 0);
}

int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t addresses[] = {RV_DEMO_CALLBACK_ADDR, RV_CONNECTION, RV_ID_DICTIONARY};
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; i++) {
        uintptr_t base = addresses[i] & ~(page - 1);
        assert(mmap((void *)base, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)base);
    }
    /* ID dictionary and client target-set globals share a page. */
    assert((RV_ID_DICTIONARY & ~(page - 1)) == (RV_CLIENT_TARGET_SET & ~(page - 1)));
    memcpy((void *)RV_DEMO_CALLBACK_ADDR, rv_demo_callback_orig, sizeof rv_demo_callback_orig);
    assert(mprotect((void *)(RV_DEMO_CALLBACK_ADDR & ~(page - 1)), page, PROT_READ | PROT_EXEC) == 0);
    put_pointer((void *)RV_CONNECTION, 0, connection);
    put_pointer((void *)RV_ID_DICTIONARY, 0, dictionary);
    put_pointer((void *)RV_CLIENT_TARGET_SET, 0, beacon_set);
    put_pointer(class_player, 0x18, "Player"); vt_player[2] = get_player;
    put_pointer(class_shape, 0x18, "StaticShape"); vt_shape[2] = get_shape;
    put_pointer(class_beacon, 0x18, "BeaconObject"); vt_beacon[2] = get_beacon;
    put_pointer(class_waypoint, 0x18, "ClientTarget"); vt_waypoint[2] = get_waypoint;
    put_word(player_info, 0x18, 2); put_word(friend_info, 0x18, 1); put_word(object_info, 0x18, 2);
    put_pointer(data, 0x44, "shapes/deploy_inventory.dts");
    init_object(&self, vt_player, 100, 0, 0, 0, friend_info);
    put_float(&self, 0x9c, 1);  /* hm2 view vector is (+0xa0, +0x9c) = +Y */
    init_object(&enemy, vt_player, 101, 0, 100, 10, player_info);
    init_object(&friend, vt_player, 102, 0, 20, 0, friend_info);
    init_object(&behind, vt_player, 103, 0, -50, 0, player_info);
    init_object(&deployable, vt_shape, 104, 50, 50, 0, object_info);
    put_pointer(&deployable, 0x2d0, data);
    init_object(&ignored, vt_shape, 105, 0, 10, 0, object_info);
    /* Missing shape filename: never treat an arbitrary StaticShape as a target. */
    for (int i = 0; i < 3; i++) {
        init_object(&beacon[i], vt_beacon, 200 + i, 1000 + i, 2000 + i, 3000 + i, player_info);
        put_word(&beacon[i], 0x888, i);
        beacon_array[2-i] = (uint32_t)(uintptr_t)&beacon[i];  /* set order differs from ID order */
    }
    put_word(beacon_set, 0x30, 3); put_pointer(beacon_set, 0x38, beacon_array);
    put_pointer(connection, 0x825c, &self); put_word(connection, 0x8434, 1);
    put_word(connection, 0x8220, 1);
    store_hook_option("review.cheat_view", "1"); store_hook_option("review.autobeacon", "1");
    review_patches_applied = 1;
    rv_native_beacon_render = fake_render;

    expected_count = 3;
    expect_position(0, &enemy, 0, 0); expect_position(1, &deployable, 0, 2); expect_position(2, &behind, 0, 0);
    render_and_check_restore();
    assert(rv_tracking.count == 3);  /* self/friend/beacon/unlisted shape excluded */
    assert(copied_xyz[0][1] == 100 && rv_float(&beacon[0], 0xb8) == 2000);

    /* Cached targets follow current positions on intervening frames. */
    put_float(&enemy, 0xb8, 150);
    expect_position(0, &enemy, 0, 0);
    render_and_check_restore();
    assert(rv_tracking.frame == 2);

    /* Removing a cached target, even while it is still readable, must not
       consume a beacon or duplicate the next target (hm2's i/j bug). */
    dictionary[101] = 0;
    expect_position(0, &deployable, 0, 2); expect_position(1, &behind, 0, 0); expect_position(2, &beacon[2], 0, 1);
    render_and_check_restore();

    /* A new waypoint is discovered on the next 20-frame ranking boundary. */
    init_object(&waypoint, vt_waypoint, 106, 0, 0, 0, NULL);
    put_word(&waypoint, 0x30, 2);
    put_float(&waypoint, 0x38, 0); put_float(&waypoint, 0x3c, 10); put_float(&waypoint, 0x40, 5);
    render_and_check_restore();  /* newly spawned waypoint is not cached yet */
    rv_tracking.frame = RV_REFRESH_FRAMES;
    expect_position(0, &waypoint, 1, 0); expect_position(1, &deployable, 0, 2); expect_position(2, &behind, 0, 0);
    render_and_check_restore();

    /* Truly unmapped cached memory must never be dereferenced after deletion.
       The transient player's ID also exercises a nontrivial hash bucket. */
    struct fake_object *transient = mmap(NULL, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(transient != MAP_FAILED && sizeof *transient <= page);
    init_object(transient, vt_player, 4097, 0, 5, 0, player_info);
    rv_tracking.frame = RV_REFRESH_FRAMES;
    expect_position(0, &waypoint, 1, 0); expect_position(1, transient, 0, 0); expect_position(2, &deployable, 0, 2);
    render_and_check_restore();
    dictionary[1] = 0;
    assert(munmap(transient, page) == 0);
    expect_position(0, &waypoint, 1, 0); expect_position(1, &deployable, 0, 2); expect_position(2, &behind, 0, 0);
    render_and_check_restore();

    /* Changed team / invalid positions are rechecked without reranking. */
    put_word(object_info, 0x18, 1);
    expect_position(1, &behind, 0, 0); expect_position(2, &beacon[2], 0, 1);
    render_and_check_restore();
    put_word(&behind, 0xa8, 0x7fc00000);  /* NaN: never propagate to a beacon */
    expect_position(1, &beacon[1], 0, 1);
    render_and_check_restore();

    /* Disabling autobeacon, live state before swap-time reversion, observer
       group, and no control object all preserve native placement and types. */
    for (int i = 0; i < 3; i++) expect_position(i, &beacon[i], 0, i);
    store_hook_option("review.autobeacon", "0"); render_and_check_restore();
    store_hook_option("review.autobeacon", "1"); put_word(connection, 0x8220, 0); render_and_check_restore();
    put_word(connection, 0x8220, 1); put_word(connection, 0x8434, 0); render_and_check_restore();
    put_word(connection, 0x8434, 1); put_pointer(connection, 0x825c, NULL); render_and_check_restore();

    /* A demo without recorded beacons never synthesizes any. */
    put_pointer(connection, 0x825c, &self); put_word(beacon_set, 0x30, 0);
    render_and_check_restore();
    assert(rv_tracking.last_beacons == 0 && rv_tracking.last_targets == 0);

    /* The two pool limits and yaw ordering stay bounded and deterministic. */
    memset(&rv_tracking, 0, sizeof rv_tracking);
    for (uint32_t i = 200; i > 0; i--) {
        struct rv_target t = {NULL, i, RV_PLAYER, 0, 0}; rv_add_target(&t);
        t.kind = RV_SHAPE; t.id += 1000; rv_add_target(&t);
    }
    assert(rv_tracking.count == 128);
    qsort(rv_tracking.targets, rv_tracking.count, sizeof rv_tracking.targets[0], rv_compare_targets);
    assert(rv_tracking.targets[0].id == 1 && rv_tracking.targets[63].id == 64);
    assert(rv_tracking.targets[64].id == 1001 && rv_tracking.targets[127].id == 1064);
    float zero[3] = {0}, forward[3] = {0, 1, 0}, side[3] = {1, 0, 0}, back[3] = {0, -1, 0};
    assert(rv_yaw_rank(forward, zero, 0, 1) < rv_yaw_rank(side, zero, 0, 1));
    assert(rv_yaw_rank(side, zero, 0, 1) < rv_yaw_rank(back, zero, 0, 1));

    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; i++)
        assert(munmap((void *)(addresses[i] & ~(page - 1)), page) == 0);
    puts("Autobeacon: selection, tracking, lifetime checks, pool limits, draw-time restoration and gates passed");
    return 0;
}
