"""Execute verbatim startup/cleanup C from freshly verified, fully patched sources.

Set KLOGG_IOS_NATIVE_ARCHIVE_ROOT to require the disconnected runtime contract.
Without it, discovery uses the local archive cache when present, otherwise skips.
No prepared build tree or copied implementation is accepted as source authority.
"""
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = ROOT / "3rdparty/libimobiledevice/libimobiledevice.lock.json"
HARNESS = pathlib.Path(__file__).with_name("ostrace_startup_harness.c")
spec = importlib.util.spec_from_file_location(
    "ostrace_startup_source_build", ROOT / "scripts/build_ios_native_stack.py"
)
BUILD = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BUILD)


def extract(source: str, pattern: str) -> str:
    matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one pinned C definition: {pattern}")
    return matches[0].group(0)


def runtime_source(tree: pathlib.Path) -> str:
    """Select whole definitions verbatim; substitute effects, never startup logic."""
    source = (tree / "src/ostrace.c").read_text()
    public = (tree / "include/libimobiledevice/ostrace.h").read_text()
    private = (tree / "src/ostrace.h").read_text()
    definitions = [
        extract(public, r"^typedef enum \{.*?^\} ostrace_error_t;"),
        "typedef struct ostrace_client_private *ostrace_client_t;",
        extract(private, r"^struct ostrace_client_private \{.*?^\};"),
    ]
    for name in ("ostrace_activity_cb_t", "ostrace_record_cb_t", "ostrace_terminal_cb_t"):
        definitions.append(extract(public, rf"^typedef void \(\*{name}\)\([^\n]+;"))
    definitions.append(extract(source, r"^struct ostrace_worker_thread \{.*?^\};"))
    definitions.extend(re.findall(r"^#define OSTRACE_[^\n]+", source, re.MULTILINE))
    functions = []
    for name in (
        "ostrace_error", "ostrace_receive_interruptible", "_ostrace_check_result",
        "ostrace_worker", "ostrace_start_activity_internal", "ostrace_start_activity",
        "ostrace_start_activity_with_error",
        "ostrace_start_activity_with_record_type_and_error", "ostrace_stop_activity",
        "ostrace_client_free",
    ):
        function = extract(
            source, rf"^(?:static )?(?:ostrace_error_t |void \*){name}\([^\n]*\)\n\{{.*?^\}}"
        )
        # Real wrapper forward declarations, independent of extraction order.
        definitions.append(function.split("\n", 1)[0] + ";")
        functions.append(function)
    return "\n".join(definitions) + "\n/* TEST_EFFECTS */\n" + "\n\n".join(functions)


class OsTraceStartupRuntimeContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("KLOGG_IOS_NATIVE_ARCHIVE_ROOT")
        archives = pathlib.Path(configured) if configured else ROOT / "build_root/ios-native-sources"
        lock = json.loads(LOCK.read_text())
        lock["sources"] = [item for item in lock["sources"] if item["id"] == "libimobiledevice"]
        lock["patches"] = [item for item in lock["patches"] if item["source_id"] == "libimobiledevice"]
        if not configured and not (archives / lock["sources"][0]["archive_file"]).is_file():
            raise unittest.SkipTest("pinned archive absent; set KLOGG_IOS_NATIVE_ARCHIVE_ROOT to require runtime contract")
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            raise AssertionError("C compiler required for pinned ostrace startup runtime contract")
        temporary = tempfile.TemporaryDirectory(prefix="klogg-ostrace-startup-")
        cls.addClassCleanup(temporary.cleanup)
        work = pathlib.Path(temporary.name)
        tree = BUILD.prepare_sources(lock, archives, work / "sources", ROOT)["libimobiledevice"]
        print(f"ostrace runtime provenance: archive={lock['sources'][0]['archive_sha256']} "
              f"patched_tree={BUILD.tree_sha256(tree)}", flush=True)
        declarations, functions = runtime_source(tree).split("/* TEST_EFFECTS */")
        template = HARNESS.read_text()
        translation_unit = template.replace("/* PINNED_DECLARATIONS */", declarations).replace(
            "/* PINNED_FUNCTIONS */", functions
        )
        cls.work = work
        cls.compiler = compiler
        cls.translation_unit = translation_unit
        cls.binary = cls.compile_source(translation_unit, "startup")

    @classmethod
    def compile_source(cls, source: str, name: str) -> pathlib.Path:
        path = cls.work / f"{name}.c"
        path.write_text(source)
        binary = cls.work / name
        flags = shlex.split(os.environ.get("KLOGG_OSTRACE_TEST_CFLAGS", ""))
        result = subprocess.run(
            [*cls.compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pthread",
             *flags, str(path), "-o", str(binary)],
            text=True, capture_output=True, check=False,
        )
        if result.returncode:
            raise AssertionError(f"harness compilation failed (not behavioral RED):\n{result.stderr}")
        return binary

    def test_negotiated_startup_lifecycle(self):
        # Success first proves negotiation reaches worker creation before injected failures.
        for wrapper in ("typed", "legacy-error", "legacy"):
            for scenario in (
                "success", "malloc-failure", "thread-failure", "thread-failure-mutated",
                "send-failure", "receive-failure", "reply-rejected", "missing-status",
                "invalid-status", "with-options", "callback-stop", "callback-free", "natural-terminal",
            ):
                with self.subTest(wrapper=wrapper, scenario=scenario):
                    result = subprocess.run(
                        [str(self.binary), wrapper, scenario], text=True, capture_output=True,
                        check=False, timeout=30
                    )
                    print(result.stdout, end="")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_executable_startup_rejects_early_return_and_dead_code_mutants(self):
        # Deliberate test mutants of the verified source, NOT source authority or
        # build inputs. Both retain the structurally checked negotiation tail.
        marker = "\tres = _ostrace_check_result(dict);"
        self.assertEqual(self.translation_unit.count(marker), 1)
        end = "\n\treturn res;\n}\n\nostrace_error_t ostrace_start_activity("
        self.assertEqual(self.translation_unit.count(end), 1)
        mutations = {
            "early-return": self.translation_unit.replace(
                marker, "\treturn OSTRACE_E_SUCCESS;\n" + marker, 1
            ),
            "dead-branch": self.translation_unit.replace(
                marker, "\tif (0) {\n" + marker, 1
            ).replace(end, "\n\t}" + end, 1),
            "disabled-tail": self.translation_unit.replace(
                marker, "#if 0\n" + marker, 1
            ).replace(
                end, "\n#endif\n\t(void)terminal_callback; (void)user_data;"
                "\n\t(void)_ostrace_check_result; (void)thread_new;" + end, 1
            ),
        }
        for name, source in mutations.items():
            with self.subTest(mutation=name):
                binary = self.compile_source(source, name)
                result = subprocess.run(
                    [str(binary), "typed", "malloc-failure"], text=True,
                    capture_output=True, check=False, timeout=30,
                )
                self.assertNotEqual(result.returncode, 0, "unreachable startup escaped the behavioral guard")
                self.assertIn("ready == successful", result.stderr)
                print(f"rejected executable startup mutant: {name}", flush=True)

    def test_executable_startup_accepts_comment_and_adjacent_valid_cleanup(self):
        marker = "\tres = _ostrace_check_result(dict);"
        source = self.translation_unit.replace(
            marker, '/* return OSTRACE_E_SUCCESS; #if 0 */\n' + marker, 1
        ).replace(
            "\t\t\tclient->worker = THREAD_T_NULL;\n\t\t\tfree(oswt);",
            "\t\t\tfree(oswt);\n\t\t\tclient->worker = THREAD_T_NULL;", 1,
        )
        binary = self.compile_source(source, "adjacent-valid")
        result = subprocess.run(
            [str(binary), "typed", "thread-failure-mutated"], text=True,
            capture_output=True, check=False, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class OsTraceStartupInputContractTest(unittest.TestCase):
    def test_required_disconnected_invocation_fails_for_missing_and_corrupt_archives(self):
        lock = json.loads(LOCK.read_text())
        archive_name = next(
            item["archive_file"] for item in lock["sources"] if item["id"] == "libimobiledevice"
        )
        with tempfile.TemporaryDirectory(prefix="klogg-ostrace-bad-input-") as temporary:
            archives = pathlib.Path(temporary)
            for corrupt in (False, True):
                with self.subTest(corrupt=corrupt):
                    if corrupt:
                        (archives / archive_name).write_bytes(b"not the pinned source archive")
                    result = subprocess.run(
                        [sys.executable, str(pathlib.Path(__file__).resolve()),
                         "OsTraceStartupRuntimeContractTest.test_negotiated_startup_lifecycle"],
                        env={**os.environ, "KLOGG_IOS_NATIVE_ARCHIVE_ROOT": str(archives)},
                        text=True, capture_output=True, check=False, timeout=30,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        "locked source archive sha256 mismatch" if corrupt else "missing locked source archive",
                        result.stderr,
                    )
                    self.assertNotIn("skipped", result.stderr)


if __name__ == "__main__":
    unittest.main()
