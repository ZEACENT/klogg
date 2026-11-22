"""Control CLI distribution contracts without Qt builds or user installation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "src/app/CMakeLists.txt"


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def action_run(name):
    text = read(".github/actions/agent-package-mac/action.yml")
    step = text.split("    - name: " + name + "\n", 1)[1].split("    - name:", 1)[0]
    return "\n".join(line[8:] for line in step.split("      run: |\n", 1)[1].splitlines())


class ControlCliPackagingTest(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "DESTDIR contract requires POSIX installation paths")
    def test_runtime_component_installs_cli_beside_gui_with_destdir_and_custom_bindir(self):
        """Execute the real install declarations using inert prebuilt fixtures."""
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("requires CMake")
        text = APP.read_text(encoding="utf-8")
        install = text[text.index("install(\n  TARGETS klogg"):text.index("if(WIN32)\n  if(TARGET tbb)")]
        with tempfile.TemporaryDirectory(prefix="control package ") as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "dummy.c").write_text("/* Not compiled. */\n", encoding="utf-8")
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\nproject(ControlInstall NONE)\n"
                "set(APPLE FALSE)\nset(WIN32 FALSE)\nset(CMAKE_SKIP_RPATH TRUE)\n"
                "set(CMAKE_C_LINK_EXECUTABLE unused)\n"
                "set(CMAKE_INSTALL_BINDIR tools)\n"
                "set(KLOGG_RUNTIME_INSTALL_COMPONENT klogg-runtime)\n"
                "foreach(target klogg kloggctl)\n"
                "  add_executable(${target} dummy.c)\n"
                "  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)\n"
                "endforeach()\n" + install, encoding="utf-8")
            build = root / "build"
            result = subprocess.run([cmake, "-S", str(source), "-B", str(build)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ("klogg", "kloggctl"):
                (build / name).write_bytes(name.encode("ascii"))
                (build / name).chmod(0o755)
            dest = root / "destdir"
            environment = dict(os.environ, DESTDIR=str(dest))
            result = subprocess.run([cmake, "--install", str(build), "--prefix", "/relocated",
                                     "--component", "klogg-runtime"], env=environment,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            sibling = dest / "relocated/tools"
            self.assertTrue((sibling / "klogg").is_file())
            self.assertTrue((sibling / "kloggctl").is_file(), "runtime component omitted kloggctl")
            self.assertEqual((sibling / "kloggctl").read_bytes(), b"kloggctl")
            self.assertTrue(os.access(sibling / "kloggctl", os.X_OK))
            self.assertFalse((dest / "relocated/bin").exists())

    def test_gui_only_builds_also_build_the_cli_on_non_apple_platforms(self):
        text = APP.read_text(encoding="utf-8")
        # Arch's packaging recipe explicitly builds only klogg before install.
        block = text[text.index("if(APPLE)\n  foreach(target IN ITEMS klogg klogg_portable)"):
                     text.index("add_dependencies(klogg klogg_adb_helper)")]
        self.assertRegex(block, r"else\(\)\s*foreach\(target IN ITEMS klogg klogg_portable\)\s*"
                               r"add_dependencies\(\$\{target\} kloggctl\)")

    @unittest.skipIf(os.name == "nt", "macOS packaging invokes a POSIX shell")
    def test_macos_deploy_tool_receives_the_bundled_console_executable(self):
        with tempfile.TemporaryDirectory(prefix="control deploy ") as directory:
            root = Path(directory)
            tool = root / "Qt/bin/macdeployqt"
            tool.parent.mkdir(parents=True)
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGUMENTS"\n', encoding="utf-8")
            tool.chmod(0o755)
            app = root / "relocated app/klogg.app"
            cli = app / "Contents/MacOS/kloggctl"
            cli.parent.mkdir(parents=True)
            cli.write_text("fixture", encoding="utf-8")
            cli.chmod(0o755)
            arguments = root / "arguments"
            environment = dict(os.environ, KLOGG_QT_DIR=str(tool.parent.parent),
                               KLOGG_MAC_APP=str(app), ARGUMENTS=str(arguments))
            result = subprocess.run(["bash", "-c", action_run("Mac deploy Qt")],
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("-executable=" + str(cli), arguments.read_text().splitlines(),
                          "macdeployqt does not automatically deploy extra console executables")
            cli.unlink()
            arguments.unlink()
            result = subprocess.run(["bash", "-c", action_run("Mac deploy Qt")],
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, "missing CLI must fail before packaging")
            self.assertFalse(arguments.exists())

    def test_windows_installer_and_portable_tree_include_cli_with_shared_runtime(self):
        prepare = read("packaging/windows/prepare_release.cmd")
        installer = read("packaging/windows/klogg.nsi")
        self.assertRegex(prepare, r'xcopy "[^"\n]*output\\kloggctl\.exe" "[^"\n]*release\\" /y')
        self.assertIn('if not exist "%KLOGG_WORKSPACE%\\release\\kloggctl.exe"', prepare)
        self.assertRegex(installer, r"SetOutPath \$INSTDIR\s+File release\\klogg.exe\s+File release\\kloggctl.exe")
        self.assertIn('Delete "$INSTDIR\\kloggctl.exe"', installer)
        archive = next(line for line in prepare.splitlines() if line.startswith("7z a "))
        self.assertIn(r".\*", archive)
        self.assertNotIn("-x!kloggctl", archive)
        for module in ("Core", "Network"):
            self.assertIn("%KLOGG_QT%" + module + ".dll", prepare)
            self.assertIn("File release\\${QT_MAJOR}" + module + ".dll", installer)

    def test_appimage_deploys_cli_dependencies_and_runs_final_cli_directly(self):
        text = read("packaging/linux/appimage/generate_appimage.sh")
        deploys = [line for line in text.splitlines()
                   if line.startswith("VERSION=") and "linuxdeployqt" in line]
        self.assertEqual(len(deploys), 2)
        for line in deploys:
            self.assertIn("-executable=appdir/usr/bin/kloggctl", line)
        final = text.split('"$package" --appimage-extract', 1)[1]
        self.assertRegex(final, r"(?m)^\./squashfs-root/usr/bin/kloggctl --version$")
        self.assertRegex(final, r"(?m)^\./squashfs-root/usr/bin/kloggctl --help")


if __name__ == "__main__":
    unittest.main()
