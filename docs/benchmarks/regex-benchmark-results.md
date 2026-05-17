# Regex Benchmark Results

Generated from current-run JSON: `2026-05-17T04:36:47Z` to `2026-05-17T04:42:48Z` (UTC).

This snapshot covers Full, Incremental, and ANSI Streaming search across 50MB and 500MB generated corpora. Streaming cases were run with `--streaming-render-ansi`, which decorates live input with ANSI SGR sequences and measures a bottom-of-view visible-window display read (`streaming_display_ms`) in addition to append/update and final search catch-up.

## Environment

- Product: `macOS Tahoe (26.4.1)`
- Kernel: `darwin 25.4.0`
- CPU architecture: `x86_64`
- Qt: `6.10.1`
- Iterations: `5` measured + `1` warmup
- Seed: `20260301`
- Streaming ANSI render: `True`
- Streaming visible lines: `120`

## Full Search

### simple

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 63.23 | 790.84 | 3,600,464 | 573.93 | 871.32 | 3,966,855 |
| Vectorscan generic | 20.32 | 2460.51 | 11,202,027 | 169.90 | 2943.39 | 13,400,364 |
| Vectorscan AVX | 17.64 | 2835.45 | 12,909,013 | 165.90 | 3014.26 | 13,723,010 |

### normal

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 67.10 | 745.26 | 3,392,980 | 606.89 | 823.99 | 3,751,391 |
| Vectorscan generic | 33.95 | 1473.02 | 6,706,243 | 208.23 | 2401.60 | 10,933,738 |
| Vectorscan AVX | 31.80 | 1572.66 | 7,159,892 | 215.07 | 2325.16 | 10,585,726 |

### complex

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 121.57 | 411.34 | 1,872,699 | 1046.55 | 477.83 | 2,175,425 |
| Vectorscan generic | 46.79 | 1068.74 | 4,865,679 | 235.67 | 2121.97 | 9,660,675 |
| Vectorscan AVX | 47.45 | 1054.01 | 4,798,604 | 244.48 | 2045.48 | 9,312,445 |

## Incremental Search - 10% Tail

### simple

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 11.43 | 437.33 | 1,991,105 | 67.46 | 741.24 | 3,374,629 |
| Vectorscan generic | 3.15 | 1585.11 | 7,216,850 | 17.51 | 2855.66 | 13,000,989 |
| Vectorscan AVX | 2.74 | 1824.85 | 8,308,376 | 17.01 | 2940.73 | 13,388,245 |

### normal

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 12.79 | 390.88 | 1,779,622 | 68.33 | 731.84 | 3,331,854 |
| Vectorscan generic | 4.69 | 1065.73 | 4,852,170 | 21.49 | 2327.29 | 10,595,442 |
| Vectorscan AVX | 4.15 | 1205.26 | 5,487,415 | 21.68 | 2306.44 | 10,500,525 |

### complex

| Engine | 50MB ms | 50MB MiB/s | 50MB lines/s | 500MB ms | 500MB MiB/s | 500MB lines/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | 27.66 | 180.78 | 823,078 | 104.17 | 480.07 | 2,185,632 |
| Vectorscan generic | 4.50 | 1110.36 | 5,055,348 | 22.43 | 2229.07 | 10,148,289 |
| Vectorscan AVX | 4.43 | 1129.54 | 5,142,674 | 24.63 | 2029.94 | 9,241,728 |

## ANSI Streaming - 50MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Append/update ms | Display ms | Catchup ms | Ops | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 689.29 | 380,309 | 22,878 | 152.07 | 9.23 | 541.38 | 2.00 | 0.00 | 25.00 |
| Qt | normal | 773.55 | 338,884 | 7,864 | 148.18 | 9.56 | 620.30 | 2.00 | 0.00 | 25.00 |
| Qt | complex | 1068.70 | 245,291 | 10,486 | 172.27 | 10.43 | 890.58 | 2.00 | 0.00 | 25.00 |
| Vectorscan generic | simple | 488.12 | 537,052 | 22,878 | 128.97 | 8.71 | 354.71 | 2.00 | 0.00 | 25.00 |
| Vectorscan generic | normal | 535.79 | 489,264 | 7,864 | 144.10 | 10.10 | 391.69 | 2.00 | 0.00 | 25.00 |
| Vectorscan generic | complex | 566.62 | 462,642 | 10,486 | 159.93 | 9.05 | 396.64 | 2.00 | 0.00 | 25.00 |
| Vectorscan AVX | simple | 515.73 | 508,300 | 22,878 | 133.37 | 10.21 | 372.85 | 2.00 | 0.00 | 25.00 |
| Vectorscan AVX | normal | 545.74 | 480,349 | 7,864 | 143.57 | 10.04 | 402.43 | 2.00 | 0.00 | 25.00 |
| Vectorscan AVX | complex | 568.15 | 461,396 | 10,486 | 151.25 | 9.72 | 408.65 | 2.00 | 0.00 | 25.00 |

