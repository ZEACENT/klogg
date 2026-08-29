from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
PROJECT_CMAKE = ROOT / "CMakeLists.txt"
PREPARE_VERSION = ROOT / "cmake" / "prepare_version.cmake"
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
PLIST_TEMPLATE = ROOT / "cmake" / "MacOSXBundleInfo.plist.in"
DISTRIBUTION = ROOT / "packaging" / "osx" / "distribution.xml"
DISTRIBUTION_TEMPLATE = ROOT / "packaging" / "osx" / "distribution.xml.in"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
CI_CONTAINER_CONSUMERS = (
    CI_BUILD,
    ROOT / ".github" / "actions" / "docker-build" / "action.yml",
    ROOT / ".github" / "actions" / "docker-run-tests" / "action.yml",
    ROOT / ".github" / "actions" / "docker-package" / "action.yml",
)
PERSISTENT_INFO = ROOT / "src" / "settings" / "src" / "persistentinfo.cpp"
DESKTOP_FILE = ROOT / "packaging" / "linux" / "klogg.desktop"

LEGACY_OWNER = "vari" + "ar"
CANONICAL_OWNER = "ZEACENT"
CANONICAL_OWNER_LOWER = CANONICAL_OWNER.lower()
CANONICAL_IMAGE_PREFIX = f"{CANONICAL_OWNER_LOWER}/klogg"
KNOWN_LEGACY_FORKS = {
    f"{LEGACY_OWNER}/maddy",
    f"{LEGACY_OWNER}/klogg_karchive",
    f"{LEGACY_OWNER}/KDSingleApplication",
    f"{LEGACY_OWNER}/klogg_exprtk",
    f"{LEGACY_OWNER}/oneTBB",
}
CI_IMAGE_EXPRESSION = (
    "${{ env.KLOGG_CI_IMAGE_PREFIX }}"
    "${{ matrix.config.container_suffix }}"
)


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


GENERATED_ROOTS = {
    ".ccache",
    ".git",
    "build_root",
    "cpm_cache",
    "prefetch_artifacts",
    "test_tmp",
}
GENERATED_PATHS = {("3rdparty", "boost")}


def repository_files(root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    root = root.resolve()
    if not root.is_dir() or not (root / "CMakeLists.txt").is_file():
        raise RuntimeError(f"invalid klogg source root: {root}")

    try:
        result = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            check=False,
            capture_output=True,
        )
    except OSError:
        result = None

    if result is not None and result.returncode == 0:
        files = [
            pathlib.Path(relative.decode("utf-8"))
            for relative in result.stdout.split(b"\0")
            if relative
        ]
    else:
        files = []
        for path in root.rglob("*"):
            relative = path.relative_to(root)
            if relative.parts[0] in GENERATED_ROOTS:
                continue
            if any(relative.parts[: len(prefix)] == prefix for prefix in GENERATED_PATHS):
                continue
            if path.is_file():
                files.append(relative)

    files = sorted(set(files), key=lambda path: path.as_posix())
    if not files:
        raise RuntimeError(f"source inventory is empty: {root}")
    return files


def tracked_text_lines(root: pathlib.Path = ROOT):
    for relative in repository_files(root):
        path = root / relative
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        for line_number, line in enumerate(text.splitlines(), 1):
            yield relative.as_posix(), line_number, line


def cmake_sources(root: pathlib.Path = ROOT) -> dict[str, str]:
    return {
        relative.as_posix(): read(root / relative)
        for relative in repository_files(root)
        if relative.name == "CMakeLists.txt" or relative.suffix == ".cmake"
    }


