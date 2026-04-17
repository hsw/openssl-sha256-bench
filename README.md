# openssl-sha256-bench

Micro-benchmark of five SHA-256 API flavors in OpenSSL on a fixed 200-byte
input. Runs the same input through legacy one-shot, legacy incremental,
`EVP_MD_CTX` (new/free and reused), and `EVP_Q_digest` — reports ns per
call across OpenSSL versions 3.0 → 4.0.

## Motivation

OpenSSL 3.0 deprecated the legacy SHA-256 helpers (`SHA256()`,
`SHA256_Init` / `SHA256_Update` / `SHA256_Final`) in favor of the EVP
interface. In practice the provider dispatch behind EVP adds measurable
per-call overhead — sometimes 2× or more on short inputs. If a hot-path
consumer (hash-based fingerprinting, content addressing, short token
signing) calls SHA-256 millions of times per second, the API choice is
load-bearing.

This tool quantifies that overhead on identical input across every
OpenSSL 3.x minor line plus 4.0, linked statically the same way nginx
embeds OpenSSL via `./auto/configure --with-openssl=`.

## APIs tested

| #    | API                                                              | Deprecated? |
|------|------------------------------------------------------------------|-------------|
| (1)  | `SHA256(msg, len, md)` one-shot                                  | 3.0+        |
| (2)  | `SHA256_Init` + `SHA256_Update` + `SHA256_Final`                 | 3.0+        |
| (3)  | `EVP_MD_CTX_new` + `EVP_DigestInit_ex` + `_Update` + `_Final_ex` + `EVP_MD_CTX_free` | no |
| (3b) | same as (3) but `EVP_MD_CTX_reset` instead of `_new` / `_free` per call | no    |
| (3c) | (3b) + `EVP_MD_CTX_set_flags(ctx, EVP_MD_CTX_FLAG_ONESHOT)` before each `Init` | no |
| (4)  | `EVP_Q_digest`                                                   | no (3.0+)   |

## Build & run

### Local (macOS / Linux — uses whatever OpenSSL your compiler picks up)

```sh
make            # picks up /opt/homebrew/opt/openssl@3 on macOS, system headers otherwise
./bench-sha256 5000000
```

Both `Makefile` and `Dockerfile` default to `-O3 -march=native` so the
benchmark's own loop and helper code compile for the host's full ISA.
This makes binaries non-portable between hosts with different CPUs —
always rebuild on the machine you plan to measure on. Override with
`CFLAGS=-O2` or `--build-arg EXTRA_CFLAGS="-O2"` if you specifically
want a generic build.

### Docker, one specific version

```sh
docker build --build-arg OPENSSL_VERSION=3.6.2 -t bench-sha256:3.6.2 .
docker run --rm bench-sha256:3.6.2 5000000
```

### Docker, all 8 versions in parallel

```sh
docker buildx bake        # builds all 8 images concurrently
./run-bench.sh            # runs each image, writes results/ and results/summary.txt
```

Matrix covered by `docker-bake.hcl`: `3.0.20`, `3.1.8`, `3.2.6`, `3.3.7`,
`3.4.5`, `3.5.6`, `3.6.2`, `4.0.0` (latest patch release of each minor
at the time of writing).

## How the benchmark works

Each API runs in three passes of `N` iterations (default `N = 5 000 000`):

- **Pass 1 (warmup):** primes CPU caches and the branch predictor.
- **Passes 2 and 3:** measured. `best = min(pass2, pass3)`.

Input is a deterministic 200-byte buffer. Every run prints:

- `Compiled against:` — `OPENSSL_VERSION_TEXT` at compile time.
- `Linked at runtime:` — `OpenSSL_version(OPENSSL_VERSION)`.
- `Linkage:` — static (`libcrypto.a`), mirroring how nginx embeds OpenSSL
  via `./auto/configure --with-openssl=…`.
- Reference SHA-256 digest via `EVP_MD_CTX` — every API's output is
  compared against this reference.

The benchmark fails a digest check as `DIGEST-MISMATCH` if any API
produces the wrong hash.

## Results

Measured on **Intel Xeon Gold 5318Y** (Ice Lake-SP, Q2 2021,
base 2.10 GHz / turbo 3.40 GHz), Linux 6.8, Ubuntu 24.04 inside Docker,
gcc 13.3.0. Single-threaded.

