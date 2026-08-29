#!/usr/bin/env python3
"""Build the complete ADB executable from the verified, disconnected source closure."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shlex
import shutil
import subprocess
import sys


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, text=True, capture_output=True, env=env, check=False)
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        raise RuntimeError(f"command failed with exit code {result.returncode}: {' '.join(command)}")
    return result.stdout


def run_smoke(command: list[str], report_path: pathlib.Path) -> None:
    try:
        run(command)
    except RuntimeError as error:
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            report = None
        if isinstance(report, dict):
            detail = report.get("error") or report.get("cleanup_error")
            if isinstance(detail, str) and detail:
                raise RuntimeError(f"ADB helper smoke failed: {detail}") from error
        raise


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def apply_locked_patch(
    source: pathlib.Path, patch_path: pathlib.Path, record: dict
) -> None:
    apply_tool = record.get("apply_tool")
    if apply_tool != "gnu-patch":
        raise RuntimeError(
            f"locked ADB patch requires explicit supported apply_tool=gnu-patch: {patch_path}"
        )
    run(
        [
            os.environ.get("KLOGG_ADB_PATCH_EXECUTABLE", "patch"),
            "--directory",
            str(source),
            "--strip",
            "1",
            "--batch",
            "--forward",
            "--input",
            str(patch_path),
        ]
    )


def verify_dependency_resolution(superbuild: pathlib.Path, prefix: pathlib.Path) -> None:
    cache = superbuild / "adb_android_tools-prefix/src/adb_android_tools-build/CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"ADB dependency resolution cache is missing: {cache}")
    prefix_resolved = prefix.resolve()
    checked = 0
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line or line.startswith(("//", "#")):
            continue
        key, value = line.split("=", 1)
        name = key.split(":", 1)[0]
        if not (
            name.startswith("ZLIB_LIBRARY")
            or name.startswith("Protobuf_")
            or name.startswith("pkgcfg_lib_libbrotli")
            or name.startswith("pkgcfg_lib_liblz4")
            or name.startswith("pkgcfg_lib_libzstd")
        ):
            continue
        if not value or value.endswith("-NOTFOUND") or not pathlib.Path(value).is_absolute():
            continue
        checked += 1
        resolved = pathlib.Path(value).resolve()
        if prefix_resolved != resolved and prefix_resolved not in resolved.parents:
            raise RuntimeError(f"system dependency substitution detected in {name}: {value}")
        is_link_library = (
            name.startswith("ZLIB_LIBRARY")
            or (name.startswith("Protobuf_") and "LIBRARY" in name)
            or name.startswith("pkgcfg_lib_libbrotli")
            or name.startswith("pkgcfg_lib_liblz4")
            or name.startswith("pkgcfg_lib_libzstd")
        )
        if is_link_library and resolved.suffix != ".a":
            raise RuntimeError(
                f"ADB dependency must resolve to a pinned static archive in {name}: {value}"
            )
    if checked == 0:
        raise RuntimeError("ADB build did not expose any locked dependency resolution paths")

    build_directory = cache.parent
    compile_commands = build_directory / "compile_commands.json"
    if not compile_commands.is_file():
        raise RuntimeError(f"ADB compile command audit is missing: {compile_commands}")
    commands = json.loads(compile_commands.read_text(encoding="utf-8"))
    if not isinstance(commands, list) or not commands:
        raise RuntimeError("ADB compile command audit is empty")
    forbidden_paths = ("/usr/local/include", "/usr/local/lib")
    for entry in commands:
        if not isinstance(entry, dict):
            raise RuntimeError("ADB compile command audit contains a non-object entry")
        arguments = entry.get("arguments")
        if not isinstance(arguments, list):
            command = entry.get("command")
            if not isinstance(command, str):
                raise RuntimeError("ADB compile command lacks arguments and command text")
            arguments = shlex.split(command)
        for index, argument in enumerate(arguments):
            text = str(argument)
            next_text = str(arguments[index + 1]) if index + 1 < len(arguments) else ""
            for forbidden in forbidden_paths:
                if (
                    text == forbidden
                    or text in ("-I" + forbidden, "-L" + forbidden, "-isystem" + forbidden)
                    or (text in ("-I", "-L", "-isystem") and next_text == forbidden)
                ):
                    raise RuntimeError(
                        f"uncontrolled {forbidden} path in ADB compile command: {entry.get('file')}"
                    )

    for dependency_file in build_directory.rglob("*.d"):
        dependencies = dependency_file.read_text(encoding="utf-8", errors="replace")
        if "/usr/local/include/" in dependencies:
            raise RuntimeError(
                f"ADB build consumed an uncontrolled /usr/local header: {dependency_file}"
            )

    for link_command in build_directory.rglob("link.txt"):
        command = link_command.read_text(encoding="utf-8", errors="replace")
        if "/usr/local/lib/" in command:
            raise RuntimeError(
                f"ADB build consumed an uncontrolled /usr/local library: {link_command}"
            )


def normalized_host_target() -> tuple[str, str]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    os_name = {"darwin": "macos", "linux": "linux", "windows": "windows"}.get(system, system)
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "aarch64": "arm64", "arm64": "arm64"}.get(machine, machine)
    return os_name, arch


def verify_forbidden_imports(imports: list[str], target_plan: dict) -> None:
    import_names = {
        pathlib.PurePosixPath(item.replace("\\", "/")).name.lower() for item in imports
    }
    violations = []
    for forbidden in target_plan.get("usb", {}).get("forbidden_imports", []):
        normalized = str(forbidden).lower()
        if normalized.endswith(".dll"):
            matched = normalized in import_names
        else:
            matched = any(normalized in imported for imported in import_names)
        if matched:
            violations.append(str(forbidden))
    if violations:
        raise RuntimeError(
            "ADB helper imports forbidden runtime material: " + ", ".join(violations)
        )


def inspect_binary(
    target: str,
    helper: pathlib.Path,
    helper_dir: pathlib.Path,
    target_plan: dict | None = None,
) -> tuple[list[str], list[str], list[str], str, str | None, str | None, list[str]]:
    target_plan = target_plan or {}
    expected_arch = target.split("-", 1)[1]
    if target.startswith("macos-"):
        output = run(["otool", "-L", str(helper)])
        imports = [line.strip().split(" ", 1)[0] for line in output.splitlines()[1:] if line.strip()]
        frameworks = []
        for framework in ("IOKit", "CoreFoundation"):
            if any(f"/{framework}.framework/" in item for item in imports):
                frameworks.append(framework)
        if any("libusb" in item.lower() for item in imports):
            raise RuntimeError("macOS helper imports libusb instead of using the native IOKit backend")
        allowed_imports = target_plan.get("allowed_dynamic_imports")
        if allowed_imports is not None:
            if not isinstance(allowed_imports, list) or not all(
                isinstance(item, str) for item in allowed_imports
            ):
                raise RuntimeError("macOS locked dynamic import allowlist must be an array of paths")
            unexpected_imports = sorted(set(imports) - set(allowed_imports))
            if unexpected_imports:
                raise RuntimeError(
                    "macOS helper has an unlocked dynamic import: "
                    + ", ".join(unexpected_imports)
                )
        unbundled_imports = [
            item
            for item in imports
            if not item.startswith(("/System/Library/", "/usr/lib/"))
        ]
        if unbundled_imports:
            raise RuntimeError(
                "macOS helper has unbundled non-system dynamic imports: "
                + ", ".join(unbundled_imports)
            )
        architectures = run(["lipo", "-archs", str(helper)]).split()
        if architectures != [expected_arch]:
            raise RuntimeError(
                f"macOS helper architecture mismatch: expected {expected_arch}, got {architectures}"
            )
        load_commands = run(["otool", "-l", str(helper)])
        deployment_target = None
        in_build_version = False
        for line in load_commands.splitlines():
            stripped = line.strip()
            if stripped == "cmd LC_BUILD_VERSION":
                in_build_version = True
            elif in_build_version and stripped.startswith("minos "):
                deployment_target = stripped.split(None, 1)[1]
                break
        if deployment_target is None:
            raise RuntimeError("macOS helper lacks LC_BUILD_VERSION/minos deployment metadata")
        verify_forbidden_imports(imports, target_plan)
        return imports, frameworks, architectures, "not-applicable", deployment_target, None, []
    if target.startswith("linux-"):
        output = run(["readelf", "-d", str(helper)])
        imports = []
        runpaths = []
        for line in output.splitlines():
            if "(NEEDED)" in line and "[" in line and "]" in line:
                imports.append(line.split("[", 1)[1].split("]", 1)[0])
            if "(RUNPATH)" in line and "[" in line and "]" in line:
                runpaths.append(line.split("[", 1)[1].split("]", 1)[0])
        allowed_imports = target_plan.get("allowed_dynamic_imports")
        if allowed_imports is not None:
            if not isinstance(allowed_imports, list) or not all(
                isinstance(item, str) for item in allowed_imports
            ):
                raise RuntimeError("Linux locked dynamic import allowlist must be an array")
            unexpected_imports = sorted(set(imports) - set(allowed_imports))
            if unexpected_imports:
                raise RuntimeError(
                    "Linux helper has an unlocked dynamic import: "
                    + ", ".join(unexpected_imports)
                )
        if "libusb-1.0.so.0" not in imports:
            raise RuntimeError("Linux helper does not dynamically import private libusb-1.0.so.0")
        if runpaths != ["$ORIGIN"]:
            raise RuntimeError(
                f"Linux helper RUNPATH must be exactly $ORIGIN, got {runpaths}"
            )
        header = run(["readelf", "-h", str(helper)])
        machine_match = re.search(r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE)
        machine = machine_match.group(1).lower() if machine_match else ""
        actual_arch = (
            "x86_64" if "x86-64" in machine or "amd64" in machine
            else "arm64" if "aarch64" in machine
            else machine
        )
        if actual_arch != expected_arch:
            raise RuntimeError(
                f"Linux helper architecture mismatch: expected {expected_arch}, got {actual_arch or 'unknown'}"
            )
        ldd = run(["ldd", str(helper)])
        resolution = re.search(r"^\s*libusb-1\.0\.so\.0\s+=>\s+(\S+)", ldd, re.MULTILINE)
        if resolution is None or resolution.group(1) == "not":
            raise RuntimeError("Linux private libusb replacement/resolution probe failed")
        resolved_libusb = pathlib.Path(resolution.group(1)).resolve()
        if resolved_libusb.parent != helper_dir.resolve():
            raise RuntimeError(
                f"Linux libusb resolved outside the private helper directory: {resolved_libusb}"
            )

        baseline = target_plan.get("glibc_baseline")
        if not isinstance(baseline, str) or not re.fullmatch(r"\d+\.\d+", baseline):
            raise RuntimeError(f"Linux target lacks a valid locked GLIBC baseline: {baseline}")
        symbol_maximums = target_plan.get("symbol_version_maximums", {"GLIBC": baseline})
        if not isinstance(symbol_maximums, dict) or symbol_maximums.get("GLIBC") != baseline:
            raise RuntimeError("Linux symbol version policy must bind GLIBC to glibc_baseline")
        parsed_maximums: dict[str, tuple[int, ...]] = {}
        for namespace, maximum in symbol_maximums.items():
            if (
                namespace not in ("GLIBC", "GLIBCXX", "CXXABI")
                or not isinstance(maximum, str)
                or re.fullmatch(r"\d+(?:\.\d+)+", maximum) is None
            ):
                raise RuntimeError(f"invalid locked Linux symbol maximum {namespace}={maximum}")
            parsed_maximums[namespace] = tuple(int(part) for part in maximum.split("."))

        versioned_files = [helper]
        for runtime_name in target_plan.get("usb", {}).get("runtime_files", []):
            versioned_files.append(helper_dir / runtime_name)
        required_versions: dict[str, set[tuple[int, ...]]] = {
            namespace: set() for namespace in parsed_maximums
        }
        for binary_path in versioned_files:
            version_info = run(["readelf", "--version-info", str(binary_path)])
            for namespace, version in re.findall(
                r"\b(GLIBCXX|GLIBC|CXXABI)_(\d+(?:\.\d+)+)\b", version_info
            ):
                parsed = tuple(int(part) for part in version.split("."))
                if namespace not in parsed_maximums:
                    raise RuntimeError(
                        f"Linux helper closure uses unlocked {namespace}_{version} symbol contract"
                    )
                required_versions[namespace].add(parsed)
        if not required_versions["GLIBC"]:
            raise RuntimeError("Linux helper closure exposes no GLIBC version requirements")
        for namespace, versions in required_versions.items():
            if not versions:
                continue
            actual_maximum = max(versions)
            if actual_maximum > parsed_maximums[namespace]:
                actual_text = ".".join(str(part) for part in actual_maximum)
                raise RuntimeError(
                    f"Linux helper closure requires {namespace}_{actual_text}, newer than locked maximum {namespace}_{symbol_maximums[namespace]}"
                )
        maximum_text = ".".join(
            str(part) for part in max(required_versions["GLIBC"])
        )
        verify_forbidden_imports(imports, target_plan)
        return imports, [], [actual_arch], "passed", None, maximum_text, []
    if target == "windows-x86_64":
        import ctypes

        closure = [helper]
        closure.extend(
            helper_dir / name
            for name in target_plan.get("usb", {}).get("runtime_files", [])
        )
        imports: list[str] = []
        for binary in closure:
            output = run(["dumpbin", "/nologo", "/dependents", str(binary)])
            for match in re.finditer(r"^\s*([A-Za-z0-9_.+-]+\.dll)\s*$", output, re.MULTILINE | re.IGNORECASE):
                imported = match.group(1)
                if imported.lower() not in {item.lower() for item in imports}:
                    imports.append(imported)

        headers = run(["dumpbin", "/nologo", "/headers", str(helper)])
        machine_match = re.search(r"machine\s*\(([^)]+)\)", headers, re.IGNORECASE)
        machine = machine_match.group(1).lower() if machine_match else ""
        actual_arch = "x86_64" if machine in ("x64", "amd64") else machine
        if actual_arch != expected_arch:
            raise RuntimeError(
                f"Windows helper architecture mismatch: expected {expected_arch}, got {actual_arch or 'unknown'}"
            )

        runtime_loads: list[str] = []
        dll_scope = os.add_dll_directory(str(helper_dir)) if hasattr(os, "add_dll_directory") else None
        try:
            for runtime_name in target_plan.get("usb", {}).get("runtime_files", []):
                ctypes.WinDLL(str(helper_dir / runtime_name))
                runtime_loads.append(runtime_name)
        finally:
            if dll_scope is not None:
                dll_scope.close()
        verify_forbidden_imports(imports, target_plan)
        return imports, [], [actual_arch], "passed", None, None, runtime_loads
    raise RuntimeError(f"no native binary inspector is available for locked target {target}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=pathlib.Path)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-root", required=True, type=pathlib.Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    parser.add_argument("--release-assets-root", required=True, type=pathlib.Path)
    parser.add_argument("--parallel", default=str(os.cpu_count() or 2))
    parser.add_argument(
        "--inspection-only",
        action="store_true",
        help="allow a macOS thin cross-build for binary inspection without claiming executable smoke evidence",
    )
    args = parser.parse_args()

    try:
        parallel = int(args.parallel)
    except ValueError as error:
        raise RuntimeError(f"invalid ADB build parallelism: {args.parallel}") from error
    if parallel <= 0:
        raise RuntimeError(f"invalid ADB build parallelism: {args.parallel}")
    build_env = os.environ.copy()
    build_env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(parallel)

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    target_plan = lock.get("targets", {}).get(args.target)
    if not isinstance(target_plan, dict):
        raise RuntimeError(f"unknown locked ADB helper target: {args.target}")
    toolchain = lock.get("toolchains", {}).get(target_plan.get("toolchain"))
    generator = toolchain.get("cmake_generator") if isinstance(toolchain, dict) else None
    if generator not in ("Ninja", "Unix Makefiles"):
        raise RuntimeError(f"unsupported or missing locked CMake generator: {generator}")
    host_os, host_arch = normalized_host_target()
    native_build = (host_os, host_arch) == (target_plan["os"], target_plan["arch"])
    macos_thin_cross_build = (
        args.inspection_only
        and host_os == "macos"
        and target_plan["os"] == "macos"
        and host_arch in ("x86_64", "arm64")
        and target_plan["arch"] in ("x86_64", "arm64")
    )
    if not native_build and not macos_thin_cross_build:
        raise RuntimeError(
            f"native source build required for {args.target}; current host is {host_os}-{host_arch} and only an explicit macOS thin inspection cross-build is allowed"
        )

    work_sources = args.build_root / "sources"
    install_prefix = args.build_root / "install"
    superbuild = args.build_root / "superbuild"
    shutil.rmtree(args.build_root, ignore_errors=True)
    shutil.copytree(args.source_root, work_sources, symlinks=True)
    android_source = work_sources / "android-tools-release"
    if not android_source.is_dir():
        raise RuntimeError(f"verified android-tools release source is missing: {android_source}")

    for patch in lock.get("patches", []):
        targets = patch.get("targets")
        if isinstance(targets, list) and args.target not in targets:
            continue
        excluded_targets = patch.get("exclude_targets")
        if isinstance(excluded_targets, list) and args.target in excluded_targets:
            continue
        source_id = patch.get("applies_to", "android-tools-release")
        patch_source = work_sources / source_id
        if not patch_source.is_dir():
            raise RuntimeError(f"locked ADB patch source is missing: {source_id}")
        patch_path = args.repository_root / patch["path"]
        actual = sha256(patch_path)
        if actual != patch["sha256"]:
            raise RuntimeError(f"locked ADB patch sha256 mismatch: {patch_path}")
        if patch.get("apply", True) is False:
            continue
        apply_locked_patch(patch_source, patch_path, patch)

    if args.target == "windows-x86_64":
        development_source = work_sources / "windows-platform-development"
        vendor_development = android_source / "vendor/development"
        if not development_source.is_dir():
            raise RuntimeError(
                f"locked platform/development Windows source is missing: {development_source}"
            )
        shutil.copytree(development_source, vendor_development, symlinks=True)
        definition = args.repository_root / "packaging/adb/patches/msys2-android-tools/AdbWinApi.def"
        if not definition.is_file():
            raise RuntimeError(f"locked MSYS2 AdbWinApi definition is missing: {definition}")
        shutil.copy2(definition, android_source / "vendor/AdbWinApi.def")

    configure = [
        "cmake",
        "-S", str(args.repository_root / "packaging/adb/superbuild"),
        "-B", str(superbuild),
        "-G", generator,
        f"-DKLOGG_ADB_SOURCE_ROOT={work_sources}",
        f"-DKLOGG_ADB_INSTALL_PREFIX={install_prefix}",
        f"-DKLOGG_ADB_TARGET={args.target}",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
    ]
    run(configure, env=build_env)
    run(
        [
            "cmake",
            "--build",
            str(superbuild),
            "--target",
            "klogg_adb_helper",
            "--parallel",
            str(parallel),
        ],
        env=build_env,
    )
    verify_dependency_resolution(superbuild, install_prefix)

    installed_helper = install_prefix / "bin" / ("adb.exe" if host_os == "windows" else "adb")
    if not installed_helper.is_file():
        raise RuntimeError(f"complete ADB executable was not installed: {installed_helper}")

    helper_dir = args.artifact_root / "helpers"
    shutil.rmtree(args.artifact_root, ignore_errors=True)
    helper_dir.mkdir(parents=True)
    helper = helper_dir / installed_helper.name
    shutil.copy2(installed_helper, helper)
    helper.chmod(helper.stat().st_mode | 0o755)
    runtime_closure: list[dict] = []
    for runtime_name in target_plan.get("usb", {}).get("runtime_files", []):
        if pathlib.PurePath(runtime_name).name != runtime_name or any(
            separator in runtime_name for separator in ("/", "\\")
        ):
            raise RuntimeError(f"unsafe locked ADB runtime file name: {runtime_name}")
        installed_runtime = install_prefix / "bin" / runtime_name
        if not installed_runtime.exists():
            raise RuntimeError(
                f"private ADB runtime closure is missing from the install: {installed_runtime}"
            )
        resolved_runtime = installed_runtime.resolve(strict=True)
        if not resolved_runtime.is_file():
            raise RuntimeError(f"ADB runtime closure is not a regular file: {resolved_runtime}")
        destination = helper_dir / runtime_name
        shutil.copy2(resolved_runtime, destination)
        runtime_closure.append(
            {
                "name": runtime_name,
                "sha256": sha256(destination),
                "symlink": False,
            }
        )

    (
        imports,
        frameworks,
        architectures,
        replacement,
        deployment_target,
        glibc_maximum_required,
        runtime_loads,
    ) = inspect_binary(args.target, helper, helper_dir, target_plan)

    smoke_report = args.artifact_root / "smoke.json"
    if native_build:
        run_smoke(
            [
                sys.executable,
                str(args.repository_root / "scripts/smoke_adb_helper.py"),
                "--adb", str(helper),
                "--port", "0",
                "--timeout-seconds", "15",
                "--json-output", str(smoke_report),
            ],
            smoke_report,
        )
        smoke = json.loads(smoke_report.read_text(encoding="utf-8"))
    else:
        smoke = {
            "schema_version": 1,
            "receipt_kind": "binary-smoke",
            "status": "not-run",
            "reason": "cross-build-inspection-only",
            "passed_probes": [],
        }
        smoke_report.write_text(
            json.dumps(smoke, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    release_assets = json.loads(
        (args.release_assets_root / "adb-helper-release-assets.json").read_text(encoding="utf-8")
    )
    source_set_assets = [
        asset
        for asset in release_assets
        if isinstance(asset, dict) and asset.get("kind") == "source-set-receipt"
    ]
    if len(source_set_assets) != 1:
        raise RuntimeError("ADB release assets must contain exactly one source-set receipt")
    source_set_receipt_sha256 = source_set_assets[0].get("sha256")
    if not isinstance(source_set_receipt_sha256, str):
        raise RuntimeError("ADB source-set receipt lacks sha256 binding")
    receipt = {
        "schema_version": 1,
        "receipt_kind": "binary-build",
        "lock_sha256": sha256(args.lock),
        "target": args.target,
        "layout": "artifact",
        "qualification": target_plan.get("qualification", {}),
        "source_set_receipt_sha256": source_set_receipt_sha256,
        "helper": {
            "path": f"helpers/{helper.name}",
            "sha256": sha256(helper),
            "executable": True,
            "symlink": False,
            "kind": "complete-adb-executable",
        },
        "binary_verification": {
            "dynamic_imports": imports,
            "native_frameworks": frameworks,
            "architectures": architectures,
            "runtime_closure": runtime_closure,
            "runtime_loads": runtime_loads,
            "deployment_target": deployment_target,
            "glibc_maximum_required": glibc_maximum_required,
            "libusb_replacement_probe": replacement,
            "version_probe": "passed" if "version" in smoke.get("passed_probes", []) else "not-run",
            "complete_client_probe": "passed" if "complete-client" in smoke.get("passed_probes", []) else "not-run",
        },
        "release_assets": release_assets,
    }
    (args.artifact_root / "receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
