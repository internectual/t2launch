#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <gmp.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static char result_buf[2048];

static int random_bytes(unsigned char *buffer, size_t length) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, buffer + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            return 0;
        }
        offset += (size_t)count;
    }
    close(fd);
    return 1;
}

static int random_prime(mpz_t prime, unsigned int bits) {
    unsigned int byte_count = (bits + 7) / 8;
    unsigned char bytes[64];
    if (byte_count > sizeof(bytes)) return 0;
    for (;;) {
        if (!random_bytes(bytes, byte_count)) return 0;
        mpz_import(prime, byte_count, 1, 1, 0, 0, bytes);
        memset(bytes, 0, byte_count);
        mpz_setbit(prime, bits - 1);
        mpz_setbit(prime, 0);
        mpz_nextprime(prime, prime);
        if (mpz_sizeinbase(prime, 2) == bits) return 1;
    }
}

static int generate_rsa_key(unsigned int bits) {
    if (bits < 512 || bits > 1024 || bits % 2 != 0) return 1;

    mpz_t p, q, n, phi, e, d, tmp, gcd;
    mpz_inits(p, q, n, phi, e, d, tmp, gcd, NULL);

    unsigned int prime_bits = bits / 2;
    do {
        if (!random_prime(p, prime_bits) || !random_prime(q, prime_bits)) {
            fprintf(stderr, "Unable to read cryptographic randomness\n");
            mpz_clears(p, q, n, phi, e, d, tmp, gcd, NULL);
            return 1;
        }

        mpz_sub(tmp, p, q);
        mpz_abs(tmp, tmp);
    } while (mpz_sizeinbase(tmp, 2) < prime_bits / 2 || mpz_cmp(p, q) == 0);

    mpz_mul(n, p, q);
    if (mpz_sizeinbase(n, 2) != bits) {
        /* A product of two half-width primes can be one bit short. */
        mpz_clears(p, q, n, phi, e, d, tmp, gcd, NULL);
        return generate_rsa_key(bits);
    }

    mpz_sub_ui(tmp, p, 1);
    mpz_sub_ui(phi, q, 1);
    mpz_mul(phi, phi, tmp);
    const unsigned long candidates[] = {3, 5, 7};
    int found_exponent = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        mpz_set_ui(e, candidates[i]);
        mpz_gcd(gcd, e, phi);
        if (mpz_cmp_ui(gcd, 1) == 0) {
            found_exponent = 1;
            break;
        }
    }
    if (!found_exponent || mpz_invert(d, e, phi) == 0) {
        mpz_clears(p, q, n, phi, e, d, tmp, gcd, NULL);
        return generate_rsa_key(bits);
    }

    gmp_printf("%Zx\t%Zx\t%Zx\n", n, e, d);
    mpz_clears(p, q, n, phi, e, d, tmp, gcd, NULL);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--keygen") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long bits = strtoul(argv[2], &end, 10);
        if (errno || !end || *end ||
            (bits != 512 && bits != 768 && bits != 1024)) {
            fprintf(stderr, "Usage: %s --keygen <512|768|1024>\n", argv[0]);
            return 1;
        }
        return generate_rsa_key((unsigned int)bits);
    }

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