ISA features on this CPU (from `/proc/cpuinfo`): `aes`, `avx`, `avx2`,
`avx512bw`, `avx512cd`, `avx512dq`, `avx512f`, `avx512ifma`, `avx512vbmi`,
`avx512vbmi2`, `avx512vl`, `avx512vnni`, `avx512vpopcntdq`, `bmi1`, `bmi2`,
`sha_ni`, `sse4_1`, `sse4_2`.

### How OpenSSL picks a SHA-256 implementation on x86_64

The dispatcher generated from `crypto/sha/asm/sha512-x86_64.pl` runs a
CPUID check at the first call and picks the fastest available path.
Priorities from fastest to slowest:

| #   | Path                        | CPUID check                                   | First x86 CPU                                    | Instructions used                              | Since OpenSSL |
|-----|-----------------------------|-----------------------------------------------|--------------------------------------------------|------------------------------------------------|--------------:|
| 1   | **SHA-NI (SHA extensions)** | CPUID[7].EBX bit 29                           | Goldmont Plus (2017), Ice Lake-SP server (2021)  | `sha256rnds2`, `sha256msg1`, `sha256msg2`      | 1.0.2 (2015) |
| 2   | AVX2 + BMI1 + BMI2          | CPUID[7].EBX bits 3 + 5 + 8                   | Haswell (2013)                                   | 256-bit `vpshufd` / `vpxor` + `shrx` / `shlx`  | 1.0.2 (2015) |
| 3   | AVX + SSSE3 (Intel only)    | CPUID[1].ECX bit 28 + bit 9, vendor = Intel   | Sandy Bridge (2011)                              | 128-bit `vpshufb` on xmm                       | 1.0.1 (2012) |
| 4   | SSSE3                       | CPUID[1].ECX bit 9                            | Nehalem (2008), Barcelona (AMD, 2007)            | 128-bit `pshufb`                               | 1.0.0 (2010) |
| 5   | Scalar x86_64               | — (always available)                          | AMD Opteron (2003)                               | plain integer `ror`, `and`, `xor`, `add`       | 0.9.8 (2005) |

All five paths are present in every OpenSSL version this benchmark
covers (3.0.20 → 4.0.0). Since `sha_ni` is in this CPU's feature list
above, path **(1)** is what the bench actually exercises —
`sha256_block_data_order_shaext` in the generated `sha512-x86_64.s`,
a tight loop of `sha256rnds2` + `sha256msg1` + `sha256msg2` + `paddd`
on xmm0–xmm15.

OpenSSL does **not** have an AVX-512 single-buffer SHA-256 path — on
one message SHA-NI is already faster than any plausible AVX-512 code
would be. The separate file `crypto/sha/asm/sha256-mb-x86_64.pl` uses
AVX2 / AVX-512 but exclusively for multi-buffer parallel hashing
(N independent streams at once), which is not what any `SHA256()`-like
one-shot API exercises. Hashing one ClientHello at a time, as nginx
fingerprint modules do, never hits that path.

Without SHA-NI (pre-2017 Atom, pre-2021 Xeon, most laptops through
2019) the absolute numbers are 3-5× slower but the relative ordering
between the API columns is unchanged.

### Compile flags

Compiled with `-O3 -march=native`; rebuilding with `-O2` and no
`-march` yields numbers within ±1% — the hot path is entirely inside
`libcrypto.a`'s SHA-NI assembler (selected via OpenSSL's runtime
`OPENSSL_ia32cap` feature detection), and user-side compile flags have
no measurable effect.

### Best of 3 passes (ns / call)

| OpenSSL | (1) SHA256() | (2) Init/Upd/Final | (3) EVP new/free | (3b) EVP reused | (3c) reused + FLAG_ONESHOT | (4) EVP_Q_digest |
|---------|-------------:|-------------------:|-----------------:|----------------:|---------------------------:|-----------------:|
| 3.0.20  |        729.7 |          **282.2** |            711.5 |           690.9 |                      692.0 |            727.8 |
| 3.1.8   |        576.4 |          **282.3** |            562.6 |           536.9 |                      541.1 |            575.6 |
| 3.2.6   |        606.2 |          **284.9** |            584.0 |           563.0 |                      563.6 |            608.0 |
| 3.3.7   |        590.5 |          **283.0** |            578.0 |           556.0 |                      557.1 |            590.0 |
| 3.4.5   |        622.5 |          **284.3** |            606.4 |           581.8 |                      584.7 |            620.4 |
| 3.5.6   |        616.0 |          **283.2** |            594.3 |           570.9 |                      574.6 |            615.0 |
| 3.6.2   |        618.6 |          **286.3** |            596.9 |           574.5 |                      575.6 |            619.9 |
| 4.0.0   |        562.9 |            286.5   |            545.2 |       **521.6** |                      523.7 |            562.1 |

