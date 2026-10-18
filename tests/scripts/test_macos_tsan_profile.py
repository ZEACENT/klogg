"""Real CMake fixtures for the opt-in Intel macOS TSan optimization profile."""

import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "cmake" / "CiMacosTsanProfile.cmake"
GUARD = ROOT / "scripts" / "verify_macos_tsan_profile.py"


def load_guard():
    spec = importlib.util.spec_from_file_location("tsan_profile_guard", GUARD)
    assert spec is not None and spec.loader is not None
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    return guard


class CompileDatabaseGuardTest(unittest.TestCase):
    def setUp(self):
        self.guard = load_guard()

    @staticmethod
    def entry(flags="-O2 -g -fsanitize=thread -O1 -g1", source="source.cpp"):
        return {"directory": "/tmp", "file": source,
                "command": f"clang++ {flags} -c {shlex.quote(source)}"}

    def test_accepts_final_o1_preserving_debug_and_thread_instrumentation(self):
        entries = [self.entry(), self.entry(source="vendor.c"),
                   self.entry(flags="-O3", source="assembly.S")]
        self.assertEqual(self.guard.verify_entries(entries), 2)

    def test_accepts_preserved_dependency_full_debug_information(self):
        self.assertEqual(self.guard.verify_entries([
            self.entry("-O1 -g1 -g -fsanitize=thread")
        ]), 1)

    def test_allows_unchanged_uninstrumented_dependency_optimization(self):
        self.assertEqual(self.guard.verify_entries([self.entry("-O1 -g1")]), 1)

    @unittest.skipIf(os.name == "nt", "the existing scope shell gate uses POSIX paths")
    def test_cli_preserves_existing_first_party_and_required_dependency_scope(self):
        with tempfile.TemporaryDirectory(prefix="klogg-tsan-scope-") as directory:
            database = Path(directory) / "compile_commands.json"
            first_party = str(ROOT / "src" / "profile_fixture.cpp")
            dependency = str(Path(directory) / "cpm_cache" / "vectorscan" / "dep.cpp")
            allocator = str(Path(directory) / "cpm_cache" / "mimalloc" / "dep.c")
            entries = [self.entry(source=first_party),
                       self.entry("-O1 -g1", source=dependency),
                       self.entry(source=allocator)]
            for mutation, accepted in ((None, True), (0, False), (1, False), (2, False)):
                with self.subTest(mutation=mutation):
                    changed = [dict(entry) for entry in entries]
                    if mutation is not None:
                        flags = "-O1 -g1 -fsanitize=thread" if mutation == 1 else "-O1 -g1"
                        changed[mutation] = self.entry(flags, source=changed[mutation]["file"])
                    database.write_text(json.dumps(changed))
                    result = subprocess.run(["python3", str(GUARD), str(database)],
                                            text=True, capture_output=True, check=False)
                    self.assertEqual(result.returncode == 0, accepted,
                                     result.stdout + result.stderr)

    def test_accepts_argument_array_and_explicit_cxx_language(self):
        entry = self.entry(source="generated.inc")
        entry.pop("command")
        entry["arguments"] = ["clang++", "-x", "c++", "-O1", "-g1",
                              "-fsanitize=thread", "-c", "generated.inc"]
        self.assertEqual(self.guard.verify_entries([entry]), 1)

    def test_rejects_lost_optimization_debug_instrumentation_or_no_lto(self):
        for flags in (
            "-O1 -g1 -fsanitize=thread -O3",
            "-O1 -g1 -fsanitize=thread -g0",
            "-O1 -g1 -fsanitize=address",
            "-O1 -g1 -fsanitize=thread -fno-sanitize=thread",
            "-O1 -g1 -fsanitize=thread -fno-sanitize=all",
            "-O1 -g1 -fsanitize=thread -flto=thin",
            "-O1 -g1 -fsanitize=thread -fno-sanitize-thread-memory-access",
            "-O1 -g1 -fsanitize=thread -fsanitize-ignorelist=ignore.txt",
            "-O1 -g1 -fsanitize=thread @opaque.rsp",
        ):
            with self.subTest(flags=flags):
                with self.assertRaisesRegex(ValueError, "source.cpp"):
                    self.guard.verify_entries([self.entry(flags)])

    def test_rejects_empty_malformed_and_unknown_language_databases(self):
        for entries in ([], {}, [self.entry(source="unknown.xyz")],
                        [{"file": "source.cpp"}], ["invalid"]):
            with self.subTest(entries=entries):
                with self.assertRaises(ValueError):
                    self.guard.verify_entries(entries)


@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "x86_64",
                     "the enabled profile requires Intel macOS AppleClang")
class MacosTsanProfileCmakeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="klogg-tsan-profile-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.env = dict(os.environ)
        developer = Path("/Applications/Xcode_26.6.app/Contents/Developer")
        if developer.is_dir():
            self.env["DEVELOPER_DIR"] = str(developer)
        (self.root / "dependency").mkdir()
        (self.root / "dependency" / "dep.c").write_text("int dependency(void) { return 7; }\n")
        (self.root / "dependency" / "CMakeLists.txt").write_text(
            "project(dependency LANGUAGES C)\n"
            "add_compile_options(-O3)\n"
            "add_library(dependency STATIC dep.c)\n"
            "target_compile_options(dependency PRIVATE -O3)\n"
        )
        (self.root / "main.cpp").write_text(
            'extern "C" int dependency(void);\n'
            "int main() { return dependency() == 7 ? 0 : 1; }\n"
        )
        (self.root / "empty.S").write_text(".text\n")

    def configure(self, *extra, enabled=True, suffix=""):
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\n"
            "project(profile_fixture LANGUAGES C CXX ASM)\n"
            'set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g1 -DNDEBUG")\n'
            'set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g1 -DNDEBUG")\n'
            "add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=thread>)\n"
            "add_link_options(-fsanitize=thread)\n"
            "add_subdirectory(dependency)\n"
            "add_executable(consumer main.cpp empty.S)\n"
            "target_compile_options(consumer PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-O3>)\n"
            "target_link_libraries(consumer PRIVATE dependency)\n"
            "enable_testing()\n"
            "add_test(NAME smoke COMMAND consumer)\n" + suffix
        )
        return subprocess.run(
            ["cmake", "-S", str(self.root), "-B", str(self.build), "-G", "Ninja",
             f"-DCMAKE_PROJECT_INCLUDE={PROFILE}",
             f"-DKLOGG_CI_MACOS_TSAN_O1={'ON' if enabled else 'OFF'}",
             "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DENABLE_SANITIZER_THREAD=ON",
             "-DKLOGG_CI_LIGHT_DEBUG=ON", "-DKLOGG_USE_LTO=OFF",
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *extra],
            text=True, capture_output=True, env=self.env, check=False,
        )

    def commands(self):
        return json.loads((self.build / "compile_commands.json").read_text())

    def test_late_target_profile_beats_dependency_o3_without_touching_assembly(self):
        result = self.configure(suffix=(
            'target_compile_options(consumer BEFORE PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-O1>")\n'
        ))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for entry in self.commands():
            args = shlex.split(entry["command"])
            if entry["file"].endswith(".S"):
                self.assertNotIn("-O1", args)
                self.assertNotIn("-g1", args)
            else:
                self.assertEqual([arg for arg in args if arg.startswith("-O")][-1], "-O1")
                self.assertIn("-g1", args)
                self.assertIn("-fsanitize=thread", args)
        self.assertEqual(load_guard().verify_entries(self.commands()), 2)
        build = subprocess.run(["cmake", "--build", str(self.build), "--parallel", "1"],
                               text=True, capture_output=True, env=self.env, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        smoke = subprocess.run([str(self.build / "consumer")], env=self.env,
                               text=True, capture_output=True, check=False)
        self.assertEqual(smoke.returncode, 0, smoke.stdout + smoke.stderr)

    def test_profile_preserves_every_original_debug_flag_including_dependency_g(self):
        suffix = "target_compile_options(dependency PRIVATE -g)\n"
        baseline = self.configure(enabled=False, suffix=suffix)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        before = {entry["file"]: [arg for arg in shlex.split(entry["command"])
                                 if arg.startswith("-g")]
                  for entry in self.commands()}
        candidate = self.configure(suffix=suffix)
        self.assertEqual(candidate.returncode, 0, candidate.stdout + candidate.stderr)
        after = {entry["file"]: [arg for arg in shlex.split(entry["command"])
                                if arg.startswith("-g")]
                 for entry in self.commands()}
        self.assertEqual(before, after)
        self.assertEqual(after[str(self.root / "dependency" / "dep.c")][-1], "-g")
        self.assertEqual(load_guard().verify_entries(self.commands()), 2)

    def test_disabled_profile_leaves_dependency_options_unchanged(self):
        result = self.configure("-DENABLE_SANITIZER_THREAD=OFF", enabled=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(all("-O1" not in entry["command"] for entry in self.commands()))

    def test_unsupported_modes_fail_closed(self):
        for option in ("-DENABLE_SANITIZER_THREAD=OFF", "-DKLOGG_USE_LTO=ON",
                       "-DKLOGG_CI_LIGHT_DEBUG=OFF", "-DCMAKE_BUILD_TYPE=Release",
                       "-DENABLE_SANITIZER_ADDRESS=ON", "-DENABLE_COVERAGE=ON",
                       "-DCMAKE_OSX_ARCHITECTURES=arm64"):
            with self.subTest(option=option):
                result = self.configure(option)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("KLOGG_CI_MACOS_TSAN_O1", result.stdout + result.stderr)
                # A new cache for every case prevents one rejection masking another.
                shutil.rmtree(self.build)

    def test_late_source_and_transitive_overrides_are_caught_by_database_guard(self):
        for suffix in (
            'set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-O3")\n',
            "add_library(interface_options INTERFACE)\n"
            "target_compile_options(interface_options INTERFACE -O2)\n"
            "target_link_libraries(consumer PRIVATE interface_options)\n",
        ):
            with self.subTest(suffix=suffix):
                result = self.configure(suffix=suffix)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with self.assertRaisesRegex(ValueError, "main.cpp"):
                    load_guard().verify_entries(self.commands())
                shutil.rmtree(self.build)

    def test_rejects_target_ipo_and_non_native_compilers(self):
        for suffix in (
            "set_property(TARGET consumer PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)\n",
            "set(CMAKE_CXX_COMPILER_ID GNU)\n",
            "set(CMAKE_SYSTEM_NAME Linux)\n",
        ):
            with self.subTest(suffix=suffix):
                result = self.configure(suffix=suffix)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("KLOGG_CI_MACOS_TSAN_O1", result.stdout + result.stderr)
                shutil.rmtree(self.build)


class DisabledProfileCmakeTest(unittest.TestCase):
    def test_unset_profile_remains_inert_for_the_developer_project_minimum(self):
        with tempfile.TemporaryDirectory(prefix="klogg-tsan-disabled-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.14)\n"
                "project(developer_project LANGUAGES NONE)\n"
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build"),
                 f"-DCMAKE_PROJECT_INCLUDE={PROFILE}"],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
