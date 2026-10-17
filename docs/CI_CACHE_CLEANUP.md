# Closed-PR cache cleanup

`ci-cache-cleanup.yml` is a manual-only maintenance workflow. Its default is a
read-only audit. It does not run on PR events, pushes, or a schedule, and it does
not change the build workflow's cache budgets.

## Deletion policy

Only caches whose API `ref` is exactly `refs/pull/N/merge` or `refs/pull/N/head`
can become candidates. The pull request API must confirm that PR `N` is closed;
both merged and closed-without-merge PRs qualify. The cache key is never deletion
authority. This also covers Windows `windows-sccache-v1-...-pr-N-...` entries
without depending on their key format.

The script protects all master, branch, tag, open-PR, and other nonmatching refs.
It checks every supported nonterminal run status (`queued`, `in_progress`,
`waiting`, `requested`, and `pending`), matching PR number, PR ref, branch, head
SHA, and merge SHA. An active matching run protects that PR's caches, including
older attempts identifiable by PR or branch. Branch/SHA matches are deliberately
conservative and can retain caches unnecessarily.

Malformed or unavailable API evidence fails closed. Pagination rejects missing
pages, duplicate IDs, changing totals, and oversized inventories. Cache listings
are limited to 10,000 records; each filtered active-run listing must contain fewer
than GitHub's 1,000-result cap. These bounds cause refusal, not partial evidence
being treated as proof that a cache is unused.

Apply builds a fresh plan rather than consuming saved JSON. Before **each**
delete it rechecks the PR, exact cache ID and metadata (including last access),
active runs, and PR again. It deletes by ID only, never by key or ref. Any observed
change or API failure stops remaining deletes; errors report IDs already deleted.
This workflow does not delete artifacts, dependency packages, or registry images.

## Audit first

After the reviewed workflow and script are available on `master`, open **Actions
> Audit or Clean Closed PR Caches > Run workflow**, select `master`, keep `apply`
false, and leave `confirmation` empty. The audit job has only `contents: read`,
`actions: read`, and `pull-requests: read` permissions.

A local read-only audit is also available with an appropriately scoped `gh`
authentication context or `GH_TOKEN`:

```sh
python3 scripts/pr_cache_cleanup.py --repo ZEACENT/klogg
```

The JSON output contains eligible `candidates`, `protected` caches with reasons,
and `usage_by_platform`. Linux, macOS, Windows, and shared/unknown usage each
report total bytes, protected master bytes, and eligible closed-PR bytes.
Classification is informational: unknown keys do not weaken ref protection.
Platform retention/build budgets remain a separate policy in the build workflow
and [CI environment documentation](CI_ENVIRONMENTS.md). Exceeding a budget never
authorizes this tool to evict master or open/active PR caches.

## Apply authorization and environment setup

Before enabling apply, a repository administrator must configure the
`pr-cache-cleanup` environment with:

- Required reviewers and prevention of self-review.
- Deployment branches restricted to `master`.
- No approval bypass where repository policy supports that restriction.

**Merely naming an environment in YAML does not configure approval protection.**
The workflow does not create or modify those repository settings.

To request deletion, dispatch the workflow from `master` with `apply` true and
`confirmation` exactly `delete-closed-pr-caches`. Review the completed read-only
audit before approving the environment. Apply is a separate job with
`actions: write`, checks out the trusted dispatch SHA rather than a PR head, and
regenerates its plan after approval. Approval authorizes the policy-based cleanup
operation, not an immutable list from an earlier audit.

The CLI additionally requires explicit `--apply` and the exact `--confirm` value,
and refuses apply unless its GitHub context identifies a master
`workflow_dispatch` in the same repository. These context checks supplement the
workflow/token/environment controls; locally fabricated environment variables
are not an independent proof of GitHub approval.

## Quiet-window requirement and race boundary

GitHub provides no atomic cache compare-and-delete or PR/run snapshot lock.
Rechecks detect observed changes but cannot eliminate a reopen or rerun between
the final read and deletion. Approve apply only during a quiet window with no
reopens or reruns for candidate PRs. The workflow concurrency group serializes
cleanup jobs only; it does not lock ordinary CI runs or PR state. If a truly
atomic guarantee is required, keep the workflow in audit-only use.

## Retained read-only inventory

The 2026-10-17 live audit is retained at
[`ci/evidence/closed-pr-cache-audit-20261017.json`](ci/evidence/closed-pr-cache-audit-20261017.json).
It observed 54 entries: 29 candidates belonging to closed PRs #77 and #79
(4.174 GiB) and 25 protected entries (5.631 GiB). No caches were deleted. These
are timestamped observations, not an executable deletion plan or proof that
those PRs remain inactive; apply must obtain and recheck fresh API evidence.

## Local verification

```sh
python3 -m unittest discover -s tests/scripts -p test_pr_cache_cleanup.py -v
actionlint .github/workflows/ci-cache-cleanup.yml
```

These tests use fake API responses and mocked `gh` calls. They cover exact closed,
open, and protected refs; all active-run match paths; pagination and malformed
responses; stale PR/cache evidence; partial-delete failure reporting; and real
workflow mutations of events, permissions, checkout, and approval guards. They do
not run a remote workflow or delete remote caches.
