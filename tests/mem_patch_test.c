/* Synthetic executable mappings only: exercises memPatch modes without a game.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/mem_patch_test.c
 *     -o /tmp/opencode/mem_patch_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static char last_script[768];
static unsigned int evaluate_count;
static unsigned int registered_count;
static int registered_mem_patch, registered_heading, registered_compass_markers;

static const char *fake_evaluate(const char *code, unsigned char echo,
                                 const char *filename, unsigned char no_calls) {
    assert(!echo && !no_calls);
    assert(filename && strcmp(filename, "t2launch/memPatch.cs") == 0);
    snprintf(last_script, sizeof last_script, "%s", code);
    evaluate_count++;
    return "";
}

static void fake_add_method(const char *namespace_name, const char *name,
                            const char *(*callback)(void *, S32, const char **),
                            const char *usage, S32 minimum, S32 maximum) {
    (void)callback; (void)usage;
    assert(namespace_name == NULL);
    if (strcmp(name, "memPatch") == 0) {
        assert(minimum == 3 && maximum == 4);
        registered_mem_patch = 1;
    } else if (strcmp(name, "hook_hud_demo_heading") == 0) {
        assert(minimum == 1 && maximum == 1);
        registered_heading = 1;
    } else if (strcmp(name, "hook_hud_demo_compass_markers") == 0) {
        assert(minimum == 6 && maximum == 6);
        registered_compass_markers = 1;
    } else assert(!"unexpected early command registration");
    registered_count++;
}

static const char *fake_startup_evaluate(const char *code, unsigned char echo,
                                         const char *filename, unsigned char no_calls) {
    assert(code && !echo && !no_calls);
    assert(filename && strcmp(filename, "startup.cs") == 0);
    assert(registered_count == 3 && registered_mem_patch && registered_heading &&
           registered_compass_markers);
    return "startup evaluated";
}

static const char *invoke_mem_patch(const char *address, const char *patch,
                                    const char *checksum, S32 argc) {
    const char *argv[4] = {"memPatch", address, patch, checksum};
    return mem_patch_cb(NULL, argc, argv);
}

int main(void) {
    char sha[65];
    struct sha256_context hash;
    sha256_init(&hash);
    sha256_update(&hash, (const uint8_t *)"abc", 3);
    sha256_final(&hash, sha);
    assert(strcmp(sha, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    uint8_t repeated[1000];
    memset(repeated, 'a', sizeof repeated);
    sha256_init(&hash);
    for (unsigned int i = 0; i < 1000; i++)
        sha256_update(&hash, repeated, sizeof repeated);
    sha256_final(&hash, sha);
    assert(strcmp(sha, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

    const char *executable_hash = mem_patch_executable_hash();
    assert(executable_hash && strlen(executable_hash) == 64);
    char digest[65];
    memcpy(digest, executable_hash, sizeof digest);
    char checksum[72];
    snprintf(checksum, sizeof checksum, "sha256:%s", digest);
    assert(mem_patch_checksum_matches(checksum, digest));
    assert(mem_patch_checksum_matches(digest, digest));
    assert(!mem_patch_checksum_matches("sha256:bad", digest));
    Con__evaluate = fake_evaluate;

    /* The evaluator gate must install the compatibility command before the
       first user script is evaluated, not merely after the first rendered frame. */
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    uintptr_t console_page = (uintptr_t)target->string_table & ~((uintptr_t)page_size - 1);
    void *console_mapping = mmap((void *)console_page, (size_t)page_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(console_mapping == (void *)console_page);
    uint32_t console_object = (uint32_t)(console_page + 0x100);
    *(volatile uint32_t *)target->string_table = console_object;
    *(volatile uint32_t *)(console_object + CONSOLE_TABLE_SIZE_OFFS) = 1;
    Con__addMethodS = fake_add_method;
    real_con_evaluate = fake_startup_evaluate;
    assert(strcmp(con_evaluate_gate("exec(\"autoexec.cs\");", 0, "startup.cs", 0),
                  "startup evaluated") == 0);
    assert(early_compat_commands_registered);
    real_con_evaluate = NULL;
    Con__addMethodS = NULL;
    assert(munmap(console_mapping, (size_t)page_size) == 0);

    unsigned char *mapping = mmap(NULL, (size_t)page_size * 2,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    memset(mapping, 0x90, (size_t)page_size * 2);
    assert(mprotect(mapping, (size_t)page_size * 2, PROT_READ | PROT_EXEC) == 0);

    char address[32];
    uintptr_t patch_address = (uintptr_t)mapping + (uintptr_t)page_size - 2;
    snprintf(address, sizeof address, "%lx", (unsigned long)patch_address);
    unsigned char original[4];
    memcpy(original, (void *)patch_address, sizeof original);

    store_hook_option("hook.mem_patch", "off");
    evaluate_count = 0;
    assert(strcmp(invoke_mem_patch(address, "AABBCCDD", checksum, 4), "") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);
    assert(evaluate_count == 0);

    store_hook_option("hook.mem_patch", "warn");
    assert(strcmp(invoke_mem_patch(address, "AABBCCDD", NULL, 3), "warn-only") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);
    assert(strstr(last_script, "legacy two-argument call refused") != NULL);
    assert(strstr(last_script, checksum) != NULL);
    assert(strcmp(invoke_mem_patch(address, "AABBCCDD", checksum, 4), "warn-only") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);
    assert(strstr(last_script, "warn-only mode") != NULL);

    store_hook_option("hook.mem_patch", "enabled");
    assert(strcmp(invoke_mem_patch(address, "AABBCCDD", NULL, 3), "checksum required") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);
    assert(strcmp(invoke_mem_patch(address, "AABBCCDD", "sha256:0000000000000000000000000000000000000000000000000000000000000000", 4),
                  "checksum mismatch") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);
    assert(strcmp(invoke_mem_patch(address, "not-hex", checksum, 4), "invalid patch") == 0);
    assert(memcmp((void *)patch_address, original, sizeof original) == 0);

    /* Odd-length hex retains TribesNEXT's old left-zero-nibble convention. */
    assert(strcmp(invoke_mem_patch(address, "ABC", checksum, 4), "ok") == 0);
    const unsigned char odd_expected[] = {0x0a, 0xbc};
    assert(memcmp((void *)patch_address, odd_expected, sizeof odd_expected) == 0);
    int protection = 0;
    assert(readable_range(patch_address, sizeof odd_expected, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));

    /* Data mappings are supported too; original permissions are retained. */
    unsigned char *data = mmap(NULL, (size_t)page_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(data != MAP_FAILED);
    data[0] = 0x5a;
    snprintf(address, sizeof address, "%lx", (unsigned long)(uintptr_t)data);
    assert(strcmp(invoke_mem_patch(address, "00", checksum, 4), "ok") == 0);
    assert(data[0] == 0);
    assert(readable_range((uintptr_t)data, 1, &protection));
    assert(protection == (PROT_READ | PROT_WRITE));
    assert(strcmp(invoke_mem_patch("1000", "00", checksum, 4), "patch refused") == 0);
    assert(strstr(last_script, "readable memory mapping") != NULL);

    assert(munmap(data, (size_t)page_size) == 0);
    assert(munmap(mapping, (size_t)page_size * 2) == 0);

    uintptr_t evaluate_page = target->con_evaluate & ~((uintptr_t)page_size - 1);
    void *evaluate_mapping = mmap((void *)evaluate_page, (size_t)page_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(evaluate_mapping == (void *)evaluate_page);
    memcpy((void *)target->con_evaluate, con_evaluate_gate_original,
           sizeof con_evaluate_gate_original);
    assert(mprotect(evaluate_mapping, (size_t)page_size, PROT_READ | PROT_EXEC) == 0);
    assert(install_console_evaluate_gate());
    const unsigned char *gate_bytes = (const unsigned char *)target->con_evaluate;
    assert(gate_bytes[0] == 0xe9 && gate_bytes[5] == 0x90 &&
           gate_bytes[6] == 0x90 && gate_bytes[7] == 0x90);
    assert(readable_range(target->con_evaluate, sizeof con_evaluate_gate_original, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    assert(munmap(evaluate_mapping, (size_t)page_size) == 0);
    puts("memPatch modes, startup compatibility, SHA-256 identity, legacy warning, and page restoration passed");
    return 0;
}
