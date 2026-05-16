#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="Release"
SIZES="50MB,500MB"
PROFILES="simple,normal,complex"
ITERATIONS="5"
WARMUP="1"
SEED="20260301"
TMPFS_DIR=""

default_tmpfs_dir() {
  case "$(uname -s)" in
    Linux)
      printf '%s\n' "/dev/shm/klogg-bench"
      ;;
    Darwin)
      printf '%s\n' "/Volumes/RAMDisk/klogg-bench"
      ;;
    *)
      printf '%s\n' "${TMPDIR:-/tmp}/klogg-bench"
      ;;
  esac
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --tmpfs-dir PATH     Memory-backed directory for generated log files.
  --sizes CSV          Size buckets to benchmark. Default: ${SIZES}
  --profiles CSV       Regex profiles to benchmark. Default: ${PROFILES}
  --iterations N       Measured iterations per case. Default: ${ITERATIONS}
  --warmup N           Warmup iterations per case. Default: ${WARMUP}
  --seed N             Deterministic corpus seed. Default: ${SEED}
  --build-type TYPE    CMake build type. Default: ${BUILD_TYPE}
  --help               Show this help text.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tmpfs-dir)
      TMPFS_DIR="$2"
      shift 2
      ;;
    --sizes)
      SIZES="$2"
      shift 2
      ;;
    --profiles)
      PROFILES="$2"
      shift 2
      ;;
    --iterations)
      ITERATIONS="$2"
      shift 2
      ;;
    --warmup)
      WARMUP="$2"
      shift 2
      ;;
    --seed)
      SEED="$2"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "${TMPFS_DIR}" ]]; then
  TMPFS_DIR="$(default_tmpfs_dir)"
fi

mkdir -p "${TMPFS_DIR}"

RAW_DIR="$(mktemp -d "${TMPDIR:-/tmp}/klogg-regex-bench.XXXXXX")"

cleanup_generated_inputs() {
  rm -f "${TMPFS_DIR}"/regex-bench-v1-*.log
}

cleanup() {
  rm -rf "${RAW_DIR}"
  cleanup_generated_inputs
}

trap cleanup EXIT

configure_build() {
  local build_dir="$1"
  shift
  cmake -S "${ROOT_DIR}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" "$@"
}

build_target() {
  local build_dir="$1"
  cmake --build "${build_dir}" --config "${BUILD_TYPE}" --target regex_search_benchmark
}

benchmark_binary() {
  local build_dir="$1"

  if [[ -x "${build_dir}/output/regex_search_benchmark" ]]; then
    printf '%s\n' "${build_dir}/output/regex_search_benchmark"
    return 0
  fi

  if [[ -x "${build_dir}/output/${BUILD_TYPE}/regex_search_benchmark" ]]; then
    printf '%s\n' "${build_dir}/output/${BUILD_TYPE}/regex_search_benchmark"
    return 0
  fi

  if [[ -x "${build_dir}/output/regex_search_benchmark.exe" ]]; then
    printf '%s\n' "${build_dir}/output/regex_search_benchmark.exe"
    return 0
  fi

  if [[ -x "${build_dir}/output/${BUILD_TYPE}/regex_search_benchmark.exe" ]]; then
    printf '%s\n' "${build_dir}/output/${BUILD_TYPE}/regex_search_benchmark.exe"
    return 0
  fi

  echo "Unable to find regex_search_benchmark under ${build_dir}/output" >&2
  return 1
}

run_benchmark() {
  local build_dir="$1"
  local engine="$2"
  local label="$3"
  local output_json="${RAW_DIR}/${label}.json"
  local binary_path

  binary_path="$(benchmark_binary "${build_dir}")"

  "${binary_path}" \
    --engine "${engine}" \
    --label "${label}" \
    --tmpfs-dir "${TMPFS_DIR}" \
    --sizes "${SIZES}" \
    --profiles "${PROFILES}" \
    --iterations "${ITERATIONS}" \
    --warmup "${WARMUP}" \
    --seed "${SEED}" \
    --keep-files \
    --output "${output_json}"
}

