#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef unsigned long mp_limb_t;
typedef struct { int _mp_alloc, _mp_size; mp_limb_t *_mp_d; } __mpz_struct;
typedef __mpz_struct mpz_t[1];

void __gmpz_init(mpz_t x);
void __gmpz_clear(mpz_t x);
void __gmpz_set_str(mpz_t rop, const char *str, int base);
void __gmpz_powm(mpz_t rop, const mpz_t base, const mpz_t exp, const mpz_t mod);
char* __gmpz_get_str(char *str, int base, const mpz_t op);

static char result_buf[2048];

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <base_hex> <exp_hex> <mod_hex>\n", argv[0]);
        return 1;
    }
    mpz_t b, e, m, r;
    __gmpz_init(b); __gmpz_init(e); __gmpz_init(m); __gmpz_init(r);
    __gmpz_set_str(b, argv[1], 16);
    __gmpz_set_str(e, argv[2], 16);
    __gmpz_set_str(m, argv[3], 16);
    __gmpz_powm(r, b, e, m);
    char *s = __gmpz_get_str(result_buf, 16, r);
    printf("%s\n", s);
    fflush(stdout);
    __gmpz_clear(b); __gmpz_clear(e); __gmpz_clear(m); __gmpz_clear(r);
    return 0;
}
