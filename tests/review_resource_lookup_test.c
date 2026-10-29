/* Execute the installed client's resource hash/insert/find against synthetic
   entries, then verify the review lookup fix. No game is launched. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <elf.h>

static FILE *binary;
static Elf32_Phdr segments[16];
static size_t segment_count;
static unsigned char entries[3][0x40];
static uint32_t bucket, dictionary[3];
static const char shape_path[] = "shapes", flag_name[] = "flag.dts";
static const char *lookup_path = shape_path, *lookup_name = flag_name;
static void put_word(void *object, size_t offset, uint32_t value) {
    memcpy((unsigned char *)object + offset, &value, 4);
}
static void put_pointer(void *object, size_t offset, const void *pointer) {
    put_word(object, offset, (uint32_t)(uintptr_t)pointer);
}
static void read_address(void *destination, uint32_t address, size_t length) {
    for (size_t i = 0; i < segment_count; i++) {
        Elf32_Phdr *s = &segments[i];
        if (s->p_type != PT_LOAD || address < s->p_vaddr ||
            (uint64_t)address + length > (uint64_t)s->p_vaddr + s->p_filesz) continue;
        assert(fseek(binary, s->p_offset + address - s->p_vaddr, SEEK_SET) == 0);
        assert(fread(destination, 1, length, binary) == length);
        return;
    }
    assert(!"range is not file-backed ELF data");
}
typedef void *(*native_find_t)(void *, const char *, const char *, unsigned char);
static void *native_manager_find(void *manager, const char *name, unsigned char force) {
    char expected[256];
    snprintf(expected, sizeof expected, "%s/%s", lookup_path, lookup_name);
    assert(manager == dictionary && strcmp(name, expected) == 0);
    return ((native_find_t)0x0810614c)(manager, lookup_path, lookup_name, force);
}

int main(void) {
    char path[4096];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", getenv("HOME") ? getenv("HOME") : "");
    binary = fopen(path, "rb");
    if (!binary) { puts("SKIP: installed Loki client unavailable for resource lookup fixture"); return 0; }
    Elf32_Ehdr header;
    assert(fread(&header, 1, sizeof header, binary) == sizeof header);
    assert(memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 && header.e_ident[EI_CLASS] == ELFCLASS32);
    assert(header.e_machine == EM_386 && header.e_type == ET_EXEC);
    assert(header.e_phentsize == sizeof segments[0] && header.e_phnum <= 16);
    segment_count = header.e_phnum;
    assert(fseek(binary, header.e_phoff, SEEK_SET) == 0);
    assert(fread(segments, sizeof segments[0], segment_count, binary) == segment_count);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert(page == 4096);
    uintptr_t pages[] = {0x08106000, 0x080e8000, 0x08107000, 0x08108000, 0x082fb000};
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(mmap((void *)pages[i], page, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)pages[i]);
    read_address((void *)0x0810612c, 0x0810612c, 0x1b6); /* hash/find/insert/remove */
    read_address((void *)RV_MODEL_FIND_CALL, RV_MODEL_FIND_CALL, sizeof rv_model_find_orig);
    read_address((void *)RV_BITMAP_FIND_CALL, RV_BITMAP_FIND_CALL, sizeof rv_bitmap_find_orig);
    read_address((void *)RV_HUD_WAKE_CALL, RV_HUD_WAKE_CALL, sizeof rv_hud_wake_orig);
    fclose(binary);
    assert(memcmp((void *)RV_MODEL_FIND_CALL, rv_model_find_orig, sizeof rv_model_find_orig) == 0);
    assert(memcmp((void *)RV_BITMAP_FIND_CALL, rv_bitmap_find_orig, sizeof rv_bitmap_find_orig) == 0);
    assert(memcmp((void *)RV_HUD_WAKE_CALL, rv_hud_wake_orig, sizeof rv_hud_wake_orig) == 0);
    memcpy((void *)0x080e8b48, "\xb0\x00\xc3", 3); /* ordinary native lookup mode */
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(mprotect((void *)pages[i], page, PROT_READ | PROT_EXEC) == 0);

    /* A collision chain can contain an unrelated volume before a loose flag.
       Native volume registration (mask=2) inserts a stock flag ahead of both. */
    dictionary[0] = (uint32_t)(uintptr_t)&bucket; dictionary[1] = 2; dictionary[2] = 1;
    bucket = (uint32_t)(uintptr_t)entries[0];
    put_pointer(entries[0], 0x18, shape_path); put_pointer(entries[0], 0x1c, "other.dts");
    put_word(entries[0], 0x14, 1); put_pointer(entries[0], 8, entries[1]);
    put_pointer(entries[1], 0x18, shape_path); put_pointer(entries[1], 0x1c, flag_name);
    put_word(entries[1], 0x14, 2);
    put_pointer(entries[1], 0x20, "base/shapes"); put_pointer(entries[1], 0x24, flag_name);
    put_pointer(entries[2], 0x18, shape_path); put_pointer(entries[2], 0x1c, flag_name);
    put_word(entries[2], 0x14, 1);
    ((void (*)(void *, void *, uint32_t))0x08106228)(dictionary, entries[2], 2);
    assert((void *)(uintptr_t)bucket == entries[2]);
    assert(native_manager_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    unsigned char before[sizeof entries]; memcpy(before, entries, sizeof entries);
    rv_native_resource_find = native_manager_find;
    store_hook_option("review.cheat_view", "1"); store_hook_option("review.replace_models", "1");
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[1]);
    assert(memcmp(before, entries, sizeof entries) == 0);
    store_hook_option("review.replace_models", "0");
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    install_review_model_selection();
    assert(memcmp((void *)RV_MODEL_FIND_CALL, rv_model_find_orig, sizeof rv_model_find_orig) == 0);
    assert(memcmp((void *)RV_BITMAP_FIND_CALL, rv_bitmap_find_orig, sizeof rv_bitmap_find_orig) == 0);
    assert(memcmp((void *)RV_HUD_WAKE_CALL, rv_hud_wake_orig, sizeof rv_hud_wake_orig) == 0);
    store_hook_option("review.replace_models", "1"); store_hook_option("review.cheat_view", "0");
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    store_hook_option("review.cheat_view", "1"); target = &game_targets[1];
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    target = &game_targets[0];
    put_pointer(entries[1], 0x20, "some_other_mod/shapes");
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    put_pointer(entries[1], 0x20, "base/shapes");
    put_word(entries[1], 0x14, 10); /* do not cross native eligibility categories */
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    put_word(entries[1], 0x14, 2); put_word(entries[2], 0x14, 9);
    assert(review_model_find(dictionary, "shapes/flag.dts", 1) == entries[2]);
    put_word(entries[2], 0x14, 1);
    put_pointer(entries[2], 8, NULL);
    assert(review_model_find(dictionary, "shapes/flag.dts", 0) == entries[2]);
    assert(!rv_prefer_loose_resource(NULL));

    /* The separate uncached-bitmap route must use the same collision correction
       and preserve the native source/eligibility policy. */
    lookup_path = "textures"; lookup_name = "blue_blink4.PNG";
    for (int i = 1; i < 3; i++) {
        put_pointer(entries[i], 0x18, lookup_path);
        put_pointer(entries[i], 0x1c, lookup_name);
    }
    put_pointer(entries[1], 0x20, "base/textures");
    put_pointer(entries[1], 0x24, lookup_name);
    put_pointer(entries[2], 0x20, "base");
    put_pointer(entries[2], 0x24, "textures.vl2");
    put_pointer(entries[2], 8, entries[0]);
    game_main_thread = pthread_self();
    char *log = NULL; size_t log_length = 0;
    FILE *saved_stderr = stderr;
    stderr = open_memstream(&log, &log_length); assert(stderr);
    assert(review_bitmap_find(dictionary, "textures/blue_blink4.PNG", 0) == entries[1]);
    assert(fflush(stderr) == 0);
    size_t first_length = log_length;
    assert(strstr(log, "Review bitmap source: logical=\"textures/blue_blink4.PNG\" backing=\"base/textures/blue_blink4.PNG\" kind=loose"));
    assert(review_bitmap_find(dictionary, "textures/blue_blink4.PNG", 0) == entries[1]);
    assert(fflush(stderr) == 0 && log_length == first_length);
    store_hook_option("review.replace_models", "0");
    assert(review_bitmap_find(dictionary, "textures/blue_blink4.PNG", 0) == entries[2]);
    store_hook_option("review.replace_models", "1");
    put_word(entries[1], 0x14, 10);
    assert(review_bitmap_find(dictionary, "textures/blue_blink4.PNG", 0) == entries[2]);
    assert(fclose(stderr) == 0); stderr = saved_stderr; free(log);

    install_review_model_selection();
    int32_t relative; memcpy(&relative, (void *)(RV_MODEL_FIND_CALL + 1), 4);
    assert(RV_MODEL_FIND_CALL + 5 + relative == (uintptr_t)review_model_find);
    memcpy(&relative, (void *)(RV_BITMAP_FIND_CALL + 1), 4);
    assert(RV_BITMAP_FIND_CALL + 5 + relative == (uintptr_t)review_bitmap_find);
    memcpy(&relative, (void *)(RV_HUD_WAKE_CALL + 1), 4);
    assert(RV_HUD_WAKE_CALL + 5 + relative == (uintptr_t)review_hud_wake);
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(munmap((void *)pages[i], page) == 0);
    puts("review resource lookup: native collision precedence reproduced; loose preference and gates passed");
    return 0;
}
