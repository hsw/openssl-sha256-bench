/*
 * bench-sha256.c — benchmark 5 OpenSSL SHA-256 API flavors on identical
 * 200-byte input. 3 timed passes per API; pass 1 primes CPU caches / branch
 * predictors, passes 2-3 are the real measurements. "best" = min of 3.
 *
 * APIs covered:
 *   (1)  SHA256()                               legacy one-shot       [deprecated 3.0]
 *   (2)  SHA256_Init + _Update + _Final         legacy incremental    [deprecated 3.0]
 *   (3)  EVP_MD_CTX_new + EVP_Digest{Init,Update,Final}_ex + EVP_MD_CTX_free
 *                                               current incremental
 *   (3b) same as (3) but EVP_MD_CTX_reset instead of new/free per call
 *                                               current incremental, reused ctx
 *   (3c) same as (3b) but with EVP_MD_CTX_FLAG_ONESHOT set before each Init
 *                                               (no-op in 1.1+, see README)
 *   (4)  EVP_Q_digest                           current one-shot      [3.0+]
 *
 * Build: make
 * Run:   ./bench-sha256              (defaults to 5M iterations per pass)
 *        ./bench-sha256 2000000
 */

#define _POSIX_C_SOURCE 199309L
#define OPENSSL_SUPPRESS_DEPRECATED

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#define INPUT_LEN 200
#define PASSES    3

/* Abort on OpenSSL failure — cheap, eliminates M1 from audit. */
#define XCHK(expr) do {                                           \
    if ((expr) != 1) {                                            \
        fprintf(stderr, "OpenSSL call failed at %s:%d: " #expr "\n", \
                __FILE__, __LINE__);                              \
        abort();                                                  \
    }                                                             \
} while (0)

#define XNOTNULL(p) do {                                          \
    if ((p) == NULL) {                                            \
        fprintf(stderr, "NULL from %s at %s:%d\n",                \
                #p, __FILE__, __LINE__);                          \
        abort();                                                  \
    }                                                             \
} while (0)

static double
wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- per-API one-call wrappers ---- */

static const unsigned char *g_msg;
static unsigned char         g_md[32];
static EVP_MD_CTX           *g_reuse_ctx;  /* for (3b) */

static void
api_sha256_oneshot(void)
{
    SHA256(g_msg, INPUT_LEN, g_md);
}

static void
api_sha256_incremental(void)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, g_msg, INPUT_LEN);
    SHA256_Final(g_md, &ctx);
}

/*
 * Hot-path API wrappers — no return-value checks on purpose. Adding branches
 * here would measure "API + error-check" instead of "API alone". The reference
 * digest comparison at the end of time_api() catches any silent corruption.
 */
static void
api_evp_new_free(void)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int n;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, g_msg, INPUT_LEN);
    EVP_DigestFinal_ex(ctx, g_md, &n);
    EVP_MD_CTX_free(ctx);
}

