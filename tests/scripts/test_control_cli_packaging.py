"""Control CLI distribution contracts without Qt builds or user installation."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "src/app/CMakeLists.txt"


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def cli_install_rules():
    text = APP.read_text(encoding="utf-8")
    return text[text.index("install(\n  TARGETS klogg"):text.index("if(WIN32)\n  if(TARGET tbb)")]


def action_run(name):
    text = read(".github/actions/agent-package-mac/action.yml")
    step = text.split("    - name: " + name + "\n", 1)[1].split("    - name:", 1)[0]
    return "\n".join(line[8:] for line in step.split("      run: |\n", 1)[1].splitlines())


class ControlCliPackagingTest(unittest.TestCase):
    def configure_install_fixture(self, source, build, *options):
        """Apply the product rules to real targets without building Qt or cross-compiling."""
        source.mkdir(exist_ok=True)
        (source / "dummy.c").write_text("/* Not compiled. */\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(ControlInstall NONE)\n"
            "set(APPLE FALSE)\nset(CMAKE_SKIP_RPATH TRUE)\n"
            "set(CMAKE_C_LINK_EXECUTABLE unused)\n"
            "if(DEFINED PROBE_SYSTEM)\n  set(CMAKE_SYSTEM_NAME \"${PROBE_SYSTEM}\")\n"
            "  set(CMAKE_INSTALL_PREFIX \"${PROBE_PREFIX}\")\nendif()\n"
            "set(KLOGG_RUNTIME_INSTALL_COMPONENT klogg-runtime)\n"
            "foreach(target klogg kloggctl)\n"
            "  add_executable(${target} dummy.c)\n"
            "  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)\n"
            "endforeach()\n" + cli_install_rules() +
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/cli-rpath" CONTENT "$<TARGET_PROPERTY:kloggctl,INSTALL_RPATH>")\n',
            encoding="utf-8")
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build), *options],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipIf(os.name == "nt", "DESTDIR contract requires POSIX installation paths")
    def test_runtime_component_installs_cli_beside_gui_with_destdir_and_custom_bindir(self):
        """Execute the real install declarations using inert prebuilt fixtures."""
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("requires CMake")
        with tempfile.TemporaryDirectory(prefix="control package ") as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            self.configure_install_fixture(source, build, "-DCMAKE_INSTALL_BINDIR=tools")
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

    def test_threaded_control_transport_target_declares_its_link_dependency(self):
        # On glibc before 2.34 the transport test's std::thread::join needs a
        # separate pthread library; newer systems can hide the missing dependency.
        from test_xcode_generator_contract import cmake_command_tokens
        unit_cmake = read("tests/unit/CMakeLists.txt")
        dependencies = [lib for target, *libs in cmake_command_tokens(unit_cmake, "target_link_libraries")
                        if target == "klogg_control_tests" for lib in libs]
        self.assertIn("Threads::Threads", dependencies)

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

    def test_cli_uses_relocatable_install_rpath_only_for_linux_usr_bin(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cases = (("Linux", "/usr", "bin", "$ORIGIN/../lib"), ("Darwin", "/usr", "bin", ""),
                     ("Linux", "/opt", "bin", ""), ("Linux", "/usr", "tools", ""))
            for index, (system, prefix, bindir, expected) in enumerate(cases):
                with self.subTest(system=system, prefix=prefix, bindir=bindir):
                    build = root / ("build" + str(index))
                    self.configure_install_fixture(root / "source", build, "-DPROBE_SYSTEM=" + system,
                                                   "-DPROBE_PREFIX:STRING=" + prefix,
                                                   "-DCMAKE_INSTALL_BINDIR=" + bindir)
                    self.assertEqual((build / "cli-rpath").read_text(encoding="utf-8"), expected)

    def test_linux_consumer_checks_the_final_appimage_before_upload(self):
        from test_ci_environment_workflow import QUALITY
        steps = QUALITY.workflow_job_steps(read(".github/workflows/ci-build.yml"))["LinuxPackages"]
        rows = [QUALITY.workflow_step_fields(step) for step in steps]
        package = next(i for i, (f, _) in enumerate(rows) if f.get("uses") == "./.github/actions/docker-package")
        check, = [i for i, (f, _) in enumerate(rows) if f.get("name") == "Verify packaged AppImage control CLI"]
        upload = next(i for i, (_, c) in enumerate(rows) if c.get("with", {}).get("name", "").startswith("packages-"))
        self.assertLess(package, check)
        self.assertLess(check, upload)
        fields, _ = rows[check]
        self.assertEqual(fields["if"], "${{ env.KLOGG_PACKAGE_ENABLED == 'true' && "
                                       "env.KLOGG_CONFIG_OS == 'ubuntu_appimage' }}")
        self.assertNotIn("continue-on-error", fields)
        args = shlex.split(fields["run"].replace("\\\n", ""), comments=True)
        self.assertEqual(args[:2], ["docker", "run"])
        mounts = [args[i + 1] for i, arg in enumerate(args) if arg == "-v"]
        self.assertCountEqual(mounts, ["$KLOGG_WORKSPACE/$KLOGG_BUILD_ROOT/packages:/packages:ro",
                                       "$KLOGG_WORKSPACE/scripts/verify_appimage_cli.py:/verify_appimage_cli.py:ro"])
        self.assertEqual(args[-5:], ["${{ env.KLOGG_CI_IMAGE_PREFIX }}${{ env.KLOGG_CONTAINER_SUFFIX }}",
                                     "python3", "-I", "/verify_appimage_cli.py",
                                     "/packages/klogg-${KLOGG_VERSION}-appimage-${KLOGG_PACKAGE_TAG}.AppImage"])

    @unittest.skipIf(os.name == "nt", "Linux AppImage uses POSIX executables")
    def test_appimage_smoke_requires_extracted_cli_and_bundled_qt(self):
        with tempfile.TemporaryDirectory(prefix="control smoke ") as directory:
            root = Path(directory)
            def executable(name, shell):
                path = root / name
                path.write_text("#!/bin/sh\nset -eu\n" + shell, encoding="utf-8")
                path.chmod(0o755)
                return path

            package = executable("shipping.AppImage", '''
[ "$1" = --appimage-extract ]
mkdir -p squashfs-root/usr/bin squashfs-root/usr/lib
cp "$(dirname "$0")/case" squashfs-root/case
if [ "$(cat squashfs-root/case)" != missing-cli ]; then
    cp "$(dirname "$0")/control" squashfs-root/usr/bin/kloggctl
fi
touch squashfs-root/usr/lib/libQt5Core.so.5 squashfs-root/usr/lib/libQt5Network.so.5
''')
            executable("control", '''
[ -z "${LD_LIBRARY_PATH:-}" ]
[ "$1" != "$(cat squashfs-root/case)" ]
case "$1" in
    --version) echo '{"result":{"version":"1"}}';;
    --help) echo 'Usage: kloggctl COMMAND';;
    *) exit 1;;
esac
''')
            executable("ldd", '''
[ -z "${LD_LIBRARY_PATH:-}" ]
lib=$(dirname "$1")/../lib
core="$lib/libQt5Core.so.5"
mode=$(cat squashfs-root/case)
case "$mode" in
    host-qt) core="$(dirname "$0")/control";;
    symlink-escape) ln -sf "$(dirname "$0")/control" "$core";;
esac
echo "libQt5Core.so.5 => $core (0x01)"
if [ "$mode" != missing-network ]; then echo "libQt5Network.so.5 => $lib/libQt5Network.so.5 (0x02)"; fi
''')
            environment = dict(os.environ, PATH=str(root) + os.pathsep + os.defpath, LD_LIBRARY_PATH="/host/qt")
            for mode in ("bundled", "host-qt", "symlink-escape", "missing-network", "missing-cli", "--help", "--version"):
                with self.subTest(mode=mode):
                    (root / "case").write_text(mode, encoding="utf-8")
                    result = subprocess.run([sys.executable, "-I", str(ROOT / "scripts/verify_appimage_cli.py"),
                                             str(package)], env=environment, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode == 0, mode == "bundled", result.stderr)


if __name__ == "__main__":
    unittest.main()