def unexpected_legacy_owner_references(root: pathlib.Path = ROOT):
    legacy_pattern = re.compile(re.escape(LEGACY_OWNER), re.IGNORECASE)
    commit_url = re.compile(
        rf"https://github\.com/{re.escape(LEGACY_OWNER)}/klogg/commit/[0-9a-f]+",
        re.IGNORECASE,
    )
    readme_reference = re.compile(
        rf"{re.escape(LEGACY_OWNER)}(?:/[A-Za-z0-9_.-]+)?", re.IGNORECASE
    )
    readme_history_lines = {
        f"### Comparing with {LEGACY_OWNER}/klogg",
        (
            f"This fork builds on [{LEGACY_OWNER}/klogg]"
            f"(https://github.com/{LEGACY_OWNER}/klogg) and adds:"
        ),
        f"* **[Anton Filimonov](https://github.com/{LEGACY_OWNER})**",
    }
    funding_lines = {
        f"github: {LEGACY_OWNER}",
        f"ko_fi: {LEGACY_OWNER}",
        f"patreon: {LEGACY_OWNER}",
        (
            "custom: "
            f'["https://paypal.me/{LEGACY_OWNER}fav", '
            f'"https://www.buymeacoffee.com/{LEGACY_OWNER}"]'
        ),
    }

    unexpected = []
    for relative, line_number, line in tracked_text_lines(root):
        if legacy_pattern.search(line) is None:
            continue

        remainder = line
        if relative == "CHANGELOG.md":
            remainder = commit_url.sub("", remainder)
        elif relative == "README.md":
            references = {match.lower() for match in readme_reference.findall(line)}
            history_references = {
                LEGACY_OWNER.lower(),
                f"{LEGACY_OWNER}/klogg".lower(),
            }
            dependency_row = (
                line.startswith("| ")
                and references.issubset({fork.lower() for fork in KNOWN_LEGACY_FORKS})
                and any(
                    f"https://github.com/{fork}" in line and f"`{fork}`" in line
                    for fork in KNOWN_LEGACY_FORKS
                )
            )
            history_line = line in readme_history_lines and references.issubset(
                history_references
            )
            if history_line or dependency_row:
                remainder = readme_reference.sub("", remainder)
        elif relative == "3rdparty/CMakeLists.txt":
            if line.strip() in KNOWN_LEGACY_FORKS:
                remainder = ""
        elif relative == "cmake/prefetch_cpm/CMakeLists.txt":
            if line.strip() in {
                f"GITHUB_REPOSITORY {fork}" for fork in KNOWN_LEGACY_FORKS
            }:
                remainder = ""
        elif relative == ".github/FUNDING.yml":
            if line.strip().lower() in {allowed.lower() for allowed in funding_lines}:
                remainder = ""

        if legacy_pattern.search(remainder) is not None:
            unexpected.append(f"{relative}:{line_number}: {line.strip()}")

    return unexpected


