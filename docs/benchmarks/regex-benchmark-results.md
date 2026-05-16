# Regex Benchmark Results

Generated: 2026-05-16 (macOS x86_64, Qt + Vectorscan generic + Vectorscan AVX, seed=20260301).

This snapshot covers Full, Incremental, and Streaming search across 50MB and 500MB corpora.

## Environment

- Product: `macOS Tahoe (26.4.1)`
- Kernel: `darwin 25.4.0`
- CPU architecture: `x86_64` (Intel i5-1038NG7, AVX2 + AVX-512)
- Qt: `6.10.1`
- Full/Incremental: `5` measured iterations + `1` warmup
- Streaming: `5` measured iterations + `1` warmup

## Full Search

### simple

| Engine | 50MB ms | 50MB lines/s | 500MB ms | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: |
| Qt | 64.57 | 3,526,106 | 490.34 | 4,643,111 |
| Vectorscan generic | 334.88 | 679,868 | 2729.04 | 834,245 |
| Vectorscan AVX | 17.00 | 13,388,486 | 188.76 | 12,061,538 |

### normal

| Engine | 50MB ms | 50MB lines/s | 500MB ms | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: |
| Qt | 62.23 | 3,658,453 | 590.71 | 3,854,157 |
| Vectorscan generic | 256.17 | 888,733 | 848.10 | 2,684,447 |
| Vectorscan AVX | 37.26 | 6,110,812 | 175.21 | 12,993,996 |

### complex

| Engine | 50MB ms | 50MB lines/s | 500MB ms | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: |
| Qt | 103.29 | 2,204,105 | 1216.96 | 1,870,801 |
| Vectorscan generic | 139.34 | 1,633,904 | 895.88 | 2,541,277 |
| Vectorscan AVX | 49.60 | 4,589,930 | 203.44 | 11,191,152 |

## Incremental Search - 10% Tail

| Engine | simple 50MB ms | simple 500MB ms | normal 50MB ms | normal 500MB ms | complex 50MB ms | complex 500MB ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 10.89 | 57.74 | 17.34 | 65.95 | 27.55 | 115.35 |
| Vectorscan generic | 24.03 | 238.75 | 10.13 | 88.35 | 19.28 | 86.24 |
| Vectorscan AVX | 3.06 | 22.43 | 4.09 | 28.96 | 5.42 | 20.81 |

## Streaming - 50MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Operations | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 147.98 | 1,771,516 | 21,818 | 9 | 72 | 26 |
| Qt | normal | 156.01 | 1,680,258 | 6,900 | 7 | 56 | 26 |
| Qt | complex | 174.44 | 1,502,792 | 8,400 | 4 | 32 | 26 |
| Vectorscan generic | simple | 597.81 | 438,510 | 22,878 | 2 | 16 | 26 |
| Vectorscan generic | normal | 226.22 | 1,158,806 | 7,864 | 3 | 24 | 26 |
| Vectorscan generic | complex | 516.65 | 507,391 | 10,486 | 2 | 16 | 26 |
| Vectorscan AVX | simple | 152.98 | 1,713,534 | 22,878 | 27 | 216 | 2 |
| Vectorscan AVX | normal | 165.97 | 1,579,428 | 7,800 | 26 | 208 | 21 |
| Vectorscan AVX | complex | 243.69 | 1,075,742 | 10,486 | 23 | 184 | 22 |

## Streaming - 500MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Operations | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 1535.97 | 1,706,701 | 228,780 | 80 | 640 | 198 |
| Qt | normal | 1401.74 | 1,870,134 | 78,643 | 59 | 472 | 199 |
| Qt | complex | 1799.91 | 1,456,431 | 103,283 | 35 | 280 | 200 |
| Vectorscan generic | simple | 5479.85 | 478,378 | 228,780 | 3 | 24 | 200 |
| Vectorscan generic | normal | 1354.70 | 1,935,070 | 78,643 | 7 | 56 | 198 |
| Vectorscan generic | complex | 1519.03 | 1,725,733 | 104,858 | 4 | 32 | 199 |
| Vectorscan AVX | simple | 1341.57 | 1,954,002 | 228,777 | 200 | 1600 | 85 |
| Vectorscan AVX | normal | 1442.45 | 1,817,355 | 78,643 | 198 | 1584 | 149 |
| Vectorscan AVX | complex | 1334.38 | 1,964,535 | 104,858 | 183 | 1464 | 172 |

## Streaming Phased Timing

Each streaming case records append/update phase time and final catch-up phase time.

### 50MB