## ANSI Streaming - 500MB Live Ingest

| Engine | Profile | Median ms | Lines/s | Matches | Append/update ms | Display ms | Catchup ms | Ops | Matchers | Coalesced |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Qt | simple | 6710.22 | 390,663 | 228,780 | 1521.35 | 76.67 | 5085.32 | 2.00 | 8.00 | 199.00 |
| Qt | normal | 7895.99 | 331,996 | 78,643 | 1505.74 | 74.04 | 6363.61 | 2.00 | 8.00 | 199.00 |
| Qt | complex | 10641.96 | 246,330 | 104,858 | 1460.08 | 73.23 | 9184.83 | 2.00 | 8.00 | 199.00 |
| Vectorscan generic | simple | 4991.86 | 525,143 | 228,780 | 1585.53 | 80.00 | 3329.25 | 2.00 | 8.00 | 199.00 |
| Vectorscan generic | normal | 5192.75 | 504,827 | 78,643 | 1579.41 | 77.72 | 3614.63 | 2.00 | 8.00 | 199.00 |
| Vectorscan generic | complex | 5327.53 | 492,055 | 104,858 | 1560.41 | 77.21 | 3679.99 | 2.00 | 8.00 | 199.00 |
| Vectorscan AVX | simple | 4904.07 | 534,544 | 228,780 | 1450.01 | 79.28 | 3433.10 | 2.00 | 8.00 | 199.00 |
| Vectorscan AVX | normal | 5189.37 | 505,155 | 78,643 | 1461.97 | 78.92 | 3691.04 | 2.00 | 8.00 | 199.00 |
| Vectorscan AVX | complex | 5440.63 | 481,826 | 104,858 | 1587.64 | 79.12 | 3900.03 | 2.00 | 8.00 | 199.00 |

## Streaming Bottleneck Readout

The ANSI streaming benchmark splits the total median into append/update, visible-window display reads, and final catch-up. The display read simulates the main view reading the bottom visible window with ANSI color spans enabled; it does not paint pixels, so it isolates data/ANSI parsing cost from QPainter cost.

| Engine | Profile | 500MB total ms | Append/update % | Display % | Catchup % |
| --- | --- | ---: | ---: | ---: | ---: |
| Qt | simple | 6710.22 | 22.7% | 1.1% | 75.8% |
| Qt | normal | 7895.99 | 19.1% | 0.9% | 80.6% |
| Qt | complex | 10641.96 | 13.7% | 0.7% | 86.3% |
| Vectorscan generic | simple | 4991.86 | 31.8% | 1.6% | 66.7% |
| Vectorscan generic | normal | 5192.75 | 30.4% | 1.5% | 69.6% |
| Vectorscan generic | complex | 5327.53 | 29.3% | 1.4% | 69.1% |
| Vectorscan AVX | simple | 4904.07 | 29.6% | 1.6% | 70.0% |
| Vectorscan AVX | normal | 5189.37 | 28.2% | 1.5% | 71.1% |
| Vectorscan AVX | complex | 5440.63 | 29.2% | 1.5% | 71.7% |

## Optimization Direction

1. The previous streaming search optimization is effective for scheduler churn: 500MB ANSI streaming now starts about `2` operations and creates about `8` matchers per case, down from the prior non-ANSI streaming baseline of roughly `192-200` operations and `1,536-1,600` matcher creations on Vectorscan.
2. The reported iOS live-log issue is only partially covered by search coalescing. The new live refresh throttle covers the UI refresh storm, and the ANSI display cache covers repeated visible-line ANSI parsing; the remaining large cost is ANSI normalization in the filter scan path.
3. On 500MB ANSI streaming, visible-window display reads are about `73-80 ms` total across the ingest, while final catch-up is about `3.3-3.9 s` with Vectorscan and `5.1-9.2 s` with Qt. Further work should focus on cached/streaming ANSI-stripped raw blocks for search, not on matcher creation.
4. Vectorscan remains the best engine for ANSI streaming catch-up: around `4.9-5.4 s` total at 500MB versus Qt at `6.7-10.6 s`, but AVX-specific gains are small and profile-dependent.
5. UI responsiveness should be protected by frame-rate throttling first: live append notifications now coalesce before `CrawlerWidget::loadingFinishedHandler()` drives overview/main-view refresh, so input rates above 1,000 lines/s should not force one repaint per input chunk.

## Match Count Verification

- Qt full: simple 50MB: 19,870; normal 50MB: 6,831; complex 50MB: 9,108; simple 500MB: 198,692; normal 500MB: 68,301; complex 500MB: 91,068
- Vectorscan generic full: simple 50MB: 19,870; normal 50MB: 6,831; complex 50MB: 9,108; simple 500MB: 198,692; normal 500MB: 68,301; complex 500MB: 91,068
- Vectorscan AVX full: simple 50MB: 19,870; normal 50MB: 6,831; complex 50MB: 9,108; simple 500MB: 198,692; normal 500MB: 68,301; complex 500MB: 91,068
