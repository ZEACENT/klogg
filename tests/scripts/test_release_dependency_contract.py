import pathlib
import re
import shlex
import unittest


ROOT = pathlib.Path(__file__).parents[2]
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH_CMAKE = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
AGENT_BUILD_ACTION = ROOT / ".github" / "actions" / "agent-build" / "action.yml"
NOTICE = ROOT / "NOTICE"

# These dependencies participate only in tests or in generating/repairing build
# artifacts. They are not linked into, copied into, or loaded by release
# binaries. Everything else declared through CPM is release legal/pinning
# surface and must be covered by the checks below.
NON_RELEASE_DEPENDENCIES = {
    "backward-cpp",
    "Catch2",
    "macdeployqtfix",
    "maddy",
}

# CMake package names do not always match the upstream project's legal name.
LEGAL_NAME_ALIASES = {
    "KF5Archive": "karchive",
}

IMMUTABLE_REVISION = re.compile(r"[0-9a-f]{40}", re.IGNORECASE)


def cmake_calls(source: str, command: str) -> list[str]:
    """Return balanced argument bodies for a CMake command."""
    calls = []
    lowered = source.lower()
    needle = f"{command.lower()}("
    position = 0

    while True:
        start = lowered.find(needle, position)
        if start < 0:
            return calls

        body_start = start + len(needle)
        depth = 1
        quote = None
        escaped = False
        cursor = body_start
        while cursor < len(source) and depth:
            character = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
            elif character in ('"', "'"):
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            cursor += 1

        if depth:
            raise AssertionError(f"Unterminated {command} call")
        calls.append(source[body_start : cursor - 1])
        position = cursor


def cmake_tokens(body: str) -> list[str]:
    uncommented = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(uncommented, posix=True)


def cmake_command_tokens(source: str, command: str) -> list[list[str]]:
    return [cmake_tokens(body) for body in cmake_calls(source, command)]


def cpm_dependencies(path: pathlib.Path) -> dict[str, str]:
    dependencies = {}
    source = path.read_text(encoding="utf-8")
    for tokens in cmake_command_tokens(source, "cpmaddpackage"):
        if len(tokens) == 1 and tokens[0].startswith("gh:"):
            repository, revision = tokens[0][3:].rsplit("@", 1)
            dependencies[repository.rsplit("/", 1)[-1]] = revision
            continue

        def value_after(keyword: str):
            try:
                return tokens[tokens.index(keyword) + 1]
            except (ValueError, IndexError):
                return None

        name = value_after("NAME")
        revision = value_after("GIT_TAG") or value_after("VERSION")
        if name is not None:
            dependencies[name] = revision or ""
    return dependencies


def release_dependencies(path: pathlib.Path) -> dict[str, str]:
    return {
        name: revision
        for name, revision in cpm_dependencies(path).items()
        if name not in NON_RELEASE_DEPENDENCIES
    }


def normalized_legal_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", name.lower())


class ReleaseDependencyContractTest(unittest.TestCase):
    def test_macos_bundle_sources_carry_copying_and_notice(self):
        source = APP_CMAKE.read_text(encoding="utf-8")
        source_sets = [
            tokens
            for tokens in cmake_command_tokens(source, "set")
            if tokens[:1] == ["KLOGG_UI_SOURCES"]
        ]
        self.assertEqual(len(source_sets), 1)
        bundle_sources = source_sets[0][1:]

        missing = [
            legal_file
            for legal_file in ("${COPYING_FILE}", "${NOTICE_FILE}")
            if legal_file not in bundle_sources
        ]
        self.assertEqual(
            missing,
            [],
            "macOS app target sources omit legal files, so "
            "MACOSX_PACKAGE_LOCATION never copies them into the bundle: "
            + ", ".join(missing),
        )

        for legal_file in ("${COPYING_FILE}", "${NOTICE_FILE}"):
            self.assertRegex(
                source,
                re.compile(
                    rf"set_source_files_properties\(\s*{re.escape(legal_file)}\s+"
                    rf"PROPERTIES\s+MACOSX_PACKAGE_LOCATION\s+SharedSupport\s*\)",
                    re.MULTILINE,
                ),
            )

        install_calls = cmake_command_tokens(source, "install")
        self.assertTrue(
            any(
                tokens[:2] == ["TARGETS", "klogg"]
                and "BUNDLE" in tokens
                and "DESTINATION" in tokens
                for tokens in install_calls
            ),
            "the install rules must install the complete klogg application bundle",
        )

    def test_release_dependencies_have_repository_legal_coverage(self):
        declared = release_dependencies(THIRD_PARTY_CMAKE)
        notice = normalized_legal_name(NOTICE.read_text(encoding="utf-8"))

        missing = sorted(
            dependency
            for dependency in declared
            if normalized_legal_name(LEGAL_NAME_ALIASES.get(dependency, dependency))
            not in notice
        )

        self.assertEqual(
            missing,
            [],
            "release dependencies missing from NOTICE: " + ", ".join(missing),
        )

    def test_release_dependency_sources_are_immutable_and_prefetched(self):
        declared = release_dependencies(THIRD_PARTY_CMAKE)
        prefetched = release_dependencies(PREFETCH_CMAKE)

        missing_from_prefetch = sorted(set(declared) - set(prefetched))
        mismatched = sorted(
            name
            for name, revision in declared.items()
            if prefetched.get(name) != revision
        )
        unpinned = sorted(
            name
            for name, revision in declared.items()
            if IMMUTABLE_REVISION.fullmatch(revision) is None
        )

        self.assertEqual(
            missing_from_prefetch,
            [],
            "release dependencies are not present in the prefetch manifest: "
            + ", ".join(missing_from_prefetch),
        )
        self.assertEqual(
            mismatched,
            [],
            "release dependency revisions differ between build and prefetch manifests: "
            + ", ".join(mismatched),
        )
        self.assertEqual(
            unpinned,
            [],
            "release dependency revisions must be immutable 40-character commits: "
            + ", ".join(unpinned),
        )

    def test_release_configure_cannot_fall_back_to_network_fetches(self):
        action = AGENT_BUILD_ACTION.read_text(encoding="utf-8")
        configure_commands = [
            line.strip()
            for line in action.splitlines()
            if line.strip().startswith("if cmake ")
        ]
        self.assertEqual(len(configure_commands), 1)
        self.assertIn(
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            configure_commands[0],
            "the shared release configure action must fail closed when the "
            "prefetched dependency cache is incomplete",
        )


if __name__ == "__main__":
    unittest.main()
