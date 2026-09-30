# Qualified CI environments

[Documentation](README.md) · [Build guide](BUILD.md) · [Dependencies](DEPENDENCIES.md)

## Current status

All six Linux environment families have reviewed digest pins and detached
evidence in `ci/environments/lock.json` and `ci/environments/evidence/`.
Ordinary Linux application CI is wired to consume locked environments rather
than rebuild them on a cold BuildKit cache. The ADB lock/verifier changes first
invalidated four packaging families, blocking four package jobs plus Jammy
ASan/LSan and UBSan. The shared Docker build action's optional-metrics fix
also changes the verification policy for the TSan and analysis families. On
this checkout **all six** families' old signed receipts fail closed before
image pull until they are requalified and their pins are reviewed. Do not
substitute calculated policy hashes for producer evidence. A signed lock from
an earlier revision is not current-head validation. Evidence-only run
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
after offline verification; that change still needs new-head hosted qualification.
None of these attempts produced replacement environment evidence. The reviewed signed
analysis-image recipe bytes and existing evidence status are unchanged.

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
POSIX private, no-replace binary staging from an externally authenticated digest;
it is not a signed consumer, and Windows staging remains blocked until private
ACL acquisition is audited. Existing ADB/iOS schema-1 build receipts bind the
full source lock and cannot be reinterpreted as cross-version core receipts.

## Consuming locked environments

Ordinary CI never builds an environment image. Two consumption shapes exist:

- `ci-build.yml` Linux legs run on the host and use
  `.github/actions/prepare-linux-environment`, which verifies the reviewed lock,
  signed provenance, and public registry identity, then pulls the exact digest
  and assigns the fixed local tag the existing build/test/package actions expect.
  All Docker invocations in these legs use `--pull=never`; a missing local image
  is a consumption failure, not a reason to rebuild. The clean-distro DEB
  installation smoke checks remain a narrow, deliberate exception because their
  package-manager use is the behavior under test.
- The CodeQL, Coverage, and Static analysis workflows resolve the locked
  `noble-qt693-analysis` digest in an upstream `ResolveLinuxEnvironment` job
  (verification only, no pull) and run the expensive job as a whole-job
  container on exactly that digest. Whole-job containers execute as root while
  the mounted workspace stays owned by the runner user, so the first step after
  checkout grants git a `safe.directory` exception; raw git commands fail
  closed without it. Host provisioning (apt installs, agent-setup) must not
  reappear in these jobs; the image already carries the compilers, analysis
  tools, Qt, and Boost, and only the shared CPM source cache is restored on top.

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
