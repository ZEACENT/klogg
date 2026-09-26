# Qualified CI environments

[Documentation](README.md) · [Build guide](BUILD.md) · [Dependencies](DEPENDENCIES.md)

## Bootstrap status

The producer and verification tooling is being introduced before the first
qualified environment lock. Ordinary application CI still uses its existing
preparation paths until all six families have real qualification, publication,
public-access verification, and reviewed digest pins. The presence of a recipe
or passing script tests does **not** mean an environment has been published.

Do not create placeholder locks, substitute a mutable tag, or fall back to
building an environment when verified consumption fails.

## Ownership

Application CI should build and test the current application, not repeatedly
construct its compiler environment. Three different things have different
lifecycles:

1. **Compiler environments:** distribution, compiler, Qt, and build tools.
2. **Product dependencies:** immutable helper binaries and their source/legal cores.
3. **Application outputs:** current-source executables, tests, packages, and
   version-specific source-publication overlays.

This producer initially covers compiler environments. ADB/iOS immutable-core
reuse is a separate migration; the Linux package qualification fixture is not a
claim that those cores have already migrated.

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

## Producing and publishing

Initial bootstrap uses the already registered `CI Build` workflow at the trusted
feature ref. Its `environment-mode` input is `off`, `qualify`, or `publish`.
Producer mode also requires the exact source SHA and an ancestor analysis base
SHA, and cannot be combined with signed-release qualification.

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

Never package runner-owned Xcode or Visual Studio into this artifact layer.

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
python3 -m unittest discover -s tests/scripts -p 'test_ci_environment*.py'
python3 -m unittest discover -s tests/scripts -p 'test_*ci_environment*.py'
```

Unit and workflow-contract tests cover malformed input, substitution, incomplete
qualification, privilege boundaries, and event projections. They do not replace
real image builds, full sanitizer tests, package smoke tests, CodeQL tracing,
anonymous retrieval, or current-head cross-platform acceptance.

`scripts/ci_build_metrics.py run-build` wraps the unchanged Linux
`ci_build` invocation and reports the fresh Ninja log, measured wall time, and
global ccache counter deltas (or an explicit `disabled`/`unmeasured` state) to
`ci-build-metrics.json` inside the build root. Work-time sums are not elapsed
time or critical-path measurements. Do not serialize normal builds, delete live
Ninja logs, invent cache hit rates, or turn timing reports into CI wall-clock
gates.