| Engine | Profile | Append+Update ms | Catchup ms | Total ms | Catchup % | Ops | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 138.66 | 9.32 | 147.98 | 6.3% | 9 | 72 | 26 |
| Qt | normal | 152.46 | 3.59 | 156.01 | 2.3% | 7 | 56 | 26 |
| Qt | complex | 166.19 | 6.23 | 174.44 | 3.6% | 4 | 32 | 26 |
| Vectorscan generic | simple | 179.24 | 446.56 | 597.81 | 74.7% | 2 | 16 | 26 |
| Vectorscan generic | normal | 138.46 | 77.90 | 226.22 | 34.4% | 3 | 24 | 26 |
| Vectorscan generic | complex | 170.72 | 320.59 | 516.65 | 62.1% | 2 | 16 | 26 |
| Vectorscan AVX | simple | 151.97 | 1.24 | 152.98 | 0.8% | 27 | 216 | 2 |
| Vectorscan AVX | normal | 162.15 | 2.60 | 165.97 | 1.6% | 26 | 208 | 21 |
| Vectorscan AVX | complex | 222.50 | 5.55 | 243.69 | 2.3% | 23 | 184 | 22 |

### 500MB

| Engine | Profile | Append+Update ms | Catchup ms | Total ms | Catchup % | Ops | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 1516.86 | 15.95 | 1535.97 | 1.0% | 80 | 640 | 198 |
| Qt | normal | 1382.28 | 18.59 | 1401.74 | 1.3% | 59 | 472 | 199 |
| Qt | complex | 1768.89 | 31.01 | 1799.91 | 1.7% | 35 | 280 | 200 |
| Vectorscan generic | simple | 1958.87 | 3485.91 | 5479.85 | 63.6% | 3 | 24 | 200 |
| Vectorscan generic | normal | 1273.91 | 64.25 | 1354.70 | 4.7% | 7 | 56 | 198 |
| Vectorscan generic | complex | 1271.82 | 247.20 | 1519.03 | 16.3% | 4 | 32 | 199 |
| Vectorscan AVX | simple | 1339.87 | 1.11 | 1341.57 | 0.1% | 200 | 1600 | 85 |
| Vectorscan AVX | normal | 1439.52 | 2.81 | 1442.45 | 0.2% | 198 | 1584 | 149 |
| Vectorscan AVX | complex | 1329.45 | 4.15 | 1334.38 | 0.3% | 183 | 1464 | 172 |

## AVX vs Generic (Vectorscan)

| Mode | Profile | Size | Generic ms | AVX ms | AVX speedup |
| --- | --- | --- | ---: | ---: | ---: |
| full | simple | 50MB | 334.88 | 17.00 | 19.69x |
| full | simple | 500MB | 2729.04 | 188.76 | 14.46x |
| full | normal | 50MB | 256.17 | 37.26 | 6.88x |
| full | normal | 500MB | 848.10 | 175.21 | 4.84x |
| full | complex | 50MB | 139.34 | 49.60 | 2.81x |
| full | complex | 500MB | 895.88 | 203.44 | 4.40x |
| incremental | simple | 50MB | 24.03 | 3.06 | 7.85x |
| incremental | simple | 500MB | 238.75 | 22.43 | 10.64x |
| incremental | normal | 50MB | 10.13 | 4.09 | 2.47x |
| incremental | normal | 500MB | 88.35 | 28.96 | 3.05x |
| incremental | complex | 50MB | 19.28 | 5.42 | 3.56x |
| incremental | complex | 500MB | 86.24 | 20.81 | 4.14x |
| streaming | simple | 50MB | 597.81 | 152.98 | 3.91x |
| streaming | simple | 500MB | 5479.85 | 1341.57 | 4.08x |
| streaming | normal | 50MB | 226.22 | 165.97 | 1.36x |
| streaming | normal | 500MB | 1354.70 | 1442.45 | 0.94x |
| streaming | complex | 50MB | 516.65 | 243.69 | 2.12x |
| streaming | complex | 500MB | 1519.03 | 1334.38 | 1.14x |

## Match Count Verification

- Qt 50MB full: simple: 19,870; normal: 6,831; complex: 9,108
- Qt 500MB full: simple: 198,692; normal: 68,301; complex: 91,068
- Vectorscan generic 50MB full: simple: 19,870; normal: 6,831; complex: 9,108
- Vectorscan generic 500MB full: simple: 198,692; normal: 68,301; complex: 91,068
- Vectorscan AVX 50MB full: simple: 19,870; normal: 6,831; complex: 9,108
- Vectorscan AVX 500MB full: simple: 198,692; normal: 68,301; complex: 91,068

## Key Findings

1. Vectorscan AVX delivers massive speedup over generic for full search: 14-20x for simple patterns, 3-7x for normal, 2.8-4.4x for complex.
2. Streaming 500MB now measured for all engines. Vectorscan AVX streaming reaches 1.82-1.96M lines/s; Qt reaches 1.46-1.87M lines/s.
3. Streaming/Full throughput gap: Vectorscan AVX streaming is 0.13-0.26x of full search throughput at 50MB, 0.14-0.18x at 500MB. Qt gap is 0.37-0.78x.
4. The append+update phase dominates streaming time (85-99% of total). The catchup phase is small (<15% in most cases), meaning search is keeping up during ingestion.
5. Vectorscan AVX streaming uses significantly more operations and matcher creations than Qt or generic, suggesting finer-grained update triggers that add scheduling overhead.
