#!/usr/bin/env python3

import shlex
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"


def cmake_calls(source, command):
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


def cmake_command_tokens(source, command):
    return [shlex.split(body, posix=True) for body in cmake_calls(source, command)]


class XcodeGeneratorContractTest(unittest.TestCase):
    def test_generated_resources_have_one_object_library_owner(self):
        cmake = APP_CMAKE.read_text(encoding="utf-8")
        libraries = {
            tokens[0]: tokens[1:]
            for tokens in cmake_command_tokens(cmake, "add_library")
        }
        executables = {
            tokens[0]: tokens[1:]
            for tokens in cmake_command_tokens(cmake, "add_executable")
        }
        source_sets = {
            tokens[0]: tokens[1:]
            for tokens in cmake_command_tokens(cmake, "set")
            if tokens
        }

        self.assertEqual(
            libraries["klogg_common_resources"],
            [
                "OBJECT",
                "${CMAKE_CURRENT_SOURCE_DIR}/klogg.qrc",
                "${KLOGG_I18N_RES}",
                "${KLOGG_QT_TRANSLATION_RES}",
            ],
        )
        self.assertEqual(
            libraries["klogg_documentation_resources"],
            ["OBJECT", "${DOCUMENTATION_RESOURCE}"],
        )

        self.assertEqual(
            set(executables),
            {"klogg", "klogg_portable", "klogg_grep"},
        )
        for target in ("klogg", "klogg_portable", "klogg_grep"):
            with self.subTest(target=target):
                self.assertIn(
                    "$<TARGET_OBJECTS:klogg_common_resources>",
                    executables[target],
                )
        for target in ("klogg", "klogg_portable"):
            with self.subTest(target=target):
                self.assertIn(
                    "$<TARGET_OBJECTS:klogg_documentation_resources>",
                    executables[target],
                )
        self.assertNotIn(
            "$<TARGET_OBJECTS:klogg_documentation_resources>",
            executables["klogg_grep"],
        )

        for generated_resource in (
            "${KLOGG_I18N_RES}",
            "${KLOGG_QT_TRANSLATION_RES}",
        ):
            self.assertNotIn(generated_resource, source_sets["MAIN_SOURCES"])
        self.assertNotIn(
            "${DOCUMENTATION_RESOURCE}", source_sets["KLOGG_UI_SOURCES"]
        )


if __name__ == "__main__":
    unittest.main()
