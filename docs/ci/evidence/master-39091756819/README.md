# First merged-master cache migration evidence

- Master run: 39091756819, attempt 1, SHA `f0ea6a09935c8c1a3407811446536482427fc931`.
- Comparison PR run: 39053583829, attempt 1; compiled merge `ef79c643c55ae4128563a6ddfe83171713c9cf6f`.
- Both source trees match, but their checkout SHAs and cache scopes differ. These
  are observations, not a controlled cold/three-warm acceptance cohort.
- `master-evidence.json` and `pr-evidence.json` join original API/metrics payloads.
  `manifest.json` hashes the copied original payloads and complete downloaded log
  archives. No binary archives are committed.
- Master Intel package metrics are missing because artifact creation timed out.
  The missing report is recorded as an error, not synthesized from log totals.
- `selected-cache-log-lines.json` preserves original line numbers and full-log
  hashes, including the earlier trusted seed run 39031248659. Those seed logs
  support explicitly log-derived counter subtraction, not a new metrics artifact.
- Cache inventory/usage are point-in-time observations. They do not prove which
  entries were evicted or why a reservation failed.

Offline reproduction from the repository root:

```sh
python3 -I scripts/ci_perf_report.py report \
  docs/ci/evidence/master-39091756819/master-evidence.json \
  docs/ci/evidence/master-39091756819/pr-evidence.json
```

No DAG is inferred by this command. The actual workflow dependency chain and
step timing interpretation are documented in [the analysis](../../MASTER_CACHE_MIGRATION.md).