class RepositoryInventoryFallbackTest(unittest.TestCase):
    def make_root(self) -> pathlib.Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = pathlib.Path(temporary.name)
        (root / "CMakeLists.txt").write_text("project(klogg)\n", encoding="utf-8")
        return root

    def unavailable_git(self):
        return mock.patch.object(
            subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=["git"], returncode=128, stdout=b"", stderr=b"not a repository"
            ),
        )

    def test_gitless_inventory_audits_unknown_first_party_roots(self):
        root = self.make_root()
        source = root / "future-component" / "nested.txt"
        source.parent.mkdir(parents=True)
        source.write_text(f"unexpected {LEGACY_OWNER} owner\n", encoding="utf-8")

        with self.unavailable_git():
            failures = unexpected_legacy_owner_references(root)

        self.assertEqual(
            failures,
            [f"future-component/nested.txt:1: unexpected {LEGACY_OWNER} owner"],
        )

    def test_gitless_cmake_inventory_uses_the_same_complete_file_set(self):
        root = self.make_root()
        policy = root / "future-component" / "policy.cmake"
        policy.parent.mkdir(parents=True)
        policy.write_text("set(FUTURE_COMPONENT ON)\n", encoding="utf-8")

        with self.unavailable_git():
            sources = cmake_sources(root)

        self.assertEqual(
            set(sources), {"CMakeLists.txt", "future-component/policy.cmake"}
        )

    def test_gitless_inventory_excludes_only_generated_workspace_roots(self):
        root = self.make_root()
        for relative in (
            "build_root/generated.txt",
            "cpm_cache/dependency.txt",
            "3rdparty/boost/generated.txt",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"generated {LEGACY_OWNER}\n", encoding="utf-8")
        source = root / "new-first-party" / "source.txt"
        source.parent.mkdir()
        source.write_text(f"first-party {LEGACY_OWNER}\n", encoding="utf-8")

        with self.unavailable_git():
            failures = unexpected_legacy_owner_references(root)

        self.assertEqual(
            failures,
            [f"new-first-party/source.txt:1: first-party {LEGACY_OWNER}"],
        )

    def test_gitless_inventory_fails_closed_without_source_sentinel(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        with self.unavailable_git(), self.assertRaisesRegex(
            RuntimeError, "invalid klogg source root"
        ):
            repository_files(pathlib.Path(temporary.name))


class ApplicationIdentityContractTest(unittest.TestCase):
    def test_cmake_defines_one_canonical_identity_source(self):
        sources = cmake_sources()
        combined = "\n".join(sources.values())
        project = read(PROJECT_CMAKE)
        failures = []

        owner_declarations = re.findall(
            r"set\(\s*KLOGG_REPOSITORY_OWNER\s+\"?([^\s\)\"]+)\"?\s*\)",
            combined,
            re.IGNORECASE,
        )
        if owner_declarations != [CANONICAL_OWNER]:
            failures.append(
                "expected exactly one CMake KLOGG_REPOSITORY_OWNER declaration "
                f"equal to {CANONICAL_OWNER}, found {owner_declarations}"
            )

        lower_derivations = re.findall(
            r"string\(\s*TOLOWER\s+\"?\$\{KLOGG_REPOSITORY_OWNER\}\"?\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*\)",
            combined,
            re.IGNORECASE,
        )
        if len(lower_derivations) != 1:
            failures.append(
                "lowercase repository owner must be derived exactly once with "
                "string(TOLOWER ${KLOGG_REPOSITORY_OWNER} <variable>)"
            )
            lower_owner = "KLOGG_REPOSITORY_OWNER_LOWER"
        else:
            lower_owner = lower_derivations[0]
            if re.search(rf"set\(\s*{re.escape(lower_owner)}\b", combined, re.IGNORECASE):
                failures.append(
                    f"{lower_owner} must be derived with string(TOLOWER), not set as another literal"
                )

        if re.search(
            r"set\(\s*PROJECT_HOMEPAGE_URL\s+"
            r"\"https://github\.com/\$\{KLOGG_REPOSITORY_OWNER\}/\$\{PROJECT_NAME\}\"\s*\)",
            project,
            re.IGNORECASE,
        ) is None:
            failures.append(
                "PROJECT_HOMEPAGE_URL must derive from KLOGG_REPOSITORY_OWNER and PROJECT_NAME"
            )

        application_id_pattern = re.compile(
            rf"set\(\s*KLOGG_APPLICATION_ID\s+\"com\.github\.\$\{{{re.escape(lower_owner)}\}}\."
            r"\$\{PROJECT_NAME\}\"\s*\)",
            re.IGNORECASE,
        )
        if application_id_pattern.search(project) is None:
            failures.append(
                "KLOGG_APPLICATION_ID must derive as "
                f"com.github.${{{lower_owner}}}.${{PROJECT_NAME}}"
            )

        generic_identifier_uses = []
        for relative, source in sources.items():
            if re.search(r"set\(\s*IDENTIFIER\b|\$\{IDENTIFIER\}", source):
                generic_identifier_uses.append(relative)
        if generic_identifier_uses:
            failures.append(
                "generic CMake IDENTIFIER remains in " + ", ".join(generic_identifier_uses)
            )

        company_declarations = re.findall(
            r"set\(\s*COMPANY\s+\"([^\"]+)\"\s*\)", combined
        )
        if company_declarations != ["Anton Filimonov"]:
            failures.append(
                "expected exactly one COMPANY declaration retaining publisher compatibility"
            )
        if re.search(r"KLOGG_PUBLISHER", combined):
            failures.append("publisher metadata must not gain an identity-specific abstraction")
        if "${COMPANY}" not in read(PREPARE_VERSION):
            failures.append("prepare_version.cmake no longer consumes COMPANY")
        if re.search(
            r"set\(\s*CPACK_PACKAGE_VENDOR\s+\$\{COMPANY\}\s*\)", project
        ) is None:
            failures.append("CPack vendor no longer consumes COMPANY")

        self.assertEqual(failures, [], "\n".join(failures))

    def test_macos_bundle_and_distribution_share_the_application_id(self):
        app_cmake = read(APP_CMAKE)
        plist = read(PLIST_TEMPLATE)
        project_sources = cmake_sources()
        failures = []

        if re.search(
            r"set\(\s*MACOSX_BUNDLE_GUI_IDENTIFIER\s+\$\{KLOGG_APPLICATION_ID\}\s*\)",
            app_cmake,
        ) is None:
            failures.append(
                "src/app must set MACOSX_BUNDLE_GUI_IDENTIFIER from KLOGG_APPLICATION_ID; "
                "the old generic or literal bundle ID is still wired"
            )
        if "${MACOSX_BUNDLE_GUI_IDENTIFIER}" not in plist:
            failures.append("plist template no longer delegates CFBundleIdentifier to CMake")
        owner_literals = "|".join(
            (r"com\.github\.", re.escape(CANONICAL_OWNER), re.escape(LEGACY_OWNER))
        )
        if re.search(owner_literals, plist, re.IGNORECASE):
            failures.append("plist template contains a literal owner or application ID")

        if DISTRIBUTION.exists():
            failures.append(
                "packaging/osx/distribution.xml must be renamed to distribution.xml.in"
            )
        if not DISTRIBUTION_TEMPLATE.exists():
            failures.append("packaging/osx/distribution.xml.in is missing")
        distribution_source = ""
        if DISTRIBUTION_TEMPLATE.exists():
            distribution_source = read(DISTRIBUTION_TEMPLATE)
        elif DISTRIBUTION.exists():
            distribution_source = read(DISTRIBUTION)

        legacy_ids = re.findall(
            rf"com\.github\.{re.escape(LEGACY_OWNER)}\.klogg",
            distribution_source,
            re.IGNORECASE,
        )
        if legacy_ids:
            failures.append(
                "macOS distribution still contains old bundle/package IDs: "
                + ", ".join(sorted(set(legacy_ids)))
            )
        if distribution_source.count("@KLOGG_APPLICATION_ID@") != 5:
            failures.append(
                "distribution template must use @KLOGG_APPLICATION_ID@ for all five package IDs"
            )
        for placeholder in ("%klogg_version%", "%klogg_pkg%"):
            if placeholder not in distribution_source:
                failures.append(f"distribution template lost {placeholder}")

        configure_calls = []
        for relative, source in project_sources.items():
            for body in re.findall(r"configure_file\((.*?)\)", source, re.DOTALL | re.IGNORECASE):
                if "packaging/osx/distribution.xml.in" in body:
                    configure_calls.append((relative, body))
        if len(configure_calls) != 1:
            failures.append(
                "CMake must configure distribution.xml.in to exactly one generated distribution.xml"
            )
        else:
            _, configure_body = configure_calls[0]
            if not re.search(r"\$\{CMAKE_[A-Z_]*BINARY_DIR\}[^\s\)]*/distribution\.xml", configure_body):
                failures.append("configured distribution.xml must be written under a CMake binary directory")
            if "@ONLY" not in configure_body:
                failures.append(
                    "distribution configure_file must use @ONLY so %klogg_version%/%klogg_pkg% survive"
                )

        self.assertEqual(failures, [], "\n".join(failures))

    def test_deb_and_rpm_reuse_the_project_homepage(self):
        project = read(PROJECT_CMAKE)
        failures = []
        for variable in (
            "CPACK_DEBIAN_PACKAGE_HOMEPAGE",
            "CPACK_RPM_PACKAGE_URL",
        ):
            assignments = re.findall(
                rf"set\(\s*{variable}\s+([^\)]+)\)", project, re.IGNORECASE
            )
            if assignments != ["${PROJECT_HOMEPAGE_URL}"]:
                failures.append(
                    f"{variable} must directly reuse ${{PROJECT_HOMEPAGE_URL}}; "
                    f"found duplicated literal/value {assignments}"
                )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_first_party_ci_images_use_one_prefix_and_matrix_suffixes(self):
        workflow = read(CI_BUILD)
        failures = []

        prefix_declarations = re.findall(
            r"^\s*KLOGG_CI_IMAGE_PREFIX:\s*([^\s#]+)", workflow, re.MULTILINE
        )
        if prefix_declarations != [CANONICAL_IMAGE_PREFIX]:
            failures.append(
                "workflow must define exactly one lowercase KLOGG_CI_IMAGE_PREFIX "
                f"equal to {CANONICAL_IMAGE_PREFIX}; found {prefix_declarations}"
            )
        workflow_env_pattern = re.compile(
            rf"^env:\s*\n(?:  .*\n)*?  KLOGG_CI_IMAGE_PREFIX:\s*"
            rf"{re.escape(CANONICAL_IMAGE_PREFIX)}\s*$",
            re.MULTILINE,
        )
        if workflow_env_pattern.search(workflow) is None:
            failures.append("KLOGG_CI_IMAGE_PREFIX must be declared in workflow-level env")

        suffixes = re.findall(
            r"^\s*container_suffix:\s*([^\s#]+)", workflow, re.MULTILINE
        )
        expected_suffixes = {
            "_ubuntu20.04",
            "_ubuntu22.04",
            "_ubuntu22.04-tsan",
            "_ubuntu24.04",
            "_ubuntu26.04",
        }
        if not expected_suffixes.issubset(set(suffixes)):
            failures.append(
                "Linux matrix must keep first-party image suffixes; missing "
                + ", ".join(sorted(expected_suffixes - set(suffixes)))
            )
        invalid_suffixes = sorted(
            suffix for suffix in suffixes if re.fullmatch(r"_[a-z0-9.-]+", suffix) is None
        )
        if invalid_suffixes:
            failures.append(
                "matrix image suffixes contain full tags or invalid values: "
                + ", ".join(invalid_suffixes)
            )

        repeated_tag_pattern = re.compile(
            rf"(?:{re.escape(LEGACY_OWNER)}/klogg|"
            rf"{re.escape(CANONICAL_IMAGE_PREFIX)})_",
            re.IGNORECASE,
        )
        repeated_tag_lines = [
            f"{line_number}: {line.strip()}"
            for line_number, line in enumerate(workflow.splitlines(), 1)
            if repeated_tag_pattern.search(line)
        ]
        if repeated_tag_lines:
            failures.append(
                "repeated first-party CI image tags remain instead of prefix + suffix: "
                + "; ".join(repeated_tag_lines)
            )

        for consumer in CI_CONTAINER_CONSUMERS:
            source = read(consumer)
            relative = consumer.relative_to(ROOT).as_posix()
            if re.search(r"matrix\.config\.container(?![A-Za-z0-9_])", source):
                failures.append(f"{relative} still consumes the old matrix.config.container full tag")
            if CI_IMAGE_EXPRESSION not in source:
                failures.append(
                    f"{relative} does not resolve the active image from "
                    "KLOGG_CI_IMAGE_PREFIX + matrix.config.container_suffix"
                )
            unresolved = source.replace(CI_IMAGE_EXPRESSION, "")
            if "matrix.config.container_suffix" in unresolved:
                failures.append(f"{relative} consumes container_suffix without the unified prefix")

        for external_image in (
            "check_container: ubuntu:22.04",
            "check_container: ubuntu:24.04",
            "check_container: ubuntu:26.04",
            "quay.io/pypa/manylinux2014_x86_64@sha256:",
            "quay.io/pypa/manylinux2014_aarch64@sha256:",
        ):
            if external_image not in workflow:
                failures.append(f"external image was changed or removed: {external_image}")

        self.assertEqual(failures, [], "\n".join(failures))

    def test_legacy_owner_occurrences_are_confined_to_explicit_history_and_forks(self):
        failures = unexpected_legacy_owner_references()
        dependency_forks = {}
        for relative in (
            "3rdparty/CMakeLists.txt",
            "cmake/prefetch_cpm/CMakeLists.txt",
        ):
            repositories = set(
                re.findall(
                    r"GITHUB_REPOSITORY\s+([^\s\)]+)",
                    read(ROOT / relative),
                    re.IGNORECASE,
                )
            )
            dependency_forks[relative] = {
                repository
                for repository in repositories
                if repository.lower().startswith(f"{LEGACY_OWNER.lower()}/")
            }
            if dependency_forks[relative] != KNOWN_LEGACY_FORKS:
                failures.append(
                    f"{relative} legacy fork allowlist differs from the known forks: "
                    f"{sorted(dependency_forks[relative])}"
                )
        if len(set(map(frozenset, dependency_forks.values()))) != 1:
            failures.append("3rdparty and prefetch legacy fork sets do not match")

        self.assertEqual(
            failures,
            [],
            "legacy owner appears outside CHANGELOG commit links, README upstream/"
            "dependency/personal references, known matching dependency forks, or FUNDING handles",
        )

    def test_compatibility_identities_and_urls_remain_unchanged(self):
        project = read(PROJECT_CMAKE)
        persistent = read(PERSISTENT_INFO)
        desktop = read(DESKTOP_FILE)
        failures = []

        if re.search(r"project\(\s*klogg\b", project, re.IGNORECASE) is None:
            failures.append("CMake project/package name klogg changed")
        for setting_identity in (
            'ApplicationSessionFile[] = "klogg"',
            'SessionSettingsFile[] = "klogg_session"',
            'QSettings>( format, QSettings::UserScope, "klogg"',
        ):
            if setting_identity not in persistent:
                failures.append(f"QSettings compatibility identity changed: {setting_identity}")
        for desktop_entry in ("Name=klogg", "Exec=klogg %F", "Icon=klogg"):
            if desktop_entry not in desktop:
                failures.append(f"klogg.desktop compatibility entry changed: {desktop_entry}")

        preserved_package_ids = {
            "packaging/windows/chocolatey/klogg.nuspec": "<id>klogg</id>",
            "packaging/windows/klogg.nsi": 'InstallDirRegKey HKLM Software\\klogg ""',
            "packaging/linux/arch/PKGBUILD": "pkgname=klogg",
        }
        for relative, package_id in preserved_package_ids.items():
            if package_id not in read(ROOT / relative):
                failures.append(f"package identity changed in {relative}: {package_id}")

        preserved_urls = {
            "packaging/windows/klogg.nsi": "https://klogg.filimonov.dev/",
            "packaging/windows/scoop/klogg.json": "https://klogg.filimonov.dev",
            "packaging/windows/chocolatey/klogg.nuspec": "https://klogg.filimonov.dev",
            "packaging/linux/gentoo/klogg-22.06.0.1289.ebuild": "https://klogg.filimonov.dev",
            "packaging/linux/arch/PKGBUILD": "https://klogg.filimonov.dev",
            "website/config.toml": "https://klogg.filimonov.dev/",
            "src/crash_handler/src/crashhandler.cpp": (
                "https://klogg.filimonov.dev/docs/privacy_policy"
            ),
        }
        for relative, url in preserved_urls.items():
            if url not in read(ROOT / relative):
                failures.append(f"legacy website/privacy URL changed in {relative}: {url}")

        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
