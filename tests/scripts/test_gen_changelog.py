import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "gen_changelog.py"


def run(cmd, cwd, **kwargs):
    env = os.environ.copy()
    env.update(
        {
            "GIT_AUTHOR_NAME": "Test Author",
            "GIT_AUTHOR_EMAIL": "author@example.invalid",
            "GIT_COMMITTER_NAME": "Test Committer",
            "GIT_COMMITTER_EMAIL": "committer@example.invalid",
        }
    )
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, check=True, **kwargs)


def commit(repo, message, file_name):
    path = repo / file_name
    path.write_text(message + "\n", encoding="utf-8")
    run(["git", "add", file_name], repo)
    run(["git", "commit", "-m", message], repo, stdout=subprocess.PIPE)


def init_repo(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    run(["git", "init"], repo, stdout=subprocess.PIPE)
    commit(repo, "chore: initial", "initial.txt")
    run(["git", "tag", "v1.0.0"], repo)
    commit(repo, "fix: stable fix", "stable.txt")
    run(["git", "tag", "v1.1.0"], repo)
    commit(repo, "feat: continuous feature", "continuous.txt")
    run(["git", "tag", "continuous"], repo)
    commit(repo, "fix: follow-up release fix", "release.txt")
    return repo


class GenChangelogTest(unittest.TestCase):
    def test_prerelease_notes_start_at_latest_version_release(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            repo = init_repo(Path(tmp_dir))

            result = run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--mode",
                    "prerelease",
                    "--to-ref",
                    "HEAD",
                ],
                repo,
                stdout=subprocess.PIPE,
            )

        self.assertIn("continuous feature", result.stdout)
        self.assertIn("follow-up release fix", result.stdout)
        self.assertNotIn("stable fix", result.stdout)
        self.assertNotIn("initial", result.stdout)

    def test_release_notes_exclude_current_release_tag_when_selecting_previous(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            repo = init_repo(Path(tmp_dir))
            run(["git", "tag", "v1.2.0"], repo)

            result = run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--mode",
                    "release",
                    "--current-tag",
                    "v1.2.0",
                    "--to-ref",
                    "v1.2.0",
                ],
                repo,
                stdout=subprocess.PIPE,
            )

        self.assertIn("continuous feature", result.stdout)
        self.assertIn("follow-up release fix", result.stdout)
        self.assertNotIn("stable fix", result.stdout)
        self.assertNotIn("initial", result.stdout)

    def test_non_conventional_feature_message_uses_feature_group(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            repo = init_repo(Path(tmp_dir))
            commit(repo, "Add search feature polish", "feature-polish.txt")

            result = run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--mode",
                    "prerelease",
                    "--to-ref",
                    "HEAD",
                ],
                repo,
                stdout=subprocess.PIPE,
            )

        feature_header = "## New features:"
        bug_fixes_header = "## Bug fixes:"
        message = "Add search feature polish"
        self.assertIn(feature_header, result.stdout)
        self.assertIn(bug_fixes_header, result.stdout)
        self.assertLess(result.stdout.index(feature_header), result.stdout.index(message))
        self.assertLess(result.stdout.index(message), result.stdout.index(bug_fixes_header))


if __name__ == "__main__":
    unittest.main()