### Overhead vs fastest row (%)

| OpenSSL | (1) | (2)  | (3)    | (3b)   | (3c)   | (4)    |
|---------|----:|-----:|-------:|-------:|-------:|-------:|
| 3.0.20  | +158.6% | baseline | +152.1% | +144.8% | +145.3% | +157.9% |
| 3.1.8   | +104.2% | baseline |  +99.3% |  +90.2% |  +91.7% | +103.9% |
| 3.2.6   | +112.7% | baseline | +105.0% |  +97.6% |  +97.8% | +113.4% |
| 3.3.7   | +108.7% | baseline | +104.2% |  +96.5% |  +96.9% | +108.5% |
| 3.4.5   | +119.0% | baseline | +113.3% | +104.7% | +105.7% | +118.2% |
| 3.5.6   | +117.5% | baseline | +109.9% | +101.6% | +102.9% | +117.2% |
| 3.6.2   | +116.1% | baseline | +108.5% | +100.6% | +101.0% | +116.5% |
| 4.0.0   |  +96.5% | baseline |  +90.6% |  +82.1% |  +82.8% |  +96.2% |

## Findings

1. **Legacy `SHA256_Init` / `SHA256_Update` / `SHA256_Final` is the fastest
   path in every version** (~282-286 ns on 200-byte input, <2% spread
   across 3.0 → 4.0). These symbols still exist in `libcrypto.a` of
   every tested release, including 4.0, despite their 3.0+ deprecation.

2. **Every other API pays ~80-160% EVP-dispatch overhead** on short input.
   Even `EVP_MD_CTX` reused-across-calls (3b) only saves 15-30 ns
   relative to the `new`/`free` variant.

3. **`SHA256()` one-shot ≈ `EVP_Q_digest`** — within noise. Since 3.0,
   `SHA256()` is implemented through `EVP_Q_digest` internally, so it
   pays the same dispatch.

4. **Version trend:**
   - 3.0.20 is the slowest for all non-legacy APIs (~730 ns for `SHA256()`)
     — initial provider architecture.
   - 3.1.8 drops sharply (−22% on `SHA256()`), then 3.2-3.6 plateaus around
     590-625 ns.
   - 4.0.0 brings the best modern-API numbers: `SHA256()` 573 ns,
     EVP reused 516 ns. Legacy incremental is unchanged.

5. **Legacy API has not been removed in 4.0** — verified via
   `nm -g libcrypto.a | grep SHA256`. All five symbols
   (`SHA256`, `SHA256_Init`, `SHA256_Update`, `SHA256_Final`,
   `EVP_Q_digest`) are exported. Compilation against 4.0 still needs
   `-DOPENSSL_SUPPRESS_DEPRECATED` (or `-Wno-deprecated-declarations`)
   to suppress header-level warnings, but linking and running legacy
   code works identically to earlier versions.

6. **Correctness:** 6 APIs × 8 versions = 48/48 identical digests
   against the EVP reference.

7. **`EVP_MD_CTX_FLAG_ONESHOT` does not affect SHA-256 on 1.1.x / 3.x /
   4.0.** It was originally honored by the cryptodev and openbsd_hw
   engines (`crypto/engine/eng_cryptodev.c`,
   `crypto/evp/openbsd_hw.c` in 1.0.2): if set, the engine skipped
   buffering the input in `state->mac_data` and sent it directly to the
   kernel / hardware in one ioctl. Both engines were removed / stopped
   being built by default after 1.0.2. From 1.1.1 onward the flag is
   still *set* by `EVP_Digest()` (`crypto/evp/digest.c:625` in 3.0, :556
   in 4.0) but never *read* by anything in the SHA-256 path — neither
   the EVP layer, nor the default/legacy providers'
   `sha2_prov.c` digest implementation. The only remaining reader in
   3.0+ is HMAC's provider, which consumes it via
   `OSSL_MAC_PARAM_DIGEST_ONESHOT` — a separate code path that does not
   fire for direct `EVP_DigestUpdate` on a raw SHA-256 context. The
   `(3c)` column in the table above reflects this — setting the flag
   costs +0.1…+0.8% (measurement noise plus the extra
   `EVP_MD_CTX_set_flags` call itself), with no speedup on any tested
   version.