static void
api_evp_reused(void)
{
    unsigned int n;
    EVP_MD_CTX_reset(g_reuse_ctx);
    EVP_DigestInit_ex(g_reuse_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(g_reuse_ctx, g_msg, INPUT_LEN);
    EVP_DigestFinal_ex(g_reuse_ctx, g_md, &n);
}

/* (3c) reused ctx + EVP_MD_CTX_FLAG_ONESHOT. Originally consumed by the
 * 1.0.2 cryptodev/openbsd_hw engines to skip buffering input across
 * multiple Update() calls. The flag is still exposed on modern OpenSSL
 * but is read nowhere on the SHA-256 path since 1.1.1 — see README. */
static void
api_evp_reused_oneshot(void)
{
    unsigned int n;
    EVP_MD_CTX_reset(g_reuse_ctx);
    EVP_MD_CTX_set_flags(g_reuse_ctx, EVP_MD_CTX_FLAG_ONESHOT);
    EVP_DigestInit_ex(g_reuse_ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(g_reuse_ctx, g_msg, INPUT_LEN);
    EVP_DigestFinal_ex(g_reuse_ctx, g_md, &n);
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static void
api_evp_q_digest(void)
{
    size_t n = 0;
    EVP_Q_digest(NULL, "SHA256", NULL, g_msg, INPUT_LEN, g_md, &n);
}
#endif

/* ---- timing harness ---- */

typedef void (*api_fn)(void);

static void
time_api(api_fn fn, size_t iters, const unsigned char *ref, int *digest_ok,
         double *passes)
{
    int p;
    size_t i;
    double t0, t1;

    for (p = 0; p < PASSES; p++) {
        memset(g_md, 0, sizeof(g_md));
        t0 = wall_ns();
        for (i = 0; i < iters; i++) fn();
        t1 = wall_ns();
        passes[p] = (t1 - t0) / (double)iters;
    }
    *digest_ok = (memcmp(g_md, ref, 32) == 0);
}

static void
print_row(const char *label, const double *passes, int digest_ok, double best)
{
    printf("  %-44s  warm=%6.1f  run1=%6.1f  run2=%6.1f  best=%6.1f  %s\n",
           label,
           passes[0], passes[1], passes[2],
           best,
           digest_ok ? "digest-ok" : "DIGEST-MISMATCH");
}

static double
min3_of_pass_12(const double *passes)
{
    /* pass 0 = warmup (CPU caches cold); report best of passes 1, 2 */
    return (passes[1] < passes[2]) ? passes[1] : passes[2];
}

int
main(int argc, char **argv)
{
    size_t iters;
    unsigned char msg[INPUT_LEN];
    unsigned char ref[32];
    size_t i;

    if (argc > 1) {
        long long raw = atoll(argv[1]);
        if (raw <= 0) {
            fprintf(stderr, "usage: %s [iterations]  (must be > 0)\n", argv[0]);
            return 2;
        }
        iters = (size_t)raw;
    } else {
        iters = 5000000;
    }

    for (i = 0; i < INPUT_LEN; i++) msg[i] = (unsigned char)(i * 31 + 7);
    g_msg = msg;

    /* reference digest via EVP */
    {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        unsigned int n;
        XNOTNULL(ctx);
        XCHK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
        XCHK(EVP_DigestUpdate(ctx, msg, INPUT_LEN));
        XCHK(EVP_DigestFinal_ex(ctx, ref, &n));
        EVP_MD_CTX_free(ctx);
    }

    g_reuse_ctx = EVP_MD_CTX_new();
    XNOTNULL(g_reuse_ctx);

    printf("Compiled against: %s (0x%08lx)\n",
           OPENSSL_VERSION_TEXT, (unsigned long)OPENSSL_VERSION_NUMBER);
    printf("Linked at runtime: %s\n", OpenSSL_version(OPENSSL_VERSION));
#ifdef BENCH_STATIC_LINK
    printf("Linkage:    static (libcrypto.a, same as nginx --with-openssl=)\n");
#else
    printf("Linkage:    dynamic (-lcrypto)\n");
#endif
    printf("input:      %d bytes\n", INPUT_LEN);
    printf("iterations: %zu × %d passes (pass 1 = warmup, report best of passes 2-3)\n",
           iters, PASSES);
    printf("reference:  "); for (i = 0; i < 32; i++) printf("%02x", ref[i]); printf("\n");
    printf("\nResults (ns per call):\n");

    int ok;
    double passes[PASSES];

    time_api(api_sha256_oneshot, iters, ref, &ok, passes);
    double best_1 = min3_of_pass_12(passes);
    print_row("(1) SHA256() one-shot [deprecated]", passes, ok, best_1);

    time_api(api_sha256_incremental, iters, ref, &ok, passes);
    double best_2 = min3_of_pass_12(passes);
    print_row("(2) SHA256_Init/_Update/_Final [deprecated]", passes, ok, best_2);

    time_api(api_evp_new_free, iters, ref, &ok, passes);
    double best_3 = min3_of_pass_12(passes);
    print_row("(3) EVP_MD_CTX new/free per call", passes, ok, best_3);

    time_api(api_evp_reused, iters, ref, &ok, passes);
    double best_3b = min3_of_pass_12(passes);
    print_row("(3b) EVP_MD_CTX reused (reset)", passes, ok, best_3b);

    time_api(api_evp_reused_oneshot, iters, ref, &ok, passes);
    double best_3c = min3_of_pass_12(passes);
    print_row("(3c) EVP_MD_CTX reused + FLAG_ONESHOT", passes, ok, best_3c);

    double best_4 = -1.0;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    time_api(api_evp_q_digest, iters, ref, &ok, passes);
    best_4 = min3_of_pass_12(passes);
    print_row("(4) EVP_Q_digest one-shot [3.0+]", passes, ok, best_4);
#else
    printf("  (4) EVP_Q_digest one-shot                         n/a (OpenSSL < 3.0)\n");
#endif

    /* relative */
    double fastest = best_1;
    if (best_2  < fastest) fastest = best_2;
    if (best_3  < fastest) fastest = best_3;
    if (best_3b < fastest) fastest = best_3b;
    if (best_3c < fastest) fastest = best_3c;
    if (best_4 > 0 && best_4 < fastest) fastest = best_4;

    printf("\nBest vs fastest (%.1f ns):\n", fastest);
    printf("  (1)  %+6.1f%%\n", 100.0 * (best_1 / fastest - 1.0));
    printf("  (2)  %+6.1f%%\n", 100.0 * (best_2 / fastest - 1.0));
    printf("  (3)  %+6.1f%%\n", 100.0 * (best_3 / fastest - 1.0));
    printf("  (3b) %+6.1f%%\n", 100.0 * (best_3b / fastest - 1.0));
    printf("  (3c) %+6.1f%%\n", 100.0 * (best_3c / fastest - 1.0));
    if (best_4 > 0) printf("  (4)  %+6.1f%%\n", 100.0 * (best_4 / fastest - 1.0));

    EVP_MD_CTX_free(g_reuse_ctx);
    return 0;
}
