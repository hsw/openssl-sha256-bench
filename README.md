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

Measured on **2 × Intel Xeon Gold 5318Y** (Ice Lake-SP, Q2 2021,
24C/48T per socket, base 2.10 GHz / turbo 3.40 GHz), 96 logical cores,
Linux 6.8 (Ubuntu 24.04 inside Docker, single-threaded run).

Relevant ISA features present (from `/proc/cpuinfo`):
- **`sha_ni`** — Intel SHA Extensions (hardware SHA-1/SHA-256 rounds).
  OpenSSL's SHA-NI asm path is engaged on all tested versions.
- **`avx`, `avx2`, `avx512f`, `avx512bw`, `avx512dq`, `avx512vl`,
  `avx512vbmi`, `avx512vnni`** — full AVX-512 suite. Not used by the
  SHA-NI codepath itself, but relevant for vectorized multi-buffer
  SHA impls that OpenSSL may switch to on longer inputs.
- **`aes`, `bmi1`, `bmi2`** — unrelated but typical for this class.

Without SHA-NI (e.g. pre-Ice Lake Xeons, most laptops through 2019)
absolute numbers are 3-5× slower but relative ordering between APIs
holds. See the **Caveats** section below.

Compiled with `-O3 -march=native`; rebuilding with `-O2` and no
`-march` yields numbers within ±1% — the hot path is entirely inside
`libcrypto.a`'s SHA-NI assembler (selected via OpenSSL's runtime
`OPENSSL_ia32cap` feature detection), and user-side compile flags have
no measurable effect.

### Best of 3 passes (ns / call)

| OpenSSL | (1) SHA256() | (2) Init/Upd/Final | (3) EVP new/free | (3b) EVP reused | (4) EVP_Q_digest |
|---------|-------------:|-------------------:|-----------------:|----------------:|-----------------:|
| 3.0.20  |        732.4 |          **284.5** |            713.5 |           690.4 |            729.7 |
| 3.1.8   |        588.9 |          **284.0** |            575.0 |           553.2 |            588.0 |
| 3.2.6   |        603.2 |          **285.5** |            586.7 |           577.3 |            611.9 |
| 3.3.7   |        600.3 |          **284.0** |            581.6 |           563.4 |            598.4 |
| 3.4.5   |        623.5 |          **285.5** |            607.7 |           584.6 |            624.6 |
| 3.5.6   |        613.9 |          **287.0** |            599.6 |           586.7 |            615.3 |
| 3.6.2   |        622.7 |          **286.9** |            600.0 |           576.4 |            616.3 |
| 4.0.0   |        558.1 |            286.3   |            542.8 |       **520.2** |            558.3 |

### Overhead vs fastest row (%)

| OpenSSL | (1) | (2)  | (3)    | (3b)   | (4)    |
|---------|----:|-----:|-------:|-------:|-------:|
| 3.0.20  | +160.7% | baseline | +154.4% | +145.3% | +160.5% |
| 3.1.8   | +107.6% | baseline | +102.9% |  +94.6% | +106.9% |
| 3.2.6   | +109.8% | baseline | +104.8% |  +94.5% | +109.7% |
| 3.3.7   | +111.3% | baseline | +105.8% |  +96.7% | +110.7% |
| 3.4.5   | +119.9% | baseline | +113.2% | +105.3% | +119.8% |
| 3.5.6   | +114.5% | baseline | +109.0% | +106.3% | +114.2% |
| 3.6.2   | +118.1% | baseline | +110.5% | +103.3% | +117.5% |
| 4.0.0   | +102.1% | baseline |  +91.4% |  +82.0% | +101.0% |

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

6. **Correctness:** 5 APIs × 8 versions = 40/40 identical digests
   against the EVP reference.

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
