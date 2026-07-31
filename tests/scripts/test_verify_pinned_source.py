import hashlib
import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
VERIFY_SCRIPT = ROOT / "cmake" / "verify_pinned_source.cmake"


def source_tree_digest(source: pathlib.Path) -> str:
    files = [
        path for path in source.rglob("*") if path.is_file() and ".git" not in path.parts
    ]
    manifest = ""
    for path in sorted(files, key=lambda item: item.relative_to(source).as_posix()):
        relative = path.relative_to(source).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest += f"{relative}:{digest}\n"
    return hashlib.sha256(manifest.encode()).hexdigest()


class VerifyPinnedSourceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(self.root / "isolated-home"),
            }
        )
        self.git = [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "commit.gpgsign=false",
        ]
        self.source = self.root / "source"
        self.source.mkdir()
        self.sentinel = self.source / "sentinel.txt"
        self.sentinel.write_text("pinned contents\n")
        self.second_sentinel = self.source / "second.txt"
        self.second_sentinel.write_text("other pinned contents\n")
        self.clean_digest = source_tree_digest(self.source)

    def tearDown(self):
        self.temporary.cleanup()

    def verify(self, revision="expected-revision", digests=None):
        approved = digests or [self.clean_digest]
        driver = self.root / "verify.cmake"
        driver.write_text(
            f"include([[{VERIFY_SCRIPT.as_posix()}]])\n"
            "klogg_require_pinned_source(\n"
            f"  test-dependency [[{self.source.as_posix()}]] [[{revision}]]\n"
            + "\n".join(f"  [[{digest}]]" for digest in approved)
            + "\n)\n"
        )
        return subprocess.run(
            ["cmake", "-P", str(driver)],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

    def initialize_git(self):
        subprocess.run(
            self.git + ["init", "-q", str(self.source)],
            check=True,
            env=self.environment,
        )
        subprocess.run(
            self.git + ["add", "."],
            cwd=self.source,
            check=True,
            env=self.environment,
        )
        subprocess.run(
            self.git
            + [
                "-c",
                "user.name=Klogg Test",
                "-c",
                "user.email=klogg@example.invalid",
                "commit",
                "-q",
                "-m",
                "initial",
            ],
            cwd=self.source,
            check=True,
            env=self.environment,
        )
        return subprocess.check_output(
            self.git + ["rev-parse", "HEAD"],
            cwd=self.source,
            text=True,
            env=self.environment,
        ).strip()

    def test_metadata_free_cache_is_verified_by_content(self):
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_digest_order_matches_cmake_for_nested_prefix_paths(self):
        nested = self.source / "a"
        nested.mkdir()
        (nested / "file.txt").write_text("nested\n")
        (self.source / "a.txt").write_text("sibling\n")
        digest = source_tree_digest(self.source)

        result = self.verify(digests=[digest])

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_metadata_free_cache_rejects_changed_content(self):
        self.sentinel.write_text("modified contents\n")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source tree SHA-256 mismatch", result.stdout + result.stderr)

    def test_incomplete_git_metadata_falls_back_to_exact_content(self):
        (self.source / ".git").mkdir()
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_approved_patched_tree_is_accepted(self):
        self.sentinel.write_text("approved patched contents\n")
        patched_digest = source_tree_digest(self.source)
        result = self.verify(digests=[self.clean_digest, patched_digest])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_git_revision_is_authoritative_when_metadata_exists(self):
        self.initialize_git()
        result = self.verify(revision="not-the-commit")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be pinned at not-the-commit", result.stdout + result.stderr)

    def test_git_checkout_rejects_unapproved_dirty_content(self):
        revision = self.initialize_git()
        self.second_sentinel.write_text("unapproved change\n")
        result = self.verify(revision=revision)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source tree SHA-256 mismatch", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
