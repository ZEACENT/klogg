import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SANITIZERS = ROOT / "cmake" / "Sanitizers.cmake"


class SanitizerConfigurationTest(unittest.TestCase):
    def configure(self, compiler_id, *options, processor="x86_64"):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(sanitizer_guard LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
                f'set(CMAKE_SYSTEM_PROCESSOR "{processor}")\n'
                "set(CMAKE_SIZEOF_VOID_P 8)\n"
                f'include("{SANITIZERS}")\n'
                "enable_sanitizers(project_options)\n"
            )
            return subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build"), *options],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_cmake_312_uses_the_link_option_compatibility_wrapper(self):
        text = SANITIZERS.read_text()
        self.assertIn("if(COMMAND add_link_options)", text)
        self.assertEqual(
            len(re.findall(r"^\s*add_link_options\(", text, re.MULTILINE)), 1
        )
        self.assertEqual(text.count("klogg_add_link_options("), 3)

    def test_supported_clang_address_sanitizer_configures(self):
        result = self.configure("Clang", "-DENABLE_SANITIZER_ADDRESS=ON")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_gcc_tsan_because_mimalloc_requires_clang(self):
        result = self.configure("GNU", "-DENABLE_SANITIZER_THREAD=ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ThreadSanitizer builds require Clang", result.stdout + result.stderr)

    def test_rejects_unconfigured_compiler_instead_of_false_green(self):
        result = self.configure("IntelLLVM", "-DENABLE_SANITIZER_ADDRESS=ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Sanitizer builds are not configured", result.stdout + result.stderr)

    def test_msvc_address_sanitizer_marks_intercepted_dependency_allocations(self):
        text = SANITIZERS.read_text()
        self.assertIn("KLOGG_MSVC_ASAN=1", text)
        allocator_source = (
            ROOT / "src" / "regex" / "src" / "hsregularexpression.cpp"
        ).read_text()
        self.assertIn("hs_set_allocator( std::malloc, std::free )", allocator_source)

    def test_rejects_msvc_arm64_address_sanitizer(self):
        result = self.configure(
            "MSVC", "-DENABLE_SANITIZER_ADDRESS=ON", processor="ARM64"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("supported only for x64 builds", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
