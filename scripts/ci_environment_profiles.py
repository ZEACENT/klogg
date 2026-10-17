#!/usr/bin/env python3
"""Materialize explicit CI profiles without conflating recipe and policy hashes.

Policy configuration uses @WORKSPACE@, not a runner's absolute checkout path.
Only profile_environment substitutes the validated runtime workspace. Verification
file bytes affect qualification policy; application commits and output locks do
not become recipe inputs. CodeQL bundles are official, externally resolved role
materials, never guessed pins or redistributed image contents.

The github-env format is for a GitHub environment file, NOT shell evaluation.
Use shell format for quoted assignments, or JSON for structured consumers.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import shlex
import sys

import ci_environment as ci

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE_TOKEN = "@WORKSPACE@"
GENERATED_ENV = {
    "KLOGG_ENVIRONMENT_FAMILY", "KLOGG_ENVIRONMENT_PROFILE", "KLOGG_ENVIRONMENT_ROLE",
    "KLOGG_CMAKE_OPTS", "KLOGG_BUILD_ROOT", "KLOGG_WORKSPACE", "KLOGG_ARCH",
    "KLOGG_PACKAGE_ENABLED", "KLOGG_SANITIZER", "KLOGG_PACKAGE_TAG", "KLOGG_CPACK_GEN",
    "KLOGG_PACKAGE_SUFFIX", "KLOGG_ARTIFACTS_ID", "KLOGG_CONFIG_OS",
    "KLOGG_CHECK_CONTAINER", "KLOGG_CHECK_COMMAND",
}


def _require(condition, message):
    if not condition:
        raise ci.ContractError(message)


def _fields(value, keys, label):
    _require(isinstance(value, dict) and set(value) == set(keys), label + " has missing or unknown fields")


def _single_line(value, label):
    _require(isinstance(value, str) and not any(ord(char) < 32 or ord(char) == 127 for char in value),
             label + " must be a single-line string without control characters")


def _strings(values, label):
    _require(isinstance(values, list), label + " must be a list")
    for value in values:
        _single_line(value, label)
        _require(bool(value), label + " must not contain empty strings")


def _relative_path(value):
    _single_line(value, "repository path")
    _require(bool(value) and not value.startswith("/") and "\\" not in value and ":" not in value
             and all(part not in ("", ".", "..") for part in value.split("/")),
             "unsafe repository-relative path: " + value)


def _file(root, relative):
    _relative_path(relative)
    path = pathlib.Path(root).resolve()
    for part in relative.split("/"):
        path = path / part
        _require(not path.is_symlink(), "verification/recipe path traverses a symlink: " + relative)
    _require(path.is_file(), "required file is missing or not regular: " + relative)
    return path


def _verification_paths(paths):
    _strings(paths, "verification_files")
    _require(len(paths) == len(set(paths)), "duplicate verification file")
    for path in paths:
        _relative_path(path)


def _environment(environment):
    _require(isinstance(environment, dict), "environment must be an object")
    for key, value in environment.items():
        _require(isinstance(key, str) and re.fullmatch(r"[A-Z_][A-Z0-9_]*", key) is not None,
                 "invalid environment variable name")
        _single_line(value, "environment value")


def _validate_document(profiles):
    _fields(profiles, {"schema_version", "linux_defaults", "verification_files", "families"}, "profiles")
    _require(type(profiles["schema_version"]) is int and profiles["schema_version"] == 1, "profiles require schema_version 1")
    _verification_paths(profiles["verification_files"])
    defaults = profiles["linux_defaults"]
    _fields(defaults, {"cmake_options", "verification_files"}, "linux_defaults")
    _strings(defaults["cmake_options"], "default cmake_options")
    _verification_paths(defaults["verification_files"])
    _require(isinstance(profiles["families"], dict) and bool(profiles["families"]), "profile families must be nonempty")
    for family in profiles["families"].values():
        _require(isinstance(family, dict) and bool(family), "family profiles must be nonempty")
        for profile in family.values():
            _fields(profile, {"configuration", "verification_files", "role_materials"}, "profile")
            _verification_paths(profile["verification_files"])
            _strings(profile["role_materials"], "role_materials")
            config = profile["configuration"]
            _fields(config, {"cmake_options", "sanitizer", "package", "role", "environment", "build_root", "package_settings"}, "profile configuration")
            _strings(config["cmake_options"], "cmake_options")
            _require(config["role"] in ("build", "static", "coverage", "codeql"), "unknown profile role")
            _require(config["sanitizer"] in ("", "address", "undefined", "thread"), "unknown sanitizer")
            _require(type(config["package"]) is bool, "package must be a boolean")
            _require(config["role"] == "build" or (not config["package"] and not config["sanitizer"]),
                     "analysis roles cannot package or run a sanitizer profile")
            _require(not config["package"] or not config["sanitizer"], "sanitizer profiles must remain package-free")
            _relative_path(config["build_root"])
            _environment(config["environment"])
            _require(not set(config["environment"]) & GENERATED_ENV, "profile environment overrides generated settings")
            settings = config["package_settings"]
            allowed = {"tag", "generator", "suffix", "artifacts_id", "os", "check_container", "check_command"}
            _require(isinstance(settings, dict) and set(settings) <= allowed, "invalid package_settings")
            for value in settings.values():
                _single_line(value, "package setting")
            if config["package"]:
                _require(set(settings) == allowed and all(settings[key] for key in ("tag", "suffix", "artifacts_id", "os")),
                         "package profile lacks required package settings")
                _require(bool(settings["check_container"]) == bool(settings["check_command"]),
                         "package smoke container and command must be configured together")
            else:
                _require(set(settings) <= {"tag"}, "package-free profile declares packaging behavior")
            required_materials = ["codeql-bundle"] if config["role"] == "codeql" else []
            _require(profile["role_materials"] == required_materials, "role material requirements do not match the profile role")


def validate_profiles(catalog, profiles, repo_root, *, require_recipe_files=True):
    """Validate exact profile coverage; optionally require every recipe input.

    Missing staged external downloads are not recipe files. They belong to the
    separately resolved input manifest and are the producer's responsibility.
    """
    ci.validate_catalog(catalog)
    _validate_document(profiles)
    _require(set(catalog["families"]) == set(profiles["families"]), "catalog and profile family sets differ")
    for family, recipe in catalog["families"].items():
        _require(set(recipe["profiles"]) == set(profiles["families"][family]), "configured profile set mismatch: " + family)
        if require_recipe_files:
            for relative in recipe["recipe_files"]:
                _file(repo_root, relative)


def profile_configuration(profiles, family, profile):
    """Return effective option tokens and canonical, unexpanded runtime settings."""
    _validate_document(profiles)
    _require(isinstance(family, str) and family in profiles["families"], "unknown profile family")
    _require(isinstance(profile, str) and profile in profiles["families"][family], "unknown profile")
    config = copy.deepcopy(profiles["families"][family][profile]["configuration"])
    if config["role"] == "build":
        options = list(profiles["linux_defaults"]["cmake_options"]) + config["cmake_options"]
        if config["package"]:
            options += ["-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ADB_HELPER_TARGET=linux-x86_64",
                        "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=@WORKSPACE@/prefetch_artifacts/adb-helper"]
        options.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        if config["sanitizer"] != "thread":
            options += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
        options.append("-DCPM_SOURCE_CACHE=@WORKSPACE@/cpm_cache")
        config["cmake_options"] = options
    return config


def profile_environment(profiles, family, profile, *, workspace="/usr/local"):
    """Expand a validated absolute workspace into environment data, not shell code."""
    _require(isinstance(workspace, str) and re.fullmatch(r"/[A-Za-z0-9_./-]+", workspace) is not None
             and all(part not in ("", ".", "..") for part in workspace[1:].split("/")),
             "workspace must be a canonical absolute path without shell metacharacters")
    config = profile_configuration(profiles, family, profile)
    expand = lambda value: value.replace(WORKSPACE_TOKEN, workspace)
    environment = {key: expand(value) for key, value in config["environment"].items()}
    environment.update({
        "KLOGG_ENVIRONMENT_FAMILY": family, "KLOGG_ENVIRONMENT_PROFILE": profile,
        "KLOGG_ENVIRONMENT_ROLE": config["role"], "KLOGG_WORKSPACE": workspace,
        "KLOGG_BUILD_ROOT": config["build_root"], "KLOGG_ARCH": "x64",
        "KLOGG_CMAKE_OPTS": " ".join(shlex.quote(expand(option)) for option in config["cmake_options"]),
        "KLOGG_SANITIZER": config["sanitizer"], "KLOGG_PACKAGE_ENABLED": str(config["package"]).lower(),
    })
    package_env = {"tag": "KLOGG_PACKAGE_TAG", "generator": "KLOGG_CPACK_GEN", "suffix": "KLOGG_PACKAGE_SUFFIX",
                   "artifacts_id": "KLOGG_ARTIFACTS_ID", "os": "KLOGG_CONFIG_OS",
                   "check_container": "KLOGG_CHECK_CONTAINER", "check_command": "KLOGG_CHECK_COMMAND"}
    environment.update({key: expand(config["package_settings"].get(setting, "")) for setting, key in package_env.items()})
    _environment(environment)
    return environment


def _role_materials(materials, required):
    _require(isinstance(materials, list), "role materials must be a list")
    names = []
    for material in materials:
        _fields(material, {"name", "url", "sha256"}, "locked role material")
        _require(material["name"] == "codeql-bundle", "unsupported role material")
        url = material["url"]
        _require(isinstance(url, str) and re.fullmatch(
            r"https://github\.com/github/codeql-action/releases/download/"
            r"codeql-bundle-v[0-9]+\.[0-9]+\.[0-9]+/codeql-bundle-linux64\.tar\.gz", url) is not None,
            "CodeQL role material must use a versioned official GitHub bundle URL")
        digest = material["sha256"]
        _require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest) is not None and digest != "0" * 64,
                 "role material requires an externally supplied nonzero SHA-256")
        names.append(material["name"])
    _require(len(names) == len(set(names)) and set(names) == set(required),
             "missing or unexpected role material; required: " + ",".join(required))
    return sorted(copy.deepcopy(materials), key=lambda item: item["name"])


def build_policy(catalog, profiles, family, repo_root, *, role_materials=None):
    """Hash declared verification bytes and supplied role locks, never source SHAs.

    Official URL and digest syntax validation does not authenticate the download;
    the caller must verify the actual bundle against its trusted locked digest.
    """
    validate_profiles(catalog, profiles, repo_root, require_recipe_files=False)
    _require(isinstance(family, str) and family in profiles["families"], "unknown policy family")
    supplied = {} if role_materials is None else role_materials
    _require(isinstance(supplied, dict) and set(supplied) <= set(profiles["families"][family]), "unknown role material profile")
    result = {"schema_version": 1, "family": family, "profiles": {}}
    hashes = {}
    for name, profile in sorted(profiles["families"][family].items()):
        config = profile_configuration(profiles, family, name)
        paths = list(profiles["verification_files"]) + profile["verification_files"]
        if config["role"] == "build":
            paths += profiles["linux_defaults"]["verification_files"]
        for relative in sorted(set(paths)):
            if relative not in hashes:
                digest = hashlib.sha256()
                with _file(repo_root, relative).open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                hashes[relative] = "sha256:" + digest.hexdigest()
        result["profiles"][name] = {
            "configuration": config, "verification_files": {path: hashes[path] for path in sorted(set(paths))},
            "role_materials": _role_materials(supplied.get(name, []), profile["role_materials"]),
        }
    return result


def format_environment(environment, style="github-env"):
    """Render validated environment data; shell assignments are safely quoted."""
    _environment(environment)
    _require(style in ("json", "github-env", "shell"), "unknown environment format")
    if style == "json":
        return json.dumps(environment, sort_keys=True, indent=2) + "\n"
    return "".join(key + "=" + (shlex.quote(value) if style == "shell" else value) + "\n"
                   for key, value in sorted(environment.items()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("catalog-check", help="Validate family/profile coverage and recipe file prerequisites")
    environment = commands.add_parser("profile-env", help="Print one profile's effective environment")
    policy = commands.add_parser("policy", help="Print policy with current verification-file hashes")
    for command in (check, environment, policy):
        command.add_argument("--profiles", type=pathlib.Path, default=ROOT / "ci/environments/profiles.json")
    for command in (check, policy):
        command.add_argument("--catalog", type=pathlib.Path, default=ROOT / "ci/environments/recipes.json")
        command.add_argument("--repo-root", type=pathlib.Path, default=ROOT)
    for command in (environment, policy):
        command.add_argument("--family", required=True)
    environment.add_argument("--profile", required=True)
    environment.add_argument("--workspace", default="/usr/local")
    environment.add_argument("--format", choices=("json", "github-env", "shell"), default="json")
    policy.add_argument("--role-materials", type=pathlib.Path, help="JSON profile-to-material-list mapping from trusted resolution")
    args = parser.parse_args(argv)
    try:
        profiles = ci.load_json(args.profiles)
        if args.command == "profile-env":
            result = profile_environment(profiles, args.family, args.profile, workspace=args.workspace)
            print(format_environment(result, args.format), end="")
        elif args.command == "policy":
            materials = ci.load_json(args.role_materials) if args.role_materials else None
            result = build_policy(ci.load_json(args.catalog), profiles, args.family, args.repo_root, role_materials=materials)
            print(json.dumps(result, sort_keys=True, indent=2))
        else:
            catalog = ci.load_json(args.catalog)
            validate_profiles(catalog, profiles, args.repo_root)
            print(json.dumps({"schema_version": 1, "families": len(catalog["families"]), "recipe_files_present": True}, sort_keys=True))
        return 0
    except (ci.ContractError, OSError) as error:
        print("ci_environment_profiles: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
