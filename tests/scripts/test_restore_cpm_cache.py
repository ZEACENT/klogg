import os
import pathlib
import subprocess
import tarfile
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
RESTORE_SCRIPT = ROOT / "scripts" / "restore_cpm_cache.sh"
CONTRACT_SCRIPT = ROOT / "scripts" / "check_cpm_cache_contract.sh"
PREFETCH_ACTION = ROOT / ".github" / "actions" / "prefetch-cpm-cache" / "action.yml"
CROARING_ROOT = pathlib.Path(
    "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041"
)
REQUIRED_PATHS = (
    CROARING_ROOT / "roaring.pc.in",
    CROARING_ROOT / "tests/config.h.in",
    CROARING_ROOT / "src/CMakeLists.txt",
)


class RestoreCpmCacheTest(unittest.TestCase):
    def run_restore(self, workspace, runner_temp, env_overrides=None):
        env = os.environ.copy()
        env["GITHUB_WORKSPACE"] = str(workspace)
        env["RUNNER_TEMP"] = str(runner_temp)
        if env_overrides:
            env.update(env_overrides)
        return subprocess.run(
            ["bash", str(RESTORE_SCRIPT)],
            check=False,
            capture_output=True,
            text=True,
            env=env,
        )

    def write_archive(self, workspace, paths):
        staging = workspace / "staging"
        for relative_path in paths:
            target = staging / relative_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"sentinel for {relative_path}\n")
        with tarfile.open(workspace / "cpm-cache.tar.gz", "w:gz") as archive:
            archive.add(staging / "cpm_cache", arcname="cpm_cache")

    def test_shared_contract_rejects_incomplete_cache_and_accepts_complete_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for relative_path in REQUIRED_PATHS[:-1]:
                target = root / relative_path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("present\n")

            incomplete = subprocess.run(
                ["bash", str(CONTRACT_SCRIPT), "--root", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(incomplete.returncode, 0)
            self.assertIn(str(REQUIRED_PATHS[-1]), incomplete.stderr)

            missing = root / REQUIRED_PATHS[-1]
            missing.parent.mkdir(parents=True, exist_ok=True)
            missing.write_text("present\n")
            complete = subprocess.run(
                ["bash", str(CONTRACT_SCRIPT), "--root", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(complete.returncode, 0, complete.stdout + complete.stderr)

    def test_prefetch_evicts_and_revalidates_incomplete_cached_package(self):
        action = PREFETCH_ACTION.read_text(encoding="utf-8")
        self.assertGreaterEqual(action.count("check_cpm_cache_contract.sh"), 2)
        self.assertIn("rm -rf", action)
        self.assertIn('CROARING_CACHE="$KLOGG_WORKSPACE/cpm_cache/croaring"', action)

    def test_valid_archive_replaces_cache_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workspace = root / "workspace"
            runner_temp = root / "runner-temp"
            workspace.mkdir()
            runner_temp.mkdir()
            stale = workspace / "cpm_cache/stale.txt"
            stale.parent.mkdir()
            stale.write_text("stale\n")
            self.write_archive(workspace, REQUIRED_PATHS)

            result = self.run_restore(workspace, runner_temp)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(stale.exists())
            for relative_path in REQUIRED_PATHS:
                self.assertTrue((workspace / relative_path).is_file())

    def test_windows_style_workspace_is_normalized_with_cygpath(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workspace = root / "workspace"
            runner_temp = root / "runner-temp"
            fake_bin = root / "bin"
            workspace.mkdir()
            runner_temp.mkdir()
            fake_bin.mkdir()
            stale = workspace / "cpm_cache/stale.txt"
            stale.parent.mkdir()
            stale.write_text("stale\n")
            self.write_archive(workspace, REQUIRED_PATHS)

            windows_workspace = "D:\\a\\klogg\\klogg"
            windows_runner_temp = "D:\\a\\_temp"
            cygpath = fake_bin / "cygpath"
            cygpath.write_text(
                "#!/usr/bin/env bash\n"
                'case "$2" in\n'
                '  "$FAKE_WINDOWS_WORKSPACE") '
                'printf \'%s\\n\' "$FAKE_POSIX_WORKSPACE" ;;\n'
                '  "$FAKE_WINDOWS_RUNNER_TEMP") '
                'printf \'%s\\n\' "$FAKE_POSIX_RUNNER_TEMP" ;;\n'
                "  *) exit 2 ;;\n"
                "esac\n"
            )
            cygpath.chmod(0o755)

            result = self.run_restore(
                workspace,
                runner_temp,
                {
                    "GITHUB_WORKSPACE": windows_workspace,
                    "RUNNER_TEMP": windows_runner_temp,
                    "FAKE_WINDOWS_WORKSPACE": windows_workspace,
                    "FAKE_WINDOWS_RUNNER_TEMP": windows_runner_temp,
                    "FAKE_POSIX_WORKSPACE": str(workspace),
                    "FAKE_POSIX_RUNNER_TEMP": str(runner_temp),
                    "PATH": f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                },
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(stale.exists())
            for relative_path in REQUIRED_PATHS:
                self.assertTrue((workspace / relative_path).is_file())

    def test_incomplete_archive_fails_before_replacing_existing_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workspace = root / "workspace"
            runner_temp = root / "runner-temp"
            workspace.mkdir()
            runner_temp.mkdir()
            stale = workspace / "cpm_cache/stale.txt"
            stale.parent.mkdir()
            stale.write_text("keep\n")
            self.write_archive(workspace, REQUIRED_PATHS[:-1])

            result = self.run_restore(workspace, runner_temp)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("CPM cache archive is missing", result.stderr)
            self.assertTrue(stale.is_file())

    def test_corrupt_archive_fails_before_replacing_existing_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workspace = root / "workspace"
            runner_temp = root / "runner-temp"
            workspace.mkdir()
            runner_temp.mkdir()
            stale = workspace / "cpm_cache/stale.txt"
            stale.parent.mkdir()
            stale.write_text("keep\n")
            (workspace / "cpm-cache.tar.gz").write_bytes(b"not a gzip archive")

            result = self.run_restore(workspace, runner_temp)

            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(stale.is_file())


if __name__ == "__main__":
    unittest.main()
