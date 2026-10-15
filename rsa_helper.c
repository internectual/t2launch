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
    char input_base[8192], input_exp[8192], input_mod[8192];
    const char *base;
    const char *exp;
    const char *mod;
    if (argc == 2 && strcmp(argv[1], "--stdin") == 0) {
        if (!fgets(input_base, sizeof(input_base), stdin) ||
            !fgets(input_exp, sizeof(input_exp), stdin) ||
            !fgets(input_mod, sizeof(input_mod), stdin)) return 1;
        input_base[strcspn(input_base, "\r\n")] = 0;
        input_exp[strcspn(input_exp, "\r\n")] = 0;
        input_mod[strcspn(input_mod, "\r\n")] = 0;
        base = input_base;
        exp = input_exp;
        mod = input_mod;
    } else if (argc >= 4) {
        base = argv[1];
        exp = argv[2];
        mod = argv[3];
    } else {
        fprintf(stderr, "Usage: %s <base_hex> <exp_hex> <mod_hex>\n", argv[0]);
        return 1;
    }
    mpz_t b, e, m, r;
    __gmpz_init(b); __gmpz_init(e); __gmpz_init(m); __gmpz_init(r);
    __gmpz_set_str(b, base, 16);
    __gmpz_set_str(e, exp, 16);
    __gmpz_set_str(m, mod, 16);
    __gmpz_powm(r, b, e, m);
    char *s = __gmpz_get_str(result_buf, 16, r);
    printf("%s\n", s);
    fflush(stdout);
    __gmpz_clear(b); __gmpz_clear(e); __gmpz_clear(m); __gmpz_clear(r);
    return 0;
}
