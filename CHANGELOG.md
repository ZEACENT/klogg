# 2026-07:
## New features:
 - Open Folder: search an entire directory with a streaming, parallel grep-style engine; results are grouped into collapsible per-file sections with match counts and a per-file overview, clickable to open the file at the matching line, with multi-encoding support.
# 2022-06:
## Documentation:
 - [d711ddeb](https://github.com/variar/klogg/commit/d711ddeb): update build documentation [skip ci] (Anton Filimonov)
## Code refactoring:
 - [cf3947a2](https://github.com/variar/klogg/commit/cf3947a2): move io thread for regex matching outside of TBB (Anton Filimonov)
 - [35772f81](https://github.com/variar/klogg/commit/35772f81): simplify operations queue (Anton Filimonov)
## Build system:
 - [f61a2dc4](https://github.com/variar/klogg/commit/f61a2dc4): fix qt6 windows build (Anton Filimonov)
 - [12b36c62](https://github.com/variar/klogg/commit/12b36c62): use host version for osx deployment target by default (#481) (Anton Filimonov)
 - [d1ecc595](https://github.com/variar/klogg/commit/d1ecc595): fix deprecated QFontDatabase use (#481) (Anton Filimonov)
 - [29b40f99](https://github.com/variar/klogg/commit/29b40f99): use osx target 10.15 for Qt6 on Mac [skip ci] (#451) (Anton Filimonov)
## Continuous integration workflow:
 - [cca81764](https://github.com/variar/klogg/commit/cca81764): build packages for ubuntu 22.04 and fixes for Qt6 packaging (Anton Filimonov)
 - [af4ef1c3](https://github.com/variar/klogg/commit/af4ef1c3): install qt5compat for qt6 build on windows (Anton Filimonov)
 - [b55fde62](https://github.com/variar/klogg/commit/b55fde62): add qt6 packages for windows (Anton Filimonov)
 - [e3e9b246](https://github.com/variar/klogg/commit/e3e9b246): split workflow to smaller actions (Anton Filimonov)
 - [56d08c2c](https://github.com/variar/klogg/commit/56d08c2c): fix macos artifacts publishing [skip ci] (Anton Filimonov)
 - [043e864d](https://github.com/variar/klogg/commit/043e864d): use correct paths for Qt6 on mac (Anton Filimonov)
 - [de85fe7f](https://github.com/variar/klogg/commit/de85fe7f): switch for Qt6 LTS release for mac qt6 build (Anton Filimonov)
 - [8b522407](https://github.com/variar/klogg/commit/8b522407): install qt5compat for mac qt6 build (Anton Filimonov)
 - [3fe69f1c](https://github.com/variar/klogg/commit/3fe69f1c): try building with Qt6 on Mac (Anton Filimonov)
 - [ec58a1d3](https://github.com/variar/klogg/commit/ec58a1d3): fix sed for mac distribution.xml (Anton Filimonov)
 - [32c34b10](https://github.com/variar/klogg/commit/32c34b10): make better macos pkg (Anton Filimonov)
 - [6503e127](https://github.com/variar/klogg/commit/6503e127): fix mac pkg code sign (Anton Filimonov)
 - [553d2937](https://github.com/variar/klogg/commit/553d2937): import mac certs in one action (Anton Filimonov)
 - [0632e2fc](https://github.com/variar/klogg/commit/0632e2fc): let dpkg figure out dependencies for deb package (Anton Filimonov)
 - [e41b3353](https://github.com/variar/klogg/commit/e41b3353): add mac keys to same keychain (Anton Filimonov)
 - [91d5844b](https://github.com/variar/klogg/commit/91d5844b): try make better RPM deps (Anton Filimonov)
 - [51617738](https://github.com/variar/klogg/commit/51617738): add install cert to separate keychain (Anton Filimonov)
 - [d8e244a3](https://github.com/variar/klogg/commit/d8e244a3): make better deb packages (Anton Filimonov)
 - [6b50bf92](https://github.com/variar/klogg/commit/6b50bf92): fix mac codesign (Anton Filimonov)
 - [e6ac5502](https://github.com/variar/klogg/commit/e6ac5502): build pkg for mac (Anton Filimonov)
 - [789ad321](https://github.com/variar/klogg/commit/789ad321): use native compilers for each linux container (#480) (Anton Filimonov)
 - [b11582b9](https://github.com/variar/klogg/commit/b11582b9): fix path for macdeployqt (Anton Filimonov)
 - [cf41e9db](https://github.com/variar/klogg/commit/cf41e9db): fix mac build in CI (Anton Filimonov)
 - [a852c8e4](https://github.com/variar/klogg/commit/a852c8e4): add 3rdparty sources snapshot to release artifacts (Anton Filimonov)
 - [d4f7b34f](https://github.com/variar/klogg/commit/d4f7b34f): add 3rdparty sources snapshot to release artifacts (Anton Filimonov)
 - [e83ace65](https://github.com/variar/klogg/commit/e83ace65): don't depend on tests libs if not building tests (Anton Filimonov)
## Code refactoring:
 - [f88365f5](https://github.com/variar/klogg/commit/f88365f5): Code refactor, utilize unique_ptr and unused vars, lower scope code and using std::move() (#478) (Herman Semenov)
# 2022-05:
## Documentation:
 - [fb645849](https://github.com/variar/klogg/commit/fb645849): add AppImage to readme [skip ci] (Anton Filimonov)
## Tests:
 - [d92cae8d](https://github.com/variar/klogg/commit/d92cae8d): fix tests (Anton Filimonov)
 - [d759c438](https://github.com/variar/klogg/commit/d759c438): fix tests (Anton Filimonov)
## Build system:
 - [4f03e86a](https://github.com/variar/klogg/commit/4f03e86a): fix mac build (Anton Filimonov)
 - [17a87941](https://github.com/variar/klogg/commit/17a87941): fix build containers (Anton Filimonov)
 - [78a79c87](https://github.com/variar/klogg/commit/78a79c87): fix build containers (Anton Filimonov)
 - [3008f573](https://github.com/variar/klogg/commit/3008f573): add missing dep (Anton Filimonov)
## Continuous integration workflow:
 - [9d9215ee](https://github.com/variar/klogg/commit/9d9215ee): upload all artifacts to releases [skip ci] (Anton Filimonov)
 - [a86c5dba](https://github.com/variar/klogg/commit/a86c5dba): make different names for linux packages (Anton Filimonov)
 - [f5dc701f](https://github.com/variar/klogg/commit/f5dc701f): fix appimage packaging (Anton Filimonov)
 - [f0b6325b](https://github.com/variar/klogg/commit/f0b6325b): remove jammy, add oracle linux 8 (Anton Filimonov)
 - [9203ec54](https://github.com/variar/klogg/commit/9203ec54): build packages for more platforms (Anton Filimonov)
# 2022-04:
## New features:
 - [bcb71b50](https://github.com/variar/klogg/commit/bcb71b50): add a replace data in scratchpad action (#408) (Anton Filimonov)
## Documentation:
 - [156f4c77](https://github.com/variar/klogg/commit/156f4c77): update documentation for new release (Anton Filimonov)
## Build system:
 - [97189cf2](https://github.com/variar/klogg/commit/97189cf2): update tbb sha (Anton Filimonov)
 - [9d401a57](https://github.com/variar/klogg/commit/9d401a57): macdeployqtfix still uses python2 (Anton Filimonov)
 - [d40e78f4](https://github.com/variar/klogg/commit/d40e78f4): use absolute path for macdeployqtfix (Anton Filimonov)
 - [057dfd0e](https://github.com/variar/klogg/commit/057dfd0e): copy tbb libs from new location (Anton Filimonov)
 - [05aaadc2](https://github.com/variar/klogg/commit/05aaadc2): use python to call macdeployqt script (Anton Filimonov)
 - [8121e34a](https://github.com/variar/klogg/commit/8121e34a): use klogg fork of tbb for static build (Anton Filimonov)
 - [05d9a867](https://github.com/variar/klogg/commit/05d9a867): fix macdeployqtfix path (Anton Filimonov)
 - [0d6f9f13](https://github.com/variar/klogg/commit/0d6f9f13): copy tbb shared lib for win32 (Anton Filimonov)
 - [c5efb31f](https://github.com/variar/klogg/commit/c5efb31f): use shared tbb only for Win (Anton Filimonov)
 - [96d8b6aa](https://github.com/variar/klogg/commit/96d8b6aa): fix hyperscan includes (Anton Filimonov)
 - [286e78cc](https://github.com/variar/klogg/commit/286e78cc): don't use deprecated method (Anton Filimonov)
 - [8ba6b304](https://github.com/variar/klogg/commit/8ba6b304): fix uchardet include (Anton Filimonov)
 - [4cb6a1a6](https://github.com/variar/klogg/commit/4cb6a1a6): add git to build containters (Anton Filimonov)
 - [0c0fde02](https://github.com/variar/klogg/commit/0c0fde02): use own fork of hyperscan (Anton Filimonov)
 - [8d8ef200](https://github.com/variar/klogg/commit/8d8ef200): fix uchardet (Anton Filimonov)
 - [0ae6d42a](https://github.com/variar/klogg/commit/0ae6d42a): use CPM to get external dependencies (Anton Filimonov)
## Continuous integration workflow:
 - [2e3ee10c](https://github.com/variar/klogg/commit/2e3ee10c): workaround for Github broken elastic index for workflow runs [skip ci] (Anton Filimonov)
 - [45dc0279](https://github.com/variar/klogg/commit/45dc0279): add more tracing to debug failing test (Anton Filimonov)
 - [d7d890bb](https://github.com/variar/klogg/commit/d7d890bb): copy tbb only on win32 (Anton Filimonov)
# 2022-03:
## New features:
 - [fed53dee](https://github.com/variar/klogg/commit/fed53dee): show scratchpad when send data to it (#408) (Anton Filimonov)
## Documentation:
 - [901e4ade](https://github.com/variar/klogg/commit/901e4ade): update list of sponsors and contributors [skip ci] (Anton Filimonov)
## Build system:
 - [4c39210f](https://github.com/variar/klogg/commit/4c39210f): fix qt6 build on windows (Anton Filimonov)
 - [8b66d11a](https://github.com/variar/klogg/commit/8b66d11a): fix crashpad build on windows (Anton Filimonov)
 - [c5c70229](https://github.com/variar/klogg/commit/c5c70229): don't include 3rdparty readme into packages (Anton Filimonov)
 - [16225288](https://github.com/variar/klogg/commit/16225288): fix build for qt 5.9 (Anton Filimonov)
## Continuous integration workflow:
 - [ef8456bc](https://github.com/variar/klogg/commit/ef8456bc): add ts to codesign requests (Anton Filimonov)
 - [683c1c35](https://github.com/variar/klogg/commit/683c1c35): revert to windows 2019 runner (Anton Filimonov)
 - [b855c762](https://github.com/variar/klogg/commit/b855c762): update vcredist paths (Anton Filimonov)
 - [5aa5f8ce](https://github.com/variar/klogg/commit/5aa5f8ce): check new paths on windows [skip ci] (Anton Filimonov)
## Other commits:
 - [400da159](https://github.com/variar/klogg/commit/400da159): Revert "chore: update sentry to 0.4.15" (Anton Filimonov)
# 2022-02:
## Build system:
 - [8002ed8a](https://github.com/variar/klogg/commit/8002ed8a): fix build on old Qt (Anton Filimonov)
## Continuous integration workflow:
 - [4e403031](https://github.com/variar/klogg/commit/4e403031): remove rpm check for now as centos is obsolete (Anton Filimonov)
 - [243b958f](https://github.com/variar/klogg/commit/243b958f): use rhel8 to check rpm (Anton Filimonov)
 - [8fe1b1d6](https://github.com/variar/klogg/commit/8fe1b1d6): update centos8 docker to point to vault.centos.org (Anton Filimonov)
# 2022-01:
## Continuous integration workflow:
 - [aff1f1ce](https://github.com/variar/klogg/commit/aff1f1ce): sign uninstaller (Anton Filimonov)
## Other commits:
 - [3a561a2a](https://github.com/variar/klogg/commit/3a561a2a): [skip ci] update latest version (Anton Filimonov)
# 2021-12:
## New features:
 - [e874eb12](https://github.com/variar/klogg/commit/e874eb12): add more shortcuts (#407, #408) (Anton Filimonov)
 - [fb478e62](https://github.com/variar/klogg/commit/fb478e62): quick send selection to scratchpad (#408) (Anton Filimonov)
 - [80771eeb](https://github.com/variar/klogg/commit/80771eeb): reset color labeling cycle when clearing all labels (#270) (Anton Filimonov)
 - [f48dda65](https://github.com/variar/klogg/commit/f48dda65): allow to configure dark palette (#430) (Anton Filimonov)
 - [7bb565a3](https://github.com/variar/klogg/commit/7bb565a3): allow to set default encoding (#431) (Anton Filimonov)
## Continuous integration workflow:
 - [5a07c58e](https://github.com/variar/klogg/commit/5a07c58e): add win codesigning back (Anton Filimonov)
 - [a8a4eb06](https://github.com/variar/klogg/commit/a8a4eb06): add verbose output for macos notarize actions (Anton Filimonov)
## Build system:
 - [1a7a5a4d](https://github.com/variar/klogg/commit/1a7a5a4d): Revert "fix: fix link order for new tbb" (Anton Filimonov)
# 2021-11:
## Bug fixes:
 - [e75f29ab](https://github.com/variar/klogg/commit/e75f29ab): Set preferences dialog for Mac (#428) (Anton Filimonov)
## Continuous integration workflow:
 - [f1380935](https://github.com/variar/klogg/commit/f1380935): Remove codesign on windows (Anton Filimonov)
 - [94408bb7](https://github.com/variar/klogg/commit/94408bb7): Fix openssl link (Anton Filimonov)
# 2021-10:
## Build system:
 - [6d4535cd](https://github.com/variar/klogg/commit/6d4535cd): Use Qt6 if available (Stephan Vedder)
# 2021-09:
## New features:
 - [5c6c4008](https://github.com/variar/klogg/commit/5c6c4008): use better color labels (#270) (Anton Filimonov)
 - [01d942b8](https://github.com/variar/klogg/commit/01d942b8): add colors to context menu on win (Anton Filimonov)
## Code refactoring:
 - [6f503b99](https://github.com/variar/klogg/commit/6f503b99): make serial graph node explicit (Anton Filimonov)
 - [72cfdc8c](https://github.com/variar/klogg/commit/72cfdc8c): make indexing graph more readable and add traces (Anton Filimonov)
## Continuous integration workflow:
 - [d7f39db6](https://github.com/variar/klogg/commit/d7f39db6): add manual release actions (#380)[skip ci] (Anton Filimonov)
## Other commits:
 - [a7cf50f5](https://github.com/variar/klogg/commit/a7cf50f5): Fixing typo in README (#400)  (by @Lightjohn) (Jonathan)
# 2021-08:
## New features:
 - [5ba7ba91](https://github.com/variar/klogg/commit/5ba7ba91): add manifest for scoop installer [skip ci] (Anton Filimonov)
 - [9c30baa3](https://github.com/variar/klogg/commit/9c30baa3): improve boolean expression error messages (#389) (Anton Filimonov)
 - [b9ded7d9](https://github.com/variar/klogg/commit/b9ded7d9): add dark style based on windows qt style (#394) [ci release] (Anton Filimonov)
 - [cfe29e6d](https://github.com/variar/klogg/commit/cfe29e6d): allow to activate multiple highlighter sets (#360) (Anton Filimonov)
 - [c2422374](https://github.com/variar/klogg/commit/c2422374): allow to edit color label names (Anton Filimonov)
 - [939e1121](https://github.com/variar/klogg/commit/939e1121): remove single line selection fill (#385) (Anton Filimonov)
 - [4963c2c0](https://github.com/variar/klogg/commit/4963c2c0): allow to select cycling color labels (#270) (Anton Filimonov)
 - [c104fd24](https://github.com/variar/klogg/commit/c104fd24): add context menu for color labels (#270) (Anton Filimonov)
 - [0aa6f8cf](https://github.com/variar/klogg/commit/0aa6f8cf): allow to turn off quick highlight with cycle key (#270) (Anton Filimonov)
 - [4331885b](https://github.com/variar/klogg/commit/4331885b): allow to hide ansi color sequences in displayed text (#338) (Anton Filimonov)
 - [857ddf8a](https://github.com/variar/klogg/commit/857ddf8a): better copy text handling for path line (Anton Filimonov)
 - [355412ac](https://github.com/variar/klogg/commit/355412ac): allow to configure quick highlighters (#270) (Anton Filimonov)
## Documentation:
 - [f2a42b98](https://github.com/variar/klogg/commit/f2a42b98): add section about docs to contributing guide [skip ci] (Anton Filimonov)
 - [1b727814](https://github.com/variar/klogg/commit/1b727814): merge changes from lilventi/klogg [skip ci] (Anton Filimonov)
 - [c483723d](https://github.com/variar/klogg/commit/c483723d): fix typos (Anton Filimonov)
## Code refactoring:
 - [567df0c9](https://github.com/variar/klogg/commit/567df0c9): add klogg_ prefix to all klogg libs (Anton Filimonov)
 - [8f9feef1](https://github.com/variar/klogg/commit/8f9feef1): move logdata headers to top (Anton Filimonov)
 - [c8e011ea](https://github.com/variar/klogg/commit/c8e011ea): move regex wrappers to its own library (Anton Filimonov)
 - [9fc68073](https://github.com/variar/klogg/commit/9fc68073): fix unneeded initialization (Anton Filimonov)
 - [cfd4ed0c](https://github.com/variar/klogg/commit/cfd4ed0c): make parseDataBlock code more readable (Anton Filimonov)
## Build system:
 - [b3e7d673](https://github.com/variar/klogg/commit/b3e7d673): try to disable lto to fix build error on centos (Anton Filimonov)
## Continuous integration workflow:
 - [1e46c9c2](https://github.com/variar/klogg/commit/1e46c9c2): test discord webhook (#380)[skip ci] (Anton Filimonov)
 - [f9bec82a](https://github.com/variar/klogg/commit/f9bec82a): move release flow to manual workflow (#380)[skip ci] (Anton Filimonov)
 - [88665c19](https://github.com/variar/klogg/commit/88665c19): put version into separate artifact (#380) (Anton Filimonov)
 - [33e7f616](https://github.com/variar/klogg/commit/33e7f616): extract version from artifacts (#380) [skip ci] (Anton Filimonov)
 - [c1d18dea](https://github.com/variar/klogg/commit/c1d18dea): add draft for ci release workflow (#380) [skip ci] (Anton Filimonov)
 - [1502489c](https://github.com/variar/klogg/commit/1502489c): move cmake lto def to other action (Anton Filimonov)
 - [b06efccd](https://github.com/variar/klogg/commit/b06efccd): diagnose lto error (Anton Filimonov)
 - [a45ad09e](https://github.com/variar/klogg/commit/a45ad09e): disable lto only for centos (Anton Filimonov)
 - [ccf71c21](https://github.com/variar/klogg/commit/ccf71c21): try to avoid gcc lto bug [ci release] (Anton Filimonov)
 - [63a26634](https://github.com/variar/klogg/commit/63a26634): try building in containers for linux (Anton Filimonov)
 - [003f3d37](https://github.com/variar/klogg/commit/003f3d37): simplify boost preparation (Anton Filimonov)
 - [fa2a4053](https://github.com/variar/klogg/commit/fa2a4053): remove old workflows (Anton Filimonov)
 - [7e14983c](https://github.com/variar/klogg/commit/7e14983c): actually disable tests in build action (Anton Filimonov)
 - [1b472d72](https://github.com/variar/klogg/commit/1b472d72): move test to separate action (Anton Filimonov)
# 2021-07:
## New features:
 - [ccbd0b28](https://github.com/variar/klogg/commit/ccbd0b28): allow to edit search history (#309) (Anton Filimonov)
 - [1707ef8c](https://github.com/variar/klogg/commit/1707ef8c): add file to recent files list on manual close (#131) (Anton Filimonov)
 - [d7ccfe31](https://github.com/variar/klogg/commit/d7ccfe31): adjust horizontal scroll page step (#163) (Anton Filimonov)
 - [7b5feec4](https://github.com/variar/klogg/commit/7b5feec4): add more search and follow settings (#283, #363) (Anton Filimonov)
 - [09419c54](https://github.com/variar/klogg/commit/09419c54): allow to configure search history (#364) (Anton Filimonov)
 - [cac0afef](https://github.com/variar/klogg/commit/cac0afef): deselect quick highlighters on same shortcut (#270) (Anton Filimonov)
 - [ac36a09e](https://github.com/variar/klogg/commit/ac36a09e): allow to change font size with mouse wheel (#359) (Anton Filimonov)
## Documentation:
 - [02bf08ea](https://github.com/variar/klogg/commit/02bf08ea): document commit message prefixes (Anton Filimonov)
## Build system:
 - [3987a4e5](https://github.com/variar/klogg/commit/3987a4e5): fix build on older Qt (Anton Filimonov)
## Other commits:
 - [38084e32](https://github.com/variar/klogg/commit/38084e32): [skip ci] chore: update minimal required compilers (Anton Filimonov)
# 2021-06:
## New features:
 - [e84ad461](https://github.com/variar/klogg/commit/e84ad461): allow alpha-channel for highlighters (#337) (Anton Filimonov)
 - [cd6efb9e](https://github.com/variar/klogg/commit/cd6efb9e): [ci release] feature: add experimental color labels (#270) (Anton Filimonov)
 - [aee7244d](https://github.com/variar/klogg/commit/aee7244d): add list of prominent  features in dev builds (Anton Filimonov)
 - [be2d40d9](https://github.com/variar/klogg/commit/be2d40d9): allow files with more than 2147483647 lines (#339, #341) (Anton Filimonov)
 - [4657f4ae](https://github.com/variar/klogg/commit/4657f4ae): add runtime cpu check (#343) (#344) (Anton Filimonov)
 - [4c29f49f](https://github.com/variar/klogg/commit/4c29f49f): [ci release] feature: Add go to line action in menu (#334) (Anton Filimonov)
 - [406e4ba4](https://github.com/variar/klogg/commit/406e4ba4): Allow to configure main window shortcuts (#26) (Anton Filimonov)
 - [9d5f252d](https://github.com/variar/klogg/commit/9d5f252d): improve shortcuts edit dialog (#26) (Anton Filimonov)
 - [9f89d3b9](https://github.com/variar/klogg/commit/9f89d3b9): allow to set predefined filters pattern mode (#243, #305) (Anton Filimonov)
 - [6ed90ee3](https://github.com/variar/klogg/commit/6ed90ee3): allow to change predefined filters order (#243) (Anton Filimonov)
 - [564bd1db](https://github.com/variar/klogg/commit/564bd1db): Add links to Discord and Telegram groups (Anton Filimonov)
 - [f63ad53c](https://github.com/variar/klogg/commit/f63ad53c): [ci release] Allow to configure main search highlight jitter (Anton Filimonov)
 - [b9b25ff0](https://github.com/variar/klogg/commit/b9b25ff0): [ci release] Add option to variate highlight colors (Anton Filimonov)
## Bug fixes:
 - [bb2d9faa](https://github.com/variar/klogg/commit/bb2d9faa): [ci release] fix: save settings from View menu (#349) (Anton Filimonov)
 - [9711a68a](https://github.com/variar/klogg/commit/9711a68a): [ci release] fix: initialize regex checkbox (#342) (Anton Filimonov)
 - [c5482ffd](https://github.com/variar/klogg/commit/c5482ffd): Remove unintentional variance for main match (Anton Filimonov)
 - [854e9b00](https://github.com/variar/klogg/commit/854e9b00): Update SingleApplication from upstream (#228, #329) (Anton Filimonov)
## Documentation:
 - [d73edf10](https://github.com/variar/klogg/commit/d73edf10): [skip ci] add links to chats (Anton Filimonov)
 - [6cff6e9b](https://github.com/variar/klogg/commit/6cff6e9b): [skip ci] profreading (Anton Filimonov)
 - [eae77815](https://github.com/variar/klogg/commit/eae77815): [skip ci] Add article for boolean expressions (Anton Filimonov)
 - [2246de3c](https://github.com/variar/klogg/commit/2246de3c): Add mimalloc to notice (Anton Filimonov)
## Performance:
 - [24cc11b6](https://github.com/variar/klogg/commit/24cc11b6): Use more direct access to variables in boolean evaluator (Anton Filimonov)
 - [d89b561a](https://github.com/variar/klogg/commit/d89b561a): Switch to robinhoog sets in exrptk (Anton Filimonov)
## Code refactoring:
 - [f5f24395](https://github.com/variar/klogg/commit/f5f24395): [ci release] Move boolean evaluator to its own TU (Anton Filimonov)
## Build system:
 - [30c9ee8a](https://github.com/variar/klogg/commit/30c9ee8a): fix build (Anton Filimonov)
## Other commits:
 - [afec3cbf](https://github.com/variar/klogg/commit/afec3cbf): [skip ci] chore: update latest version (Anton Filimonov)
# 2021-05:
## New feature:
 - [154b8994](https://github.com/variar/klogg/commit/154b8994): [ci release] Add user-configurable shortcuts (wip) (Anton Filimonov)
 - [4acfd7bc](https://github.com/variar/klogg/commit/4acfd7bc): Add highlight for matching patterns (Anton Filimonov)
 - [c687f271](https://github.com/variar/klogg/commit/c687f271): [ci release] Add/Replace/Exclude for combination mode (Anton Filimonov)
 - [0aae3b02](https://github.com/variar/klogg/commit/0aae3b02): [ci release] Add boolean pattern combination (Anton Filimonov)
 - [cf1b8456](https://github.com/variar/klogg/commit/cf1b8456): Allow to cancel archives extraction (Anton Filimonov)
## Bug fixes:
 - [70799af6](https://github.com/variar/klogg/commit/70799af6): [ci release] Adapt shortcuts to older Qt (Anton Filimonov)
 - [7228c9b5](https://github.com/variar/klogg/commit/7228c9b5): [ci release] Fix leading zeroes count on msvc (Anton Filimonov)
 - [2a0eb5e6](https://github.com/variar/klogg/commit/2a0eb5e6): Do not touch config too frequently (Anton Filimonov)
 - [f64d8461](https://github.com/variar/klogg/commit/f64d8461): Fix crash on reloading file during search (Anton Filimonov)
 - [b0c00418](https://github.com/variar/klogg/commit/b0c00418): [ci release] Fix lifetime issues and code style (Anton Filimonov)
 - [2e25c222](https://github.com/variar/klogg/commit/2e25c222): Try use mimalloc v2 (#323) (Anton Filimonov)
 - [b4df7404](https://github.com/variar/klogg/commit/b4df7404): [ci release] Handle more error cases when copy to clipboard (Anton Filimonov)
 - [24012d20](https://github.com/variar/klogg/commit/24012d20): Make matches overview usable for a lot of matches (Anton Filimonov)
 - [283e9fc5](https://github.com/variar/klogg/commit/283e9fc5): Use proper throttling for search progress (Anton Filimonov)
 - [b3a3c100](https://github.com/variar/klogg/commit/b3a3c100): Capture exceptions in tbb threads (Anton Filimonov)
 - [51f6eb80](https://github.com/variar/klogg/commit/51f6eb80): Fix plog and QString (Anton Filimonov)
 - [60fd9a46](https://github.com/variar/klogg/commit/60fd9a46): [skip ci] fix discord link (Anton Filimonov)
 - [402a6975](https://github.com/variar/klogg/commit/402a6975): Try use Qt command line parser (#223) (Anton Filimonov)
 - [f3dd91da](https://github.com/variar/klogg/commit/f3dd91da): [ci release] Enabled crash reporting back (Anton Filimonov)
 - [ba058366](https://github.com/variar/klogg/commit/ba058366): Allow incremental quickfind mode for regex (Anton Filimonov)
 - [f2b1b998](https://github.com/variar/klogg/commit/f2b1b998): Do not break selection on Punctuation_Connector (Anton Filimonov)
 - [b39641f6](https://github.com/variar/klogg/commit/b39641f6): [ci release] Fix crash on last line (Anton Filimonov)
 - [c09d0cc7](https://github.com/variar/klogg/commit/c09d0cc7): [ci release] disable malloc override on mac (Anton Filimonov)
 - [cbcdc4ce](https://github.com/variar/klogg/commit/cbcdc4ce): Switch back to TBB malloc (Anton Filimonov)
 - [886e0de9](https://github.com/variar/klogg/commit/886e0de9): [ci release] Make Esc reset focus to main view (#168) (Anton Filimonov)
 - [8ce767af](https://github.com/variar/klogg/commit/8ce767af): Close QuickFind by Esc (#168) (Anton Filimonov)
 - [50c27808](https://github.com/variar/klogg/commit/50c27808): Refuse to index files with too long lines (Anton Filimonov)
 - [fadb25fb](https://github.com/variar/klogg/commit/fadb25fb): Fix matching with parentheses (#303) (Anton Filimonov)
 - [385c7dce](https://github.com/variar/klogg/commit/385c7dce): [skip ci] Fix latest version (Anton Filimonov)
 - [3d725b3d](https://github.com/variar/klogg/commit/3d725b3d): Set bearer timeout to very large value (Anton Filimonov)
 - [94925d40](https://github.com/variar/klogg/commit/94925d40): Fix typo (#295) (Anton Filimonov)
 - [96ba60f5](https://github.com/variar/klogg/commit/96ba60f5): Do not allow character height to be zero (Anton Filimonov)
 - [a22c7f9a](https://github.com/variar/klogg/commit/a22c7f9a): Add back option to select preferred regex engine (Anton Filimonov)
 - [9826b628](https://github.com/variar/klogg/commit/9826b628): Fix crash when enqueing new operation in worker destructor (Anton Filimonov)
 - [0baf9546](https://github.com/variar/klogg/commit/0baf9546): Actually save default splitter to config (#174) (Anton Filimonov)
 - [7e839891](https://github.com/variar/klogg/commit/7e839891): Start new window session for file from command line (Anton Filimonov)
## Documentation:
 - [c6701beb](https://github.com/variar/klogg/commit/c6701beb): [ci release] Add documentation for logical patern combination (Anton Filimonov)
 - [a354c71c](https://github.com/variar/klogg/commit/a354c71c): update notice (Anton Filimonov)
 - [51f53772](https://github.com/variar/klogg/commit/51f53772): [skip ci] try add image to readme (Anton Filimonov)
 - [ec6028ee](https://github.com/variar/klogg/commit/ec6028ee): Add Gitter badge (#317) (The Gitter Badger)
 - [3d8deb10](https://github.com/variar/klogg/commit/3d8deb10): [skip ci] Add post about memory allocation (Anton Filimonov)
 - [fc6324a6](https://github.com/variar/klogg/commit/fc6324a6): [skip ci] add KO FI link (Anton Filimonov)
 - [59816030](https://github.com/variar/klogg/commit/59816030): [skip ci] add patreon link (Anton Filimonov)
 - [6105d627](https://github.com/variar/klogg/commit/6105d627): [skip ci] provide list of fixed glogg issues on separate page (Anton Filimonov)
 - [4337ac51](https://github.com/variar/klogg/commit/4337ac51): [skip ci] add c++17 in build requirements (Anton Filimonov)
 - [27d75262](https://github.com/variar/klogg/commit/27d75262): [skip ci] fix video comment (Anton Filimonov)
 - [fc8d5699](https://github.com/variar/klogg/commit/fc8d5699): [skip ci] add list of fixed glogg issues and perf comparison (Anton Filimonov)
 - [11233a00](https://github.com/variar/klogg/commit/11233a00): [skip ci] add visible link to github releases (Anton Filimonov)
## Performance:
 - [7e80d4ac](https://github.com/variar/klogg/commit/7e80d4ac): Use faster digits count (Anton Filimonov)
 - [6aa7ee43](https://github.com/variar/klogg/commit/6aa7ee43): [ci release] Make fast path for regex matching (Anton Filimonov)
 - [dcbc65a8](https://github.com/variar/klogg/commit/dcbc65a8): Switch exprtk to robin_hood maps (Anton Filimonov)
 - [229daec3](https://github.com/variar/klogg/commit/229daec3): Decode whole search block at once (Anton Filimonov)
 - [8de8cdad](https://github.com/variar/klogg/commit/8de8cdad): Replace tbb malloc with mimalloc (Anton Filimonov)
 - [50bae545](https://github.com/variar/klogg/commit/50bae545): Using roaring bitmaps to store marks and matches (Anton Filimonov)
## Code refactoring:
 - [6c4262c8](https://github.com/variar/klogg/commit/6c4262c8): More code cleanup (Anton Filimonov)
 - [b2e5b1cf](https://github.com/variar/klogg/commit/b2e5b1cf): Add back memory stats (Anton Filimonov)
 - [58aed568](https://github.com/variar/klogg/commit/58aed568): Code cleanup (Anton Filimonov)
 - [cd5ced3b](https://github.com/variar/klogg/commit/cd5ced3b): Remove unused operations (Anton Filimonov)
 - [f00725b5](https://github.com/variar/klogg/commit/f00725b5): Reduce nesting (Anton Filimonov)
 - [6054d5ff](https://github.com/variar/klogg/commit/6054d5ff): More QtConcurrent cleanup (Anton Filimonov)
 - [9616e63f](https://github.com/variar/klogg/commit/9616e63f): [ci release] Replace some QtConcurrent uses with TBB (Anton Filimonov)
 - [10ba5fed](https://github.com/variar/klogg/commit/10ba5fed): Add more accurate resource wrapper (Anton Filimonov)
 - [ed982634](https://github.com/variar/klogg/commit/ed982634): Remove abseil dependency (Anton Filimonov)
 - [c7b86edb](https://github.com/variar/klogg/commit/c7b86edb): Use standard mutext (Anton Filimonov)
 - [469a3b8b](https://github.com/variar/klogg/commit/469a3b8b): Code cleanup (Anton Filimonov)
 - [33760b0c](https://github.com/variar/klogg/commit/33760b0c): Code cleanup (Anton Filimonov)
 - [95472955](https://github.com/variar/klogg/commit/95472955): Fixes for C++ 17 (Anton Filimonov)
 - [d5ea5954](https://github.com/variar/klogg/commit/d5ea5954): Switch to C++ 17 (Anton Filimonov)
 - [1e8c20ac](https://github.com/variar/klogg/commit/1e8c20ac): Remove immer after switching to roaring bitmaps (Anton Filimonov)
 - [c962ba93](https://github.com/variar/klogg/commit/c962ba93): Code cleanup (Anton Filimonov)
 - [881a67ec](https://github.com/variar/klogg/commit/881a67ec): Use string_view for safety and clarity (Anton Filimonov)
## Tests:
 - [a27f0ae6](https://github.com/variar/klogg/commit/a27f0ae6): Fix tests (Anton Filimonov)
 - [6736068f](https://github.com/variar/klogg/commit/6736068f): [ci release] fix tests (Anton Filimonov)
 - [72425357](https://github.com/variar/klogg/commit/72425357): [ci release] Fix test on mac (Anton Filimonov)
 - [5cdfd91f](https://github.com/variar/klogg/commit/5cdfd91f): [ci release] Fix test on mac (Anton Filimonov)
 - [cb3710f1](https://github.com/variar/klogg/commit/cb3710f1): Enable polling on Mac for tests (Anton Filimonov)
 - [d4840a9b](https://github.com/variar/klogg/commit/d4840a9b): Try wait for ui state (Anton Filimonov)
 - [dcee04ef](https://github.com/variar/klogg/commit/dcee04ef): Revert "bypass flaky tests" (Anton Filimonov)
 - [96aec79f](https://github.com/variar/klogg/commit/96aec79f): [ci release] bypass flaky tests (Anton Filimonov)
 - [305a4944](https://github.com/variar/klogg/commit/305a4944): More flaky tests (Anton Filimonov)
 - [892bda7f](https://github.com/variar/klogg/commit/892bda7f): Try fix flaky test (Anton Filimonov)

## Build system:
 - [0480506a](https://github.com/variar/klogg/commit/0480506a): Fix build (Anton Filimonov)
 - [ffe73be3](https://github.com/variar/klogg/commit/ffe73be3): [ci release] fix build (Anton Filimonov)
 - [c8ea47dd](https://github.com/variar/klogg/commit/c8ea47dd): [WIP] fix build (Anton Filimonov)
 - [c93241b6](https://github.com/variar/klogg/commit/c93241b6): Fix x86 build (Anton Filimonov)
 - [663998f4](https://github.com/variar/klogg/commit/663998f4): Fix mac build (Anton Filimonov)
 - [c03488bf](https://github.com/variar/klogg/commit/c03488bf): Use system KArchive if available (Anton Filimonov)
 - [f8286785](https://github.com/variar/klogg/commit/f8286785): Allow to use system abseil (#300) (Anton Filimonov)
 - [d5d2107c](https://github.com/variar/klogg/commit/d5d2107c): [skip ci] klogg will try to use system libraries (Anton Filimonov)
 - [a1c4c8f9](https://github.com/variar/klogg/commit/a1c4c8f9): Try using system provided libraries (#300) (Anton Filimonov)
 - [b5b00ded](https://github.com/variar/klogg/commit/b5b00ded): [ci release] fix mac build (Anton Filimonov)
 - [1793ba7b](https://github.com/variar/klogg/commit/1793ba7b): Fix x86 builds (Anton Filimonov)
## Continuous integration workflow:
 - [7fddde58](https://github.com/variar/klogg/commit/7fddde58): Build portable version only on Windows (Anton Filimonov)
 - [dbe45f7d](https://github.com/variar/klogg/commit/dbe45f7d): Use same version of boost for all CI builds (Anton Filimonov)
 - [12aa6196](https://github.com/variar/klogg/commit/12aa6196): Better visibility for Centos build (Anton Filimonov)
 - [95295ae0](https://github.com/variar/klogg/commit/95295ae0): Revert "Try to reduce pre-release notification" (Anton Filimonov)
 - [1a0e9a2c](https://github.com/variar/klogg/commit/1a0e9a2c): Try to reduce pre-release notification (Anton Filimonov)
 - [a77bf323](https://github.com/variar/klogg/commit/a77bf323): Simplify CI workflows (Anton Filimonov)
 - [3ceff618](https://github.com/variar/klogg/commit/3ceff618): Add openssl to centos build container (Anton Filimonov)
 - [cee1f57f](https://github.com/variar/klogg/commit/cee1f57f): Try to build rpm in centos docker container (Anton Filimonov)
 - [93818b11](https://github.com/variar/klogg/commit/93818b11): Try use gcc-8 to stay centos-compatible (Anton Filimonov) 
 - [eba242cc](https://github.com/variar/klogg/commit/eba242cc): Use recent appimage (Anton Filimonov)
 - [52971526](https://github.com/variar/klogg/commit/52971526): Disable rpm verification on CI builds (Anton Filimonov)
 - [0d864dfd](https://github.com/variar/klogg/commit/0d864dfd): Switch to Ubuntu 18.04 (Anton Filimonov)
 - [50fcd2f9](https://github.com/variar/klogg/commit/50fcd2f9): Do not use hyperscan on 32 bit Win builds (Anton Filimonov)
## Other commits:
 - [c16f41db](https://github.com/variar/klogg/commit/c16f41db): Add console grep-like utility (Anton Filimonov)
 - [633f177d](https://github.com/variar/klogg/commit/633f177d): Update minidump_dump (Anton Filimonov)
 - [b221456f](https://github.com/variar/klogg/commit/b221456f): [ci release] Add more memory stats to crashdumps (Anton Filimonov)
# 2021-04:
## New features:
 - [d4825701](https://github.com/variar/klogg/commit/d4825701): Move from PCRE to Hyperscan (Anton Filimonov)
 - [ae9a80b3](https://github.com/variar/klogg/commit/ae9a80b3): Add intel hyperscan (Anton Filimonov)
 - [59284807](https://github.com/variar/klogg/commit/59284807): Use automatic fallback to Qt regular expressions (Anton Filimonov)
 - [74187cd3](https://github.com/variar/klogg/commit/74187cd3): Add patch for hyperscan fat runtime build (#291) (Anton Filimonov)
 - [8e26da80](https://github.com/variar/klogg/commit/8e26da80): Switch to more simple dark style and require restart (Anton Filimonov)
 - [198f7ebe](https://github.com/variar/klogg/commit/198f7ebe): Relax add|replace search (Anton Filimonov)
 - [bf1b710e](https://github.com/variar/klogg/commit/bf1b710e): Add context menu item to search with the current selection (#285) (Dan Berindei)
 - [806b2b0d](https://github.com/variar/klogg/commit/806b2b0d): Add button to treat pattern as exclude filter (#22) (Anton Filimonov)
 - [aee2fe84](https://github.com/variar/klogg/commit/aee2fe84): Prepare for excluding patterns (Anton Filimonov)
## Bug fixes:
 - [1c686a43](https://github.com/variar/klogg/commit/1c686a43): Add KDAB to NOTICE and reduce debounce timeout (Anton Filimonov)
 - [8bbd446b](https://github.com/variar/klogg/commit/8bbd446b): Try more sofisticated signal debouncer (#286) (Anton Filimonov)
 - [8da81f49](https://github.com/variar/klogg/commit/8da81f49): Add custom tab close icons for Fusion style on Windows (#288) (Anton Filimonov)
 - [fe210507](https://github.com/variar/klogg/commit/fe210507): Fix non-monospace highlight and selection (#246) (Anton Filimonov)
 - [60267121](https://github.com/variar/klogg/commit/60267121): Fix line number area rendering (#249) (Anton Filimonov)
 - [3e7a413f](https://github.com/variar/klogg/commit/3e7a413f): Don't prevent horizontal scroll in follow mode (#247) (Anton Filimonov)
 - [1f1801d5](https://github.com/variar/klogg/commit/1f1801d5): Disable FSEvents backed (Anton Filimonov)
 - [22bdbbd2](https://github.com/variar/klogg/commit/22bdbbd2): Use common dispatch to threads (Anton Filimonov)
 - [97886ac7](https://github.com/variar/klogg/commit/97886ac7): Get rid of cmake warning (Anton Filimonov)
 - [0c77863f](https://github.com/variar/klogg/commit/0c77863f): Fix 0 for goto line (#244) (Anton Filimonov)
 - [85ac7605](https://github.com/variar/klogg/commit/85ac7605): Update selection on right click (#281) (Anton Filimonov)
 - [80f332da](https://github.com/variar/klogg/commit/80f332da): Add some bad_alloc catching (#235) (Anton Filimonov)
 - [18747786](https://github.com/variar/klogg/commit/18747786): Fix file monitor notifications spam (#286) (Anton Filimonov)
 - [b340f25e](https://github.com/variar/klogg/commit/b340f25e): Fix passing list of files to primary instance (Anton Filimonov)
 - [892494a4](https://github.com/variar/klogg/commit/892494a4): Fix command line help (Anton Filimonov)
 - [48b32815](https://github.com/variar/klogg/commit/48b32815): Made regex error text readable (#264) (Anton Filimonov)
 - [e627e1e2](https://github.com/variar/klogg/commit/e627e1e2): Fix icon reloading for dark theme (#264) (Anton Filimonov)
 - [10ce9b10](https://github.com/variar/klogg/commit/10ce9b10): Track context menu positon (#242) (Anton Filimonov)
 - [8ff6d7d8](https://github.com/variar/klogg/commit/8ff6d7d8): Fix deadlock on indexing (Anton Filimonov)
 - [30015015](https://github.com/variar/klogg/commit/30015015): Fix some dataraces from tsan (Anton Filimonov)
 - [9b0c3611](https://github.com/variar/klogg/commit/9b0c3611): Allow to set selection start and end (#242) (Anton Filimonov)
 - [6567a138](https://github.com/variar/klogg/commit/6567a138): Calculate head/tail hash after all indexing is done (Anton Filimonov)
 - [05241283](https://github.com/variar/klogg/commit/05241283): Create dump dir if not exist (Anton Filimonov)
 - [63474a05](https://github.com/variar/klogg/commit/63474a05): Replace null chars with spaces for clipboard (#227) (Anton Filimonov)
 - [c64da293](https://github.com/variar/klogg/commit/c64da293): Fix single line copy to clipboard (Anton Filimonov)
 - [a7fa2ada](https://github.com/variar/klogg/commit/a7fa2ada): Add retry to get data from clipboard (Anton Filimonov)
 - [4934d720](https://github.com/variar/klogg/commit/4934d720): Allow some time to upload dumps (Anton Filimonov)
 - [d58d5255](https://github.com/variar/klogg/commit/d58d5255): Try to recover from bad_alloc (Anton Filimonov)
 - [82cb0104](https://github.com/variar/klogg/commit/82cb0104): Enforce limit on line length (Anton Filimonov)
 - [b8865b6c](https://github.com/variar/klogg/commit/b8865b6c): Strict check for raw buffer length (#268) (Anton Filimonov)
 - [be27b07c](https://github.com/variar/klogg/commit/be27b07c): Check for bytes read from file (Anton Filimonov)
 - [4ff11665](https://github.com/variar/klogg/commit/4ff11665): Check for not full read from file (Anton Filimonov)
 - [947ebb0a](https://github.com/variar/klogg/commit/947ebb0a): Check for too long lines (Anton Filimonov)
 - [2d1d5c47](https://github.com/variar/klogg/commit/2d1d5c47): Fix race when file changes during being indexed (Anton Filimonov)
 - [88f06e7a](https://github.com/variar/klogg/commit/88f06e7a): Try fix race during encoding change (Anton Filimonov)
 - [a6b0481b](https://github.com/variar/klogg/commit/a6b0481b): Use QPlainTextEdit for crash reports (#245) (Anton Filimonov)
## Documentation:
 - [d5b372d3](https://github.com/variar/klogg/commit/d5b372d3): [skip ci] Mention options to disable Hyperscan (Anton Filimonov)
 - [d7da0341](https://github.com/variar/klogg/commit/d7da0341): [skip ci] Add link to repositories in linux install section (Anton Filimonov)
 - [76882034](https://github.com/variar/klogg/commit/76882034): [skip ci] Add current milestone badges (Anton Filimonov)
 - [230f1adf](https://github.com/variar/klogg/commit/230f1adf): Add badge from repology (Anton Filimonov)
 - [554d7215](https://github.com/variar/klogg/commit/554d7215): add more badges (Anton Filimonov)
 - [49a0f1d3](https://github.com/variar/klogg/commit/49a0f1d3): Add hyperscan to Notice file (Anton Filimonov)
 - [8c0a49c0](https://github.com/variar/klogg/commit/8c0a49c0): [skip ci] Update build instructions (Anton Filimonov)
## Performance:
 - [7480b18e](https://github.com/variar/klogg/commit/7480b18e): Move conversion to ucs to worker threads (Anton Filimonov)
 - [3b8a581e](https://github.com/variar/klogg/commit/3b8a581e): Reduce allocation on indexing (Anton Filimonov)
## Code refactoring:
 - [e921e726](https://github.com/variar/klogg/commit/e921e726): Use structure for raw lines (Anton Filimonov)
 - [e573bdc0](https://github.com/variar/klogg/commit/e573bdc0): Simplify valid regex check (Anton Filimonov)
 - [3b8454ec](https://github.com/variar/klogg/commit/3b8454ec): User smart pointers for Hyperscan RAII (Anton Filimonov)
 - [cb2838cb](https://github.com/variar/klogg/commit/cb2838cb): Allow to select regexp engine in runtime and compile time (#280) (Anton Filimonov)
 - [28161da2](https://github.com/variar/klogg/commit/28161da2): Try simplify indexing operations enPqueing (Anton Filimonov)
 - [dd87888b](https://github.com/variar/klogg/commit/dd87888b): Remove some includes (Anton Filimonov)
 - [bd32d8e9](https://github.com/variar/klogg/commit/bd32d8e9): Simplify logging macros (Anton Filimonov)
## Build system:
 - [088b9598](https://github.com/variar/klogg/commit/088b9598): Fix build without hyperscan (Anton Filimonov)
 - [678481b7](https://github.com/variar/klogg/commit/678481b7): Make cmake better (Anton Filimonov)
 - [12a93e22](https://github.com/variar/klogg/commit/12a93e22): Fix some install paths (Anton Filimonov)
 - [7440d6ad](https://github.com/variar/klogg/commit/7440d6ad): Remove some additional hyperscan targets (Anton Filimonov)
 - [32238c8d](https://github.com/variar/klogg/commit/32238c8d): Update mac target to 10.13 (Anton Filimonov)
 - [282eaf9c](https://github.com/variar/klogg/commit/282eaf9c): Try fix build (Anton Filimonov)
 - [4df167ae](https://github.com/variar/klogg/commit/4df167ae): Try use fat runtime for hyperscan (Anton Filimonov)
 - [324a3c73](https://github.com/variar/klogg/commit/324a3c73): Remove unneeded installed libs (Anton Filimonov)
 - [4bb9f388](https://github.com/variar/klogg/commit/4bb9f388): Switch to oneAPI TBB version (Anton Filimonov)
 - [25d0cb45](https://github.com/variar/klogg/commit/25d0cb45): Do not install abseil files (Anton Filimonov)
 - [f4a24b8e](https://github.com/variar/klogg/commit/f4a24b8e): Try fix build (Anton Filimonov)
 - [8e6077ea](https://github.com/variar/klogg/commit/8e6077ea): [skip ci] add ebuild for Gentoo (Anton Filimonov)
 - [22818236](https://github.com/variar/klogg/commit/22818236): Do not install efsw files (Anton Filimonov)
## Continuous integration workflow:
 - [bd87f20c](https://github.com/variar/klogg/commit/bd87f20c): Try fix win ci (Anton Filimonov)
 - [a03bfd26](https://github.com/variar/klogg/commit/a03bfd26): Switch to notarize action with longer timeout (Anton Filimonov)
 - [ca106b7b](https://github.com/variar/klogg/commit/ca106b7b): Restructure workflow (Anton Filimonov)
 - [e9d3c1d8](https://github.com/variar/klogg/commit/e9d3c1d8): Add hyperscan deps to workflow (Anton Filimonov)
 - [565a44f3](https://github.com/variar/klogg/commit/565a44f3): [skip ci] fix codeql build (Anton Filimonov)
 - [9aa74db7](https://github.com/variar/klogg/commit/9aa74db7): Build releases for more generic cpu (#290) (Anton Filimonov)
 - [72afe6a7](https://github.com/variar/klogg/commit/72afe6a7): [skip ci] Fix codeql build (Anton Filimonov)
 - [0525c6e7](https://github.com/variar/klogg/commit/0525c6e7): Revert "Move build number to patch version position after bintray incident" (Anton Filimonov)
 - [801d4471](https://github.com/variar/klogg/commit/801d4471): Move build number to patch version position after bintray incident (Anton Filimonov)
 - [dbbb4c88](https://github.com/variar/klogg/commit/dbbb4c88): Simplify codeql build (Anton Filimonov)
## Other commits:
 - [86666e73](https://github.com/variar/klogg/commit/86666e73): update site (Anton Filimonov)
 - [336529f4](https://github.com/variar/klogg/commit/336529f4): [skip ci] News about Hyperscan (Anton Filimonov)
 - [d006bca4](https://github.com/variar/klogg/commit/d006bca4): Update qdarkstyle to 3.0.2 (Anton Filimonov)
 - [9f1f8762](https://github.com/variar/klogg/commit/9f1f8762): Collect current vm use in dumps (Anton Filimonov)
# 2021-03:
## New features:
 - [1c8ad2b4](https://github.com/variar/klogg/commit/1c8ad2b4): Add context menu to save current search as filter (#253) (Marcin Twardak)
## Bug fixes:
 - [7ad885fd](https://github.com/variar/klogg/commit/7ad885fd): Some fixes for predefined filters (Anton Filimonov)
# 2021-02:
## New features:
 - [1a13b4be](https://github.com/variar/klogg/commit/1a13b4be): Add import/export feature to predefined filters (#248) (Marcin Twardak)
  - [e1af31ef](https://github.com/variar/klogg/commit/e1af31ef): Do not codesign on pull request checks (Anton Filimonov)
## Bug fixes:
 - [6b5a6e69](https://github.com/variar/klogg/commit/6b5a6e69): Sync data before reading and check window ptr (#255) (Anton Filimonov)
 - [b4c8548f](https://github.com/variar/klogg/commit/b4c8548f): Add safety check for qChecksum (#228) (Anton Filimonov)
## Continuous integration workflow:
 - [5bd5a7ed](https://github.com/variar/klogg/commit/5bd5a7ed): Update codesign action (Anton Filimonov)
 - [18a66e9e](https://github.com/variar/klogg/commit/18a66e9e): Remove more codsign from PR validation builds (Anton Filimonov)

 - [0974b578](https://github.com/variar/klogg/commit/0974b578): Make predefined filters dialog more similar to highlighters (Anton Filimonov)
# 2021-01:
## New features:
 - [be9bb39b](https://github.com/variar/klogg/commit/be9bb39b): Refactor predefined filters (#191) (Anton Filimonov)
 - [869a09a5](https://github.com/variar/klogg/commit/869a09a5): Add drop-down menu with predefined filters (#241) (Marcin Twardak)
## Build:
 - [d6a78a41](https://github.com/variar/klogg/commit/d6a78a41): Add std::hash<QString> for older Qt versions (Anton Filimonov)
## Documentation:
 - [2b27b1f9](https://github.com/variar/klogg/commit/2b27b1f9): [skip ci] Add a screenshot to readme (Anton Filimonov)
 - [83a94380](https://github.com/variar/klogg/commit/83a94380): [skip ci] Add some screenshots (Anton Filimonov)
 - [4ef7f145](https://github.com/variar/klogg/commit/4ef7f145): Update DOCUMENTATION.md (lilventi)
## Continuous integration workflow:
 - [04333561](https://github.com/variar/klogg/commit/04333561): Another codesign fix (Anton Filimonov)
 - [6f880565](https://github.com/variar/klogg/commit/6f880565): Fix codesign (Anton Filimonov)
 - [00f4d90a](https://github.com/variar/klogg/commit/00f4d90a): Fix codeql workflow (Anton Filimonov)

# 2020-12:
## Other commits:
 - [e6a2c422](https://github.com/variar/klogg/commit/e6a2c422): Add build arch to crash report (Anton Filimonov)
