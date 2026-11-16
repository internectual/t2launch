/* Sky views: spec/face/colour parsing, view and layout round trips, the face flip table, and the full-box patch
 * (applied and reverted on synthetic pages, bytes checked against the real client).
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/sky_views_test.c -o sky_views_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static unsigned char *map_site(uintptr_t address, const unsigned char *bytes, size_t length, size_t *size_out) {
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t page = address & ~((uintptr_t)size - 1);
    unsigned char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    memcpy((void *)address, bytes, length);
    *size_out = size;
    return mapping;
}

int main(void) {
    /* faces */
    assert(sky_parse_face("fr") == 0 && sky_parse_face("RT") == 1 && sky_parse_face("back") == 2);
    assert(sky_parse_face("left") == 3 && sky_parse_face("top") == 4 && sky_parse_face("down") == 5);
    assert(sky_parse_face("3") == 3 && sky_parse_face("6") == -1 && sky_parse_face("") == -1 && sky_parse_face(NULL) == -1);

    /* colours and specs */
    float color[4];
    assert(sky_parse_color("#ff8000", color) && color[0] == 1.0f && color[3] == 1.0f && color[2] == 0.0f);
    assert(sky_parse_color("#00000080", color) && color[3] > 0.49f && color[3] < 0.51f);
    assert(!sky_parse_color("ff8000", color) && !sky_parse_color("#ff80", color) && !sky_parse_color("#gg0000", color));
    assert(sky_parse_spec("builtin:plasma", color) == SKY_PLASMA);
    assert(sky_parse_spec("builtin:SOLID:#102030", color) == SKY_SOLID && color[2] > 0.18f && color[2] < 0.2f);
    assert(sky_parse_spec("builtin:nonsense", color) < 0 && sky_parse_spec("video:/x.mp4", color) < 0 && sky_parse_spec(NULL, color) < 0);

    /* views: clamping, fields, lines */
    memset(&sky, 0, sizeof sky);
    int id = sky_add_view(4, "builtin:stars", -1, 0.25f, 3, 0, 2);
    assert(id == 1);
    struct sky_view *view = sky_find_view(id);
    assert(view && view->face == 4 && view->x == 0 && view->w == 1 && view->h > 0 && view->opacity == 1);
    assert(sky_add_view(9, "builtin:stars", 0, 0, 1, 1, 1) == 0);       /* bad face */
    assert(sky_add_view(0, "builtin:zzz", 0, 0, 1, 1, 1) == 0);         /* bad spec */
    assert(sky_set_field(view, "opacity", "0.5") && view->opacity == 0.5f);
    assert(sky_set_field(view, "blend", "add") && view->blend == 1);
    assert(sky_set_field(view, "face", "lf") && view->face == 3);
    assert(sky_set_field(view, "spec", "builtin:aurora") && view->kind == SKY_AURORA);
    assert(!sky_set_field(view, "spec", "builtin:bad") && view->kind == SKY_AURORA);
    assert(!sky_set_field(view, "nonsense", "1"));
    char line[400];
    sky_view_line(view, line, sizeof line);
    assert(strstr(line, "face=lf spec=builtin:aurora") && strstr(line, "blend=add"));
    int copy = sky_view_from_line(line);
    assert(copy && copy != id);
    struct sky_view *twin = sky_find_view(copy);
    assert(twin->face == 3 && twin->kind == SKY_AURORA && twin->blend == 1 && twin->opacity == 0.5f && twin->enabled);
    assert(sky_view_count() == 2);
    for (int i = 1; i < SKY_MAX_VIEWS; i++) assert(sky_add_view(0, "builtin:solid", 0, 0, 1, 1, 1) != 0 || sky_view_count() == SKY_MAX_VIEWS);
    assert(sky_add_view(0, "builtin:solid", 0, 0, 1, 1, 1) == 0);       /* the table is full */

    /* layout names stay inside the folder */
    assert(sky_layout_name_ok("matrix") && sky_layout_name_ok("a-b_1"));
    assert(!sky_layout_name_ok("../x") && !sky_layout_name_ok("a b") && !sky_layout_name_ok("") && !sky_layout_name_ok("a/b"));

    /* the texture mapping flips: walls and the top mirrored in x, the bottom in y */
    for (int i = 0; i < 5; i++) assert(sky_face_flip_x[i] == 1 && sky_face_flip_y[i] == 0);
    assert(sky_face_flip_x[5] == 0 && sky_face_flip_y[5] == 1);

    /* the full-box patch: both stores change together and revert; wrong bytes are refused */
    size_t size;
    /* both stores are on the same page (0x08221000) */
    unsigned char *page = map_site(SKY_FULL_BOX_ADDRESS_A, sky_full_box_a_original, sizeof sky_full_box_a_original, &size);
    memcpy((void *)SKY_FULL_BOX_ADDRESS_B, sky_full_box_b_original, sizeof sky_full_box_b_original);
    unsigned char *site_a = (unsigned char *)SKY_FULL_BOX_ADDRESS_A, *site_b = (unsigned char *)SKY_FULL_BOX_ADDRESS_B;
    static const struct game_target client_target = {.executable = "tribes2.dynamic", .dedicated = 0};
    target = &client_target;
    site_b[0] = 0x00;                                                   /* an unexpected byte at the second site */
    assert(mprotect(page, size, PROT_READ | PROT_EXEC) == 0);
    assert(!sky_set_full_box(1));
    assert(memcmp(site_a, sky_full_box_a_original, sizeof sky_full_box_a_original) == 0);   /* nothing written */
    assert(mprotect(page, size, PROT_READ | PROT_WRITE) == 0);
    memcpy(site_b, sky_full_box_b_original, sizeof sky_full_box_b_original);
    assert(mprotect(page, size, PROT_READ | PROT_EXEC) == 0);
    assert(sky_set_full_box(1));
    assert(memcmp(site_a, sky_full_box_a_patched, sizeof sky_full_box_a_patched) == 0);
    assert(memcmp(site_b, sky_full_box_b_patched, sizeof sky_full_box_b_patched) == 0);
    assert(sky_set_full_box(1));                                        /* already on: nothing to do */
    assert(sky_set_full_box(0));
    assert(memcmp(site_a, sky_full_box_a_original, sizeof sky_full_box_a_original) == 0);
    assert(memcmp(site_b, sky_full_box_b_original, sizeof sky_full_box_b_original) == 0);

    /* the same bytes in the real client, when it is installed */
    int checked = 0;
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    FILE *client = fopen(path, "rb");
    if (client) {
        unsigned char found[16];
        assert(fseek(client, (long)(SKY_FULL_BOX_ADDRESS_A - 0x08048000), SEEK_SET) == 0 && fread(found, 1, 10, client) == 10);
        assert(memcmp(found, sky_full_box_a_original, 10) == 0);
        assert(fseek(client, (long)(SKY_FULL_BOX_ADDRESS_B - 0x08048000), SEEK_SET) == 0 && fread(found, 1, 12, client) == 12);
        assert(memcmp(found, sky_full_box_b_original, 12) == 0);
        fclose(client);
        checked = 1;
    }
    printf("sky views: specs, views, layouts, face flips, full-box patch and %d real-client check passed\n", checked);
    return 0;
}