BUILD_QT="${ROOT_DIR}/build-bench-qt"
BUILD_VS_GENERIC="${ROOT_DIR}/build-bench-vs-generic"
BUILD_VS_AVX="${ROOT_DIR}/build-bench-vs-avx"

configure_build \
  "${BUILD_QT}" \
  -DKLOGG_BUILD_BENCHMARKS=ON \
  -DKLOGG_USE_VECTORSCAN=OFF
build_target "${BUILD_QT}"

configure_build \
  "${BUILD_VS_GENERIC}" \
  -DKLOGG_BUILD_BENCHMARKS=ON \
  -DKLOGG_USE_VECTORSCAN=ON \
  -DKLOGG_GENERIC_CPU=ON
build_target "${BUILD_VS_GENERIC}"

configure_build \
  "${BUILD_VS_AVX}" \
  -DKLOGG_BUILD_BENCHMARKS=ON \
  -DKLOGG_USE_VECTORSCAN=ON \
  -DKLOGG_GENERIC_CPU=OFF
build_target "${BUILD_VS_AVX}"

run_benchmark "${BUILD_QT}" "qt" "qt"
run_benchmark "${BUILD_VS_GENERIC}" "vectorscan" "vectorscan-generic"
run_benchmark "${BUILD_VS_AVX}" "vectorscan" "vectorscan-avx"

mkdir -p "${ROOT_DIR}/docs/benchmarks"

python3 - "${RAW_DIR}/qt.json" "${RAW_DIR}/vectorscan-generic.json" "${RAW_DIR}/vectorscan-avx.json" "${ROOT_DIR}/docs/benchmarks/regex-benchmark-results.json" "${ROOT_DIR}/docs/benchmarks/regex-benchmark-results.md" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

raw_paths = [Path(arg) for arg in sys.argv[1:4]]
json_output = Path(sys.argv[4])
markdown_output = Path(sys.argv[5])

runs = [json.loads(path.read_text()) for path in raw_paths]
engine_order = ["qt", "vectorscan-generic", "vectorscan-avx"]
size_order = ["50MB", "500MB"]
profile_order = ["simple", "normal", "complex"]

case_index = {}
for run in runs:
    label = run["label"]
    for case in run["cases"]:
        case_index[(label, case["profile_id"], case["size_id"])] = case

summary = {
    "generated_at_utc": datetime.now(timezone.utc).isoformat(),
    "runs": runs,
    "summary": {"profiles": {}},
}

markdown = []
markdown.append("# Regex Benchmark Results")
markdown.append("")
markdown.append(f"Generated at: {summary['generated_at_utc']}")
markdown.append("")
if runs:
    host = runs[0].get("host", {})
    profiles = runs[0].get("profiles", [])
    markdown.append("## Environment")
    markdown.append("")
    markdown.append(f"- Product: `{host.get('product_name', 'unknown')}`")
    markdown.append(f"- Kernel: `{host.get('kernel_type', 'unknown')} {host.get('kernel_version', 'unknown')}`")
    markdown.append(f"- CPU architecture: `{host.get('cpu_architecture', 'unknown')}`")
    markdown.append(f"- Qt: `{host.get('qt_version', 'unknown')}`")
    markdown.append(f"- Iterations: `{runs[0].get('iterations', 'unknown')}` measured + `{runs[0].get('warmup', 'unknown')}` warmup")
    markdown.append("")
    markdown.append("## Regex Profiles")
    markdown.append("")
    markdown.append("| Profile | Pattern |")
    markdown.append("| --- | --- |")
    for profile in profiles:
        markdown.append(f"| {profile['id']} | `{profile['pattern']}` |")
    markdown.append("")

markdown.append("## Median Search Time")
markdown.append("")

