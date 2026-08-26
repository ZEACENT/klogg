# Source-built ADB helper

Klogg release packages consume a complete `adb` client/server executable built from the immutable source closure in `adb-helper.lock.json`. Google Platform-Tools binaries, Android SDK copies, `PATH` lookup, runtime downloads, floating revisions, and system-library substitution are forbidden.

The release flow has three isolated stages:

1. The explicit `PrefetchAdbHelperSources` job downloads every locked archive, verifies SHA-256, and creates the source/legal/SBOM/source-offer assets.
2. Native helper jobs extract only those verified archives and run the first-party CMake superbuild with `FETCHCONTENT_FULLY_DISCONNECTED=ON`.
3. App and package jobs download the helper artifact, receipt, and legal assets. CMake verifies them before copying the helper to the resolver-compatible `helpers` directory. Package actions verify and smoke-test the final staged layout before creating DEB, AppImage, DMG, NSIS, or 7z output.

Linux x86_64 and arm64 helpers build inside digest-bound manylinux2014 containers with networking disabled during source compilation. Their glibc 2.17 ABI requirement is stricter than the AppImage's glibc 2.31 ceiling. Linux ships a private shared `libusb-1.0.so.0` beside `adb` and uses an exact `$ORIGIN` RUNPATH. macOS compiles a thin target architecture with the package deployment target, uses the native IOKit backend, rejects libusb and non-system imports, and keeps binary inspection separate from native execution/device qualification. Windows applies the hash-locked MSYS2 Android Tools patch series to the Android 17 source closure, builds `AdbWinApi.dll` and `AdbWinUsbApi.dll` from the locked AOSP `platform/development` source with MSVC, and builds `adb.exe` plus private `libusb-1.0.dll` from source with the pinned MinGW toolchain. No MSYS2 ADB or DLL binary package is consumed.

Every helper artifact carries target-bound `receipt.json`, `smoke.json`, `package-smoke.json`, `package-verification.json`, and `SHA256SUMS` evidence and release builds attest the checksum envelope. Package qualification requires binary-build, binary-smoke, and package-verification receipts. A buildable or cross-built binary remains unqualified until native-device evidence exists; macOS release publication additionally requires verified signing and notarization receipts from secret-gated CI steps.

For a local native inspection build, first prefetch and extract the closure, generate legal assets, then invoke `scripts/build_adb_helper.py`. Local builds may use `scripts/verify_adb_helper_toolchain.py --allow-unlocked-local` for inspection only; release jobs require the exact hosted-runner identity or digest-bound container recorded in the lock. On an Intel Mac, `--target macos-arm64 --inspection-only` permits a thin arm64 source build and binary inspection, but intentionally records no executable smoke evidence.
