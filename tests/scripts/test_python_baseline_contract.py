from __future__ import annotations

import ast
import io
import pathlib
import tokenize
import unittest


ROOT = pathlib.Path(__file__).parents[2]
LINT_WORKFLOW = ROOT / ".github" / "workflows" / "lint.yml"
CI_QUALITY_RUNNER = ROOT / "scripts" / "run_ci_quality.py"
PYTHON_BASELINE = "3.8"
PEP585_BUILTINS = {"dict", "list", "set", "tuple"}
UNSUPPORTED_STRING_METHODS = {"removeprefix", "removesuffix"}
UNSUPPORTED_PATH_METHODS = {"readlink"}


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


def unsupported_parenthesized_with_lines(source):
    tokens = [
        token
        for token in tokenize.generate_tokens(io.StringIO(source).readline)
        if token.type
        not in {
            tokenize.COMMENT,
            tokenize.INDENT,
            tokenize.DEDENT,
            tokenize.NL,
            tokenize.NEWLINE,
        }
    ]
    findings = []
    for index, token in enumerate(tokens[:-1]):
        if token.string != "with" or tokens[index + 1].string != "(":
            continue
        depth = 0
        for context_token in tokens[index + 1 :]:
            if context_token.string == "(":
                depth += 1
            elif context_token.string == ")":
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and context_token.string in {",", "as"}:
                findings.append(token.start[0])
                break
    return findings


class PythonBaselineContractTest(unittest.TestCase):
    def test_fast_lint_runs_the_full_contract_suite_on_python_38(self):
        workflow = LINT_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("actions/setup-python@", workflow)
        self.assertIn(f"python-version: '{PYTHON_BASELINE}'", workflow)
        self.assertIn("python3 scripts/run_ci_quality.py", workflow)
        runner = CI_QUALITY_RUNNER.read_text(encoding="utf-8")
        for token in ("unittest", "discover", "tests/scripts", "test_*.py"):
            self.assertIn(token, runner)

    def test_parenthesized_with_detection_ignores_spoofs_and_plain_with(self):
        source = """\
# with (
message = "with ("
with (single_manager):
    consume_single()
with (manager_factory(first, second)):
    consume_factory()
with first_manager, second_manager as value:
    consume(value)
"""
        self.assertEqual(unsupported_parenthesized_with_lines(source), [])
        self.assertEqual(
            unsupported_parenthesized_with_lines(
                "with (\n    first_manager,\n    second_manager as value,\n):\n    consume(value)\n"
            ),
            [1],
        )

    def test_python_310_parenthesized_with_syntax_is_not_used(self):
        findings = []
        for path in python_sources():
            source = path.read_text(encoding="utf-8")
            findings.extend(
                f"{path.relative_to(ROOT)}:{line}"
                for line in unsupported_parenthesized_with_lines(source)
            )
        self.assertEqual(findings, [])

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

    def test_python_39_pathlib_helpers_are_not_used(self):
        findings = []
        for path in python_sources():
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if (
                    isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr in UNSUPPORTED_PATH_METHODS
                    and not (
                        isinstance(node.func.value, ast.Name)
                        and node.func.value.id == "os"
                    )
                ):
                    findings.append(
                        f"{path.relative_to(ROOT)}:{getattr(node, 'lineno', 0)}"
                    )
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
