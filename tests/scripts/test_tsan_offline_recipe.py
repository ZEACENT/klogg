"""TSan online/locked stage selection and fail-closed offline input contracts."""
from __future__ import annotations

import os
import pathlib
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[2]
DOCKERFILE = ROOT / "docker" / "ubuntu22.04-tsan" / "Dockerfile"
SH = shutil.which("sh")
INSTALLER = "/opt/klogg-ci/ci_install_locked_apt.sh"
BUNDLES = {
    "locked-bootstrap": ("bootstrap", "APT_BOOTSTRAP_LOCK_SHA256"),
    "locked-qt-source": ("source", "APT_SOURCE_LOCK_SHA256"),
    "locked-qt-deps": ("qt-builder", "APT_BUILDER_LOCK_SHA256"),
    "locked-runtime": ("runtime", "APT_RUNTIME_LOCK_SHA256"),
}


def parse_recipe(text):
    active = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    instructions = [line.strip() for line in active.replace("\\\n", " ").splitlines() if line.strip()]
    arguments = {}
    stages = {}
    current = None
    for instruction in instructions:
        if instruction.startswith("FROM "):
            fields = shlex.split(instruction)
            name = fields[3] if len(fields) == 4 and fields[2] == "AS" else "stage-" + str(len(stages))
            if name in stages:
                raise ValueError("duplicate Docker stage")
            current = {"base": fields[1], "instructions": []}
            stages[name] = current
        elif current is None:
            if instruction.startswith("ARG "):
                name, separator, value = instruction[4:].partition("=")
                if separator:
                    arguments[name] = value
        else:
            current["instructions"].append(instruction)
    return arguments, stages


def reachable(stages, target, mode):
    visited = set()
    visiting = set()

    def visit(name):
        name = name.replace("${KLOGG_APT_STAGE}", mode)
        if name == "${UBUNTU_IMAGE}":
            return
        if name not in stages or name in visiting:
            raise ValueError("unknown or cyclic stage: " + name)
        if name in visited:
            return
        visiting.add(name)
        stage = stages[name]
        visit(stage["base"])
        for instruction in stage["instructions"]:
            if instruction.startswith("COPY "):
                match = re.search(r"(?:^|\s)--from=(\S+)", instruction)
                if match:
                    visit(match.group(1))
        visiting.remove(name)
        visited.add(name)

    visit(target)
    return visited


def shell_body(instruction):
    return re.sub(r"^RUN(?:\s+--\S+)*\s+", "", instruction)