for profile in profile_order:
    summary["summary"]["profiles"][profile] = {}
    markdown.append(f"### {profile}")
    markdown.append("")
    markdown.append("| Engine | 50MB (ms) | 500MB (ms) |")
    markdown.append("| --- | ---: | ---: |")
    for engine in engine_order:
        row = [engine]
        for size in size_order:
            case = case_index.get((engine, profile, size))
            if case is None:
                value = "-"
            elif case["status"] != "ok":
                value = case["status"]
            else:
                value = f"{case['search_ms']['median']:.2f}"
            row.append(value)
            summary["summary"]["profiles"][profile].setdefault(size, {})[engine] = case
        markdown.append("| " + " | ".join(row) + " |")
    markdown.append("")

markdown.append("## Median Throughput")
markdown.append("")

for profile in profile_order:
    markdown.append(f"### {profile}")
    markdown.append("")
    markdown.append("| Engine | 50MB (MiB/s) | 500MB (MiB/s) |")
    markdown.append("| --- | ---: | ---: |")
    for engine in engine_order:
        row = [engine]
        for size in size_order:
            case = case_index.get((engine, profile, size))
            if case is None:
                value = "-"
            elif case["status"] != "ok":
                value = case["status"]
            else:
                value = f"{case['throughput_mib_per_s']['median']:.2f}"
            row.append(value)
        markdown.append("| " + " | ".join(row) + " |")
    markdown.append("")

markdown.append("## 500MB Snapshot")
markdown.append("")
markdown.append("| Profile | Qt (ms) | Vectorscan generic (ms) | Vectorscan AVX (ms) | AVX vs generic |")
markdown.append("| --- | ---: | ---: | ---: | ---: |")
for profile in profile_order:
    qt_case = case_index.get(("qt", profile, "500MB"))
    generic_case = case_index.get(("vectorscan-generic", profile, "500MB"))
    avx_case = case_index.get(("vectorscan-avx", profile, "500MB"))
    qt_value = "-" if not qt_case or qt_case["status"] != "ok" else f"{qt_case['search_ms']['median']:.2f}"
    generic_value = "-" if not generic_case or generic_case["status"] != "ok" else f"{generic_case['search_ms']['median']:.2f}"
    avx_value = "-" if not avx_case or avx_case["status"] != "ok" else f"{avx_case['search_ms']['median']:.2f}"
    if generic_case and avx_case and generic_case["status"] == "ok" and avx_case["status"] == "ok" and generic_case["search_ms"]["median"] > 0:
        delta = ((generic_case["search_ms"]["median"] - avx_case["search_ms"]["median"]) / generic_case["search_ms"]["median"]) * 100.0
        delta_value = f"{delta:+.2f}%"
    else:
        delta_value = "-"
    markdown.append(f"| {profile} | {qt_value} | {generic_value} | {avx_value} | {delta_value} |")
markdown.append("")

markdown.append("## Search Coverage And Hit Rate")
markdown.append("")
markdown.append("| Profile | Size | Engine | Searched lines | Matches | Hit rate |")
markdown.append("| --- | --- | --- | ---: | ---: | ---: |")
for profile in profile_order:
    for size in size_order:
        for engine in engine_order:
            case = case_index.get((engine, profile, size))
            if case is None:
                markdown.append(f"| {profile} | {size} | {engine} | - | - | - |")
                continue
            if case["status"] != "ok":
                markdown.append(f"| {profile} | {size} | {engine} | - | - | {case['status']} |")
                continue
            hit_rate_percent = case["hit_rate"] * 100.0
            markdown.append(
                f"| {profile} | {size} | {engine} | {case['searched_line_count']} | {case['match_count']} | {hit_rate_percent:.2f}% |"
            )
markdown.append("")

json_output.write_text(json.dumps(summary, indent=2) + "\n")
markdown_output.write_text("\n".join(markdown) + "\n")
PY

echo "Wrote ${ROOT_DIR}/docs/benchmarks/regex-benchmark-results.json"
echo "Wrote ${ROOT_DIR}/docs/benchmarks/regex-benchmark-results.md"
