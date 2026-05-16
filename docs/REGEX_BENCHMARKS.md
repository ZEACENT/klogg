# Regex Search Benchmarks

This document defines the regular-expression benchmark used for klogg's end-to-end search path.
It covers corpus generation, engine variants, runtime environment expectations, and the generated result artifacts committed under [docs/benchmarks](./benchmarks/).

## Scope

The benchmark measures the real search path used by klogg:

- corpus generation on a memory-backed filesystem
- indexing through `LogData`
- regex search through `LogFilteredData`

Generation time and indexing time are reported separately. The primary metric is search latency and throughput.

## Matrix

### Regex profiles

Three regex complexity buckets are benchmarked:

| Profile | Pattern | Notes |
| --- | --- | --- |
| `simple` | `ERROR` | Single-token severity match |
| `normal` | `level=(ERROR\|WARN).*component=(auth\|scheduler\|replication\|storage\|parser\|crawler).*msg=\"(timeout\|failed\|exception).*\".*path=/api/v1/(task\|job)/[0-9]{1,4}.*shard=[0-9]{1,2} retry=false` | Structured warning or error match with field alternation and bounded numeric suffixes |
| `complex` | `level=(INFO\|WARN) component=(auth\|scheduler\|replication\|storage\|parser\|crawler).*req=[A-F0-9]{16}( session=([A-F0-9]{8}\|[A-F0-9]{16}))?.*msg=\"(peer (reset\|disconnect(ed)?) during remote link handover\|(timeout\|failed\|exception) while processing customer ledger batch)\".*path=/api/v1/(node\|task)/[0-9]{1,4}.*tenant=prod region=us-east-1 shard=[0-9]{1,2} (window=batch-e2e\|retry=false)` | Structured log-line match with nested alternation, optional groups, and bounded repetitions |

### Corpus sizes

Three corpus buckets are generated:

- `50MB`
- `500MB`
- `5GB`

These labels map to binary-sized buckets in the benchmark tool:

- `50MB = 50 * 1024 * 1024`
- `500MB = 500 * 1024 * 1024`
- `5GB = 5 * 1024 * 1024 * 1024`

### Engine variants

The benchmark compares three engine/build variants:

| Label | Build configuration | Runtime engine |
| --- | --- | --- |
| `qt` | `-DKLOGG_USE_VECTORSCAN=OFF` | `QRegularExpression` |
| `vectorscan-generic` | `-DKLOGG_USE_VECTORSCAN=ON -DKLOGG_GENERIC_CPU=ON` | Vectorscan without AVX2/AVX512 codegen |
| `vectorscan-avx` | `-DKLOGG_USE_VECTORSCAN=ON -DKLOGG_GENERIC_CPU=OFF` | Vectorscan with the host's available AVX-enabled build |

## Runtime requirements

The corpus must live on a memory-backed filesystem to keep filesystem IO from dominating the comparison.

Recommended defaults:

- Linux: `/dev/shm/klogg-bench`
- macOS: `/Volumes/RAMDisk/klogg-bench`
- Windows: the benchmark falls back to the system temp directory by default, but that does not provide the same tmpfs guarantee. Use a RAM disk if you need apples-to-apples results.

If the target filesystem does not have enough space for a requested bucket, the benchmark marks that case as `skipped` and records the reason.

## How to run

### Full matrix

```bash
./scripts/run_regex_benchmarks.sh --tmpfs-dir /dev/shm/klogg-bench
```

### Smaller smoke run

```bash
./scripts/run_regex_benchmarks.sh \
  --tmpfs-dir /dev/shm/klogg-bench \
  --sizes 50MB \
  --profiles simple \
  --iterations 1 \
  --warmup 0
```

### Single-engine direct invocation

```bash
./build-bench-qt/output/regex_search_benchmark \
  --engine qt \
  --label qt \
  --tmpfs-dir /dev/shm/klogg-bench \
  --sizes 50MB,500MB,5GB \
  --profiles simple,normal,complex \
  --iterations 5 \
  --warmup 1 \
  --output docs/benchmarks/qt.json
```

## Generated artifacts

The runner script writes:

- [docs/benchmarks/regex-benchmark-results.md](./benchmarks/regex-benchmark-results.md)
- [docs/benchmarks/regex-benchmark-results.json](./benchmarks/regex-benchmark-results.json)

The Markdown file contains the summary tables used by README. The JSON file contains the raw per-engine outputs and aggregated summary.
Until the runner is executed on a benchmark host, those files may contain placeholders instead of a populated matrix.

The generated summaries now include fairness-oriented counters for every case:

- searched line count
- match count
- hit rate
- byte throughput (`throughput_mib_per_s`)
- line throughput (`throughput_lines_per_s`)

Those counters make it easier to verify that each regex tier is being compared on the same corpus and to spot cases where a pattern is unrealistically sparse or dense.

Streaming cases also include stage and coalescing counters:

- append/update phase time (`streaming_append_update_ms`)
- final catch-up phase time (`streaming_catchup_ms`)
- search operation starts (`operation_starts`)
- matcher creations (`matcher_creations`)
- requested updates and coalesced live updates (`update_requests`, `coalesced_live_updates`)

These fields separate live ingestion/indexing/scheduling overhead from final search catch-up and make it possible to verify that many append-triggered `updateSearch()` calls collapse into a small number of live search operations.

## Benchmark notes

- The corpus generator is deterministic and keyed by `--seed`.
- The benchmark warms each case before recording measured iterations.
- Search-result caching and file watching are disabled during the benchmark.
- The regex set intentionally avoids PCRE-only features such as lookahead and lookbehind so that Qt and Vectorscan can be compared on the same workload.

## Historical SIMD snapshot

The older README SIMD section is retained here because it captures the earlier vectorscan-only build comparison.

### SIMD benchmark snapshot (2026-02-14)

The regex backend can be built with different vectorscan SIMD settings:

- `AVX2=OFF, AVX512=OFF`
- `AVX2=ON, AVX512=OFF`
- `AVX2=ON, AVX512=ON`

To quantify the impact, we ran `hsbench` with vectorscan `5.4.11` on the same host and corpus (`~245 MB`, `1,200,000` blocks), using block mode (`-N`) and repeated scans.

Environment:

- `macOS 26.2 x86_64`
- `AppleClang 17`
- vectorscan features reported by `hsbench`: `AVX2` or `AVX512VBMI` depending on build flags

Results (mean throughput, Mbit/s):

| Config | Log-mixed workload (n=5) | Relative vs. `OFF/OFF` | Regex-heavy workload (n=5) | Relative vs. `OFF/OFF` |
| --- | ---: | ---: | ---: | ---: |
| `AVX2=OFF, AVX512=OFF` | 326.80 | baseline | 324.66 | baseline |
| `AVX2=ON, AVX512=OFF` | 326.46 | -0.10% | 319.12 | -1.71% |
| `AVX2=ON, AVX512=ON` | 324.12 | -0.82% | 344.42 | +6.09% |

Takeaway:

- The impact is workload-dependent.
- For log-mixed scans, differences were within about `1%`.
- For regex-heavy scans, enabling AVX512 improved throughput by about `6-8%` compared with lower-SIMD builds.
- Disabling AVX512 for Windows/MSVC CI stability is expected to trade off up to about `8%` in regex-heavy engine-level benchmarks, while typical end-to-end UI workflows may show smaller differences.
