# Qualified CI environments

[Documentation](README.md) · [Build guide](BUILD.md) · [Dependencies](DEPENDENCIES.md)

## Current status

All six Linux environment families have independently verified digest pins and
detached evidence in `ci/environments/lock.json` and `ci/environments/evidence/`.
Their original qualification source is
`9e3c09ec1d93a437f26c0fa660370a9ee2ad6a1b`, run
[38853661268](https://github.com/ZEACENT/klogg/actions/runs/38853661268), attempt 1.
The complete flow passed 55 checks: 25 successful and 30 mode-specific skips;
all 55 annotation endpoints were empty. Six candidates, ten qualification
roles, the strict Gate, protected Publisher and anonymous verifier actually
succeeded in that same source/run/attempt.

Independent verification checked six anonymous OCI manifest/config pairs and
12 detached signatures. All 31 original pin/evidence proposals were adopted
byte-for-byte; their source bindings were not rewritten. This replaces the
earlier source `50b9db1e` authority, not its historical outcome. Metadata and
signature verification does not claim independent execution of image layers.
The publication environment uses branch policy; its empty run review history
is not human PR approval or native-core qualification.

Ordinary Linux application CI consumes these locked environments rather than
rebuilding them on a cold BuildKit cache. It still rejects changed declared
recipe or policy bytes before image pull. Never substitute calculated hashes
for fresh producer evidence, splice successful roles across attempts, or treat
an earlier source's qualification as current-head application validation.
Native production-core consumption and controlled C++ reuse measurements
remain separate, unfinished work.

### Historical qualification and consumer failures

Earlier ADB lock/verifier changes invalidated four packaging families, blocking
four package jobs plus Jammy ASan/LSan and UBSan. The shared Docker build action's
optional-metrics change also invalidated TSan and analysis policy. Those stale
receipts failed closed; later successful publication does not erase the failures.
Evidence-only run
`36380763036` built all six candidates, but its Linux package fixture failed
while fetching an AOSP libusb archive with HTTP 503, so four package roles
were skipped and no replacement receipt was issued. The PR and fixture now
share a bounded source acquisition path: only a terminal Gitiles archive 503
can rebuild the locked manifest or libusb from their original, pinned Git
objects, and the unchanged full validator still checks every cached source
before offline extraction. A third producer attempt then encountered HTTP 503
while fetching the pinned `aosp-adb` archive; the repeated source-host failure
motivates a separate, optional already-verified cache seam. Environment run
`36447989583` imported source bytes and reached the offline prefetch validator,
which generated a manifest, but the run failed and issued no qualification
receipt. Follow-up correction `41c06eb7` keeps the imported cache archive-only
after offline verification. Later read-only run `36659032071` qualified all six
families at source `789de777`, attempt 1; that evidence remains bound to that
source and does not qualify later changes. Fresh run `38123062934` at `45daf916`,
attempt 1, completed with 13 successful, nine failed and 33 skipped checks. Its
six candidates and Linux fixture succeeded, but all seven Linux qualification
roles failed before compilation when build-private efsw cloning rejected the
host-restored cache Git directory for dubious ownership. Static analysis
independently rejected C varargs and ignored C output results in the optional
search-start trace. Coverage and CodeQL profile receipts cannot replace those
failed roles; the aggregate Gate stopped before artifact downloads and issued
no six-family qualification. At that checkpoint the production pins remained
unchanged and stale; its partial results were not promoted.

The complete ordinary source `00f1e3e7` audit covers 48 checks across CI Build
`38146809545`, its three analysis runs and lint: 25 passed, 15 failed and eight
were skipped. All seven Linux consumers and three analysis resolvers independently
rejected stale policy before application build or analysis. Hosted Python 3.8.18
lint exposed a real Git-timeout descendant-lifetime gap. All three Windows lanes
passed their pre-existing native suites but failed all three new trace invocations
before their intended executables: inherited PATH semicolons became separate
CMake command arguments. All four macOS lanes actually passed 38/38 CTest,
including the three trace modes, with explicit Xcode 26.6 and the existing
Intel 15.0/ARM 14.0 floors. Package checks remain unsigned validation, not release
or native-core qualification. Replacement source needs fresh relevant acceptance
and one complete same-source/same-attempt six-family qualification; old successful
roles cannot be spliced into it.

Optional build telemetry stays non-gating. A rejected report retains its producer
notice, including conflicting wall/Ninja values when available, but is not
uploaded if the exact current-run report is absent. This avoids treating missing
optional evidence as an upload warning without weakening timing validation or
inventing an explanation for the discrepancy. No older metrics file or candidate
receipt substitutes for current qualification.

Ordinary ADB source transport is a separate, automatic **bounded bytes-only**
path for ordinary `pull_request` and canonical `master` push (including a squash
merge). It imports only current-lock archive bytes from a prior Actions
artifact retained for 90 days; it does not produce an environment fixture,
qualified dependency core, signed receipt, or publication authority. The
read-only selector searches at most 20 name-filtered artifacts from this
repository and downloads at most two candidate ZIPs. A current-lock exact v2
cache hit with corrupted or substituted bytes is fatal: it must not be rescued
by a different artifact, a reconstructed archive, or an online download. A
missing compatible cache or invalid older v1 artifact can fall back to the
normal bounded locked-source acquisition path. Master squash reuse may cross
branches, but conveys only archive bytes, never older receipts or policy.

Producer-event eligibility is separate from canonical provenance. A PR consumer
accepts only PR producers; a canonical master push accepts PR or push producers.
After canonical metadata validation, discovery skips excluded producer events
such as `workflow_dispatch` without downloading their ZIPs. It can then try an
older eligible artifact or return the existing no-compatible-source status to
reach locked-source acquisition. Missing/malformed canonical metadata and an
eligible producer's attempt, digest, or ZIP corruption remain fatal. Dispatch
artifacts are not admitted by this exclusion rule, and no old receipt is reused.

Every accepted archive must match the current lock's exact file name and **raw**
SHA-256, within the 536870912-byte aggregate limit, before the original prefetch
validator checks the closure again with `--offline --max-cache-bytes 536870912`.
Transport does not carry a prior prefetch manifest: the ordinary job regenerates
its own current manifest before extraction or build. These steps do not alter
the signed analysis-image recipe or replace environment qualification.

An ancestor run's retained source cache supplies **archive bytes only**, not a
qualified fixture, producer identity, publication authority, or permission to
reuse an old receipt. For `prepare-fixture`, pass
`--source-cache-root /path/to/already-verified-archives` only after verifying
that ancestor's artifact provenance and archive bytes independently. The input
must be a real directory containing exactly the lock-listed regular archives;
symlinks, hardlinks, special files, missing or extra paths, incorrect raw
SHA-256, and aggregate bytes above 536870912 fail before any online source
helper runs. The fixture copies these bytes into its private `work/source-cache`,
then runs the original prefetch validator offline once without extraction and
again with `--offline --extract-root`. Omit the option to preserve the existing
online closure acquisition. The current checkout, same-run fixture validators,
package tests, and protected qualification/publisher gates remain mandatory.

Native dependency cores are a separate, unfinished migration. The seven-target
`ci/dependencies/catalog.json` defines five ADB and two iOS cores, but there is
**no** `ci/dependencies/lock.json`, no published production core digest, and no
cross-version package consumer. `dependency-mode=qualify` starts isolated
builders, but the iOS host probe records selected tools and fails before iOS
source compilation because the Autotools/pkgconf, Perl, and m4 inputs have not
been reviewed and pinned. The seven-way Gate therefore cannot issue a receipt.
`dependency-mode=publish` has no registry write permission and deliberately
fails; a passing local contract suite cannot authorize it.

The macOS 26/Xcode 26.6 workflow migration is staged but not fully qualified.
Hosted native builders in PR run `36375653076` observed Intel image
`macos-26/20260824.0517.1` and ARM image
`macos-26-arm64/20260907.0351.1`. Both matched the reviewed Xcode 26.6
build `17F113`, SDK 26.5, and Apple clang 21 identity. These successful
native builds are not package, old-OS runtime, or seven-core qualification.
The changed ADB lock also invalidates older full-lock-bound source/legal
receipts. The subsequent metrics-action change invalidates all six Linux
policy identities. Keep the Intel 15.0/ARM
14.0 deployment floors separate from the new SDK version.

Do not create placeholder locks, substitute mutable tags, or fall back to
building an environment or native core when verified consumption fails.

## Windows Qt 5 runtime input

The OpenSSL runtime is a separate Windows Qt 5 input, not a seventh Linux
compiler environment or one of the native dependency cores. The old vendor URL
returned HTTP 400 HTML while curl reported transport success. Existence-only
upload and cache-hit bypass let that response reach unchecked consumer
extraction; adding `curl --fail` alone cannot recover a discontinued download.

[`ci/tools/openssl-qt5.json`](../ci/tools/openssl-qt5.json) binds the original
OpenSSL 1.1.1w ZIP and four runtime DLL identities. Producer acquisition, restored
cache validation, and Windows consumption share
[`scripts/materialize_openssl.py`](../scripts/materialize_openssl.py). Every
accepted input must satisfy bounded ZIP/member/CRC/version/PE checks before
upload, save, or runtime materialization. A lock-digest cache key has no old-key
restore prefix; a corrupt exact hit, including a reported hit with no archive,
fails instead of acquiring another source. Existing destinations are never
silently overwritten. Consumers remain offline and Qt 5-only.

Cold acquisition reads the fixed `openssl.zip` asset from the separately
approved, non-Latest
[dependency release](https://github.com/ZEACENT/klogg/releases/tag/openssl-qt5-1.1.1w-input-v1).
Schema 2 pins repository ID `1094649085`, release ID `415601829`, asset ID
`649642882`, the exact tag and canonical download URL. Acquisition checks the
repository identity, non-draft release, release membership and independently
queried asset metadata before a size-bounded download. Full offline verification
still precedes no-overwrite publication. There is no fallback to a historical
Actions artifact, an unverified cache, or a different OpenSSL version.

The archive remains byte-identical to the original input (15,897,774 bytes,
SHA-256 `1870b15bf6749e65ffbbadf52cdff3ee0e9f02943550bf4395574bb432af3eb8`).
Its original OpenSSL/SSLeay license is retained inside the archive and as a
release sidecar, together with acknowledgments, checksums and the historical
Actions origin record. Before this one-time preservation, artifact `10928159537`
from run `36309613109`, original attempt 1, producer `108592990081` was
independently authenticated and both outer and inner byte layers were verified.
That evidence records origin, not vendor-signature proof or current-source
qualification. Routine cold acquisition does not need those historical APIs.

The former Actions artifact expires at **2026-12-26T09:30:51Z**; the release
asset has no Actions retention deadline. Administrators can still delete a
release or asset, so this is not a claim of immutable hosting. Never replace an
asset in place: any input change needs a separately reviewed release and lock.
CI only consumes the durable input with job-local read permissions and an
acquisition-step token; it cannot publish a replacement. Fork restrictions are
not bypassed. The ordinary transient upload supplies the Windows job in the
same run, while the cache is only an accelerator. No application version,
OpenSSL version, runtime DLL identity, or publication permission changes.

The shared transport and source-download helpers retain their existing Linux
qualification-policy bindings; this migration does not modify those helper
bytes or relabel signed evidence.

Run the fast offline regression guard from the repository root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/scripts -p 'test_materialize_openssl.py'
PYTHONDONTWRITEBYTECODE=1 python3 scripts/lint_ci_quality.py
```

These guards cover the producer/consumer boundary absent from local macOS
application builds, including cold acquisition after the former artifact's
expiry, substituted release identities, and corrupt/oversized downloads. To
exercise the original binary input too, set `KLOGG_OPENSSL_TEST_ARCHIVE` to the
absolute path of an independently downloaded archive when running the OpenSSL
test module; an explicit missing fixture fails rather than silently skipping.
They do not replace fresh Windows x86 compilation, actual native dump/unit/UI
execution, six-environment qualification, or independent native-core qualification. The x86 CI leg currently disables packaging, so its
passing tests must not be described as TLS package qualification.

## Ownership

Application CI should build and test the current application, not repeatedly
construct its compiler environment. Three different things have different
lifecycles:

1. **Compiler environments:** distribution, compiler, Qt, and build tools.
2. **Product dependencies:** immutable helper binaries and their source/legal cores.
3. **Application outputs:** current-source executables, tests, packages, and
   version-specific source-publication overlays.

The production lock and ordinary consumer currently cover compiler
environments. ADB/iOS binary cores have candidate and read-only qualification
machinery only; the Linux package qualification fixture does not imply that
those cores have been published or reused.

`ghcr.io/zeacent/klogg-ci-env` is the intended durable environment authority.
BuildKit, compiler, and download caches are optional accelerators, not the source
of truth. A warm cache does not count as qualification.

| Family | Required qualification |
| --- | --- |
| `focal-qt5-gcc13` | AppImage build, full tests, packaging, and extraction/execution of the final AppImage (AppRun plus packaged helper), not just the intermediate appdir |
| `jammy-qt5` | DEB, ASan/LSan, and UBSan as separate current-source builds |
| `noble-qt6` | DEB build, full tests, packaging and package checks |
| `resolute-qt6` | DEB build, full tests, packaging and package checks |
| `jammy-qt5-tsan` | Instrumented Qt, runtime/plugin checks, corresponding-source checks, and full TSan tests |
| `noble-qt693-analysis` | Static analysis, coverage ratchet, and real CodeQL extraction/analysis |

Clean-distribution DEB installation tests may use APT to check the resulting
package. This is package verification, not fallback compiler provisioning.

## Identity and trust

Authoritative declarations live under `ci/environments/`:

- `recipes.json`: family, platform, recipe files, fixed build arguments, profiles.
- `materials.json`: exact base images, APT acquisition stages, tool/source URLs,
  content hashes, licenses, source mappings, and explicit archive layouts.
- `profiles.json`: effective build/test/package settings and verification files.
- `role-materials.json`: CodeQL-only official tool material.
- `publisher-tools.json`: the pinned ORAS publisher tool.
- `package-tools.json`: pinned Linux package qualification tools.

Three identities are deliberately separate:

- **Recipe identity:** the declared build contract, not an unrelated app commit.
- **Input identity:** the actual acquired package/archive closure and transformations.
- **Policy identity:** required qualification settings, tools, and verifier bytes.

A candidate also records the exact application commit, producer workflow,
branch, run ID, and run attempt exercised. An Actions artifact ID comes from the
upload job's authenticated output, not from a self-referential manifest inside
that artifact.

The builder exports an authoritative OCI archive and a companion Docker archive
in **one BuildKit invocation**. Some Docker stores cannot load OCI archives.
Qualification therefore loads the companion, then checks its configuration
digest and rootfs DiffIDs against the untouched OCI image. Publication copies
the original OCI content; it must not rebuild or export another image after tests.

The qualification gate requires the exact six builder jobs and ten qualification
jobs, all successful, plus matching source/run/attempt, artifact IDs, raw archive
hashes, profiles, and current policy. A failed, skipped, canceled, missing, or
substituted job prevents publication. Qualify-only evidence cannot be promoted
into publish-mode evidence.

Production consumption additionally requires detached cryptographic provenance:
raw registry manifest bytes and the qualification receipt must match signatures
from the expected repository, producer workflow, source commit/ref, and hosted
runner. Parsing a receipt with `"result": "passed"` is not verification.

## Acquiring inputs and building offline

APT acquisition uses fresh private indexes, signature verification, explicit
sources, exact package versions, and retained package/index hashes. Every
materialized stage must also pass independent network-free installation.
Prerequisite stages are replayed in their declared order; TSan source, builder,
and runtime stages each depend on the CA bootstrap, not on one another.

The TSan snapshot and Qt/compiler versions remain pinned. Its temporary CA
bootstrap exception is scoped to the snapshot host, keeps APT signature checks,
and is removed on success or failure. No fallback to a current Ubuntu mirror is
permitted. Focal PPA trust is scoped to retained public key bytes and full
fingerprint selectors, never global `apt-key` acquisition.

For local material acquisition on a Linux Docker host:

```sh
python3 scripts/materialize_ci_environment.py \
  --repo-root "$PWD" --family jammy-qt5 --output /tmp/klogg-jammy-inputs
```

Choose a new output directory. The result contains `inputs.json` and `materials/`.
It is not a production lock. Candidate construction uses:

```sh
python3 scripts/build_ci_environment.py \
  --repo-root "$PWD" --family jammy-qt5 \
  --inputs /tmp/klogg-jammy-inputs/inputs.json \
  --materials /tmp/klogg-jammy-inputs/materials \
  --source /path/to/recorded-producer-source.json \
  --output /tmp/klogg-jammy-candidate --builder your-oci-capable-builder
```

Use recorded source context and the matching checkout for reproduction; do not
invent a successful GitHub run. A local build is unqualified until the controlled
workflow has exercised it and produced authentic evidence. The builder needs an
OCI-capable BuildKit backend and never changes the machine's default builder.

The ordinary Dockerfiles retain an explicitly selected online path for local
source builds during bootstrap. The producer catalog fixes the locked path and
uses `--network=none`; an absent locked input is an error, not a reason to select
the online path.

## Producing and publishing environments

Environment refresh uses the registered `CI Build` workflow at a reviewed ref.
Its `environment-mode` input is `off`, `qualify`, or `publish`. Producer mode
requires the exact source SHA and an ancestor analysis base SHA, and cannot be
combined with dependency production or signed-release qualification.

1. Run local quality checks and review the complete producer changes.
2. Ensure `ci-environment-publish` is a separately protected GitHub environment
   restricted to approved branches. Do not reuse an unrelated environment.
3. Dispatch the registered workflow at the reviewed ref with explicit source and
   analysis-base SHAs. `qualify` exercises candidates without package publication;
   `publish` also requires successful isolated SARIF processing.
4. Read-only builders and qualifiers produce immutable same-run artifacts.
5. Only the protected publisher receives package/OIDC/attestation write access.
   It rechecks all qualification evidence before copying images and emitting
   provenance. It never runs a candidate image.
6. A separate read-only job verifies public anonymous retrieval and detached
   signatures and writes a lock proposal, not an automatic repository edit.
7. Review all proposed locks, inputs, source mappings, and exact-byte evidence.
   Only then migrate ordinary consumers and run current-head platform CI.

Producer-only skipped ordinary jobs have distinct display names. In particular,
a producer dispatch must not shadow a real application `ci-gate` with a skipped
check of the same name. Normal PR, push, and ordinary dispatch gates remain strict.

## Native dependency boundary

`ci/dependencies/catalog.json` enumerates five ADB and two iOS targets. Candidate
archives contain only the helper and its target runtime files, or the iOS dylibs
and direct aliases; source, license, smoke, and application-version receipts stay
outside the immutable binary core. The parent `ci-build.yml` dispatch checks an
exact source SHA and carries seven builder results plus the independent ADB legal
job to the read-only `ci-dependencies.yml` Gate. The Gate checks immutable Actions
artifact IDs, the parent run/attempt, hosted runner labels and reviewed workflow
runners, signed **full tar bytes**, candidate/full binary equality, and current
legal/source material before it can create its eight-file qualification archive.
A candidate-only receipt is not a signed production lock.

The ordinary ADB legal producer generates its complete release closure only
once. `build_adb_helper_legal_assets.py --package-core-output` additionally
projects the package-required immutable core files and checksum sidecars,
without a source archive, source offer or versioned overlay. The original full
legal and package-support artifacts keep their publication and application
package consumers. Only the five raw helper builders consume the new package
core. Like the existing support route, it omits the corresponding-source
archive: this split adds neither per-builder archive transfers nor duplicate
source generation. It establishes an unversioned input boundary, not a measured
build-speed improvement.

After each raw build creates its original `receipt.json`, the shared build
action runs `--package-overlay-only` with the received source-set and original
build receipt SHA-256 pins. It writes a fresh, disjoint package-support root
before the unchanged package smoke, legal verifier and checksum steps. The
current version comes from `SaveVersion` through the legal job output, not from
the immutable core. Assembly accepts no source acquisition inputs and obtains
the source-offer archive identity from the validated, pinned source-set receipt.
Package scope rejects the source archive and its sidecar; full `--overlay-only`
still requires and verifies the actual complete archive. A package projection
alone does not prove that a corresponding-source asset has been published.

These ADB paths require caller-owned staging. Fresh-output, path, regular-file,
receipt and hash checks reject stale or changed material; failed core projection
retains its partial output rather than repairing foreign entries. The existing
path checks are not atomic isolation against arbitrary concurrent same-UID
namespace changes. Receipt pins establish cold byte continuity, not signed
production-core authority or an authenticated cross-run consumer.

The ordinary iOS builder now uses the existing immutable legal-core boundary:
its disconnected native writer creates a separate original stack, legal
`--core-only` generation creates an unversioned source/legal core, and
`build_ios_native_legal_assets.py --assemble-stack` combines that core's pinned
overlay with the original binaries, installed headers, direct dylib aliases,
file modes and build receipt. Assembly requires POSIX file-mode/alias support,
the original source-set, legal and build receipt SHA-256 pins, and a nonexistent
output separate from both inputs. It rejects undeclared libraries, escaping
aliases, special permissions,
changed inputs and unexpected output entries. A failed copy retains partial
output instead of deleting possible foreign entries; the ordinary workflow
publishes only after successful assembly and its existing checksum step. Use a
caller-owned staging parent: child creation, copying, aliases and metadata use
held directory handles, not a sandbox against arbitrary same-UID changes to
that parent's bootstrap.

These cold-writer pins establish local byte continuity, not authenticated
cross-run reuse. The original architecture receipt alias, complete legal
verification and downstream package layout stay unchanged. True acquisition-free
reuse still needs a signed companion binding the original headers, receipts,
corresponding source and legal/support assets to the chosen binary core and
current qualification policy. No such production lock is activated here.

`ci_dependency_full_evidence.assemble_authenticated_ios_stacks()` provides an
offline bridge from the existing seven-target full-artifact authentication to
that assembler. It verifies all seven signed full subjects, scheduler bindings,
checksums and candidate/full binary closures before either iOS output is created.
Both output paths are checked together and must be fresh, disjoint from each
other and outside the context-owned temporary tree. Receipt pins come from the
original verified full roots; binary-only cores never supply installed headers
or legal material. Assembly itself neither acquires sources nor executes native
payloads. Failed output remains partial and must not be published.

The full-release view permits only the original native entries and the bound
legal/overlay inventory. The exact architecture-specific receipt copy must match
the canonical build receipt and remains original native content, not ignored
legal metadata. POSIX extraction restores recorded directory permissions,
including the archive root, before assembly preserves native file/directory
permissions and dylib aliases. Full-tar directory modes must retain owner read,
write and search permission and have no special permission bits; unsupported
read-only/inaccessible directories are rejected before extraction rather than
making owned staging impossible to reclaim or repairing a foreign namespace.
The full projection rechecks file/directory modes
after external verifiers as well as rechecking bytes and links. This bridge is
not an ordinary acquisition-free consumer, a signed qualification receipt, a
production lock, enforced host-tool closure or permission to publish candidates.

The pinned Xcode, SDK path/version, clang, CMake, and Ninja observations are
necessary but not sufficient for iOS reuse. The dependency-only probe records
the selected Autotools/pkgconf, Perl, and m4 executable paths, versions, hashes,
and installed Brew versions, then intentionally fails before source compilation.
Historical successful runner logs are not current bottle identities or a
qualification. Review and pin the real input closure for **both** macOS runner
architectures before allowing the Gate to succeed. Normal PR/push iOS source
builds do not run this dependency-only probe.

A separate, **unreviewed** host-input diagnostic can be dispatched through the
registered CI Build workflow on a non-default canonical branch with
`qualification-mode=validation`, `environment-mode=off`,
`dependency-mode=observe-ios-host`, and the exact full `expected-source-sha`.
It calls the source-local `ci-ios-host-evidence.yml` reusable workflow: one
locked iOS source prefetch and two macOS host observations, with no dependency
builders, Gate, registry access, or production-core upload. Diagnostic JSON is
bound to the run/attempt/SHA, size-limited and retained for seven days. The
collector distinguishes published Homebrew bottle metadata from **verified**
poured bytes and reports `complete: false` whenever the actual process,
interpreter/module, dylib, or bottle closure remains unknown. A failed trace
capability probe stops before the expensive native build; even a successful
capability probe does not qualify an untraced build. These diagnostic failures
are expected evidence gaps, not permission to bypass the original Gate.

`materialize_ios_host_tools.py` can create and independently recheck a private,
architecture-bound, byte-exact **unreviewed** snapshot of a specifically
provided local tool tree on POSIX hosts. A canonical manifest lives inside
the private, atomically no-replace-published output directory. It rejects
changed modules/macros, extra files, nonportable links, hardlinks, mode drift,
unsafe paths, and stale manifest bytes before that snapshot can be reused as
diagnostic data. No real Brew,
Xcode, or system tree is pinned by this test-only tool, and a verified snapshot
alone does not prove which files a native build reads. The hosted DTrace probes
did not establish build-wide observation; no supported, enforced macOS host
read/execute boundary has yet passed child-process and negative-access checks.
Until that separate capability and actual per-architecture materials are
reviewed, iOS core qualification stays blocked.

Native publication remains disabled: the child Publisher has no package or OIDC
write scopes and exits with an error. The `publish_ci_dependency.py` library has
no connected independently authenticated Gate callback, detached signing step,
or production lock writer. Its anonymous manifest/blob check has not been run
against a real publication. Do not dispatch publication or invent seven registry
digests. A future consumer must verify the reviewed lock, both detached
Sigstore subjects, current core/policy keys, and exact OCI manifest/blob bytes
**before** using a core. `stage_core()` currently offers
private, no-replace binary staging from an externally authenticated digest.
The Windows path enforces owner-only ACL acquisition and held-handle namespace
checks; its filesystem contracts do not make it a signed production consumer.
Existing ADB/iOS schema-1 build receipts bind the
full source lock and cannot be reinterpreted as cross-version core receipts.

## Consuming locked environments

Ordinary CI never builds an environment image. Two consumption shapes exist:

- `ci-build.yml` Linux legs run on the host and use
  `.github/actions/prepare-linux-environment`, which verifies the reviewed lock,
  signed provenance, and public registry identity, then pulls the exact digest
  and assigns the fixed local tag the existing build/test/package actions expect.
  All Docker invocations in these legs use `--pull=never`; a missing local image
  is a consumption failure, not a reason to rebuild. Shared configure, build and
  test containers also use `--init` so orphaned Git descendants are reaped before
  process-group disappearance is confirmed. The clean-distro DEB installation
  smoke checks remain a narrow, deliberate pull exception because their
  package-manager use is the behavior under test.
- The CodeQL, Coverage, and Static analysis workflows resolve the locked
  `noble-qt693-analysis` digest in an upstream `ResolveLinuxEnvironment` job
  (verification only, no pull) and run the expensive job as a whole-job
  container on exactly that digest with explicit `container.options: --init`.
  The analysis qualifier likewise uses `docker run --init` for every role. Init
  is supplied by the host Docker runtime, outside the image's OCI digest; no
  init binary is bundled into the image or application, and its version is not
  inferred from diagnostic hosts. An unavailable init is not silently bypassed.
  Whole-job containers execute as root while the mounted workspace stays owned
  by the runner user, so the first step after
  checkout grants git a `safe.directory` exception; raw git commands fail
  closed without it. Host provisioning (apt installs, agent-setup) must not
  reappear in these jobs; the image already carries the compilers, analysis
  tools, Qt, and Boost, and only the shared CPM source cache is restored on top.

Build-private efsw preparation has a distinct Git ownership boundary. It first
verifies the approved shared-cache source digest, canonicalizes the cache path,
and requires ordinary, non-redirected Git metadata. The clone trusts only that
cache's exact `.git` directory, not its worktree, parent, or a wildcard. A plain
command-line `-c` exception is insufficient on older Git transports: their
`upload-pack` child can lose command configuration and reject the source again.

Preparation writes an owned temporary protected configuration beside its private
staging checkout: an empty `safe.directory` reset followed by the exact verified
Git-directory entry. Bounded config-writing commands run inside that owned
context with a private-parent discovery ceiling and repository-selector variables
unset, so an unrelated or unmapped caller worktree cannot affect file creation.
Only the clone process selects this file through
`GIT_CONFIG_GLOBAL`; inherited command-configuration parameters/count are unset
for that invocation. This is process-scoped selection of a **GLOBAL-scope input
file**, not an in-memory exception or modification of the user's/system's
persistent configuration. It replaces user-global settings for the offline local
clone and its children, without persisting trust in the cloned repository.

Preparation Git operations use a private Python 3.8-compatible process owner,
not CMake's timeout cleanup. CMake 3.31's `execute_process(TIMEOUT)` can return
without terminating its child, unlike the terminating implementations in older
KWSys-based CMake and CMake 4.2. Removing an environment wrapper alone therefore
does not repair lifetime ownership. The selected host interpreter is passed
explicitly; standalone preparation discovers a compatible interpreter or fails
before timed work. CMake 3.14 remains supported, without version-dependent paths.

Each operation retains its existing deadline and a copied child environment.
The trusted Python owner uses isolated standard-library imports, while Git still
receives the original Python-related environment. CMake records the original
`LC_CTYPE` presence/value before interpreter startup so Python's C-locale coercion
cannot silently add or replace that variable in Git's environment. Anchor
startup receives the restored environment through a private channel, not an
argument containing the caller's environment or secrets.
On POSIX, an owned session/group anchor remains unreaped until the single
terminating group signal, so termination cannot target a recycled group ID.
After bounded anchor reap, only signal-zero existence queries are permitted;
cleanup requires `ESRCH`, not anchor exit or output EOF alone. Group presence and
`EPERM` remain uncertain until the existing cleanup deadline; other query errors
fail closed. Reuse of a numeric group ID can cause a conservative failure, but
no terminating signal is sent after reap.

This disappearance barrier requires orphan reaping by the surrounding host or
container init. A nonreaping Python PID1 can retain zombie group members after
their output and sockets close, preventing `ESRCH` throughout the cleanup budget.
The explicit init prerequisite preserves normal success and timeout semantics
without weakening the barrier. Native macOS can temporarily report `EPERM` for
an exiting group before reporting `ESRCH`; permission failure alone never proves
quiescence. CI lint and actual Docker-argument contracts guard the required init
options on the configure/build/test and analysis routes, not unrelated package
or ADB fixture launches.

On Windows, suspended creation atomically attaches an owned Job Object and
restricts inherited standard handles before the command is resumed;
completion-port active-process-zero observation precedes handled filesystem
cleanup. Atomic job-list assignment requires Windows
10 or Windows Server 2016 or later on the native build host; unavailable or denied
containment fails closed, without a direct-child-only fallback. Ordinary Git
family lifetime is covered, not arbitrary intentionally detached processes or
an unbounded all-dependency transaction. Other callers of the shared source
verifier keep their existing behavior; preparation explicitly selects the owned
revision-query path.

The supervisor finishes handled process cleanup before CMake removes its owned
temporary context or partial staging. The context is removed before checkout on
normal clone completion or a failure with confirmed cleanup. If cleanup cannot
establish quiescence, preparation fails without deleting potentially live context
or staging; these uniquely named residues must never be reused or overwritten.
Abrupt process termination can likewise leave private residue.
The shared cache stays unchanged, and the detached private checkout must still
match the pinned revision and clean tree digest before private patching and
final-tree verification. Preparation, its shared verification dependency, the
process owner and interpreter wiring are explicitly included in qualification
policy file digests; importing a sibling file alone does not bind its bytes.

The CodeQL job additionally fetches the official CLI bundle pinned in
`ci/environments/role-materials.json` via `scripts/fetch_codeql_bundle.py`,
verifies its SHA-256, and passes the tarball to the pinned init action through
its `tools` input. The bundle stays in the job's private directory and is never
republished (the CodeQL CLI license prohibits redistribution).

A pull, identity, provenance, or receipt failure stops the job before the
expensive application build. There is no digest override, floating tag, or
silent rebuild fallback on the consumer side.

A public repository does not make a newly created GHCR package public. The owner
may need to change package visibility in GitHub's settings. Public visibility is
irreversible. If anonymous verification fails, keep the qualified publication
evidence and resolve visibility; do not fabricate a lock or rebuild/republish
merely to retry the read-only check. Private or missing packages are not retried
as if they were transient transport errors.

## Source and tool distribution

Retain distribution package origin/version evidence and applicable notices.
The TSan image must carry the exact original Qt archives, applied patch, build
recipe/helpers, instructions, and upstream licenses at
`/usr/share/klogg-ci/qt-sources`. Qualification verifies this payload before it can
be distributed with the image.

CodeQL is different: its CLI license prohibits redistribution. It is **not** in
the shared image, project packages, or transferred Actions artifacts. Only the
CodeQL role downloads the SHA-locked official archive, uses its bundled query
packs, and performs fresh uncached tracing. The database census must account for
current first-party and generated MOC/RCC units. Only permissible evidence and
SARIF leave that role; SARIF upload occurs in a separate narrow-permission job.

Windows Ragel/pkgconf tools are selected from SHA-locked MSYS2 packages in
`ci/tools/msys2-tools.json`. The materialized ZIP contains the necessary tools,
runtime files, notices, and corresponding Ragel/GCC source archives, not a
MinGW compiler substituted for MSVC. Boost transport is pinned and verified
before reuse. Never package runner-owned Xcode, Visual Studio, or CodeQL into
this artifact layer.

## Refresh, failures, and rollback

Input refresh is an explicit producer change, not a floating consumer refresh.
Update genuine upstream identities, review changed source/license obligations,
materialize the new closure, and run every affected qualification role. A
verifier-only change requires requalification; it does not justify claiming that
an unchanged dependency binary was rebuilt.

Consumption errors stop before the expensive app build:

- Missing/stale recipe, input, or policy: produce or requalify the intended artifact.
- Wrong source, signature, archive hash, or loaded image: investigate substitution
  or corruption; never suppress the check.
- Snapshot failure: repair acquisition of the pinned inputs without changing the
  snapshot implicitly.
- Transient registry transport failure: bounded retries preserve the same digest.
- Private/missing registry object: resolve publication/visibility, not a local build.

Rollback is a reviewed lock/evidence update to a previously qualified digest
whose recipe, input, and policy contracts still match. Do not move a mutable tag
and assume locked consumers changed, or discard source/provenance evidence.

## Validation and measurements

```sh
python3 scripts/run_ci_quality.py --json
python3 scripts/lint_ci_quality.py
python3 -m unittest discover -s tests/scripts -p 'test_*ci_environment*.py'
python3 -m unittest discover -s tests/scripts -p 'test_ci_dependency*.py'
actionlint -shellcheck= .github/workflows/ci-build.yml .github/workflows/ci-dependencies.yml
```

Unit and workflow-contract tests cover malformed input, substitution, incomplete
qualification, privilege boundaries, and event projections. They do not replace
real image builds, full sanitizer tests, package smoke tests, CodeQL tracing,
anonymous retrieval, or current-head cross-platform acceptance.

`scripts/ci_build_metrics.py run-build` wraps the single unchanged `ci_build`
invocation on Linux, macOS, and Windows. Each build leg attempts to upload
`ci-build-metrics-<run-id>-<attempt>.json` with the fresh Ninja log summary,
measured build wall time, and current-run global ccache counter deltas where
available. Missing or invalid telemetry and failed metrics uploads cannot
change a successful build's status; no report is published without fresh
valid evidence. Windows reports its object cache as `disabled`; no Windows
object-cache hit is inferred.
Work-time sums are not elapsed time or critical-path measurements. Do not
serialize normal builds, delete live Ninja logs, invent cache hit rates, or
turn timing reports into CI wall-clock gates. Historical object counts do not
justify introducing a compiled Vectorscan bundle without fresh warmed-cache
end-to-end evidence.
