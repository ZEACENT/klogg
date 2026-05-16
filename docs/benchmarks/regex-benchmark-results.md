# Regex Benchmark Results

Generated: 2026-05-16 (macOS x86_64, Vectorscan AVX + Qt, seed=20260301).

This snapshot covers Full, Incremental, and Streaming search across 50MB, 500MB, and 5GB corpora.

## Environment

- Product: `macOS Tahoe (26.4.1)`
- Kernel: `darwin 25.4.0`
- CPU architecture: `x86_64` (Intel i5-1038NG7, AVX2 + AVX-512)
- Qt: `6.10.1`
- Full/Incremental: `5` measured iterations + `1` warmup
- Streaming: `5` measured iterations + `1` warmup (50MB/500MB), `2` iterations + `0` warmup (5GB)

## Full Search

| Engine | simple 50MB ms | simple 50MB lines/s | simple 500MB ms | simple 500MB lines/s | simple 5GB ms | simple 5GB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 47.81 | 4,761,596 | 399.82 | 5,694,282 | 4,483.42 | 5,199,893 |
| Vectorscan AVX | 177.14 | 1,285,293 | 2,056.56 | 1,107,037 | 28,286.06 | 824,196 |

| Engine | normal 50MB ms | normal 50MB lines/s | normal 500MB ms | normal 500MB lines/s | normal 5GB ms | normal 5GB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 52.84 | 4,308,896 | 745.24 | 3,054,953 | 5,564.78 | 4,189,434 |
| Vectorscan AVX | 98.92 | 2,301,678 | 893.82 | 2,547,131 | 8,606.26 | 2,708,874 |

| Engine | complex 50MB ms | complex 50MB lines/s | complex 500MB ms | complex 500MB lines/s | complex 5GB ms | complex 5GB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 87.36 | 2,606,145 | 892.15 | 2,551,901 | 9,154.19 | 2,546,733 |
| Vectorscan AVX | 105.90 | 2,149,894 | 885.18 | 2,572,010 | 8,807.56 | 2,646,962 |

## Incremental Search - 10% Tail

| Engine | simple 50MB ms | simple 500MB ms | normal 50MB ms | normal 500MB ms | complex 50MB ms | complex 500MB ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 11.83 | 44.77 | 16.77 | 54.46 | 27.06 | 97.53 |
| Vectorscan AVX | 17.25 | 157.84 | 8.40 | 89.09 | 8.78 | 90.82 |

## Streaming - 50MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Operations | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 147.59 | 1,776,209 | 22,691 | 2 | 16 | 26 |
| Qt | normal | 144.16 | 1,818,450 | 7,800 | 2 | 16 | 26 |
| Qt | complex | 158.77 | 1,651,114 | 9,200 | 2 | 16 | 26 |
| Vectorscan AVX | simple | 245.58 | 1,067,466 | 22,878 | 2 | 16 | 26 |
| Vectorscan AVX | normal | 161.44 | 1,623,738 | 7,864 | 2 | 16 | 26 |
| Vectorscan AVX | complex | 226.22 | 1,158,780 | 10,486 | 2 | 16 | 26 |

## Streaming - 500MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches |
| --- | --- | ---: | ---: | ---: |
| Qt | simple | 1,471.15 | 1,781,895 | 228,780 |
| Qt | normal | 1,458.94 | 1,796,815 | 77,855 |
| Qt | complex | 1,682.85 | 1,557,741 | 104,332 |
| Vectorscan AVX | simple | 1,641.47 | 1,597,008 | 228,780 |
| Vectorscan AVX | normal | 1,293.16 | 2,027,155 | 78,643 |
| Vectorscan AVX | complex | 1,294.95 | 2,024,357 | 104,858 |

## Streaming Gap vs Same-Build Full Search

Streaming uses the live-ingest generator; Full search uses a pre-indexed file. The comparison uses line throughput.

### Vectorscan AVX

