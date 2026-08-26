# Live Capture Dependency Inventory

## Packaged Native Stack

The packaged stack was inspected at `build_root/output/klogg.app/Contents/Frameworks/ios-native`. Passive enumeration used its extended device-list and pair-record APIs.

| Library | SHA-256 |
|---|---|
| `libcrypto.3.dylib` | `3e0350a0183f2d51f0022678def4ba2a795a052862ab308b0651462989a9a9c3` |
| `libcrypto.dylib` | `3e0350a0183f2d51f0022678def4ba2a795a052862ab308b0651462989a9a9c3` |
| `libcurl.4.dylib` | `35150157687726f24899f5c9906198a43f3ee89c2b2888ab16b2cb166657b93f` |
| `libcurl.dylib` | `35150157687726f24899f5c9906198a43f3ee89c2b2888ab16b2cb166657b93f` |
| `libimobiledevice-1.0.6.dylib` | `97aae9daa5711eb6fc56620991f9fdccd3876630523038c65bf98b7329e24f0e` |
| `libimobiledevice-1.0.dylib` | `97aae9daa5711eb6fc56620991f9fdccd3876630523038c65bf98b7329e24f0e` |
| `libimobiledevice-glue-1.0.0.dylib` | `ab25cdf5bdf473c51d8b7ccbea73e5f35a3303ca7385b4056d11860e9e3cd76c` |
| `libimobiledevice-glue-1.0.dylib` | `ab25cdf5bdf473c51d8b7ccbea73e5f35a3303ca7385b4056d11860e9e3cd76c` |
| `libplist-2.0.4.dylib` | `b59ef0c0032b00aa19d28fe7014cf16a07862481321de632b36ac676ef37f4a6` |
| `libplist-2.0.dylib` | `b59ef0c0032b00aa19d28fe7014cf16a07862481321de632b36ac676ef37f4a6` |
| `libssl.3.dylib` | `2cd35b8b28d4e49eb162fab8e16c77e25d51fcda44caf597d127a0984a6d5a9d` |
| `libssl.dylib` | `2cd35b8b28d4e49eb162fab8e16c77e25d51fcda44caf597d127a0984a6d5a9d` |
| `libtatsu.0.dylib` | `27ba64ca72edce039ec6910db0ba6ffa093e74db778344220eb879c3edc1d1b5` |
| `libtatsu.dylib` | `27ba64ca72edce039ec6910db0ba6ffa093e74db778344220eb879c3edc1d1b5` |
| `libusbmuxd-2.0.7.dylib` | `0403f3515d52219c701ed9fb4c6c50bfd3e8302cc56134979498c35d9160f663` |
| `libusbmuxd-2.0.dylib` | `0403f3515d52219c701ed9fb4c6c50bfd3e8302cc56134979498c35d9160f663` |

## Baseline Tooling

- User-supplied `pymobiledevice3` executable: canonical regular executable; SHA-256 `d84096329358f40b965b373a74b55def1ae6c54979ca3e327d8300f3f49ea9b6`.
- The executable started from its exact canonical path. No PATH lookup was used.
- No tool was installed, updated, or modified.

## Benchmark Binaries

- Synthetic benchmark SHA-256: `512a330e230ea35f60fa6514f433eeebda8045e65737b310fa8733b54e571df9`
- Fixture producer SHA-256: `a38524a2a2dbcdb0d1401eda2d052d14ee0096f445f245b2c687e2dc734c5f05`
- iOS protocol benchmark SHA-256: `0b6969b375d9ea06ff55dbc1ca4fa234fdf5165b871c8589d97506a649326c8c`

## Environment Limitations

- OS: macOS-26.5.2-x86_64-i386-64bit-Mach-O
- Architecture: x86_64
- Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101)
- Qt: 6.10.1
- Python: 3.14.6
- Full CMake regeneration remained blocked by the unrelated configured native-stack root; the synthetic benchmark used the bounded direct build validated by its focused contract suite.
- Windows binary-stream behavior is source-contracted but was not executed in this environment.
- Real-device acceptance completed against the rebuilt isolated stack: two warmups and twelve balanced measured trials.


## Native Acceptance Stack

- Built in a new isolated directory from existing locked local archives; no network, package manager, user install, or substitution was used.
- Architecture: x86_64.
- Acceptance verification receipt SHA-256: `2227cc6a2f030c5a46f8842cfe7545b9a5b0faaa571385d2ab0095cb1d43809a`.
- All four locked patches and final patched-tree hash verified independently.
- Required existing-pair, os_trace error-callback, and raw syslog error-callback exports were present.
- `verify_ios_native_stack.py`, Mach-O closure, and dylib hash verification passed.
- Acceptance used native syslog relay to match the baseline syslog source.
