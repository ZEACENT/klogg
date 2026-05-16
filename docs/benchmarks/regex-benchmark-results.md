# Regex Benchmark Results

Generated: 2026-05-16 (macOS x86_64, local `build_root`, seed=20260301).

This snapshot covers Qt and Vectorscan-generic engines, Full and Incremental search over 50MB/500MB corpora, and Streaming over the 50MB live-ingest scenario. AVX-specific release builds were not regenerated in this snapshot.

## Environment

- Product: `macOS Tahoe (26.4.1)`
- Kernel: `darwin 25.4.0`
- CPU architecture: `x86_64`
- Qt: `6.10.1`
- Full search: `5` measured iterations + `1` warmup
- Incremental and Streaming: `5` measured iterations + `0` warmup

## Full Search

| Engine | simple 50MB ms | simple 50MB lines/s | simple 500MB ms | simple 500MB lines/s | normal 50MB ms | normal 50MB lines/s | normal 500MB ms | normal 500MB lines/s | complex 50MB ms | complex 50MB lines/s | complex 500MB ms | complex 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 271.4 | 838,873 | 2,441.0 | 932,668 | 142.2 | 1,601,552 | 1,140.4 | 1,996,312 | 726.2 | 313,500 | 7,037.5 | 323,507 |
| Vectorscan generic | 171.8 | 1,325,479 | 1,797.9 | 1,266,308 | 98.3 | 2,316,081 | 841.1 | 2,706,770 | 121.5 | 1,873,824 | 858.3 | 2,652,405 |

## Incremental Search - 10% Tail

| Engine | simple 50MB ms | simple 50MB lines/s | simple 500MB ms | simple 500MB lines/s | normal 50MB ms | normal 50MB lines/s | normal 500MB ms | normal 500MB lines/s | complex 50MB ms | complex 50MB lines/s | complex 500MB ms | complex 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 25.6 | 887,658 | 246.6 | 923,183 | 11.5 | 1,979,034 | 115.2 | 1,975,601 | 71.0 | 320,521 | 683.9 | 332,918 |
| Vectorscan generic | 14.6 | 1,560,040 | 143.8 | 1,583,229 | 8.7 | 2,615,836 | 87.9 | 2,590,197 | 8.4 | 2,698,676 | 89.9 | 2,533,277 |

## Streaming - 50MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Append/update ms | Catch-up ms | Operations | Matchers | Coalesced updates |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 784.1 | 334,318 | 22,878 | 161.5 | 622.2 | 2 | 16 | 26 |
| Qt | normal | 803.1 | 326,397 | 7,864 | 153.1 | 666.4 | 2 | 16 | 26 |
| Qt | complex | 1,205.4 | 217,470 | 10,486 | 146.2 | 1,065.0 | 2 | 16 | 26 |
| Vectorscan generic | simple | 637.3 | 411,356 | 22,878 | 286.2 | 355.0 | 2 | 16 | 26 |
| Vectorscan generic | normal | 274.8 | 954,001 | 7,864 | 188.2 | 87.7 | 2 | 16 | 26 |
| Vectorscan generic | complex | 314.6 | 833,348 | 10,486 | 180.7 | 135.4 | 2 | 16 | 26 |

## Streaming Gap vs Same-Build Full Search - 50MB

Streaming uses the live-ingest generator and has `262,144` searched lines, while Full 50MB has `227,671` lines. The comparison below uses line throughput plus stage timings.

| Engine | Profile | Full lines/s | Streaming lines/s | Streaming / Full | Throughput loss | Streaming ms | Append/update ms | Catch-up ms | Append share | Catch-up share |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 838,873 | 334,318 | 0.40x | 60.1% | 784.1 | 161.5 | 622.2 | 20.6% | 79.4% |
| Qt | normal | 1,601,552 | 326,397 | 0.20x | 79.6% | 803.1 | 153.1 | 666.4 | 19.1% | 83.0% |
| Qt | complex | 313,500 | 217,470 | 0.69x | 30.6% | 1,205.4 | 146.2 | 1,065.0 | 12.1% | 88.4% |
| Vectorscan generic | simple | 1,325,479 | 411,356 | 0.31x | 69.0% | 637.3 | 286.2 | 355.0 | 44.9% | 55.7% |
| Vectorscan generic | normal | 2,316,081 | 954,001 | 0.41x | 58.8% | 274.8 | 188.2 | 87.7 | 68.5% | 31.9% |
| Vectorscan generic | complex | 1,873,824 | 833,348 | 0.44x | 55.5% | 314.6 | 180.7 | 135.4 | 57.4% | 43.0% |

## Vectorscan Streaming vs Previous Current-Run Snapshot

| Profile | Previous lines/s | Current lines/s | Change | Previous ratio vs prior Full | Current ratio vs current Full |
| --- | ---: | ---: | ---: | ---: | ---: |
| simple | 997,912 | 411,356 | -58.8% | - | 0.31x |
| normal | 1,098,260 | 954,001 | -13.1% | - | 0.41x |
| complex | 1,026,139 | 833,348 | -18.8% | - | 0.44x |

## Match Count Verification

- simple: full 50MB Qt/Vectorscan `19870`/`19870`; streaming 50MB Qt/Vectorscan `22878`/`22878`
- normal: full 50MB Qt/Vectorscan `6831`/`6831`; streaming 50MB Qt/Vectorscan `7864`/`7864`
- complex: full 50MB Qt/Vectorscan `9108`/`9108`; streaming 50MB Qt/Vectorscan `10486`/`10486`

## Key Findings

1. Vectorscan Streaming did not reach the requested `0.80x+` of same-build Full throughput in this run. Current ratios are `0.31x` simple, `0.41x` normal, and `0.44x` complex.
2. The dominant remaining costs are append/update and final catch-up scheduling/search. For Vectorscan, append/update accounts for `44.9%-68.2%` of total Streaming time; catch-up accounts for `31.9%-55.7%`.
3. The correctness fix for live coalescing is important: rapid append updates now search every newly appended batch instead of advancing by a fixed chunk size after a smaller live target.
4. The next optimization should reduce live-search work issued during append bursts, or provide a true append-batch search path that scans completed append batches without rebuilding/copying chunk `RawLines` repeatedly.

