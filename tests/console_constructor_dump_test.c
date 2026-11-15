/* dumpConsoleConstructor: the walk over the engine's ConsoleConstructor list, on fake nodes, plus the real
 * clients' list-head pointers checked against their bytes.
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/console_constructor_dump_test.c -o console_constructor_dump_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static struct con_constructor_node nodes[6];

static size_t dump_to_buffer(uintptr_t head, char *buffer, size_t size) {
    FILE *out = fmemopen(buffer, size, "w");
    assert(out);
    size_t count = dump_console_constructors(head, out);
    fclose(out);
    return count;
}

static int file_has(FILE *client, long offset, const unsigned char *expected, size_t length) {
    unsigned char found[8] = {0};
    if (fseek(client, offset, SEEK_SET) != 0 || fread(found, 1, length, client) != length) return 0;
    return memcmp(found, expected, length) == 0;
}

int main(void) {
    static char text[8192];
    memset(nodes, 0, sizeof nodes);
    /* A void command, a string command, a node with no callback, a bool command, and an int one. */
    nodes[0] = (struct con_constructor_node){.vc = 0x08111111, .mina = 1, .maxa = 1,
        .usage = (uintptr_t)"quit() - exit", .func_name = (uintptr_t)"quit", .class_name = 0, .next = (uintptr_t)&nodes[1]};
    nodes[1] = (struct con_constructor_node){.sc = 0x08222222, .mina = 2, .maxa = 3,
        .usage = (uintptr_t)"obj.getName()", .func_name = (uintptr_t)"getName", .class_name = (uintptr_t)"SimObject",
        .next = (uintptr_t)&nodes[2]};
    nodes[2] = (struct con_constructor_node){.next = (uintptr_t)&nodes[3]};            /* no callback: skipped */
    nodes[3] = (struct con_constructor_node){.bc = 0x08333333, .mina = 3, .maxa = 3, .func_name = (uintptr_t)"isFile",
        .next = (uintptr_t)&nodes[4]};
    /* sc outranks ic, ic outranks fc, ... like the original and like ConsoleConstructor::setup. */
    nodes[4] = (struct con_constructor_node){.sc = 0x1, .ic = 0x2, .fc = 0x3, .vc = 0x4, .bc = 0x5,
        .func_name = (uintptr_t)"both", .class_name = (uintptr_t)"C", .usage = (uintptr_t)"line\nbreak\there", .next = 0};
    size_t count = dump_to_buffer((uintptr_t)&nodes[0], text, sizeof text);
    assert(count == 4);
    char expected_first[160];
    snprintf(expected_first, sizeof expected_first, "\t0x%08lX name: ::quit - vc: 0x08111111 1 1  usage: quit() - exit\n",
             (unsigned long)(uintptr_t)&nodes[0]);
    assert(strncmp(text, expected_first, strlen(expected_first)) == 0);
    assert(strstr(text, "name: SimObject::getName - sc: 0x08222222 2 3  usage: obj.getName()\n"));
    assert(strstr(text, "name: ::isFile - bc: 0x08333333 3 3  usage: \n"));
    assert(strstr(text, "name: C::both - sc: 0x00000001 0 0  usage: line break here\n"));   /* whitespace flattened */
    assert(!strstr(text, "- ic: 0x00000002"));

    /* The end of the chain, an unreadable next pointer and loops must stop the walk. */
    nodes[4].next = 0x10;                                         /* not a mapped address */
    assert(dump_to_buffer((uintptr_t)&nodes[0], text, sizeof text) == 4);
    nodes[4].next = (uintptr_t)&nodes[4];                         /* points at itself */
    assert(dump_to_buffer((uintptr_t)&nodes[0], text, sizeof text) == 4);
    nodes[4].next = (uintptr_t)&nodes[1];                         /* a longer loop */
    assert(dump_to_buffer((uintptr_t)&nodes[0], text, sizeof text) == 4);   /* each node once */
    assert(dump_to_buffer(0, text, sizeof text) == 0);            /* empty list */
    assert(dump_to_buffer(0x10, text, sizeof text) == 0);         /* unreadable head */

    /* The list-head pointers of the real clients, read from the mov at the top of setup(). */
    int checked = 0;
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    FILE *client = fopen(path, "rb");
    if (client) {
        static const unsigned char client_mov[] = {0x8b, 0x1d, 0xf4, 0x21, 0x49, 0x08};
        assert(game_targets[0].con_constructor_first == 0x084921F4);
        assert(file_has(client, (long)(0x080E6F27 - 0x08048000), client_mov, sizeof client_mov));
        fclose(client);
        checked++;
    }
    snprintf(path, sizeof path, "%s/t2-linux/tribes2d.dynamic", home ? home : "");
    FILE *server = fopen(path, "rb");
    if (server) {
        static const unsigned char server_mov[] = {0x8b, 0x1d, 0x34, 0x74, 0x48, 0x08};
        assert(game_targets[1].con_constructor_first == 0x08487434);
        assert(file_has(server, (long)(0x080E5E17 - 0x08048000), server_mov, sizeof server_mov));
        fclose(server);
        checked++;
    }
    printf("ConsoleConstructor dump order, callbacks, loop guards and %d real list-head pointer(s) verified passed\n", checked);
    return 0;
}
