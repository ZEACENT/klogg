import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PREPARE_SCRIPT = ROOT / "cmake" / "prepare_efsw_source.cmake"
APPLY_SERIES_SCRIPT = ROOT / "cmake" / "apply_git_patch_series.cmake"


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


def directory_bytes(source: pathlib.Path) -> dict:
    return {
        path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in source.rglob("*")
        if path.is_file()
    }


class PrepareEfswSourceTest(unittest.TestCase):
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
        self.cache = self.root / "shared-cache"
        subprocess.run(
            self.git + ["init", "-q", "-b", "fixture-pinned", str(self.cache)],
            check=True,
            env=self.environment,
        )
        (self.cache / "value.txt").write_text("clean source\n")
        subprocess.run(
            self.git + ["add", "value.txt"],
            cwd=self.cache,
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
                "fixture pinned source",
            ],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        self.revision = self.git_output("rev-parse", "HEAD")
        self.clean_digest = source_tree_digest(self.cache)
        self.patch = self.root / "already-applied.patch"
        self.patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-clean source\n"
            "+approved patched cache\n"
        )
        subprocess.run(
            self.git + ["apply", str(self.patch)],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        self.patched_digest = source_tree_digest(self.cache)
        subprocess.run(
            self.git + ["reset", "--hard", "-q", "HEAD"],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )

    def tearDown(self):
        self.temporary.cleanup()

    def git_output(self, *arguments, cwd=None):
        return subprocess.check_output(
            self.git + list(arguments),
            cwd=cwd or self.cache,
            text=True,
            env=self.environment,
        ).strip()

    def prepare(
        self,
        destination,
        cache=None,
        revision=None,
        clean_digest=None,
        approved_cache_digests=None,
        timeout=10,
    ):
        cache = cache or self.cache
        revision = revision or self.revision
        clean_digest = clean_digest or self.clean_digest
        approved_cache_digests = approved_cache_digests or [clean_digest]
        command = [
            "cmake",
            "-DDEPENDENCY=efsw",
            f"-DCACHE_SOURCE={cache}",
            f"-DDESTINATION={destination}",
            f"-DEXPECTED_REVISION={revision}",
            f"-DCLEAN_TREE_HASH={clean_digest}",
            "-DAPPROVED_CACHE_TREE_HASHES="
            + ";".join(approved_cache_digests),
            "-DLOCK_TIMEOUT=2",
            "-DGIT_TIMEOUT=2",
            "-P",
            str(PREPARE_SCRIPT),
        ]
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
            timeout=timeout,
        )

    def private_marker(self, destination):
        return destination.parent / f".{destination.name}.klogg-private-source"

    def assert_prepared_clean_checkout(self, destination):
        self.assertTrue((destination / ".git").exists())
        self.assertNotEqual(destination.resolve(), self.cache.resolve())
        self.assertEqual((destination / "value.txt").read_text(), "clean source\n")
        self.assertEqual(source_tree_digest(destination), self.clean_digest)
        self.assertEqual(self.git_output("rev-parse", "HEAD", cwd=destination), self.revision)
        self.assertEqual(self.private_marker(destination).read_text(), self.revision + "\n")

    def test_prepares_distinct_clean_private_checkout_from_clean_cache(self):
        destination = self.root / "private" / "efsw"
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_uses_pinned_commit_not_approved_patched_cache_worktree(self):
        destination = self.root / "private" / "efsw"
        subprocess.run(
            self.git + ["apply", str(self.patch)],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        cache_bytes_before = directory_bytes(self.cache)
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(
            destination,
            approved_cache_digests=[self.clean_digest, self.patched_digest],
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_later_patch_series_preserves_lf_with_global_autocrlf_true(self):
        global_config = self.root / "global-gitconfig"
        global_config.write_text("[core]\n\tautocrlf = true\n")
        self.environment["GIT_CONFIG_GLOBAL"] = str(global_config)
        destination = self.root / "private" / "efsw"
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()
        expected = self.root / "expected-patched"
        shutil.copytree(self.cache, expected)
        (expected / "value.txt").write_bytes(b"patched LF\n")
        patched_digest = source_tree_digest(expected)
        patch = self.root / "later.patch"
        patch.write_bytes(
            b"diff --git a/value.txt b/value.txt\n"
            b"--- a/value.txt\n"
            b"+++ b/value.txt\n"
            b"@@ -1 +1 @@\n"
            b"-clean source\n"
            b"+patched LF\n"
        )

        prepared = self.prepare(destination)
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        stored_autocrlf = subprocess.check_output(
            ["git", "-C", str(destination), "config", "--local", "--get", "core.autocrlf"],
            text=True,
            env=self.environment,
        ).strip()
        self.assertEqual(stored_autocrlf, "false")
        applied = subprocess.run(
            [
                "cmake",
                "-DDEPENDENCY=efsw",
                f"-DREPO_DIR={destination}",
                f"-DEXPECTED_REVISION={self.revision}",
                f"-DCLEAN_TREE_HASH={self.clean_digest}",
                f"-DAPPROVED_TREE_HASHES={self.clean_digest};{patched_digest}",
                f"-DPATCHED_TREE_HASH={patched_digest}",
                f"-DPATCH_FILES={patch}",
                "-P",
                str(APPLY_SERIES_SCRIPT),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        self.assertEqual((destination / "value.txt").read_bytes(), b"patched LF\n")
        self.assertEqual(source_tree_digest(destination), patched_digest)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_rerun_reuses_the_verified_private_pin_without_resetting_it(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        destination_bytes_before = directory_bytes(destination)
        destination_index_before = (destination / ".git" / "index").read_bytes()

        second = self.prepare(destination)

        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("already prepared", second.stdout + second.stderr)
        self.assertEqual(directory_bytes(destination), destination_bytes_before)
        self.assertEqual((destination / ".git" / "index").read_bytes(), destination_index_before)

    def test_rerun_reuses_a_user_changed_private_checkout_without_resetting_it(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        value = destination / "value.txt"
        value.write_text("user change\n")
        index_before = (destination / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("already prepared for patch series", result.stdout + result.stderr)
        self.assertEqual(value.read_text(), "user change\n")
        self.assertEqual((destination / ".git" / "index").read_bytes(), index_before)

    def test_rejects_clean_destination_without_usable_git_metadata(self):
        destination = self.root / "private" / "efsw"
        shutil.copytree(self.cache, destination)
        shutil.rmtree(destination / ".git")
        preserved_bytes = directory_bytes(destination)

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), preserved_bytes)

    def test_rejects_unmarked_git_checkout_without_overwriting_it(self):
        destination = self.root / "private" / "efsw"
        shutil.copytree(self.cache, destination)
        preserved_bytes = directory_bytes(destination)

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("was not prepared by this helper", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), preserved_bytes)
        self.assertFalse(self.private_marker(destination).exists())

    def assert_cache_unchanged(self, cache_bytes_before, cache_head_before, cache_index_before):
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_rejects_markered_checkout_with_git_dir_symlink_to_cache(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        shutil.rmtree(destination / ".git")
        (destination / ".git").symlink_to(self.cache / ".git", target_is_directory=True)
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_rejects_markered_checkout_with_redirected_worktree(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        subprocess.run(
            self.git + ["config", "core.worktree", str(self.cache)],
            cwd=destination,
            check=True,
            env=self.environment,
        )
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Git worktree is not private", result.stdout + result.stderr)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_cross_volume_relative_path_guard_is_explicit(self):
        source = PREPARE_SCRIPT.read_text()
        self.assertIn('if(IS_ABSOLUTE "${_path_remainder}")', source)
        outer = self.root / "outer"
        child = self.root / "child"
        outer.mkdir()
        child.mkdir()
        probe = self.root / "relative-path-probe.cmake"
        probe.write_text(
            f"file(RELATIVE_PATH relative [[{outer.as_posix()}]] [[{child.as_posix()}]])\n"
            "if(IS_ABSOLUTE \"${relative}\")\n"
            "  message(FATAL_ERROR \"host relative path unexpectedly absolute\")\n"
            "endif()\n"
            "if(NOT \"${relative}\" MATCHES \"^\\\\.\\\\.(/|$)\")\n"
            "  message(FATAL_ERROR \"host sibling did not escape parent\")\n"
            "endif()\n"
        )

        result = subprocess.run(
            ["cmake", "-P", str(probe)],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_cache_isolation_rejected(self, destination):
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not overlap", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_rejects_destination_equal_to_cache_source(self):
        self.assert_cache_isolation_rejected(self.cache)

    def test_rejects_destination_symlink_alias_to_cache_source(self):
        alias = self.root / "cache-alias"
        alias.symlink_to(self.cache, target_is_directory=True)

        self.assert_cache_isolation_rejected(alias)

    def test_rejects_destination_nested_within_cache_source(self):
        self.assert_cache_isolation_rejected(self.cache / "private-source")

    def test_rejects_destination_within_existing_symlink_to_cache_source(self):
        alias_parent = self.root / "cache-parent-alias"
        alias_parent.symlink_to(self.cache, target_is_directory=True)

        self.assert_cache_isolation_rejected(alias_parent / "private-source")

    def test_rejects_cache_source_nested_within_destination(self):
        self.assert_cache_isolation_rejected(self.root)

    def test_rejects_wrong_pin_without_creating_destination(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, revision="0" * 40)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be pinned", result.stdout + result.stderr)
        self.assertFalse(destination.exists())

    def test_rejects_missing_cache_path_without_creating_destination(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, cache=self.root / "missing-cache")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source directory does not exist", result.stdout + result.stderr)
        self.assertFalse(destination.exists())

    def test_partial_git_cache_fails_within_the_subprocess_bound_and_cleans_staging(self):
        partial = self.root / "partial-cache"
        shutil.copytree(self.cache, partial)
        partial_revision = self.git_output("rev-parse", "HEAD", cwd=partial)
        shutil.rmtree(partial / ".git" / "objects")
        destination = self.root / "private" / "efsw"

        result = self.prepare(
            destination,
            cache=partial,
            revision=partial_revision,
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to clone", result.stdout + result.stderr)
        self.assertFalse(destination.exists())
        self.assertEqual(list(destination.parent.glob(".efsw.prepare-*")), [])

    def test_never_overwrites_an_unknown_existing_destination(self):
        destination = self.root / "private" / "efsw"
        destination.mkdir(parents=True)
        sentinel = destination / "user-file.txt"
        sentinel.write_text("do not replace\n")

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assertEqual(sentinel.read_text(), "do not replace\n")


if __name__ == "__main__":
    unittest.main()
