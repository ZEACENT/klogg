from __future__ import annotations

import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "lint_header_self_contained.py"
SPEC = importlib.util.spec_from_file_location("lint_header_self_contained", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def make_unit(source: str, directory: str, arguments: list[str]):
    return (
        pathlib.Path(source),
        pathlib.Path(directory),
        ["/usr/bin/c++", *arguments],
    )


class EntryParsingTest(unittest.TestCase):
    def test_entry_file_supports_command_and_files_forms(self):
        self.assertEqual(MODULE.entry_file({"file": "/a/b.cpp"}), "/a/b.cpp")
        self.assertEqual(MODULE.entry_file({"files": ["/a/b.cpp"]}), "/a/b.cpp")
        self.assertEqual(MODULE.entry_file({}), "")

    def test_entry_arguments_splits_command_strings(self):
        entry = {"command": '/usr/bin/c++ -DFOO="a b" -Isrc -c foo.cpp'}
        self.assertEqual(
            MODULE.entry_arguments(entry),
            ["/usr/bin/c++", '-DFOO=a b', "-Isrc", "-c", "foo.cpp"],
        )
        entry = {"arguments": ["c++", "-DFOO", "foo.cpp"]}
        self.assertEqual(MODULE.entry_arguments(entry), ["c++", "-DFOO", "foo.cpp"])


class FirstPartyUnitsTest(unittest.TestCase):
    def test_keeps_only_src_tree_sources(self):
        database = [
            {
                "directory": "/ws/build",
                "file": "/ws/src/ui/src/mainwindow.cpp",
                "command": "c++ -c mainwindow.cpp",
            },
            {
                "directory": "/ws/build",
                "file": "/ws/build/generated/mocs_compilation.cpp",
                "command": "c++ -c mocs_compilation.cpp",
            },
            {
                "directory": "/ws/build",
                "file": "/ws/src/ui/src/relative.cpp",
                "command": "c++ -c relative.cpp",
            },
        ]
        units = MODULE.first_party_units(database, pathlib.Path("/ws/src"))
        sources = [str(unit[0]) for unit in units]
        self.assertEqual(len(units), 2)
        self.assertNotIn("mocs_compilation.cpp", " ".join(sources))

    def test_relative_paths_resolve_against_entry_directory(self):
        database = [
            {
                "directory": "/ws/build",
                "file": "../src/logdata/src/logdata.cpp",
                "command": "c++ -c logdata.cpp",
            }
        ]
        units = MODULE.first_party_units(database, pathlib.Path("/ws/src"))
        self.assertEqual(str(units[0][0]), "/ws/src/logdata/src/logdata.cpp")


class RepresentativeUnitTest(unittest.TestCase):
    def test_picks_same_module_unit(self):
        units = [
            make_unit("/ws/src/ui/src/mainwindow.cpp", "/ws/build", ["-DUI"]),
            make_unit("/ws/src/logdata/src/logdata.cpp", "/ws/build", ["-DLOGDATA"]),
        ]
        _, _, arguments = MODULE.representative_unit(
            pathlib.Path("/ws/src/logdata/include/logdata.h"), units
        )
        self.assertEqual(arguments, ["/usr/bin/c++", "-DLOGDATA"])

    def test_empty_units_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "no first-party"):
            MODULE.representative_unit(pathlib.Path("/ws/src/x.h"), [])


class IncludeFlagUnionTest(unittest.TestCase):
    def test_unions_pair_and_joined_forms_without_duplicates(self):
        units = [
            make_unit(
                "/ws/src/a/a.cpp",
                "/ws/build",
                ["-I", "/qt/include", "-I/local", "-isystem", "/boost", "-DNDEBUG"],
            ),
            make_unit(
                "/ws/src/b/b.cpp",
                "/ws/build",
                ["-I", "/qt/include", "-I/other", "-isystem/boost"],
            ),
        ]
        union = MODULE.include_flag_union(units)
        # Pair forms keep their value token; joined forms stay one token.
        self.assertEqual(
            union,
            ["-I", "/qt/include", "-I/local", "-isystem", "/boost", "-I/other", "-isystem/boost"],
        )
        # -D flags are not include paths and must not leak into the union.
        self.assertNotIn("-DNDEBUG", union)


class SyntaxOnlyCommandTest(unittest.TestCase):
    def test_strips_output_depfile_and_source_tokens(self):
        unit = make_unit(
            "/ws/src/ui/src/mainwindow.cpp",
            "/ws/build",
            [
                "-DQT_NO_KEYWORDS",
                "-std=c++17",
                "-MD",
                "-MT",
                "target",
                "-MF",
                "dep.d",
                "-o",
                "mainwindow.cpp.o",
                "-c",
                "/ws/src/ui/src/mainwindow.cpp",
            ],
        )
        command = MODULE.syntax_only_command(
            unit, pathlib.Path("/tmp/probe.cpp"), ["-I", "/extra"]
        )
        self.assertEqual(command[0], "/usr/bin/c++")
        for banned in ("-MD", "-MT", "-MF", "-o", "-c", "dep.d", "mainwindow.cpp.o"):
            self.assertNotIn(banned, command)
        self.assertNotIn("/ws/src/ui/src/mainwindow.cpp", command)
        self.assertIn("-DQT_NO_KEYWORDS", command)
        self.assertIn("-std=c++17", command)
        self.assertEqual(command[-4:], ["-fsyntax-only", "-x", "c++", "/tmp/probe.cpp"])

    def test_dedupes_union_against_carried_flags(self):
        unit = make_unit(
            "/ws/src/a/a.cpp", "/ws/build", ["-I", "/qt/include", "-I/local"]
        )
        command = MODULE.syntax_only_command(
            unit,
            pathlib.Path("/tmp/probe.cpp"),
            ["-I", "/qt/include", "-I", "/new", "-I/local2"],
        )
        self.assertEqual(command.count("-I"), len([t for t in command if t == "-I"]))
        joined = " ".join(command)
        self.assertEqual(joined.count("/qt/include"), 1)
        self.assertIn("/new", command)
        self.assertIn("-I/local2", command)

    def test_empty_command_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "empty command"):
            MODULE.syntax_only_command(
                (pathlib.Path("/x.cpp"), pathlib.Path("/ws"), []),
                pathlib.Path("/tmp/probe.cpp"),
                [],
            )


class FirstDiagnosticsTest(unittest.TestCase):
    def test_extracts_error_and_warning_lines(self):
        output = "noise\n/x.h:1:2: error: unknown type name 'QString'\nmore\n/y.h:2:3: warning: unused\n"
        diagnostics = MODULE.first_diagnostics(output)
        self.assertIn("error:", diagnostics)
        self.assertIn("warning:", diagnostics)
        self.assertNotIn("noise", diagnostics)

    def test_falls_back_to_raw_output_head(self):
        output = "line1\nline2\n"
        self.assertEqual(MODULE.first_diagnostics(output), "line1\nline2")


if __name__ == "__main__":
    unittest.main()
