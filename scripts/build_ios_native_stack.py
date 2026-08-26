#!/usr/bin/env python3
"""Build the locked macOS iOS-native dylib closure from disconnected sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile


class BuildError(RuntimeError):
    pass


def run(command: list[str], *, cwd: pathlib.Path | None = None, env: dict[str, str] | None = None) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True, check=False)
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        raise BuildError(f"command failed ({result.returncode}): {' '.join(command)}")
    return result.stdout


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_sha256(root: pathlib.Path) -> str:
    manifest = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root)
        if ".git" in relative.parts:
            continue
        manifest.append(f"{relative.as_posix()}:{sha256(path)}\n")
    return hashlib.sha256("".join(manifest).encode()).hexdigest()


def safe_extract(archive: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:*") as source:
        members = source.getmembers()
        seen: set[pathlib.PurePosixPath] = set()
        for member in members:
            relative = pathlib.PurePosixPath(member.name)
            if (
                relative.is_absolute()
                or not relative.parts
                or ".." in relative.parts
                or member.issym()
                or member.islnk()
                or not (member.isfile() or member.isdir())
                or relative in seen
            ):
                raise BuildError(f"unsafe source archive member: {member.name}")
            seen.add(relative)
        source.extractall(destination)
    roots = [item for item in destination.iterdir() if item.is_dir()]
    if len(roots) != 1:
        raise BuildError(f"source archive must contain one top-level directory: {archive}")
    return roots[0]


def source_records(lock: dict) -> dict[str, dict]:
    records = lock.get("sources")
    if not isinstance(records, list):
        raise BuildError("lock sources must be an array")
    return {str(record["id"]): record for record in records}


def prepare_sources(lock: dict, archive_root: pathlib.Path, work_root: pathlib.Path,
                    repository_root: pathlib.Path) -> dict[str, pathlib.Path]:
    prepared: dict[str, pathlib.Path] = {}
    for source_id, record in source_records(lock).items():
        archive = archive_root / record["archive_file"]
        if not archive.is_file():
            raise BuildError(f"missing locked source archive: {archive}")
        if sha256(archive) != record["archive_sha256"]:
            raise BuildError(f"locked source archive sha256 mismatch: {source_id}")
        prepared[source_id] = safe_extract(archive, work_root / source_id)

    for patch in lock.get("patches", []):
        if not patch.get("required"):
            continue
        source_id = patch["source_id"]
        source = prepared[source_id]
        source_record = source_records(lock)[source_id]
        if source_record["archive_sha256"] != patch["source_archive_sha256"]:
            raise BuildError(f"patch source archive binding mismatch: {source_id}")
        if tree_sha256(source) != patch["clean_tree_sha256"]:
            raise BuildError(f"clean source tree sha256 mismatch before mandatory patch: {source_id}")
        patch_file = repository_root / "3rdparty" / "libimobiledevice" / patch["path"]
        if not patch_file.is_file() or sha256(patch_file) != patch["sha256"]:
            raise BuildError(f"mandatory patch sha256 mismatch: {patch_file}")
        run(["patch", "--directory", str(source), "--strip", "1", "--batch", "--forward",
             "--input", str(patch_file)])
        if tree_sha256(source) != patch["patched_tree_sha256"]:
            raise BuildError(f"patched source tree sha256 mismatch: {source_id}")
    return prepared


def deployment_target(lock: dict, architecture: str) -> str:
    thin = lock["artifact_contract"]["thin_artifacts"].get(architecture)
    if not isinstance(thin, dict):
        raise BuildError(f"unsupported locked architecture: {architecture}")
    return str(thin["deployment_target"])


def build_environment(prefix: pathlib.Path, architecture: str, target: str) -> dict[str, str]:
    environment = os.environ.copy()
    arch_flag = f"-arch {architecture}"
    minimum_flag = f"-mmacosx-version-min={target}"
    environment.update(
        {
            "MACOSX_DEPLOYMENT_TARGET": target,
            "CC": environment.get("CC", "clang"),
            "CXX": environment.get("CXX", "clang++"),
            "CFLAGS": f"{arch_flag} {minimum_flag} -O2",
            "CXXFLAGS": f"{arch_flag} {minimum_flag} -O2",
            "CPPFLAGS": f"-I{prefix / 'include'}",
            "LDFLAGS": f"{arch_flag} {minimum_flag} -L{prefix / 'lib'}",
            "PKG_CONFIG_PATH": str(prefix / "lib" / "pkgconfig"),
            "PKG_CONFIG_LIBDIR": str(prefix / "lib" / "pkgconfig"),
            "ac_cv_path_CYTHON": "no",
        }
    )
    return environment


def build_openssl(source: pathlib.Path, prefix: pathlib.Path, architecture: str,
                  target: str, parallel: str, environment: dict[str, str]) -> None:
    openssl_target = {
        "x86_64": "darwin64-x86_64-cc",
        "arm64": "darwin64-arm64-cc",
    }[architecture]
    run(
        ["perl", "Configure", openssl_target, "shared", "no-tests", "no-apps",
         f"--prefix={prefix}", f"--openssldir={prefix / 'ssl'}",
         f"-mmacosx-version-min={target}"],
        cwd=source,
        env=environment,
    )
    run(["make", f"-j{parallel}"], cwd=source, env=environment)
    run(["make", "install_sw"], cwd=source, env=environment)


def configure_autotools(source: pathlib.Path, prefix: pathlib.Path, options: list[str],
                        parallel: str, environment: dict[str, str]) -> None:
    run(["autoreconf", "-fi"], cwd=source, env=environment)
    run([str(source / "configure"), f"--prefix={prefix}", "--disable-static", "--enable-shared",
         *options], cwd=source, env=environment)
    run(["make", f"-j{parallel}"], cwd=source, env=environment)
    run(["make", "install"], cwd=source, env=environment)


def dylib_dependencies(path: pathlib.Path) -> list[str]:
    output = run(["otool", "-L", str(path)])
    return [line.strip().split(" ", 1)[0] for line in output.splitlines()[1:] if line.strip()]


def dylib_rpaths(path: pathlib.Path) -> list[str]:
    lines = run(["otool", "-l", str(path)]).splitlines()
    values: list[str] = []
    for index, line in enumerate(lines):
        if line.strip() != "cmd LC_RPATH":
            continue
        for detail in lines[index + 1 : index + 5]:
            match = re.match(r"^\s*path\s+(\S+)\s+\(offset\s+\d+\)\s*$", detail)
            if match is not None:
                values.append(match.group(1))
                break
        else:
            raise BuildError(f"malformed LC_RPATH command: {path}")
    return values


def normalize_install_names(prefix: pathlib.Path) -> list[pathlib.Path]:
    libdir = prefix / "lib"
    dylibs = sorted(path for path in libdir.glob("*.dylib") if path.is_file() and not path.is_symlink())
    if not dylibs:
        raise BuildError("source build installed no dylibs")
    for dylib in dylibs:
        run(["install_name_tool", "-id", f"@rpath/{dylib.name}", str(dylib)])
        for inherited_rpath in dylib_rpaths(dylib):
            run(["install_name_tool", "-delete_rpath", inherited_rpath, str(dylib)])
        run(["install_name_tool", "-add_rpath", "@loader_path", str(dylib)])
    for dylib in dylibs:
        for dependency in dylib_dependencies(dylib):
            candidate = pathlib.Path(dependency)
            if str(candidate).startswith(str(libdir)):
                run(["install_name_tool", "-change", dependency, f"@rpath/{candidate.name}", str(dylib)])
    return dylibs


def select_runtime_dylibs(prefix: pathlib.Path, required_dylibs: list[str]) -> list[pathlib.Path]:
    libdir = (prefix / "lib").resolve()
    selected: set[pathlib.Path] = set()
    for name in required_dylibs:
        candidate = prefix / "lib" / name
        if not candidate.exists():
            raise BuildError(f"required runtime dylib was not built: {name}")
        resolved = candidate.resolve()
        if resolved.parent != libdir or not resolved.is_file():
            raise BuildError(f"required runtime dylib escapes the private closure: {name}")
        selected.add(resolved)
    return sorted(selected)


def stage_artifact(prefix: pathlib.Path, artifact_root: pathlib.Path,
                   dylibs: list[pathlib.Path]) -> list[dict]:
    shutil.rmtree(artifact_root, ignore_errors=True)
    output_lib = artifact_root / "lib"
    output_lib.mkdir(parents=True)
    shutil.copytree(prefix / "include", artifact_root / "include")
    identities = []
    for source in dylibs:
        destination = output_lib / source.name
        shutil.copy2(source, destination)
        identities.append({"name": destination.name, "sha256": sha256(destination)})
    for link in sorted(path for path in (prefix / "lib").glob("*.dylib") if path.is_symlink()):
        target = pathlib.Path(os.readlink(link)).name
        if (output_lib / target).exists():
            (output_lib / link.name).symlink_to(target)
    return identities


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", required=True, type=pathlib.Path)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive-root", required=True, type=pathlib.Path)
    parser.add_argument("--work-root", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    parser.add_argument("--architecture", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--parallel", default=str(os.cpu_count() or 2))
    parser.add_argument("--inspection-only", action="store_true")
    args = parser.parse_args()

    if platform.system() != "Darwin":
        raise BuildError("the native iOS stack superbuild is macOS-only")
    host_arch = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "arm64"}.get(
        platform.machine(), platform.machine()
    )
    native_build = host_arch == args.architecture
    if not native_build and not args.inspection_only:
        raise BuildError("cross-built thin macOS artifacts require --inspection-only")

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    target = deployment_target(lock, args.architecture)
    shutil.rmtree(args.work_root, ignore_errors=True)
    sources = prepare_sources(lock, args.archive_root, args.work_root / "sources",
                              args.repository_root)
    prefix = args.work_root / "install"
    prefix.mkdir(parents=True)

    # Locked build order is openssl -> curl -> libplist -> libtatsu ->
    # libimobiledevice-glue -> libusbmuxd -> libimobiledevice. ExternalProject consumes a flat,
    # already-verified source root. Moving the extracted trees preserves the
    # clean/patched hash evidence while keeping every configure/build step
    # inside the disconnected CMake superbuild.
    superbuild_sources = args.work_root / "prepared-sources"
    superbuild_sources.mkdir()
    records = source_records(lock)
    for source_id, source in sources.items():
        destination = superbuild_sources / source_id
        shutil.move(str(source), destination)
        if source_id != "openssl":
            # Immutable GitHub commit archives contain neither .git metadata nor the
            # release-tarball marker expected by the projects' autogen.sh scripts.
            # Stamp the exact locked version only after clean/patched tree hashes have
            # been verified so this reproducibility input cannot weaken source binding.
            record = records[source_id]
            (destination / ".tarball-version").write_text(
                str(record["version"]) + "\n", encoding="utf-8"
            )

    superbuild = args.work_root / "superbuild"
    run([
        "cmake",
        "-S", str(args.repository_root / "packaging/ios-native/superbuild"),
        "-B", str(superbuild),
        "-G", "Ninja",
        f"-DKLOGG_IOS_SOURCE_ROOT={superbuild_sources}",
        f"-DKLOGG_IOS_INSTALL_PREFIX={prefix}",
        f"-DKLOGG_IOS_ARCHITECTURE={args.architecture}",
        f"-DKLOGG_IOS_DEPLOYMENT_TARGET={target}",
        f"-DKLOGG_IOS_PARALLEL={args.parallel}",
        f"-DKLOGG_IOS_SOURCE_DATE_EPOCH={lock['release_policy']['source_date_epoch']}",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
    ])
    run([
        "cmake", "--build", str(superbuild), "--target", "klogg_ios_native_stack",
        "--parallel", args.parallel,
    ])

    normalize_install_names(prefix)
    dylibs = select_runtime_dylibs(
        prefix, lock["artifact_contract"]["required_dylibs"]
    )
    identities = stage_artifact(prefix, args.artifact_root, dylibs)
    qualification_allowed = bool(
        lock["artifact_contract"]["thin_artifacts"][args.architecture].get("native_qualified")
    )
    native_qualified = native_build and qualification_allowed
    toolchain = {
        "clang": run(["clang", "--version"]).splitlines()[0],
        "sdk_path": run(["xcrun", "--show-sdk-path"]).strip(),
        "sdk_version": run(["xcrun", "--show-sdk-version"]).strip(),
        "xcode": run(["xcodebuild", "-version"]).splitlines(),
        "cmake": run(["cmake", "--version"]).splitlines()[0],
    }
    receipt = {
        "schema_version": 1,
        "receipt_kind": "ios-native-build",
        "lock_sha256": sha256(args.lock),
        "architecture": args.architecture,
        "deployment_target": target,
        "MACOSX_DEPLOYMENT_TARGET": target,
        "SOURCE_DATE_EPOCH": lock["release_policy"]["source_date_epoch"],
        "toolchain": toolchain,
        "native_qualified": native_qualified,
        "qualification": "native" if native_qualified else "inspection-only",
        "dylibs": identities,
    }
    (args.artifact_root / "ios-native-build-receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
