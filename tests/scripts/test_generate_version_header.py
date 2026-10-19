"""Keep unchanged generated version headers stable across native build phases."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "cmake/generate_version_h.cmake"


@unittest.skipUnless(shutil.which("cmake"), "version generator requires CMake")
class VersionHeaderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        (self.build / "generated").mkdir(parents=True)
        self.header = self.build / "generated/version.h"
        self.environment = dict(os.environ, SOURCE_DATE_EPOCH="1700000000")

    def generate(self, version):
        subprocess.run(["cmake", "-DCMAKE_SOURCE_DIR=" + str(ROOT), "-DBUILD_VERSION=" + version,
                        "-P", str(GENERATOR)], cwd=self.build, env=self.environment,
                       check=True, capture_output=True, text=True)

    def test_identical_header_preserves_existing_timestamp_and_removes_temporary(self):
        self.generate("1.2.3.4")
        before = self.header.read_bytes()
        old = 1000000000
        os.utime(self.header, ns=(old, old))
        self.generate("1.2.3.4")
        self.assertEqual(self.header.read_bytes(), before)
        self.assertEqual(self.header.stat().st_mtime_ns, old)
        self.assertEqual(list((self.build / "generated").glob("version.*.tmp")), [])

    @unittest.skipUnless(os.name == "posix" and shutil.which("ninja"),
                         "native dependent-target fixture requires POSIX Ninja")
    def test_changed_version_updates_header_and_rebuilds_dependent_target(self):
        self.generate("1.2.3.4")
        source = self.root / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\nproject(VersionFixture CXX)\n"
            "add_executable(app main.cpp)\n"
            "target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/generated)\n")
        (source / "main.cpp").write_text(
            '#include "version.h"\nint main() { return KLOGG_VERSION[0] == \'2\' ? 0 : 1; }\n')
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-G", "Ninja"],
                       check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(self.build)], check=True, capture_output=True)
        obj = self.build / "CMakeFiles/app.dir/main.cpp.o"
        original_stamp = obj.stat().st_mtime_ns
        os.utime(self.header, ns=(1000000000, 1000000000))
        self.generate("1.2.3.4")
        subprocess.run(["cmake", "--build", str(self.build)], check=True, capture_output=True)
        self.assertEqual(obj.stat().st_mtime_ns, original_stamp)
        self.generate("2.0.0.0")
        self.assertIn('#define KLOGG_VERSION "2.0.0.0"', self.header.read_text())
        subprocess.run(["cmake", "--build", str(self.build)], check=True, capture_output=True)
        subprocess.run([str(self.build / "app")], check=True)
        self.assertEqual(list((self.build / "generated").glob("version.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