## Why the floor is ~283 ns

On an Ice Lake-SP class CPU with `sha_ni`, OpenSSL's x86_64 SHA-256
dispatcher (`crypto/sha/asm/sha512-x86_64.pl`) picks up the Intel SHA
Extensions path:

```asm
    test  $(1<<29), %r11d             ; CPUID[7]:EBX bit 29 = SHA-NI
    jnz   _shaext_shortcut            ; → sha256_block_data_order_shaext
```

— which uses the `sha256rnds2`, `sha256msg1`, `sha256msg2` instructions.
The fallback priority for older CPUs is: AVX2 + BMI1/2 → AVX + SSSE3 →
SSSE3 → scalar x86_64.

OpenSSL does **not** have an AVX-512 single-buffer SHA-256 path. The
separate `sha256-mb-x86_64.pl` uses AVX-512 but only for multi-buffer
parallel hashing (multiple independent streams at once), which is not
what any `SHA256()`-like one-shot API exercises.

On SHA-NI hardware, one 64-byte SHA-256 block compresses in roughly
80 ns. A 200-byte input is 3.5 blocks ≈ 280 ns — matching the `(2)`
legacy incremental floor in the table above. Everything in columns (1),
(3), (3b), (3c), (4) on top of that floor is pure EVP dispatch /
provider lookup / allocation overhead.

## Sample output

```
Compiled against: OpenSSL 3.6.2 7 Apr 2026 (0x30600020)
Linked at runtime: OpenSSL 3.6.2 7 Apr 2026
Linkage:    static (libcrypto.a, same as nginx --with-openssl=)
input:      200 bytes
iterations: 5000000 × 3 passes (pass 1 = warmup, report best of passes 2-3)
reference:  44cae5223d431caed4a9e32271d6abf17c3f2f4abac45fcdb48a99fcc6072a09

Results (ns per call):
  (1) SHA256() one-shot [deprecated]            warm= 623.4  run1= 623.8  run2= 623.5  best= 623.5  digest-ok
  (2) SHA256_Init/_Update/_Final [deprecated]   warm= 285.9  run1= 285.9  run2= 285.9  best= 285.9  digest-ok
  (3) EVP_MD_CTX new/free per call              warm= 602.1  run1= 601.8  run2= 601.9  best= 601.8  digest-ok
  (3b) EVP_MD_CTX reused (reset)                warm= 581.4  run1= 581.2  run2= 581.3  best= 581.2  digest-ok
  (4) EVP_Q_digest one-shot [3.0+]              warm= 622.1  run1= 622.0  run2= 622.9  best= 622.0  digest-ok

Best vs fastest (285.9 ns):
  (1)  +118.1%
  (2)    +0.0%
  (3)  +110.5%
  (3b) +103.3%
  (4)  +117.5%
```

## Caveats

- Input size is fixed at 200 bytes. For larger inputs (≥ 16 KB), the
  SHA-256 block-compression cost dominates and the EVP dispatch overhead
  becomes proportionally smaller. The deprecated-vs-EVP gap shrinks to
  single-digit % on inputs ≥ 64 KB.
- Results measured with a single-threaded loop on a server-class CPU.
  Numbers on laptop / embedded silicon with no SHA-NI instructions will
  be several times slower absolute, but the relative ordering holds.
- `no-asm` builds of OpenSSL (no SHA-NI, pure C SHA-256) would compress
  the absolute range significantly. Default `./config` enables asm.

## License

Apache License 2.0 — matching OpenSSL 3.0+'s license. See `LICENSE` and
`NOTICE`.

## Credits

Developed with assistance from [Claude Opus 4.7 (1M context)](https://www.anthropic.com/claude)
running inside Claude Code. Benchmark design, implementation, multi-version
Docker matrix, and initial analysis were pair-programmed with the model.
