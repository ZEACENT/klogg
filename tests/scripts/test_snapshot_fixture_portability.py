"""Fast contracts for the two NTFS-sensitive snapshot fixtures from PR #85.

Run locally: python3 -m unittest discover -s tests/scripts -p test_snapshot_fixture_portability.py
These protect fixture portability, not production filesystem behavior.
"""

import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
INDEXED_TEST = ROOT / "tests/unit/indexedcontent_test.cpp"
CONCATENATED_TEST = ROOT / "tests/ui/concatenatedlogdata_test.cpp"
INDEXED_CASE = "Indexed content leases retain exact bytes with bounded resident storage"
CONCATENATED_CASE = "Concatenated snapshots outlive their owner and all child log objects"
# Keep quoted text atomic so braces and TEST_CASE text inside it cannot select
# another body. Raw strings are recognized before ordinary identifiers.
CPP_TOKEN = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<delimiter>[^\s()\\]{0,16})\('
    r'[\s\S]*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|'
    r"'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|\d+|[^\s]"
)


def test_body(source, name):
    tokens = [
        match.group()
        for match in CPP_TOKEN.finditer(source)
        if not match.group().startswith(("//", "/*"))
    ]
    for index in range(len(tokens) - 2):
        if tokens[index:index + 3] != ["TEST_CASE", "(", json.dumps(name)]:
            continue
        opening = index + 3
        if tokens[opening:opening + 1] == [","]:
            opening += 2  # Optional Catch tags string.
        if tokens[opening:opening + 2] != [")", "{"]:
            raise AssertionError(f"Malformed test declaration: {name}")
        depth = 1
        body = []
        for token in tokens[opening + 2:]:
            if token == "{":
                depth += 1
            elif token == "}":
                depth -= 1
                if depth == 0:
                    return " ".join(body)
            # Strings cannot spoof any of the executable contracts below.
            body.append('""' if token.startswith(('"', "'", 'R"')) else token)
        raise AssertionError(f"Unterminated test body: {name}")
    raise AssertionError(f"Test not found: {name}")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_indexed_fixture(source):
    body = test_body(source, INDEXED_CASE)
    require("entryInfoList" not in body,
            "Open spill writers require handle sizes, not NTFS directory metadata")
    loop = re.search(r"for \( const auto & (\w+) : spillNames \) \{", body)
    require(loop is not None, "Measure the explicitly retained spill files")
    declaration = re.search(r"QFile (\w+) \( " + loop.group(1) + r" \) ;", body)
    require(declaration is not None, "Measure spill sizes through QFile handles")
    handle = declaration.group(1)
    opened = f"REQUIRE ( {handle} . open ( QIODevice : : ReadOnly ) ) ;"
    measured = f"spillBytes + = {handle} . size ( ) ;"
    require(opened in body and measured in body,
            "Require a read-only open before querying the same QFile handle size")
    require(declaration.start() < body.index(opened) < body.index(measured),
            "Open the spill handle before measuring it")
    require(f"{handle} . close ( )" not in body[body.index(opened):body.index(measured)],
            "The spill size query must use an open handle")
    require("REQUIRE ( spillBytes = = 26 ) ;" in body,
            "Keep the exact payload-plus-boundaries storage bound")


def check_concatenated_fixture(source):
    body = test_body(source, CONCATENATED_CASE)
    owner_removed = "REQUIRE ( source . file . remove ( ) ) ;"
    path_removed = "REQUIRE_FALSE ( QFileInfo : : exists ( source . filePath ) ) ;"
    require("QFile : : remove ( source . filePath )" not in body,
            "QTemporaryFile retains its native handle: remove through its owner")
    require(owner_removed in body and path_removed in body,
            "Remove through the temporary-file owner and prove the path is gone")
    require("REQUIRE ( child . expired ( ) ) ;" in body,
            "The snapshot must not retain its LogData child")
    require(body.index(owner_removed) < body.index(path_removed)
            < body.index("snapshot - > lineString"),
            "Read retained snapshot text only after proving source deletion")


class SnapshotFixturePortabilityTest(unittest.TestCase):
    def test_indexed_fixture_uses_open_handle_sizes(self):
        check_indexed_fixture(INDEXED_TEST.read_text(encoding="utf-8"))

    def test_concatenated_fixture_releases_temporary_file_owner(self):
        check_concatenated_fixture(CONCATENATED_TEST.read_text(encoding="utf-8"))

    def test_rejects_original_directory_metadata_measurement(self):
        source = INDEXED_TEST.read_text(encoding="utf-8")
        broken, count = re.subn(
            r"for \( const auto& spillName : spillNames \) \{.*?\n        \}",
            "for ( const auto& entry : QDir( temporary.path() ).entryInfoList( QDir::Files ) ) {\n"
            "            spillBytes += entry.size();\n        }",
            source, count=1, flags=re.DOTALL,
        )
        self.assertEqual(count, 1)
        with self.assertRaisesRegex(AssertionError, "NTFS directory metadata"):
            check_indexed_fixture(broken)

    def test_rejects_missing_or_closed_spill_handle(self):
        source = INDEXED_TEST.read_text(encoding="utf-8")
        opened = "REQUIRE( spill.open( QIODevice::ReadOnly ) );"
        self.assertIn(opened, source)
        for replacement in ("// " + opened, opened + " spill.close();"):
            with self.subTest(replacement=replacement):
                with self.assertRaises(AssertionError):
                    check_indexed_fixture(source.replace(opened, replacement, 1))

    def test_rejects_original_path_removal_even_with_comment_spoof(self):
        source = CONCATENATED_TEST.read_text(encoding="utf-8")
        removed = "REQUIRE( source.file.remove() );"
        self.assertIn(removed, source)
        broken = source.replace(
            removed, "REQUIRE( QFile::remove( source.filePath ) ); // " + removed, 1
        )
        with self.assertRaisesRegex(AssertionError, "remove through its owner"):
            check_concatenated_fixture(broken)

    def test_rejects_dropped_retention_or_deletion_assertions(self):
        for path, checker, assertion in (
            (INDEXED_TEST, check_indexed_fixture, "REQUIRE( spillBytes == 26 );"),
            (CONCATENATED_TEST, check_concatenated_fixture,
             "REQUIRE_FALSE( QFileInfo::exists( source.filePath ) );"),
        ):
            with self.subTest(assertion=assertion):
                source = path.read_text(encoding="utf-8")
                self.assertIn(assertion, source)
                with self.assertRaises(AssertionError):
                    checker(source.replace(assertion, "// " + assertion, 1))

    def test_body_extraction_ignores_neighbors_comments_and_quoted_braces(self):
        source = '''
            // TEST_CASE( "target" ) { wrong(); }
            TEST_CASE( "target-prefix" ) { wrong(); }
            TEST_CASE( "target", "[tag]" ) {
                const char* text = "}";
                const char* raw = R"delimiter( } TEST_CASE( "target" ) { )delimiter";
                /* } */ if ( true ) { right(); }
            }
            TEST_CASE( "neighbor" ) { wrong(); }
        '''
        body = test_body(source, "target")
        self.assertIn("right ( )", body)
        self.assertNotIn("wrong", body)
        with self.assertRaisesRegex(AssertionError, "Test not found"):
            test_body(source, "missing")
        with self.assertRaisesRegex(AssertionError, "Unterminated"):
            test_body('TEST_CASE( "target" ) { if ( true ) { }', "target")


if __name__ == "__main__":
    unittest.main()
