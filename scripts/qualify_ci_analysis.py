#!/usr/bin/env python3
"""Execute real static, coverage and CodeQL candidate qualification.

This helper emits analysis evidence, NOT a qualification receipt or attestation.
The workflow issues its receipt only after this command succeeds. Candidate bytes
must already have been verified and loaded by prepare-candidate. We recheck the
loaded config/diffIDs and current checkout before executing that immutable ID.

CodeQL is obtained only by its role from the supplied official, checksum-locked
bundle. Tools, databases, query source and intermediate results remain in private
scratch space; only SARIF, coverage measurements and non-binary evidence escape.
No image is built, pulled, tagged, pushed or removed here.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import io
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

import ci_build_metrics as metrics
import ci_environment as ci
import ci_environment_profiles as profiles
from first_party_compile_units import first_party_compile_units
from filter_compile_database import filter_database
from prefetch_adb_helper_sources import download, safe_extract

FAMILY = "noble-qt693-analysis"
ROLES = ("static", "coverage", "codeql")
ALLOWED_OUTPUTS = {
    "static": {"analysis-evidence.json"},
    "coverage": {"analysis-evidence.json", "coverage-summary.txt", "coverage.json"},
    "codeql": {"analysis-evidence.json", "codeql.sarif"},
}
CENSUS_QUERY = """import semmle.code.cpp.Compilation
from Compilation compilation, File file
where file = compilation.getAFileCompiled()
select file.getAbsolutePath()
"""


class AnalysisError(ci.ContractError):
    """Analysis execution or its current-source evidence is invalid."""


def require(condition, message):
    if not condition:
        raise AnalysisError(message)


def read_json(path, limit=128 * 1024 * 1024):
    require(path.is_file() and not path.is_symlink(), "missing or linked JSON evidence: " + str(path))
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "analysis JSON exceeds size limit")
    return ci._parse_json(data, str(path))


def write_json(path, document):
    path.write_text(json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n", encoding="ascii")


def file_sha(path):
    require(path.is_file() and not path.is_symlink(), "missing or linked evidence file: " + str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fresh_directory(path):
    require(not path.is_symlink() and (not path.exists() or (path.is_dir() and not any(path.iterdir()))),
            "analysis directory must be new or empty: " + str(path))
    path.mkdir(parents=True, exist_ok=True)


def relay_output(result, *, emit_stdout=True):
    for field, destination in (("stdout", sys.stdout), ("stderr", sys.stderr)):
        if field == "stdout" and not emit_stdout:
            continue
        text = getattr(result, field, None)
        if isinstance(text, bytes):
            text = text.decode("utf-8", errors="replace")
        if text:
            print(text, end="" if text.endswith("\n") else "\n", file=destination)


def execute(runner, command, *, cwd, env, emit_stdout=True):
    try:
        result = runner([str(value) for value in command], cwd=str(cwd), env=env,
                        check=True, capture_output=True, text=True, timeout=14400)
    except (OSError, subprocess.SubprocessError) as error:
        relay_output(error)
        raise AnalysisError("analysis command failed: " + str(error)) from error
    relay_output(result, emit_stdout=emit_stdout)
    require(result.returncode == 0, "analysis command failed: " + str(command[0]))
    return result


def role_environment(workspace, role):
    document = ci.load_json(workspace / "ci/environments/profiles.json")
    environment = os.environ.copy()
    environment.update(profiles.profile_environment(document, FAMILY, role, workspace=str(workspace)))
    if role == "codeql":
        environment.update({"CCACHE_DISABLE": "1", "SCCACHE_DISABLE": "1",
                            "CMAKE_C_COMPILER_LAUNCHER": "", "CMAKE_CXX_COMPILER_LAUNCHER": ""})
    return document, environment


def configure(role, workspace, build, runner, environment, document):
    require(not build.exists(), "fresh analysis build directory is required")
    options = profiles.profile_configuration(document, FAMILY, role)["cmake_options"]
    options = [value.replace("@WORKSPACE@", str(workspace)) for value in options]
    def enabled(name):
        values = [match.group(1) for option in options
                  for match in [re.fullmatch(r"-D" + name + r"(?::[A-Za-z_]+)?=(.*)", option)] if match]
        return bool(values) and values[-1] == "ON"
    require(enabled("FETCHCONTENT_FULLY_DISCONNECTED"), "analysis profile requires explicit disconnected configuration")
    if role == "codeql":
        require(enabled("CMAKE_EXPORT_COMPILE_COMMANDS"), "CodeQL census requires an explicit compile database profile option")
    execute(runner, ["cmake", "-S", workspace, "-B", build] + options, cwd=workspace, env=environment)


def codeql_prebuild(workspace, build, runner, environment):
    execute(runner, ["cmake", "--build", build, "-t", "klogg_codeql_thirdparty"], cwd=workspace, env=environment)
    plan = execute(runner, ["cmake", "--build", build, "-t", "klogg", "--", "-n"], cwd=workspace, env=environment).stdout
    require(isinstance(plan, str) and bool(plan.strip()), "empty CodeQL traced build plan")
    require(re.search(r"cpm_cache|_deps|3rdparty/CMakeFiles", plan) is None,
            "vendored compile work remains after CodeQL dependency prebuild")
    return hashlib.sha256(plan.encode("utf-8")).hexdigest()


def codeql_trace_build(workspace, build, runner, environment):
    execute(runner, ["cmake", "--build", build, "-t", "klogg"], cwd=workspace, env=environment)


def _filtered_database(database, units, output, workspace):
    try:
        records = filter_database(database, workspace, units)
    except ValueError as error:
        raise AnalysisError(str(error)) from error
    require(bool(records), "cppcheck has no selected first-party translation units")
    write_json(output, records)


def _cppcheck(workspace, database, strict):
    command = ["cppcheck", "--project=" + str(database), "--enable=warning,style", "--inline-suppr", "--quiet",
               "--suppress=missingInclude", "--suppress=syntaxError", "--suppress=unknownMacro",
               "--suppress=preprocessorErrorDirective", "--suppressions-list=" + str(workspace / "tests/cppcheck_suppressions.txt")]
    if strict:
        command.append("--error-exitcode=1")
    return command


def static_analysis(workspace, build, work, base, runner, environment, jobs):
    execute(runner, ["cmake", "--build", build, "--target", "klogg_ui_autogen", "generate_version", "-j" + str(jobs)],
            cwd=workspace, env=environment)
    database = read_json(build / "compile_commands.json")
    require(isinstance(database, list), "compile database must be an array")
    units = first_party_compile_units(database, workspace / "src")
    require(bool(units), "full static analysis requires first-party translation units")
    def tidy(path):
        return execute(runner, ["clang-tidy", "-p", build, "--extra-arg=-Wno-unknown-warning-option", path],
                       cwd=workspace, env=environment)
    # Full audit preserves the existing report-only diagnostic policy. Tool and
    # configuration errors are fatal; changed-line qualification below is strict.
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(tidy, units))
    full_project = work / "cppcheck-full.json"
    _filtered_database(database, units, full_project, workspace)
    execute(runner, _cppcheck(workspace, full_project, False), cwd=workspace, env=environment)
    execute(runner, ["python3", workspace / "scripts/run_changed_clang_tidy.py", "--base", base,
                     "--build-dir", build, "--jobs", str(jobs)], cwd=workspace, env=environment)
    changed = execute(runner, ["git", "-c", "core.quotePath=false", "diff", "--name-only", "-z", "--diff-filter=d", base, "HEAD", "--", "src/"],
                      cwd=workspace, env=environment, emit_stdout=False).stdout.split("\0")
    headers = [path for path in changed if pathlib.Path(path).suffix.lower() in (".h", ".hh", ".hpp", ".hxx")]
    sources = {str((workspace / path).resolve()) for path in changed if pathlib.Path(path).suffix.lower() in (".c", ".cc", ".cpp", ".cxx")}
    selected = units if headers else [pathlib.Path(path) for path in sorted(sources)]
    if selected:
        project = work / "cppcheck-changed.json"
        _filtered_database(database, selected, project, workspace)
        execute(runner, _cppcheck(workspace, project, True), cwd=workspace, env=environment)
    return {"full_first_party_units": len(units), "full_clang_tidy": True, "full_cppcheck": True,
            "changed_clang_tidy_strict": True, "changed_source_units": len(selected), "changed_headers": len(headers),
            "changed_cppcheck_strict": bool(selected)}


def coverage_analysis(workspace, build, work, base, runner, environment, jobs):
    execute(runner, ["cmake", "--build", build, "-t", "klogg", "klogg_grep", "klogg_test_build", "-j" + str(jobs)],
            cwd=workspace, env=environment)
    execute(runner, ["ctest", "--output-on-failure", "--parallel", str(jobs)], cwd=build, env=environment)
    report = work / "coverage_report"
    report.mkdir()
    common = ["gcovr", "--root", workspace, "--filter", "^src/", "--gcov-ignore-parse-errors", "negative_hits.warn_once_per_file"]
    summary = execute(runner, common + ["--html-details", "--html-self-contained", "--html-title", "Klogg line/branch coverage",
                                       "-o", report / "index.html", "--print-summary", build], cwd=workspace, env=environment).stdout
    (report / "summary.txt").write_text(summary, encoding="utf-8")
    execute(runner, common + ["--json", "--json-pretty", "-o", report / "coverage.json", build], cwd=workspace, env=environment)
    execute(runner, ["python3", workspace / "scripts/enforce_coverage_ratchet.py", "--summary", report / "summary.txt",
                     "--line-floor", ".github/baselines/coverage-line.txt", "--branch-floor", ".github/baselines/coverage-branch.txt",
                     "--base-sha", base], cwd=workspace, env=environment)
    return {"ctest_all": True, "build_targets": ["klogg", "klogg_grep", "klogg_test_build"], "ratchet_base": base}


def _pack(root, name):
    candidates = sorted((root / "qlpacks/codeql" / name).glob("*/qlpack.yml"))
    require(len(candidates) == 1, "locked bundle must contain exactly one codeql/" + name + " pack")
    manifest = candidates[0]
    text = manifest.read_text(encoding="utf-8")
    version = re.search(r"(?m)^version:\s*[\"']?([0-9]+\.[0-9]+\.[0-9]+)[\"']?\s*$", text)
    require(re.search(r"(?m)^name:\s*[\"']?codeql/" + re.escape(name) + r"[\"']?\s*$", text) is not None and version is not None,
            "invalid bundled query pack identity")
    return manifest.parent, {"name": "codeql/" + name, "version": version.group(1), "manifest_sha256": file_sha(manifest)}


def ninja_outputs(build):
    path = build / ".ninja_log"
    require(path.is_file(), "CodeQL census requires the actual Ninja log")
    outputs = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        fields = line.split("\t")
        require(len(fields) == 5, "malformed Ninja compilation census")
        outputs.add(str((build / fields[3]).resolve()))
    return outputs


def compilation_map(workspace, build):
    database = read_json(build / "compile_commands.json")
    require(isinstance(database, list), "CodeQL compilation database must be an array")
    paths = metrics.Paths(str(workspace), str(build))
    owners = metrics.compile_owners(database, paths)
    sources = {}
    for entry in database:
        output = metrics.command_output(entry, False)
        if output:
            directory = paths.normalize(entry.get("directory", str(build)), str(build))
            sources[paths.normalize(output, directory)] = paths.normalize(entry["file"], directory)
    # These two src/app object libraries consist solely of RCC sources, so
    # generic compile-database ownership cannot learn from an original C++ TU.
    # Keep the exception tied to their exact target and generated-source roots;
    # never promote an arbitrary unknown/generated or third-party target.
    for output, source in sources.items():
        if owners.get(output) != "unknown":
            continue
        for target in ("klogg_common_resources", "klogg_documentation_resources"):
            object_root = str(build / "src/app/CMakeFiles" / (target + ".dir")) + "/"
            generated_root = str(build / "src/app" / (target + "_autogen")) + "/"
            if output.startswith(object_root) and source.startswith(generated_root) and re.fullmatch(r"qrc_.+\.cpp", pathlib.Path(source).name):
                owners[output] = "first_party"
    return owners, sources


def validate_census(workspace, build, before, csv_file):
    owners, sources = compilation_map(workspace, build)
    require(not any(owners.get(output) == "first_party" for output in before), "first-party objects were built before CodeQL tracing")
    new_objects = {path for path in ninja_outputs(build) - before if pathlib.Path(path).suffix.lower() in (".o", ".obj")}
    require(bool(new_objects) and all(owners.get(path) == "first_party" for path in new_objects),
            "CodeQL census contains unknown or vendored traced compilation")
    expected = {sources[path] for path in new_objects}
    require(csv_file.is_file(), "CodeQL database census was not decoded")
    with csv_file.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.reader(stream))
    require(bool(rows) and all(len(row) == 1 and pathlib.Path(row[0]).is_absolute() for row in rows), "invalid CodeQL database census rows")
    extracted = {str(pathlib.Path(row[0]).resolve()) for row in rows}
    require(extracted == expected, "CodeQL database census does not match the actually compiled first-party units")
    first_party = [path for path in extracted if path.startswith(str(workspace / "src") + "/")]
    moc = [path for path in extracted if pathlib.Path(path).name.startswith(("mocs_compilation", "moc_"))]
    rcc = [path for path in extracted if pathlib.Path(path).name.startswith("qrc_")]
    require(str(workspace / "src/app/main.cpp") in first_party and moc and rcc,
            "CodeQL census must contain application, generated MOC and generated RCC units")
    def relative(path):
        if path.startswith(str(workspace) + "/"):
            return pathlib.Path(path).relative_to(workspace).as_posix()
        return "build/" + pathlib.Path(path).relative_to(build).as_posix()
    return {"matched_translation_units": len(extracted), "first_party_units": len(first_party),
            "moc_units": len(moc), "rcc_units": len(rcc), "units": sorted(relative(path) for path in extracted)}


def check_sarif(path):
    document = read_json(path)
    require(isinstance(document, dict) and document.get("version") == "2.1.0"
            and isinstance(document.get("runs"), list) and bool(document["runs"]), "missing or malformed CodeQL SARIF")
    for run in document["runs"]:
        require(isinstance(run, dict), "invalid SARIF run")
        tool = run.get("tool")
        driver = tool.get("driver") if isinstance(tool, dict) else None
        require(isinstance(driver, dict) and driver.get("name") == "CodeQL", "SARIF was not produced by CodeQL")
        require(isinstance(run.get("results", []), list), "invalid SARIF results")


def codeql_analysis(workspace, build, work, root, expected_version, runner, environment, jobs):
    require(root is not None and isinstance(expected_version, str), "CodeQL requires a verified private bundle and expected version")
    executable = root / "codeql"
    require(executable.is_file() and not executable.is_symlink(), "verified bundle has no CodeQL executable")
    queries, query_identity = _pack(root, "cpp-queries")
    _, library_identity = _pack(root, "cpp-all")
    suite = queries / "codeql-suites/cpp-code-scanning.qls"
    require(suite.is_file(), "locked bundle lacks the code-scanning query suite")
    query_identity["suite_sha256"] = file_sha(suite)
    version_output = execute(runner, [executable, "version", "--format=json"], cwd=workspace, env=environment).stdout
    version = ci._parse_json(version_output.encode("utf-8"), "CodeQL version")
    require(isinstance(version, dict) and version.get("version") == expected_version, "CodeQL version differs from the locked bundle release")
    plan_sha = codeql_prebuild(workspace, build, runner, environment)
    before = ninja_outputs(build)
    database = work / "codeql-database"
    require(not database.exists(), "CodeQL database must be fresh")
    trace = ["sh", str(workspace / "scripts/run_linux_analysis.sh"), "codeql-trace-build", "--role", "codeql",
             "--workspace", str(workspace), "--build-dir", str(build)]
    execute(runner, [executable, "database", "create", database, "--language=c-cpp", "--source-root=" + str(workspace),
                     "--threads=" + str(jobs), "--command=" + " ".join(shlex.quote(value) for value in trace)], cwd=workspace, env=environment)
    census_root = work / "census"
    census_root.mkdir()
    (census_root / "qlpack.yml").write_text("name: klogg/qualification-census\nversion: 0.0.0\ndependencies:\n  codeql/cpp-all: "
                                            + library_identity["version"] + "\n", encoding="ascii")
    (census_root / "census.ql").write_text(CENSUS_QUERY, encoding="ascii")
    packs = root / "qlpacks"
    # All dependencies are in the exact bundle. Docker --network=none makes a
    # missing pack a hard failure, never an implicit remote acquisition.
    execute(runner, [executable, "pack", "install", "--additional-packs=" + str(packs), str(census_root)], cwd=workspace, env=environment)
    bqrs = work / "census.bqrs"
    execute(runner, [executable, "query", "run", "--database=" + str(database), "--additional-packs=" + str(packs),
                     "--output=" + str(bqrs), census_root / "census.ql"], cwd=workspace, env=environment)
    census_csv = work / "census.csv"
    execute(runner, [executable, "bqrs", "decode", "--format=csv", "--no-titles", "--output=" + str(census_csv), bqrs], cwd=workspace, env=environment)
    census = validate_census(workspace, build, before, census_csv)
    sarif = work / "codeql.sarif"
    execute(runner, [executable, "database", "analyze", database, suite, "--format=sarif-latest", "--output=" + str(sarif),
                     "--sarif-category=klogg-ci-environment", "--threads=" + str(jobs), "--no-download", "--additional-packs=" + str(packs)],
            cwd=workspace, env=environment)
    check_sarif(sarif)
    return {"codeql_version": version["version"], "query_pack": query_identity, "library_pack": library_identity,
            "traced_plan_sha256": plan_sha, "census": census}


def run_analysis(role, workspace, work_root, output, analysis_base_sha, *, codeql_root=None, expected_codeql_version=None, runner=None, jobs=1):
    """Run current-source tools; a successful role is never inferred from probes."""
    runner = subprocess.run if runner is None else runner
    workspace, work_root, output = (pathlib.Path(path).resolve() for path in (workspace, work_root, output))
    require(role in ROLES, "unknown analysis role")
    require(isinstance(analysis_base_sha, str) and re.fullmatch(r"[0-9a-f]{40}", analysis_base_sha) is not None
            and analysis_base_sha != "0" * 40, "analysis requires a full nonzero base SHA")
    require(type(jobs) is int and jobs > 0, "analysis jobs must be positive")
    require(output != work_root and output not in work_root.parents and work_root not in output.parents,
            "result directory must be separate from private analysis work")
    fresh_directory(work_root)
    fresh_directory(output)
    private_home = work_root / "home"
    private_home.mkdir()
    document, environment = role_environment(workspace, role)
    environment["HOME"] = str(private_home)
    execute(runner, ["git", "cat-file", "-e", analysis_base_sha + "^{commit}"], cwd=workspace, env=environment)
    build_root = profiles.profile_configuration(document, FAMILY, role)["build_root"]
    # gcovr 7.0 derives relative object directories from --root; a build tree
    # outside the workspace makes them escape with bogus ../../../ prefixes and
    # gcov fails with no_working_dir_found. Coverage therefore builds inside
    # the workspace, matching the production coverage workflow's geometry.
    build = (workspace / build_root) if role == "coverage" else (work_root / build_root)
    configure(role, workspace, build, runner, environment, document)
    if role == "static":
        checks = static_analysis(workspace, build, work_root, analysis_base_sha, runner, environment, jobs)
    elif role == "coverage":
        checks = coverage_analysis(workspace, build, work_root, analysis_base_sha, runner, environment, jobs)
        shutil.copyfile(work_root / "coverage_report/summary.txt", output / "coverage-summary.txt")
        shutil.copyfile(work_root / "coverage_report/coverage.json", output / "coverage.json")
    else:
        checks = codeql_analysis(workspace, build, work_root, pathlib.Path(codeql_root) if codeql_root else None,
                                 expected_codeql_version, runner, environment, jobs)
        shutil.copyfile(work_root / "codeql.sarif", output / "codeql.sarif")
    evidence = {"schema_version": 1, "kind": "analysis-evidence", "role": role, "result": "passed",
                "analysis_base_sha": analysis_base_sha, "checks": checks}
    write_json(output / "analysis-evidence.json", evidence)
    return evidence


def _mount(source, target, readonly=False):
    stream = io.StringIO()
    fields = ["type=bind", "source=" + str(source), "target=" + str(target)]
    if readonly:
        fields.append("readonly")
    csv.writer(stream, lineterminator="").writerow(fields)
    return stream.getvalue()


def qualify(role, candidate_root, workspace, analysis_base_sha, role_materials, output, *, runner=None, downloader=None, extractor=None):
    """Run one already-prepared candidate; return evidence, never issue receipts."""
    runner = subprocess.run if runner is None else runner
    downloader = download if downloader is None else downloader
    extractor = safe_extract if extractor is None else extractor
    workspace = pathlib.Path(workspace).resolve()
    candidate_root = pathlib.Path(candidate_root).resolve()
    output = pathlib.Path(output).absolute()
    require(role in ROLES, "unknown analysis role")
    require(not output.is_symlink() and (not output.exists() or (output.is_dir() and not any(output.iterdir()))), "analysis output must be new or empty")
    catalog = ci.load_json(workspace / "ci/environments/recipes.json")
    candidate = ci.load_json(candidate_root / "candidate.json")
    ci.validate_candidate(candidate, catalog)
    require(candidate["family"] == FAMILY, "analysis requires the downloaded-Qt analysis family")
    require(ci.recipe_identity(catalog, FAMILY, workspace) == candidate["recipe_digest"], "candidate recipe differs from current checkout")
    require(ci.input_identity(ci.load_json(candidate_root / "inputs.json")) == candidate["input_digest"], "candidate resolved input identity mismatch")
    profile_document = ci.load_json(workspace / "ci/environments/profiles.json")
    policy = profiles.build_policy(catalog, profile_document, FAMILY, workspace, role_materials=role_materials)
    require(isinstance(analysis_base_sha, str) and re.fullmatch(r"[0-9a-f]{40}", analysis_base_sha) is not None
            and analysis_base_sha != "0" * 40, "analysis requires an explicit base SHA")
    environment = os.environ.copy()
    head = execute(runner, ["git", "rev-parse", "HEAD"], cwd=workspace, env=environment, emit_stdout=False).stdout.strip()
    require(head == candidate["source"]["sha"], "analysis checkout differs from candidate source SHA")
    execute(runner, ["git", "diff", "--quiet", "HEAD", "--"], cwd=workspace, env=environment)
    untracked = execute(runner, ["git", "ls-files", "--others", "--exclude-standard", "-z", "--", "src/"], cwd=workspace, env=environment, emit_stdout=False).stdout
    require(not untracked, "untracked application sources cannot be qualified")
    execute(runner, ["git", "cat-file", "-e", analysis_base_sha + "^{commit}"], cwd=workspace, env=environment)
    inspected = execute(runner, ["docker", "image", "inspect", candidate["image"]["config_digest"]], cwd=workspace, env=environment, emit_stdout=False).stdout
    ci.compare_loaded_image(candidate["image"], ci._parse_json(inspected.encode("utf-8"), "loaded candidate image"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="klogg-analysis-", dir=str(output.parent)) as temporary:
        private = pathlib.Path(temporary)
        work, result = private / "work", private / "result"
        work.mkdir()
        result.mkdir()
        command = ["docker", "run", "--rm", "--pull=never", "--platform=linux/amd64", "--network=none",
                   "--user", str(os.getuid()) + ":" + str(os.getgid()),
                   "--mount", _mount(workspace, workspace), "--mount", _mount(work, "/klogg-analysis-work"),
                   "--mount", _mount(result, "/klogg-analysis-result")]
        profile_env = profiles.profile_environment(profile_document, FAMILY, role, workspace=str(workspace))
        for key, value in sorted(profile_env.items()):
            command += ["--env", key + "=" + value]
        command += ["--env", "HOME=/klogg-analysis-work/home", "--workdir", str(workspace)]
        inner = ["sh", str(workspace / "scripts/run_linux_analysis.sh"), "qualify", "--role", role, "--workspace", str(workspace),
                 "--work-root", "/klogg-analysis-work", "--output", "/klogg-analysis-result", "--analysis-base-sha", analysis_base_sha]
        if role == "codeql":
            material = policy["profiles"][role]["role_materials"][0]
            archive = private / "codeql-official.tar.gz"
            downloader(material["url"], archive)
            require(file_sha(archive) == material["sha256"], "official CodeQL bundle checksum mismatch")
            tools = private / "tools"
            extractor(archive, tools)
            require((tools / "codeql").is_file() and not (tools / "codeql").is_symlink(), "official bundle lacks the CodeQL executable")
            version = re.search(r"/codeql-bundle-v([0-9]+\.[0-9]+\.[0-9]+)/", material["url"]).group(1)
            command += ["--mount", _mount(tools, "/klogg-codeql", True), "--env", "CCACHE_DISABLE=1", "--env", "SCCACHE_DISABLE=1"]
            inner += ["--codeql-root", "/klogg-codeql", "--expected-codeql-version", version]
        command += [candidate["image"]["config_digest"]] + inner
        execute(runner, command, cwd=workspace, env=environment)
        require({path.name for path in result.iterdir()} == ALLOWED_OUTPUTS[role], "analysis output contains missing evidence or forbidden payloads")
        require(all(path.is_file() and not path.is_symlink() for path in result.iterdir()), "analysis output must contain regular evidence files only")
        evidence = ci.load_json(result / "analysis-evidence.json")
        require(evidence.get("schema_version") == 1 and evidence.get("role") == role and evidence.get("result") == "passed"
                and evidence.get("analysis_base_sha") == analysis_base_sha and isinstance(evidence.get("checks"), dict) and evidence["checks"],
                "analysis process did not emit matching completed-role evidence")
        if role == "codeql":
            check_sarif(result / "codeql.sarif")
        final_head = execute(runner, ["git", "rev-parse", "HEAD"], cwd=workspace, env=environment, emit_stdout=False).stdout.strip()
        require(final_head == head, "analysis source SHA changed during execution")
        execute(runner, ["git", "diff", "--quiet", "HEAD", "--"], cwd=workspace, env=environment)
        evidence.update({"candidate_digest": ci.canonical_digest(candidate), "policy_digest": ci.policy_identity(policy),
                         "source_sha": head, "config_digest": candidate["image"]["config_digest"],
                         "role_materials": policy["profiles"][role]["role_materials"],
                         "artifacts": {path.name: file_sha(path) for path in result.iterdir() if path.name != "analysis-evidence.json"}})
        write_json(result / "analysis-evidence.json", evidence)
        require(not output.exists() or (output.is_dir() and not output.is_symlink() and not any(output.iterdir())), "analysis output changed during execution")
        os.replace(result, output)
        return evidence


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else argv
    inside = bool(arguments and arguments[0] == "inside")
    parser = argparse.ArgumentParser(description=__doc__)
    if inside:
        parser.add_argument("phase", choices=("configure", "codeql-prebuild", "codeql-trace-build", "qualify"))
    parser.add_argument("--role", choices=ROLES, required=True)
    parser.add_argument("--workspace", type=pathlib.Path, required=True)
    parser.add_argument("--analysis-base-sha")
    parser.add_argument("--output", type=pathlib.Path)
    if inside:
        parser.add_argument("--build-dir", type=pathlib.Path)
        parser.add_argument("--work-root", type=pathlib.Path)
        parser.add_argument("--codeql-root", type=pathlib.Path)
        parser.add_argument("--expected-codeql-version")
        parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    else:
        parser.add_argument("--candidate-root", type=pathlib.Path, required=True)
        parser.add_argument("--role-materials", type=pathlib.Path, required=True)
    args = parser.parse_args(arguments[1:] if inside else arguments)
    try:
        if not inside:
            require(args.output is not None, "--output is required")
            qualify(args.role, args.candidate_root, args.workspace, args.analysis_base_sha,
                    ci.load_json(args.role_materials), args.output)
        elif args.phase == "qualify":
            require(args.work_root is not None and args.output is not None, "qualification requires private work/output directories")
            run_analysis(args.role, args.workspace, args.work_root, args.output, args.analysis_base_sha,
                         codeql_root=args.codeql_root, expected_codeql_version=args.expected_codeql_version, jobs=args.jobs)
        else:
            require(args.build_dir is not None, "shared analysis phase requires --build-dir")
            workspace, build = args.workspace.resolve(), args.build_dir.resolve()
            document, environment = role_environment(workspace, args.role)
            if args.phase == "configure":
                configure(args.role, workspace, build, subprocess.run, environment, document)
            else:
                require(args.role == "codeql", "prebuild/trace phases are CodeQL-only")
                require(build.is_dir(), "CodeQL build directory has not been configured")
                function = codeql_prebuild if args.phase == "codeql-prebuild" else codeql_trace_build
                function(workspace, build, subprocess.run, environment)
        return 0
    except (ci.ContractError, OSError, ValueError, RuntimeError) as error:
        print("qualify_ci_analysis: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
