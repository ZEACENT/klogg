from __future__ import annotations

import ast
import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
LINT_WORKFLOW = ROOT / ".github" / "workflows" / "lint.yml"
CI_QUALITY_RUNNER = ROOT / "scripts" / "run_ci_quality.py"
PYTHON_BASELINE = "3.8"
PEP585_BUILTINS = {"dict", "list", "set", "tuple"}
UNSUPPORTED_STRING_METHODS = {"removeprefix", "removesuffix"}


def python_sources():
    for directory in (ROOT / "scripts", ROOT / "tests" / "scripts"):
        yield from directory.rglob("*.py")


def annotations(tree):
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.returns is not None:
                yield node.returns
            for argument in (
                list(node.args.posonlyargs)
                + list(node.args.args)
                + list(node.args.kwonlyargs)
            ):
                if argument.annotation is not None:
                    yield argument.annotation
            if node.args.vararg is not None and node.args.vararg.annotation is not None:
                yield node.args.vararg.annotation
            if node.args.kwarg is not None and node.args.kwarg.annotation is not None:
                yield node.args.kwarg.annotation
        elif isinstance(node, ast.AnnAssign):
            yield node.annotation


class PythonBaselineContractTest(unittest.TestCase):
    def test_fast_lint_runs_the_full_contract_suite_on_python_38(self):
        workflow = LINT_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("actions/setup-python@", workflow)
        self.assertIn(f"python-version: '{PYTHON_BASELINE}'", workflow)
        self.assertIn("python3 scripts/run_ci_quality.py", workflow)
        runner = CI_QUALITY_RUNNER.read_text(encoding="utf-8")
        for token in ("unittest", "discover", "tests/scripts", "test_*.py"):
            self.assertIn(token, runner)

    def test_pep585_annotations_are_postponed_for_python_38(self):
        findings = []
        for path in python_sources():
            source = path.read_text(encoding="utf-8")
            tree = ast.parse(source, filename=str(path))
            uses_pep585 = any(
                isinstance(node, ast.Subscript)
                and isinstance(node.value, ast.Name)
                and node.value.id in PEP585_BUILTINS
                for annotation in annotations(tree)
                for node in ast.walk(annotation)
            )
            if uses_pep585 and "from __future__ import annotations" not in source:
                findings.append(str(path.relative_to(ROOT)))
        self.assertEqual(findings, [])

    def test_python_39_string_prefix_helpers_are_not_used(self):
        findings = []
        for path in python_sources():
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if (
                    isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr in UNSUPPORTED_STRING_METHODS
                ):
                    findings.append(
                        f"{path.relative_to(ROOT)}:{getattr(node, 'lineno', 0)}"
                    )
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
