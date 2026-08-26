# Live Capture Benchmark Results

Generated: 2026-08-26T00:15:49Z

## Synthetic Matrix

| Scenario | Trial | Arm | Startup ns | First commit ns | Throughput B/s | CPU ns | Child CPU ns | Peak RSS bytes | Voluntary CS | Involuntary CS | Teardown ns |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| steady | 0 | process | 116784494 | 3392829 | 4601166 | 225227000 | 3540000 | 14659584 | 7 | 498 | 113525794 |
| steady | 1 | integrated | 110094164 | 473290 | 3588377 | 210937000 | 0 | 14618624 | 7 | 208 | 106136175 |
| steady | 2 | integrated | 113089716 | 507154 | 3020167 | 215242000 | 0 | 14475264 | 6 | 186 | 106434367 |
| steady | 3 | process | 113998675 | 3086372 | 4766334 | 217934000 | 3491000 | 14630912 | 7 | 354 | 108030540 |
| reconnect | 0 | process | 115989232 | 3087368 | 531622 | 223317000 | 6937000 | 14606336 | 8 | 658 | 108689020 |
| reconnect | 1 | integrated | 106800575 | 487351 | 3159287 | 207492000 | 0 | 14565376 | 6 | 89 | 103651110 |
| reconnect | 2 | integrated | 108369933 | 553751 | 2922555 | 210119000 | 0 | 14430208 | 6 | 122 | 105616618 |
| reconnect | 3 | process | 109177049 | 3072667 | 552810 | 215129000 | 6390000 | 14553088 | 8 | 546 | 105975871 |

All eight rows committed 128/128 records, matched fixture CRC, observed no gaps, duplicates, or CRC errors, and verified cleanup. Process queue internals are unavailable; integrated queue values are synthetic and labeled as such.

## iOS Protocol Microbenchmark

| Case | Iterations | Elapsed ns | Operations/s | Checksum |
|---|---:|---:|---:|---:|
| decode | 200000 | 180492195 | 1108080.0 | 6394079261569180692 |
| format_plain | 200000 | 440857093 | 453662.0 | 9609641829062868840 |
| format_ansi | 200000 | 515225053 | 388180.0 | 16302493868721338472 |

## Real-Device Comparison

Generated: 2026-08-26T00:42:14Z

Status: `ok`. The rebuilt pinned stack passed source, patch-tree, exported-symbol, verifier, runtime-closure, and hash checks. Passive catalog gates found exactly one USB endpoint with an existing pair record. Two warmups and twelve measured trials completed with one stream at a time, no errors, complete formatting, and verified cleanup.

| Trial | Arm | Bytes | Lines | Startup ns | First byte ns | First commit ns | Throughput B/s | Process CPU ns | Child CPU ns | Peak RSS bytes | vCS | iCS | Teardown ns | Queue HWM bytes/chunks |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | native | 116114 | 1118 | 168194834 | 25448532 | 25962757 | 58057 | 877915000 | 0 | 19390464 | 6 | 122517 | 159831417 | 937/2 |
| 1 | baseline | 558095 | 2897 | 367334200 | 645074861 | 645918213 | 279047 | 965888000 | 1021112000 | 16166912 | 6 | 131264 | 132083776 | unavailable |
| 2 | baseline | 138930 | 948 | 364733934 | 745288993 | 745813855 | 69465 | 1001469000 | 939606000 | 14991360 | 6 | 123025 | 195129699 | unavailable |
| 3 | native | 85644 | 877 | 177903343 | 25579042 | 26112500 | 42822 | 921747000 | 0 | 19296256 | 7 | 119460 | 169390851 | 1730/13 |
| 4 | native | 87067 | 841 | 180253739 | 73875075 | 74402560 | 43533 | 878109000 | 0 | 19349504 | 6 | 123082 | 144196149 | 937/2 |
| 5 | baseline | 162918 | 1126 | 365372789 | 685535983 | 686071007 | 81459 | 958539000 | 903941000 | 15114240 | 6 | 129210 | 109760855 | unavailable |
| 6 | baseline | 289223 | 2093 | 365671869 | 688096109 | 688669189 | 144611 | 975478000 | 971120000 | 15540224 | 6 | 127418 | 110131601 | unavailable |
| 7 | native | 27221 | 271 | 170405200 | 67851446 | 68337940 | 13610 | 847744000 | 0 | 19066880 | 6 | 122999 | 147673361 | 660/1 |
| 8 | native | 73129 | 710 | 176429173 | 205512607 | 206006223 | 36564 | 864154000 | 0 | 19210240 | 6 | 123922 | 140741758 | 456/3 |
| 9 | baseline | 168521 | 1128 | 365897655 | 769000340 | 769887206 | 84260 | 996660000 | 986297000 | 15196160 | 6 | 122509 | 160452120 | unavailable |
| 10 | baseline | 140324 | 944 | 372175854 | 738573954 | 739134426 | 70162 | 973050000 | 960420000 | 15147008 | 7 | 127993 | 153877078 | unavailable |
| 11 | native | 109560 | 1032 | 171239606 | 162316701 | 163128472 | 54780 | 889706000 | 0 | 19349504 | 6 | 119529 | 109043398 | 1125/3 |

