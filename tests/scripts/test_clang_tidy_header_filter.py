import importlib.util
import pathlib
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "clang_tidy_header_filter.py"
SPEC = importlib.util.spec_from_file_location("clang_tidy_header_filter", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ClangTidyHeaderFilterTest(unittest.TestCase):
    def test_collects_added_header_hunks_only(self):
        patch = """\
diff --git a/src/ui/include/example.h b/src/ui/include/example.h
--- a/src/ui/include/example.h
+++ b/src/ui/include/example.h
@@ -3,0 +4,2 @@
+one
+two
@@ -10 +12 @@
-old
+new
diff --git a/src/ui/src/example.cpp b/src/ui/src/example.cpp
--- a/src/ui/src/example.cpp
+++ b/src/ui/src/example.cpp
@@ -1,0 +2 @@
+ignored
"""

        self.assertEqual(
            MODULE.build_filter(patch),
            [{"name": "src/ui/include/example.h", "lines": [[4, 5], [12, 12]]}],
        )

    def test_ignores_deleted_header_hunks(self):
        patch = """\
diff --git a/src/ui/include/deleted.h b/src/ui/include/deleted.h
--- a/src/ui/include/deleted.h
+++ /dev/null
@@ -1,2 +0,0 @@
-old
-lines
"""

        self.assertEqual(MODULE.build_filter(patch), [])

    def test_added_text_that_looks_like_a_file_header_does_not_reset_scope(self):
        patch = """\
diff --git a/src/ui/include/example.h b/src/ui/include/example.h
--- a/src/ui/include/example.h
+++ b/src/ui/include/example.h
@@ -3,0 +4,1 @@
+++ b/not_a_header;
@@ -10,0 +12,1 @@
+int still_in_header;
"""
        self.assertEqual(
            MODULE.build_filter(patch),
            [{"name": "src/ui/include/example.h", "lines": [[4, 4], [12, 12]]}],
        )

    def test_deleted_text_cannot_spoof_a_paired_file_header(self):
        patch = """\
diff --git a/src/ui/include/example.h b/src/ui/include/example.h
--- a/src/ui/include/example.h
+++ b/src/ui/include/example.h
@@ -3,2 +3,2 @@
--- a/not_a_header
+++ b/not_a_header
@@ -10,0 +11,1 @@
+int still_in_example;
"""
        self.assertEqual(
            MODULE.build_filter(patch),
            [{"name": "src/ui/include/example.h", "lines": [[3, 4], [11, 11]]}],
        )

    def test_accepts_unquoted_non_ascii_header_paths(self):
        patch = """\
diff --git a/src/ui/include/ümlaut.hxx b/src/ui/include/ümlaut.hxx
--- a/src/ui/include/ümlaut.hxx
+++ b/src/ui/include/ümlaut.hxx
@@ -1,0 +2,1 @@
+int value;
"""
        self.assertEqual(
            MODULE.build_filter(patch),
            [{"name": "src/ui/include/ümlaut.hxx", "lines": [[2, 2]]}],
        )

    def test_deletion_only_retained_header_is_a_successful_empty_filter(self):
        patch = """\
diff --git a/src/ui/include/example.h b/src/ui/include/example.h
--- a/src/ui/include/example.h
+++ b/src/ui/include/example.h
@@ -4,2 +4,0 @@
-old
-lines
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "patch.diff"
            path.write_text(patch)
            result = subprocess.run(
                ["python3", str(SCRIPT), str(path)],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "[]")


if __name__ == "__main__":
    unittest.main()