class TsanOfflineRecipeTest(unittest.TestCase):
    def assert_locked_contract(self, text):
        arguments, stages = parse_recipe(text)
        self.assertEqual(arguments.get("KLOGG_APT_STAGE"), "online")
        self.assertEqual(arguments["UBUNTU_IMAGE"],
                         "public.ecr.aws/ubuntu/ubuntu:22.04@sha256:"
                         "0199853f6d6b20b0424f3c5694a72a62764f01e6a771b1eb48a4197848986c7e")
        selected = reachable(stages, "build-environment", "locked")
        self.assertTrue(set(BUNDLES) <= selected)
        self.assertFalse(any(name.startswith("online-") for name in selected))
        for name in selected:
            for instruction in stages[name]["instructions"]:
                if instruction.startswith("RUN "):
                    self.assertTrue(instruction.startswith("RUN --network=none "), (name, instruction))
                    self.assertNotRegex(instruction, r"\b(?:apt-get|curl|wget|apt_snapshot_retry\.sh)\b")
        for name, (bundle, variable) in BUNDLES.items():
            instructions = stages[name]["instructions"]
            self.assertIn("ARG " + variable, instructions)
            self.assertIn("COPY inputs/" + bundle + "/ /opt/klogg-ci-inputs/" + bundle + "/", instructions)
            installs = [item for item in instructions if item.startswith("RUN ") and INSTALLER in item]
            self.assertEqual(len(installs), 1)
            self.assertIn("sh " + INSTALLER + " /opt/klogg-ci-inputs/" + bundle, installs[0])
            self.assertRegex(installs[0], r"\$\{" + variable + r":\?[^}]+\}")
            self.assertNotIn("||", installs[0], "locked installation cannot fall back")
            ancestors = reachable(stages, name, "locked")
            self.assertEqual(ancestors & set(BUNDLES), {name, "locked-bootstrap"})
        self.assertIn("COPY ci_install_locked_apt.sh " + INSTALLER,
                      stages["locked-bootstrap"]["instructions"])
        return stages

    def test_locked_graph_prunes_all_network_acquisition_and_keeps_parallel_bundles(self):
        self.assert_locked_contract(DOCKERFILE.read_text())

    def test_online_default_prunes_missing_offline_inputs_and_keeps_original_fetches(self):
        _, stages = parse_recipe(DOCKERFILE.read_text())
        selected = reachable(stages, "build-environment", "online")
        self.assertFalse(any(name.startswith("locked-") for name in selected))
        active = "\n".join(instruction for name in selected for instruction in stages[name]["instructions"])
        self.assertNotIn("COPY inputs/", active)
        self.assertNotRegex(active, r"(?m)^COPY\s+(?:--\S+\s+)*ci_install_locked_apt\.sh\b")
        self.assertNotIn("sh /opt/klogg-ci/ci_install_locked_apt.sh", active)
        self.assertIn("apt_snapshot_retry.sh ca-certificates", active)
        self.assertIn("curl --fail --location", active)
        self.assertIn("wget --tries=3 --timeout=30", active)
        self.assertIn("Acquire::https::snapshot.ubuntu.com::Verify-Peer", active)

    def test_archive_copies_are_explicit_and_qt_compile_patch_and_checks_are_shared(self):
        text = DOCKERFILE.read_text()
        stages = self.assert_locked_contract(text)
        copies = [line for line in stages["locked-qt-source"]["instructions"] if line.startswith("COPY inputs/qt/")]
        self.assertEqual(len(copies), 4)
        for module in ("qtbase", "qtsvg", "qttools", "qttranslations"):
            self.assertIn("COPY inputs/qt/" + module
                          + "-everywhere-opensource-src-${QT_VERSION}.tar.xz /tmp/qt-src/", copies)
        self.assertIn("COPY inputs/cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh",
                      stages["locked-runtime"]["instructions"])
        self.assertEqual(text.count("./configure"), 1)
        self.assertEqual(text.count("patch --batch --forward --fuzz=0"), 1)
        self.assertEqual(text.count("-release -shared -sanitize thread"), 1)
        self.assertEqual(text.count('for tool in lconvert lrelease lupdate; do'), 1)
        self.assertEqual(text.count('Qt artifact is not compiler-instrumented for TSan:'), 1)
        self.assertEqual(text.count('/usr/local/bin/verify_elf_runtime_closure.sh "${QT_TSAN_PREFIX}"'), 1)
        self.assertEqual(text.count("sha256sum --check --strict"), 2)
        self.assertEqual(text.count("clang++-14 -fsanitize=thread /tmp/tsan-probe.cpp"), 2)

    def test_final_image_contains_compressed_sources_patch_instructions_and_build_provenance(self):
        text = DOCKERFILE.read_text()
        _, stages = parse_recipe(text)
        source_root = "/usr/share/klogg-ci/qt-sources"
        self.assertIn("COPY --from=qt-source " + source_root + "/ " + source_root + "/",
                      stages["build-environment"]["instructions"])
        source = "\n".join(stages["qt-source"]["instructions"])
        self.assertIn("mv -- *.tar.xz " + source_root + "/archives/", source)
        self.assertNotIn("rm -- *.tar.xz", source)
        self.assertIn("COPY Dockerfile " + source_root + "/recipe/Dockerfile", source)
        self.assertIn("COPY README.md " + source_root + "/README.md", source)
        self.assertIn("COPY apt_snapshot_retry.sh verify_elf_runtime_closure.sh " + source_root + "/recipe/", source)
        self.assertIn("mv /tmp/fix_qt5_qobject_tsan_publication.patch " + source_root + "/patches/", source)
        self.assertIn("LICENSE", source)
        self.assertIn("if [ -f /opt/klogg-ci/ci_install_locked_apt.sh ]", source)
        self.assertNotIn("COPY ci_install_locked_apt.sh", source,
                         "legacy online context must not require an unstaged offline helper")
        self.assertIn("COPY --from=qtbase-builder /usr/share/klogg-ci/qt-builder-provenance/ " + source_root + "/apt/",
                      stages["build-environment"]["instructions"])
        for mode in ("online", "locked"):
            selected = reachable(stages, "build-environment", mode)
            self.assertIn("qt-source", selected)
            self.assertIn("qtbase-builder", selected)

    @unittest.skipUnless(SH, "requires a POSIX shell")
    def test_source_capture_copies_original_licenses_without_fabricating_online_provenance(self):
        _, stages = parse_recipe(DOCKERFILE.read_text())
        instructions = stages["qt-source"]["instructions"]
        capture = next(line for line in instructions if line.startswith("RUN ") and "/LICENSE*" in line)
        patch = next(line for line in instructions if line.startswith("RUN ") and "patch --batch" in line)
        self.assertLess(instructions.index(capture), instructions.index(patch),
                        "preserve original license files before applying repository modifications")
        for locked, missing in ((False, False), (True, False), (False, True)):
            with self.subTest(locked=locked, missing=missing):
                with tempfile.TemporaryDirectory(prefix="qt-license-capture-") as directory:
                    root = pathlib.Path(directory)
                    image = root / "image"
                    payload = image / "qt-sources"
                    (payload / "recipe").mkdir(parents=True)
                    original = b"Synthetic original license bytes\n"
                    for module in ("qtbase", "qtsvg", "qttools", "qttranslations"):
                        source = root / (module + "-everywhere-src-5.15.19")
                        source.mkdir()
                        if not (missing and module == "qttools"):
                            (source / "LICENSE.FIXTURE").write_bytes(original)
                        (source / "LICENSE.SYMLINK").symlink_to(root / "external-license")
                    (root / "external-license").write_bytes(b"not an original regular license file")
                    helper = root / "ci_install_locked_apt.sh"
                    if locked:
                        helper.write_bytes(b"actual helper fixture bytes\n")
                        for name in ("bootstrap-inputs.json", "source-inputs.json"):
                            (image / name).write_bytes(b"actual manifest fixture bytes\n")
                    command = shell_body(capture).replace("/usr/share/klogg-ci", str(image))
                    command = command.replace("/opt/klogg-ci/ci_install_locked_apt.sh", str(helper))
                    result = subprocess.run([SH, "-c", command], cwd=root,
                                            env=dict(os.environ, QT_VERSION="5.15.19"),
                                            capture_output=True, text=True, timeout=10)
                    if missing:
                        self.assertNotEqual(result.returncode, 0)
                        continue
                    self.assertEqual(result.returncode, 0, result.stderr)
                    for module in ("qtbase", "qtsvg", "qttools", "qttranslations"):
                        self.assertEqual((payload / "licenses" / module / "LICENSE.FIXTURE").read_bytes(), original)
                        self.assertFalse((payload / "licenses" / module / "LICENSE.SYMLINK").exists())
                    self.assertEqual((payload / "recipe" / "ci_install_locked_apt.sh").exists(), locked)
                    self.assertEqual(len(list((payload / "apt").iterdir())), 2 if locked else 0)

    def test_mutations_cannot_reconnect_online_stages_remove_locks_or_enable_network(self):
        text = DOCKERFILE.read_text()
        mutations = (
            text.replace("FROM ${KLOGG_APT_STAGE}-bootstrap AS jammy-snapshot", "FROM online-bootstrap AS jammy-snapshot"),
            text.replace("FROM ${KLOGG_APT_STAGE}-qt-source AS qt-source", "FROM online-qt-source AS qt-source"),
            text.replace("RUN --network=none set -eu;", "RUN set -eu;", 1),
            text.replace("COPY inputs/runtime/ /opt/klogg-ci-inputs/runtime/", "# absent runtime input"),
            re.sub(r"\$\{APT_SOURCE_LOCK_SHA256:\?[^}]+\}", "${APT_SOURCE_LOCK_SHA256}", text),
            text + "\nRUN curl https://example.invalid/fallback\n",
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation[-100:]):
                self.assertNotEqual(mutation, text)
                with self.assertRaises((AssertionError, ValueError)):
                    self.assert_locked_contract(mutation)

    @unittest.skipUnless(SH, "requires a POSIX shell")
    def test_each_locked_install_fails_on_missing_lock_missing_bundle_and_helper_failure(self):
        _, stages = parse_recipe(DOCKERFILE.read_text())
        for stage, (bundle, variable) in BUNDLES.items():
            with self.subTest(stage=stage):
                install = next(line for line in stages[stage]["instructions"] if line.startswith("RUN ") and INSTALLER in line)
                for lock, present, helper_status in ((None, True, 0), ("", True, 0), ("a" * 64, False, 0),
                                                      ("a" * 64, True, 73), ("a" * 64, True, 0)):
                    with self.subTest(lock=lock, present=present, helper_status=helper_status):
                        with tempfile.TemporaryDirectory(prefix="tsan-locked-") as directory:
                            root = pathlib.Path(directory)
                            fake_bin = root / "bin"
                            fake_bin.mkdir()
                            log = root / "calls"
                            stub = fake_bin / "sh"
                            stub.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$FAKE_LOG"\n'
                                            '[ -f "$2/manifest.json" ] || exit 42\nexit "$FAKE_HELPER_STATUS"\n')
                            stub.chmod(0o755)
                            target = root / "inputs" / bundle
                            if present:
                                target.mkdir(parents=True)
                                (target / "manifest.json").write_text("{}\n")
                            command = shell_body(install).replace("/opt/klogg-ci-inputs/", str(root / "inputs") + "/")
                            command = command.replace("/opt/klogg-ci/", str(root / "tools") + "/")
                            command = command.replace("/usr/share/klogg-ci", str(root / "metadata"))
                            env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                                       FAKE_LOG=str(log), FAKE_HELPER_STATUS=str(helper_status))
                            env.pop(variable, None)
                            if lock is not None:
                                env[variable] = lock
                            result = subprocess.run([SH, "-c", command], env=env, cwd=root,
                                                    capture_output=True, text=True, timeout=10)
                            if not lock:
                                self.assertNotEqual(result.returncode, 0)
                                self.assertFalse(log.exists(), "missing lock reached installer")
                            else:
                                self.assertEqual(result.returncode, helper_status if present else 42, result.stderr)

    @unittest.skipUnless(SH and shutil.which("sha256sum"), "requires POSIX shell and SHA-256 utility")
    def test_bad_qt_archives_and_cmake_installer_are_rejected_before_execution(self):
        _, stages = parse_recipe(DOCKERFILE.read_text())
        arguments = {}
        for stage in stages.values():
            for instruction in stage["instructions"]:
                if instruction.startswith("ARG ") and "=" in instruction:
                    name, value = instruction[4:].split("=", 1)
                    arguments[name] = value
        for name in ("qt-source", "build-environment"):
            with self.subTest(stage=name):
                instruction = next(line for line in stages[name]["instructions"] if line.startswith("RUN ") and "sha256sum --check --strict" in line)
                with tempfile.TemporaryDirectory(prefix="tsan-hashes-") as directory:
                    root = pathlib.Path(directory)
                    fake_bin = root / "bin"
                    fake_bin.mkdir()
                    log = root / "executed"
                    stub = fake_bin / "tar"
                    stub.write_text('#!/bin/sh\nprintf "extracted\\n" >> "$FAKE_LOG"\n')
                    stub.chmod(0o755)
                    for module in ("qtbase", "qtsvg", "qttools", "qttranslations"):
                        (root / (module + "-everywhere-opensource-src-5.15.19.tar.xz")).write_bytes(b"tampered archive")
                    installer = root / "cmake-installer.sh"
                    installer.write_text('#!/bin/sh\nprintf "executed\\n" >> "$FAKE_LOG"\n')
                    command = shell_body(instruction).replace("/tmp/cmake-installer.sh", str(installer))
                    command = command.replace("/usr/local/bin/verify_elf_runtime_closure.sh", str(root / "unreachable-verifier"))
                    command = command.replace("/opt/qt5-tsan", str(root / "qt"))
                    env = dict(os.environ, **arguments)
                    env.update(PATH=str(fake_bin) + os.pathsep + os.environ["PATH"], FAKE_LOG=str(log), QT_TSAN_PREFIX=str(root / "qt"))
                    result = subprocess.run([SH, "-c", command], cwd=root, env=env, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0, result.stderr)
                    self.assertFalse(log.exists(), "unverified input was executed or extracted")


if __name__ == "__main__":
    unittest.main()
