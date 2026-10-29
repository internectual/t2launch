/* Synthetic resource cache: inspect loaded sources without loading any game. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static unsigned char manager[0x890], entries[5][0x40];
static uint32_t buckets[3];
static uint32_t object_dictionary[4096];
static unsigned char objects[3][0x2d4], datablocks[2][0xbc], item_class[0x1c];
static void *item_vtable[3];
static void *get_item_class(void *unused) { (void)unused; return item_class; }

static void put_word(void *object, size_t offset, uint32_t value) {
    memcpy((unsigned char *)object + offset, &value, 4);
}
static void put_pointer(void *object, size_t offset, const void *pointer) {
    put_word(object, offset, (uint32_t)(uintptr_t)pointer);
}
static void entry(int index, const char *name, const char *backing_path,
                  const char *backing_name, uint32_t flags, int loaded) {
    put_pointer(entries[index], 0x18, "shapes");
    put_pointer(entries[index], 0x1c, name);
    put_pointer(entries[index], 0x20, backing_path);
    put_pointer(entries[index], 0x24, backing_name);
    put_word(entries[index], 0x14, flags);
    put_word(entries[index], 0x28, 1234);
    put_word(entries[index], 0x3c, 0xdeadbeef);
    if (loaded) put_pointer(entries[index], 0x34, manager);
}
static char *dump(const char *filter, const char **error, unsigned int *count) {
    struct rv_memory memory;
    assert(rv_memory_snapshot(&memory));
    char *text = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&text, &length);
    assert(stream);
    *error = rv_dump_model_resources(&memory, manager, filter, stream, count);
    assert(fclose(stream) == 0);
    return text;
}

int main(void) {
    put_pointer(manager, 0x884, buckets);
    put_word(manager, 0x88c, 3);
    entry(0, "flag.dts", "base", "shapes.vl2", 1, 1);
    entry(1, "mine.DTS", "base/shapes", "mine.dts", 2, 1);
    entry(2, "flag.dts", "base/shapes", "flag.dts", 2, 0); /* registered, not loaded */
    entry(3, "terrain.ter", "base", "terrain.vl2", 1, 1);
    entry(4, "pack.dts", NULL, "pack.dts", 2, 1); /* root-level backing file */
    put_pointer(entries[0], 8, entries[1]);
    put_pointer(entries[1], 8, entries[2]);
    put_pointer(entries[3], 8, entries[4]);
    buckets[0] = (uint32_t)(uintptr_t)entries[0];
    buckets[2] = (uint32_t)(uintptr_t)entries[3];
    unsigned char before_manager[sizeof manager], before_entries[sizeof entries];
    uint32_t before_buckets[3];
    memcpy(before_manager, manager, sizeof manager);
    memcpy(before_entries, entries, sizeof entries);
    memcpy(before_buckets, buckets, sizeof buckets);

    const char *error;
    unsigned int count;
    char *text = dump("", &error, &count);
    assert(!error && count == 3);
    assert(strstr(text, "logical=\"shapes/flag.dts\" backing=\"base/shapes.vl2\" kind=volume"));
    assert(strstr(text, "logical=\"shapes/mine.DTS\" backing=\"base/shapes/mine.dts\" kind=loose"));
    assert(strstr(text, "backing=\"pack.dts\" kind=loose"));
    assert(strstr(text, "offset=1234 flags=0x1 cached_crc=0xdeadbeef"));
    assert(!strstr(text, "backing=\"base/shapes/flag.dts\"") && !strstr(text, "terrain.ter"));
    free(text);
    text = dump("FLAG", &error, &count);
    assert(!error && count == 1 && strstr(text, "shapes.vl2") && !strstr(text, "mine.DTS"));
    free(text);
    text = dump("missing", &error, &count);
    assert(!error && count == 0 && !*text);
    free(text);
    assert(memcmp(manager, before_manager, sizeof manager) == 0);
    assert(memcmp(entries, before_entries, sizeof entries) == 0);
    assert(memcmp(buckets, before_buckets, sizeof buckets) == 0);

    /* Resolve actual ghost references, rather than choosing an arbitrary cached
       copy of the same shape. The flag uses the volume entry, mine the loose one. */
    put_pointer(item_class, 0x18, "Item"); item_vtable[2] = get_item_class;
    for (int i = 0; i < 3; i++) {
        put_pointer(objects[i], 0, item_vtable);
        put_word(objects[i], 0x20, 200 + i);
        put_word(objects[i], 0x40, i < 2 ? 2 : 0); /* third item is not a ghost */
        put_pointer(objects[i], 0x2d0, datablocks[i % 2]);
        object_dictionary[200 + i] = (uint32_t)(uintptr_t)objects[i];
    }
    for (int i = 0; i < 2; i++) {
        put_word(datablocks[i], 0x20, i ? 546 : 982);
        put_pointer(datablocks[i], 0x44, i ? "mine.dts" : "flag.dts");
        put_pointer(datablocks[i], 0xb8, entries[i]);
    }
    struct rv_memory memory;
    assert(rv_memory_snapshot(&memory));
    char *uses = NULL;
    size_t uses_length = 0;
    FILE *uses_stream = open_memstream(&uses, &uses_length);
    assert(uses_stream);
    unsigned char before_objects[sizeof objects], before_data[sizeof datablocks];
    memcpy(before_objects, objects, sizeof objects);
    memcpy(before_data, datablocks, sizeof datablocks);
    assert(!rv_dump_model_users(&memory, object_dictionary, "", uses_stream, &count) && count == 2);
    assert(fflush(uses_stream) == 0);
    char expected[128];
    snprintf(expected, sizeof expected, "datablock=982 shape=\"flag.dts\" resource=%p", (void *)entries[0]);
    assert(strstr(uses, expected));
    snprintf(expected, sizeof expected, "datablock=546 shape=\"mine.dts\" resource=%p", (void *)entries[1]);
    assert(strstr(uses, expected) && !strstr(uses, "object=202"));
    assert(!rv_dump_model_users(&memory, object_dictionary, "FLAG", uses_stream, &count) && count == 1);
    assert(memcmp(objects, before_objects, sizeof objects) == 0);
    assert(memcmp(datablocks, before_data, sizeof datablocks) == 0);
    put_pointer(objects[0], 0x2d0, (void *)1);
    assert(!rv_dump_model_users(&memory, object_dictionary, "", uses_stream, &count) && count == 1);
    assert(rv_dump_model_users(&memory, (void *)1, "", uses_stream, &count));
    assert(fclose(uses_stream) == 0); free(uses);

    /* Reject malformed/unmapped dictionaries and strings; bound cyclic chains. */
    put_word(manager, 0x88c, UINT32_MAX);
    text = dump("", &error, &count);
    assert(error && count == 0); free(text);
    put_word(manager, 0x88c, 3);
    put_word(manager, 0x884, 1);
    text = dump("", &error, &count);
    assert(error && count == 0); free(text);
    put_pointer(manager, 0x884, buckets);
    buckets[0] = 1;
    text = dump("", &error, &count);
    assert(error && count == 0); free(text);
    buckets[0] = (uint32_t)(uintptr_t)entries[0];
    put_word(entries[0], 0x24, 1);
    text = dump("", &error, &count);
    assert(error && count == 0); free(text);
    put_pointer(entries[0], 0x24, "shapes.vl2");
    put_pointer(entries[2], 8, entries[0]);
    text = dump("missing", &error, &count);
    assert(error && strcmp(error, "resource traversal limit reached") == 0); free(text);
    put_word(entries[2], 8, 0);

    const char *query[] = {"hook_review_models"};
    assert(strcmp(hook_review_models_cb(NULL, 1, query), "game thread required") == 0);
    game_main_thread_known = 1; game_main_thread = pthread_self();
    /* No game code is mapped: the command must refuse this process's layout. */
    assert(strcmp(hook_review_models_cb(NULL, 1, query), "unsupported resource layout") == 0);

    /* Playback-entry capture must wait through any number of loading swaps
       until the gameplay HUD is drawn; failures still emit only once. */
    char *automatic = NULL;
    size_t automatic_length = 0;
    FILE *saved_stderr = stderr;
    stderr = open_memstream(&automatic, &automatic_length);
    assert(stderr);
    store_hook_option("review.cheat_view", "0");
    review_model_hud_seen = 1;
    update_review_model_diagnostic(1);
    store_hook_option("review.cheat_view", "1");
    update_review_model_diagnostic(0);
    for (int i = 0; i < 1000; i++) update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length == 0);
    review_model_hud_seen = 1;
    update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length > 0);
    size_t first_length = automatic_length;
    for (int i = 0; i < 1000; i++) update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length == first_length);
    update_review_model_diagnostic(0);
    assert(!review_model_hud_seen);
    update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length == first_length);
    review_model_hud_seen = 1;
    update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length == first_length * 2);
    assert(strstr(automatic, "Automatic review model diagnostic: unsupported resource layout"));
    target = &game_targets[1];
    review_model_hud_seen = 1;
    update_review_model_diagnostic(1);
    assert(fflush(stderr) == 0 && automatic_length == first_length * 2);
    assert(fclose(stderr) == 0);
    stderr = saved_stderr;
    free(automatic);
    assert(strcmp(hook_review_models_cb(NULL, 1, query), "client only") == 0);
    puts("review models: cached sources, filtering, read-only traversal and malformed-memory gates passed");
    return 0;
}
