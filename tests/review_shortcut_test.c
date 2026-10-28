/* Execute only the installed client's standalone ActionMap descriptor parser
 * in synthetic mappings, with ordinary C string helpers. No game is launched.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/review_shortcut_test.c
 *     -o /tmp/opencode/review_shortcut_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <elf.h>
#include <strings.h>

static FILE *binary;
static Elf32_Phdr segments[16];
static size_t segment_count;
static void read_address(void *destination, uint32_t address, size_t length) {
    for (size_t i = 0; i < segment_count; i++) {
        Elf32_Phdr *s = &segments[i];
        if (s->p_type != PT_LOAD || address < s->p_vaddr ||
            (uint64_t)address + length > (uint64_t)s->p_vaddr + s->p_filesz) continue;
        assert(fseek(binary, s->p_offset + address - s->p_vaddr, SEEK_SET) == 0);
        assert(fread(destination, 1, length, binary) == length);
        return;
    }
    assert(!"requested range is not file-backed ELF data");
}

static char *string_copy(char *destination, const char *source) { return strcpy(destination, source); }
static const char *string_char(const char *string, int c) { return strchr(string, c); }
static char *string_token(char *string, const char *delimiter) { return strtok(string, delimiter); }
static int string_compare(const char *a, const char *b) { return strcasecmp(a, b); }
static size_t string_length(const char *string) { return strlen(string); }
static void redirect(uintptr_t address, void *function) {
    unsigned char jump[5] = {0xe9};
    int32_t relative = (int32_t)((uintptr_t)function - (address + sizeof jump));
    memcpy(jump + 1, &relative, 4);
    memcpy((void *)address, jump, sizeof jump);
}

struct action_descriptor { uint8_t modifiers, type; uint16_t code; };
typedef unsigned char (*parse_action_t)(const char *, struct action_descriptor *);

int main(void) {
    char path[4096];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    binary = fopen(path, "rb");
    if (!binary) {
        puts("SKIP: installed Loki client unavailable for native descriptor-parser fixture");
        return 0;
    }
    Elf32_Ehdr header;
    assert(fread(&header, 1, sizeof header, binary) == sizeof header);
    assert(memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 && header.e_ident[EI_CLASS] == ELFCLASS32);
    assert(header.e_machine == EM_386 && header.e_type == ET_EXEC);
    assert(header.e_phentsize == sizeof segments[0] && header.e_phnum <= 16);
    segment_count = header.e_phnum;
    assert(fseek(binary, header.e_phoff, SEEK_SET) == 0);
    assert(fread(segments, sizeof segments[0], segment_count, binary) == segment_count);

    const uintptr_t pages[] = {0x081f3000, 0x081cf000, 0x0844b000, 0x08495000};
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    assert(page_size == 4096);
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        assert(mmap((void *)pages[i], page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)pages[i]);
    }
    /* Only the parser is copied as code; names and lookup tables stay read-only.
       Data PT_LOAD uses a different file/VA delta than the code segment. */
    read_address((void *)0x081f389c, 0x081f389c, 0x26d);
    read_address((void *)0x0844b000, 0x0844b000, page_size);
    read_address((void *)0x08495000, 0x08495000, page_size);
    fclose(binary);
    assert(memcmp((void *)0x081f389c, "\x55\x89\xe5\x81\xec\x0c\x01\x00\x00", 9) == 0);
    assert(strcmp((char *)0x0844b294, "-") == 0);
    assert(strcmp((char *)0x0844b397, "shift-ctrl ") == 0);
    redirect(0x081cf428, string_copy);
    redirect(0x081cf590, string_char);
    redirect(0x081cf670, string_token);
    redirect(0x081cf3cc, string_compare);
    redirect(0x081cf464, string_length);
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(mprotect((void *)pages[i], page_size,
                        PROT_READ | (i < 2 ? PROT_EXEC : 0)) == 0);

    parse_action_t parse = (parse_action_t)0x081f389c;
    struct action_descriptor result = {0};
    assert(!parse("ctrl shift f6", &result));
    assert(!parse("ctrl shift f7", &result));

    /* Read the descriptors from the ACTUAL in-memory script, so regression to
       an unsupported spelling fails even if test-only examples remain valid. */
    const char *start = strstr(review_shortcuts_script, "%keys = \"");
    assert(start);
    start += strlen("%keys = \"");
    const char *end = strchr(start, '"');
    char keys[128];
    assert(end && (size_t)(end - start) < sizeof keys);
    memcpy(keys, start, end - start); keys[end - start] = 0;
    char *second = strstr(keys, "\\t");
    assert(second); *second = 0; second += 2;
    assert(parse(keys, &result));
    assert(result.modifiers == 0x0f && result.type == 10 && result.code == 0x5c);
    assert(parse(second, &result));
    assert(result.modifiers == 0x0f && result.type == 10 && result.code == 0x5d);
    assert(parse("f6", &result) && result.modifiers == 0 && result.code == 0x5c);
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        assert(munmap((void *)pages[i], page_size) == 0);
    puts("Review shortcuts: native parser rejects old descriptors and accepts script Ctrl+Shift+F6/F7 bindings");
    return 0;
}
