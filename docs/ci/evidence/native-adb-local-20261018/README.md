# Local Intel ADB object-cache parity

These are local inspection measurements, **not hosted qualification**, ARM-native
acceptance, or proof that the complete CI workflow meets its latency target.

The source artifacts came from master run 39091756819 and were verified offline
against the unchanged ADB lock. The experiment used Xcode 26.6 build 17F113,
Apple clang 21.0.0 (`clang-2100.1.1.101`), CMake 3.31.6, Ninja 1.12.1 and eight
build workers. `--allow-unlocked-local` explicitly permitted local inspection;
no hosted runner identity or release qualification was fabricated.

A new object directory established the cold sample. Every sample removed the
native build tree, reran the original source build/link and builder smoke,
created a fresh legal/package overlay, and ran the separate package smoke and
artifact verifier. The same object directory was retained for three warm samples.

| Sample | Source build | Build plus package verification | Cacheable calls hit |
| --- | ---: | ---: | ---: |
| Cold | 340.686 s | 342.233 s | 50 / 752 |
| Warm 1 | 50.960 s | 52.374 s | 707 / 752 |
| Warm 2 | 44.884 s | 46.281 s | 707 / 752 |
| Warm 3 | 43.102 s | 44.756 s | 707 / 752 |

Warm build median is **44.884 s**, worst **50.960 s**, with **94.02%** cacheable
hits. A cold build can have intra-build hits; cold does not mean every invocation
is unique. The payload grew from 43,691,853 to 43,877,310 bytes, below the 128 MB
cap, with no cleanup or compiler-check failures. Two compiler failures per sample
are configure feature probes, not failed build or smoke exits. Existing vendored
compiler warnings remained visible.

`summary.json` contains the original counters/timings. `toolchain.log` contains
actual version output. Each `verified-artifact-N` directory retains the original
build, smoke, package smoke and package verification receipts. Their
`release_qualified: false` / local classification is intentionally unchanged.
The manifest includes the original payload and complete local log hashes.

Fresh relinking changes Mach-O UUIDs and object timestamps, so raw helper hashes
differ. No binary or receipt was modified to force equality, and reproducible
whole-binary output is not claimed. Binary payloads and full compiler logs remain
outside the repository at `/private/tmp/klogg-native-cache-parity.r90J0o`.