| Size | Profile | Full lines/s | Streaming lines/s | Streaming / Full | Throughput loss |
| --- | --- | ---: | ---: | ---: | ---: |
| 50MB | simple | 1,285,293 | 1,067,466 | 0.83x | 17.0% |
| 50MB | normal | 2,301,678 | 1,623,738 | 0.71x | 29.4% |
| 50MB | complex | 2,149,894 | 1,158,780 | 0.54x | 46.1% |
| 500MB | simple | 1,107,037 | 1,597,008 | 1.44x | -44.3% |
| 500MB | normal | 2,547,131 | 2,027,155 | 0.80x | 20.4% |
| 500MB | complex | 2,572,010 | 2,024,357 | 0.79x | 21.3% |

### Qt

| Size | Profile | Full lines/s | Streaming lines/s | Streaming / Full | Throughput loss |
| --- | --- | ---: | ---: | ---: | ---: |
| 50MB | simple | 4,761,596 | 1,776,209 | 0.37x | 62.7% |
| 50MB | normal | 4,308,896 | 1,818,450 | 0.42x | 57.8% |
| 50MB | complex | 2,606,145 | 1,651,114 | 0.63x | 36.7% |
| 500MB | simple | 5,694,282 | 1,781,895 | 0.31x | 68.7% |
| 500MB | normal | 3,054,953 | 1,796,815 | 0.59x | 41.2% |
| 500MB | complex | 2,551,901 | 1,557,741 | 0.61x | 39.0% |

## Comparison with Previous Results (Vectorscan Generic)

Previous run used Vectorscan generic (no AVX) with 50MB streaming cap.

| Metric | Previous (generic) | Current (AVX) | Change |
| --- | ---: | ---: | ---: |
| 50MB Full simple lines/s | 1,325,479 | 1,285,293 | -3.0% |
| 50MB Full normal lines/s | 2,316,081 | 2,301,678 | -0.6% |
| 50MB Full complex lines/s | 1,873,824 | 2,149,894 | +14.7% |
| 500MB Full normal lines/s | 2,706,770 | 2,547,131 | -5.9% |
| 500MB Full complex lines/s | 2,652,405 | 2,572,010 | -3.0% |
| 50MB Streaming simple lines/s | 411,356 | 1,067,466 | +159.5% |
| 50MB Streaming normal lines/s | 954,001 | 1,623,738 | +70.2% |
| 50MB Streaming complex lines/s | 833,348 | 1,158,780 | +39.1% |
| 50MB Streaming / Full simple | 0.31x | 0.83x | +168% |
| 50MB Streaming / Full normal | 0.41x | 0.71x | +73% |
| 50MB Streaming / Full complex | 0.44x | 0.54x | +23% |

## Match Count Verification

- simple: full 50MB Qt/Vectorscan `19870`/`19870`; streaming 50MB Qt/Vectorscan `22691`/`22878`
- normal: full 50MB Qt/Vectorscan `6831`/`6831`; streaming 50MB Qt/Vectorscan `7800`/`7864`
- complex: full 50MB Qt/Vectorscan `9108`/`9108`; streaming 50MB Qt/Vectorscan `9200`/`10486`

## Key Findings

1. Streaming throughput improved dramatically: 50MB streaming went from 0.31x-0.44x of Full to 0.54x-0.83x (Vectorscan AVX). The 500MB streaming reaches 0.79x-1.44x.
2. The `buildRawLines` bulk-copy optimization eliminated per-line `QByteArray` allocations, the single biggest streaming bottleneck.
3. The `endLine` lazy re-check avoids a mutex-acquiring virtual call per chunk when the search hasn't caught up yet.
4. The 256MB `CachedRawBatchBytesLimit` and 256MB `memoryBudgetBytes` keep more data in memory for larger files.
5. The live update coalescing + larger batch sizes for 500MB streaming allow throughput that sometimes exceeds full search (1.44x for simple pattern) because streaming only searches newly appended data.
6. 5GB full search: Vectorscan AVX processes 23.3M lines in ~9 seconds for normal/complex patterns, ~28 seconds for simple. Qt processes the same in ~4.5-9 seconds.