All measured rows reported `format_complete=1`, `error_count=0`, `cleanup_verified=1`, ANSI escape count 0, and normal bounded stop. Natural event equality and zero-drop are intentionally not claimed.

## Production-Default os_trace Acceptance

Generated: 2026-08-26T12:00:19Z

Status: `ok`. Passive gates found exactly one USB endpoint and an existing pair record through the verified isolated native stack. The production-default `os_trace` service completed two warmups and three measured two-second trials through `IosNativeTransport -> LiveLogController -> AdbLogcatSource -> StreamingLogData -> CaptureStore`, one stream at a time with ANSI disabled and the same `volatile-capture-v1` configuration. Every row reported complete formatting, zero errors, zero ANSI escapes, zero child processes, zero backpressure events, normal bounded stop, and verified empty capture roots.

The pre-fix warmup had reported sanitized `SpanOutOfBounds` error code 7. The clean-room regression identified type 2 activity bodies being interpreted as type 8 log-message spans. The post-fix real-device run confirms the bounded control-plist/type-2/type-8 handling without weakening strict type 8 checks or the 16 MiB packet limit.

### Warmups

| Warmup | Bytes | Lines | Startup ns | First byte ns | First commit ns | Throughput B/s | CPU ns | Peak RSS bytes | vCS | iCS | Teardown ns | Queue HWM bytes/chunks |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | 541052 | 2699 | 537950322 | 165515525 | 166492881 | 270526 | 1053468000 | 20529152 | 835 | 119355 | 177076328 | 1489/8 |
| 1 | 293754 | 1500 | 169104391 | 30436003 | 30984372 | 146877 | 919916000 | 19951616 | 6 | 115086 | 173564920 | 1333/5 |

### Measured trials

| Trial | Bytes | Lines | Startup ns | First byte ns | First commit ns | Throughput B/s | CPU ns | Peak RSS bytes | vCS | iCS | Teardown ns | Queue HWM bytes/chunks |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | 564159 | 3322 | 173313214 | 12930757 | 13816126 | 282079 | 987174000 | 20668416 | 7 | 119080 | 204747785 | 1334/7 |
| 1 | 322818 | 1728 | 171879848 | 19768951 | 20199954 | 161409 | 906083000 | 20000768 | 6 | 121948 | 116336389 | 1333/4 |
| 2 | 284195 | 1502 | 171999260 | 57122260 | 57593428 | 142097 | 914890000 | 19898368 | 6 | 118190 | 170200239 | 1333/4 |

Measured totals were 1,171,172 bytes and 6,552 committed lines. Medians were: startup 171,999,260 ns; first byte 19,768,951 ns; first commit 20,199,954 ns; throughput 161,409 B/s; process CPU 914,890,000 ns; peak RSS 20,000,768 bytes; teardown 170,200,239 ns; queue high-water 1,333 bytes and 4 chunks. Natural event equality and zero-drop are not claimed; the native drop counter remains unavailable.
