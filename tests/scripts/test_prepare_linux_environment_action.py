import json
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTION = ROOT / ".github/actions/prepare-linux-environment/action.yml"


class PrepareLinuxEnvironmentActionTest(unittest.TestCase):
    def body(self):
        self.assertTrue(ACTION.is_file(), "the verified read-only environment action is missing")
        text = ACTION.read_text(encoding="utf-8")
        self.assertIn("value: ${{ steps.resolve.outputs.image }}", text)
        return textwrap.dedent(text.split("      run: |\n", 1)[1])

    def execute(self, pull="true", retag="true", family="jammy-qt5"):
        body = self.body()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fake_python = root / "python3"
            fake_python.write_text(
                "#!" + sys.executable + "\n"
                "import json,os,pathlib,sys\n"
                "pathlib.Path(os.environ['CALL_LOG']).write_text(json.dumps(sys.argv[1:]))\n",
                encoding="utf-8",
            )
            fake_python.chmod(0o755)
            env = os.environ.copy()
            env.update(PATH=str(root) + os.pathsep + env.get("PATH", ""),
                       KLOGG_ENV_FAMILY=family, KLOGG_ENV_PULL=pull, KLOGG_ENV_RETAG=retag,
                       GITHUB_WORKSPACE=str(ROOT), GITHUB_OUTPUT=str(root / "outputs"),
                       CALL_LOG=str(root / "calls"))
            result = subprocess.run(["bash", "-c", body], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=20)
            calls = json.loads((root / "calls").read_text()) if (root / "calls").exists() else None
            return result, calls

    def test_preparation_calls_only_the_verified_digest_consumer(self):
        result, arguments = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(arguments[:5], ["scripts/consume_ci_environment.py", "--family", "jammy-qt5", "--repo-root", str(ROOT)])
        self.assertIn("--github-output", arguments)
        self.assertEqual(arguments[-2:], ["--pull", "--retag"])
        body = self.body()
        for forbidden in ("apt-get", "docker build", "docker login", "latest", "continue-on-error"):
            self.assertNotIn(forbidden, body)

    def test_resolver_mode_does_not_pull_or_retag(self):
        result, arguments = self.execute("false", "false", "noble-qt693-analysis")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("--pull", arguments)
        self.assertNotIn("--retag", arguments)

    def test_invalid_boolean_configuration_fails_before_consumer(self):
        for pull, retag in (("yes", "true"), ("true", ""), ("false", "true")):
            with self.subTest(pull=pull, retag=retag):
                result, arguments = self.execute(pull, retag)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(arguments)

    def test_family_is_data_not_shell_syntax(self):
        family = 'jammy-qt5"; exit 79; #'
        result, arguments = self.execute(family=family)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(arguments[2], family)


if __name__ == "__main__":
    unittest.main()
