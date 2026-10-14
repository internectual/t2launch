/* Test the actual patch code with read-only pages at the game's addresses. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

int main(void) {
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
    puts("Hook WON patch: all-or-nothing validation, replacement, permissions passed");
    return 0;
}
