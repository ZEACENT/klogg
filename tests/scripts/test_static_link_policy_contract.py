import pathlib
import re
import shlex
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PROJECT_CMAKE = ROOT / "CMakeLists.txt"
UNIT_TEST_CMAKE = ROOT / "tests" / "unit" / "CMakeLists.txt"
UI_CMAKE = ROOT / "src" / "ui" / "CMakeLists.txt"
UTILS_CMAKE = ROOT / "src" / "utils" / "CMakeLists.txt"

LINK_SCOPES = {"PUBLIC", "PRIVATE", "INTERFACE"}


def cmake_without_comments(source: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in source.splitlines())


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
    return shlex.split(body, posix=True)


def target_link_calls(source: str, target: str) -> list[list[str]]:
    uncommented = cmake_without_comments(source)
    return [
        tokens
        for body in cmake_calls(uncommented, "target_link_libraries")
        if (tokens := cmake_tokens(body)) and tokens[0] == target
    ]


def direct_dependencies(source: str, target: str) -> set[str]:
    return {
        token
        for call in target_link_calls(source, target)
        for token in call[1:]
        if token not in LINK_SCOPES
    }


def scoped_dependencies(source: str, target: str, scope: str) -> set[str]:
    dependencies = set()
    for call in target_link_calls(source, target):
        current_scope = None
        for token in call[1:]:
            if token in LINK_SCOPES:
                current_scope = token
            elif current_scope == scope:
                dependencies.add(token)
    return dependencies


class StaticLinkPolicyContractTest(unittest.TestCase):
    def test_static_link_dedup_policies_are_guarded_after_project_policy_reset(self):
        source = cmake_without_comments(PROJECT_CMAKE.read_text(encoding="utf-8"))
        reset = re.search(r"cmake_policy\s*\(\s*VERSION\s+3\.14\s*\)", source)
        prepare_version = re.search(r"include\s*\(\s*prepare_version\s*\)", source)

        self.assertIsNotNone(reset, "project-level CMake 3.14 policy reset is missing")
        self.assertIsNotNone(prepare_version, "prepare_version include is missing")
        self.assertLess(reset.end(), prepare_version.start())
        policy_window = source[reset.end() : prepare_version.start()]

        for policy in ("CMP0156", "CMP0179"):
            with self.subTest(policy=policy):
                guard = re.compile(
                    rf"if\s*\(\s*POLICY\s+{policy}\s*\)", re.IGNORECASE
                )
                policy_set = re.compile(
                    rf"cmake_policy\s*\(\s*SET\s+{policy}\s+NEW\s*\)",
                    re.IGNORECASE,
                )
                guarded_new_policy = re.compile(
                    rf"{guard.pattern}\s*{policy_set.pattern}\s*"
                    r"endif\s*\(\s*\)",
                    re.IGNORECASE,
                )

                self.assertEqual(len(guard.findall(source)), 1)
                self.assertEqual(len(policy_set.findall(source)), 1)
                self.assertRegex(
                    policy_window,
                    guarded_new_policy,
                    f"{policy} must be set to NEW in its own POLICY guard after "
                    "cmake_policy(VERSION 3.14) and before include(prepare_version)",
                )

    def test_unit_tests_preserve_real_direct_dependencies(self):
        source = UNIT_TEST_CMAKE.read_text(encoding="utf-8")
        dependencies = direct_dependencies(source, "klogg_tests")
        required = {"klogg_ui", "klogg_utils", "klogg_logging", "klogg_livecapture"}

        self.assertEqual(
            required - dependencies,
            set(),
            "klogg_tests must retain real direct dependencies rather than relying "
            "on static-link transitive duplication",
        )

    def test_library_interfaces_preserve_required_public_dependencies(self):
        ui_public = scoped_dependencies(
            UI_CMAKE.read_text(encoding="utf-8"), "klogg_ui", "PUBLIC"
        )
        utils_public = scoped_dependencies(
            UTILS_CMAKE.read_text(encoding="utf-8"), "klogg_utils", "PUBLIC"
        )

        self.assertIn("klogg_livecapture", ui_public)
        self.assertIn("klogg_logging", utils_public)


if __name__ == "__main__":
    unittest.main()
