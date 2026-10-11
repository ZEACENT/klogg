import concurrent.futures
import hashlib
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PREPARE_SCRIPT = ROOT / "cmake" / "prepare_efsw_source.cmake"
APPLY_SERIES_SCRIPT = ROOT / "cmake" / "apply_git_patch_series.cmake"


def source_tree_digest(source: pathlib.Path) -> str:
    files = [
        path for path in source.rglob("*") if path.is_file() and ".git" not in path.parts
    ]
    manifest = ""
    for path in sorted(files, key=lambda item: item.relative_to(source).as_posix()):
        relative = path.relative_to(source).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest += f"{relative}:{digest}\n"
    return hashlib.sha256(manifest.encode()).hexdigest()


def directory_bytes(source: pathlib.Path) -> dict:
    return {
        path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in source.rglob("*")
        if path.is_file()
    }


def directory_ownership(source: pathlib.Path) -> dict:
    return {
        path.relative_to(source).as_posix(): (path.lstat().st_uid, path.lstat().st_gid)
        for path in [source] + list(source.rglob("*"))
    }


class PrepareEfswSourceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        # Do not inherit command-scope trust, repository redirects, or Git's
        # ownership-test flags from the caller. No user config is modified.
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith("GIT_")
        }
        self.global_config = self.root / "global-gitconfig"
        self.global_config.write_bytes(b"")
        self.environment.update(
            {
                "GIT_CONFIG_GLOBAL": str(self.global_config),
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(self.root / "isolated-home"),
            }
        )
        self.git = [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "commit.gpgsign=false",
        ]
        self.cache = self.root / "shared-cache"
        subprocess.run(
            self.git + ["init", "-q", "-b", "fixture-pinned", str(self.cache)],
            check=True,
            env=self.environment,
        )
        (self.cache / "value.txt").write_text("clean source\n")
        subprocess.run(
            self.git + ["add", "value.txt"],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        subprocess.run(
            self.git
            + [
                "-c",
                "user.name=Klogg Test",
                "-c",
                "user.email=klogg@example.invalid",
                "commit",
                "-q",
                "-m",
                "fixture pinned source",
            ],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        self.revision = self.git_output("rev-parse", "HEAD")
        self.clean_digest = source_tree_digest(self.cache)
        self.patch = self.root / "already-applied.patch"
        self.patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-clean source\n"
            "+approved patched cache\n"
        )
        subprocess.run(
            self.git + ["apply", str(self.patch)],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        self.patched_digest = source_tree_digest(self.cache)
        subprocess.run(
            self.git + ["reset", "--hard", "-q", "HEAD"],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )

    def tearDown(self):
        self.temporary.cleanup()

    def git_output(self, *arguments, cwd=None):
        return subprocess.check_output(
            self.git + list(arguments),
            cwd=cwd or self.cache,
            text=True,
            env=self.environment,
        ).strip()

    def prepare(
        self,
        destination,
        cache=None,
        revision=None,
        clean_digest=None,
        approved_cache_digests=None,
        timeout=10,
        script=PREPARE_SCRIPT,
        cwd=None,
    ):
        cache = cache or self.cache
        revision = revision or self.revision
        clean_digest = clean_digest or self.clean_digest
        approved_cache_digests = approved_cache_digests or [clean_digest]
        command = [
            "cmake",
            "-DDEPENDENCY=efsw",
            f"-DCACHE_SOURCE={cache}",
            f"-DDESTINATION={destination}",
            f"-DEXPECTED_REVISION={revision}",
            f"-DCLEAN_TREE_HASH={clean_digest}",
            "-DAPPROVED_CACHE_TREE_HASHES="
            + ";".join(approved_cache_digests),
            "-DLOCK_TIMEOUT=2",
            "-DGIT_TIMEOUT=2",
            "-P",
            str(script),
        ]
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
            timeout=timeout,
            cwd=cwd,
        )

    def enable_different_owner_clone(
        self, scrub_upload_pack=False, fail_config_write=0, fail_clone=False,
        timeout_address=None,
    ):
        # POSIX executable shim: only the local clone and its transport child
        # see the ownership flag. Ordinary forwarding replaces the shim process.
        real_git = shutil.which("git", path=self.environment["PATH"])
        self.assertIsNotNone(real_git)
        shim_directory = self.root / "git-shim"
        shim_directory.mkdir()
        self.git_log = self.root / "git-log.jsonl"
        self.git_log.write_text("")
        common = (
            f"#!{sys.executable}\n"
            "import json, os, pathlib, shlex, socket, subprocess, sys\n"
            f"real_git = {real_git!r}\n"
            f"log_path = {str(self.git_log)!r}\n"
        ) + textwrap.dedent("""\
            def record(kind, arguments, environment, advertisement=None):
                item = {
                    'kind': kind, 'arguments': arguments,
                    'global_config': environment.get('GIT_CONFIG_GLOBAL'),
                    'parameters': environment.get('GIT_CONFIG_PARAMETERS'),
                    'count': environment.get('GIT_CONFIG_COUNT'),
                    'git_dir': environment.get('GIT_DIR'),
                    'git_work_tree': environment.get('GIT_WORK_TREE'),
                    'git_common_dir': environment.get('GIT_COMMON_DIR'),
                    'ceiling': environment.get('GIT_CEILING_DIRECTORIES'),
                    'working_directory': str(pathlib.Path.cwd()),
                    'different_owner': environment.get('GIT_TEST_ASSUME_DIFFERENT_OWNER') == '1',
                }
                if advertisement is not None:
                    item['advertisement'] = advertisement.decode(errors='replace')
                if kind == 'other' and 'checkout' in arguments:
                    with open(log_path) as log:
                        previous = [json.loads(line) for line in log]
                    item['clone_context_exists'] = any(
                        pathlib.Path(call['global_config']).parent.exists()
                        for call in previous if call['kind'] == 'clone'
                    )
                if kind in ('clone', 'upload-pack'):
                    config = pathlib.Path(environment['GIT_CONFIG_GLOBAL'])
                    item['regular_config'] = config.is_file() and not config.is_symlink()
                    item['config_context_symlink'] = config.parent.is_symlink()
                    item['config_context_owner'] = config.parent.stat().st_uid
                    item['config_owner'] = config.stat().st_uid if config.exists() else None
                    values = subprocess.check_output(
                        [real_git, 'config', '--file', str(config), '--null', '--get-all', 'safe.directory'],
                        env=environment,
                    ) if config.exists() and config.stat().st_size else b''
                    item['trust_entries'] = values.decode().split('\\0')[:-1]
                    item['config_keys'] = subprocess.check_output(
                        [real_git, 'config', '--file', str(config), '--name-only', '--list'],
                        env=environment,
                    ).decode().splitlines() if config.exists() else []
                    if kind == 'upload-pack':
                        effective = subprocess.run(
                            [real_git, 'config', '--null', '--get-all', 'safe.directory'],
                            capture_output=True, env=environment,
                        )
                        item['effective_trust'] = effective.stdout.decode().split('\\0')[:-1]
                with open(log_path, 'a') as log:
                    log.write(json.dumps(item) + '\\n')
        """)
        upload_pack = shim_directory / "upload-pack"
        upload_pack.write_text(common + textwrap.dedent("""\
            environment = os.environ.copy()
            # The real Git parent's command alias starts this actual child
            # AFTER reconstructing -c into command-scope environment. This
            # simulates transport scrubbing without changing local clone mode.
            record('upload-pack-inherited', sys.argv[1:], environment)
            environment.pop('GIT_CONFIG_PARAMETERS', None)
            environment.pop('GIT_CONFIG_COUNT', None)
            record('upload-pack', sys.argv[1:], environment)
            os.execvpe(real_git, [real_git, 'upload-pack'] + sys.argv[1:], environment)
        """))
        upload_pack.chmod(0o755)
        shim = shim_directory / "git"
        shim.write_text(common + (
            f"cache = pathlib.Path({str(self.cache.resolve())!r})\n"
            f"upload_pack = {str(upload_pack)!r}\n"
            f"scrub_upload_pack = {scrub_upload_pack!r}\n"
            f"fail_config_write = {fail_config_write!r}\n"
            f"fail_clone = {fail_clone!r}\n"
            f"timeout_address = {timeout_address!r}\n"
        ) + textwrap.dedent("""\
            arguments = sys.argv[1:]
            environment = os.environ.copy()
            environment.pop('GIT_TEST_ASSUME_DIFFERENT_OWNER', None)
            kind = 'other'
            if 'clone' in arguments:
                kind = 'clone'
                if pathlib.Path(arguments[-2]).resolve() == cache:
                    environment['GIT_TEST_ASSUME_DIFFERENT_OWNER'] = '1'
            elif '--file' in arguments and '--add' in arguments:
                kind = 'config-write'
            record(kind, arguments, environment)
            if kind == 'config-write' and fail_config_write:
                with open(log_path) as log:
                    writes = sum(json.loads(line)['kind'] == 'config-write' for line in log)
                if writes == fail_config_write:
                    config = pathlib.Path(arguments[arguments.index('--file') + 1])
                    pathlib.Path(str(config) + '.lock').write_text('owned failed write')
                    sys.exit('injected configuration write failure')
            if kind == 'clone':
                if fail_clone:
                    staging = pathlib.Path(arguments[-1])
                    staging.mkdir()
                    (staging / 'partial-clone').write_text('owned partial clone')
                    sys.exit('injected clone failure')
                if timeout_address:
                    with socket.create_connection(timeout_address, timeout=8) as connection:
                        connection.settimeout(None)
                        connection.recv(1)
                    sys.exit(0)
                if scrub_upload_pack:
                    prefix = arguments[:arguments.index('clone')]
                    alias = 'alias.fixture-upload-pack=!' + shlex.quote(upload_pack)
                    child = subprocess.run(
                        [real_git] + prefix + ['-c', alias, 'fixture-upload-pack', str(cache / '.git')],
                        input=b'0000', capture_output=True, env=environment,
                        cwd=cache.parent, timeout=8,
                    )
                    if child.returncode:
                        sys.stderr.buffer.write(child.stderr)
                        sys.exit(child.returncode)
                    record('upload-pack-result', [], environment, advertisement=child.stdout)
            os.execvpe(real_git, [real_git] + arguments, environment)
        """))
        shim.chmod(0o755)
        self.environment["PATH"] = str(shim_directory) + os.pathsep + self.environment["PATH"]

    def git_calls(self):
        return [json.loads(line) for line in self.git_log.read_text().splitlines()]

    def clone_calls(self):
        return [call for call in self.git_calls() if call["kind"] == "clone"]

    def assert_no_private_source(self, destination):
        self.assertFalse(destination.exists())
        self.assertFalse(self.private_marker(destination).exists())
        self.assertEqual(list(destination.parent.glob(f".{destination.name}.prepare-*")), [])
        self.assertEqual(list(destination.parent.glob(f".{destination.name}.clone-config-*")), [])

    def apply_series(self, destination, patch, patched_digest):
        return subprocess.run(
            [
                "cmake",
                "-DDEPENDENCY=efsw",
                f"-DREPO_DIR={destination}",
                f"-DEXPECTED_REVISION={self.revision}",
                f"-DCLEAN_TREE_HASH={self.clean_digest}",
                f"-DAPPROVED_TREE_HASHES={self.clean_digest};{patched_digest}",
                f"-DPATCHED_TREE_HASH={patched_digest}",
                f"-DPATCH_FILES={patch}",
                "-P",
                str(APPLY_SERIES_SCRIPT),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_different_owner_clone_rejects_untrusted_and_worktree_only_trust(self):
        self.enable_different_owner_clone()
        for trust_worktree in (False, True):
            with self.subTest(trust_worktree=trust_worktree):
                destination = self.root / f"negative-clone-{trust_worktree}"
                command = self.git[:]
                if trust_worktree:
                    command += ["-c", f"safe.directory={self.cache.resolve()}"]
                result = subprocess.run(
                    command + ["clone", "--no-hardlinks", "--no-checkout",
                               str(self.cache.resolve()), str(destination)],
                    capture_output=True,
                    text=True,
                    env=self.environment,
                )
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("dubious ownership", result.stderr)
                self.assertIn(str(self.cache.resolve() / ".git"), result.stderr)
        self.assertEqual(len(self.clone_calls()), 2)
        self.assertTrue(all(call["different_owner"] for call in self.clone_calls()))

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_prepares_and_patches_approved_cache_with_different_owner_clone(self):
        subprocess.run(
            self.git + ["apply", str(self.patch)], cwd=self.cache,
            check=True, env=self.environment,
        )
        # Simulate historical transport command-config loss in the actual
        # upload-pack child; this is not proof of an older Git runtime.
        self.enable_different_owner_clone(scrub_upload_pack=True)
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()
        cache_owner_before = directory_ownership(self.cache)
        patch_bytes_before = self.patch.read_bytes()
        global_bytes_before = self.global_config.read_bytes()
        destination = self.root / "private" / "efsw"
        # Exercise canonical source identity, not the spelling of a cache alias.
        cache_alias = self.root / "cache-alias"
        cache_alias.symlink_to(self.cache, target_is_directory=True)

        result = self.prepare(
            destination, cache=cache_alias,
            approved_cache_digests=[self.clean_digest, self.patched_digest],
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        calls = self.clone_calls()
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["different_owner"])
        children = [call for call in self.git_calls() if call["kind"] == "upload-pack"]
        self.assertEqual(len(children), 1)
        inherited = [call for call in self.git_calls() if call["kind"] == "upload-pack-inherited"]
        self.assertEqual(len(inherited), 1)
        self.assertIn("core.autocrlf", inherited[0]["parameters"])
        self.assertEqual(children[0]["effective_trust"], ["", str(self.cache.resolve() / ".git")])
        self.assertEqual(children[0]["trust_entries"], children[0]["effective_trust"])
        self.assertEqual(children[0]["global_config"], calls[0]["global_config"])
        self.assertIsNone(children[0]["parameters"])
        self.assertIsNone(children[0]["count"])
        self.assert_clone_context(calls[0], destination)
        child_results = [call for call in self.git_calls() if call["kind"] == "upload-pack-result"]
        self.assertEqual(len(child_results), 1)
        self.assertIn(self.revision, child_results[0]["advertisement"])
        checkouts = [call for call in self.git_calls() if "checkout" in call["arguments"]]
        self.assertEqual(len(checkouts), 1)
        self.assertFalse(checkouts[0]["clone_context_exists"])
        applied = self.apply_series(destination, self.patch, self.patched_digest)
        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        self.assertEqual(source_tree_digest(destination), self.patched_digest)
        self.assertEqual(self.git_output("rev-parse", "HEAD", cwd=destination), self.revision)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)
        self.assertEqual(self.patch.read_bytes(), patch_bytes_before)
        self.assertEqual(self.global_config.read_bytes(), global_bytes_before)
        self.assertEqual(directory_ownership(self.cache), cache_owner_before)
        temporary_config = pathlib.Path(calls[0]["global_config"])
        self.assertFalse(temporary_config.parent.exists())
        writes = [call for call in self.git_calls() if call["kind"] == "config-write"]
        self.assertEqual(len(writes), 2)
        for write in writes:
            self.assertEqual(pathlib.Path(write["working_directory"]), temporary_config.parent)
            self.assertEqual(pathlib.Path(write["ceiling"]), destination.parent.resolve())
            self.assertIsNone(write["git_dir"])
            self.assertIsNone(write["git_work_tree"])
            self.assertIsNone(write["git_common_dir"])
        self.assertNotIn(str(temporary_config).encode(), (destination / ".git" / "config").read_bytes())
        for call in self.git_calls():
            if call["kind"] not in ("clone", "upload-pack", "upload-pack-inherited", "upload-pack-result"):
                self.assertEqual(call["global_config"], str(self.global_config))
        stored_trust = subprocess.run(
            self.git + ["-C", str(destination), "config", "--local", "--get-all", "safe.directory"],
            capture_output=True, text=True, env=self.environment,
        )
        self.assertEqual(stored_trust.returncode, 1, stored_trust.stdout + stored_trust.stderr)
        self.assertEqual(stored_trust.stdout, "")

    def assert_clone_context(self, call, destination):
        self.assertEqual(call["trust_entries"], ["", str(self.cache.resolve() / ".git")])
        self.assertEqual(call["config_keys"], ["safe.directory", "safe.directory"])
        self.assertTrue(call["regular_config"])
        self.assertFalse(call["config_context_symlink"])
        self.assertEqual(call["config_owner"], os.getuid())
        self.assertEqual(call["config_context_owner"], os.getuid())
        config = pathlib.Path(call["global_config"])
        self.assertEqual(config.parent.parent.resolve(), destination.parent.resolve())
        self.assertTrue(config.parent.name.startswith(f".{destination.name}.clone-config-"))
        self.assertIsNone(call["parameters"])
        self.assertIsNone(call["count"])
        self.assertFalse(any("safe.directory=" in argument for argument in call["arguments"]))

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_temporary_global_selection_is_clone_only_and_neutralizes_caller_command_config(self):
        self.global_config.write_text("[safe]\n\tdirectory = *\n[fixture]\n\tcaller = original\n")
        self.environment.update({
            "GIT_CONFIG_PARAMETERS": "'safe.directory=*' 'fixture.parameters=original'",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "fixture.number",
            "GIT_CONFIG_VALUE_0": "original",
        })
        self.enable_different_owner_clone(scrub_upload_pack=True)
        environment_before = self.environment.copy()
        global_before = self.global_config.read_bytes()
        cache_before = directory_bytes(self.cache)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assert_clone_context(self.clone_calls()[0], destination)
        self.assertEqual(self.environment, environment_before)
        self.assertEqual(self.global_config.read_bytes(), global_before)
        self.assertEqual(directory_bytes(self.cache), cache_before)
        for call in self.git_calls():
            if call["kind"] not in ("clone", "upload-pack", "upload-pack-inherited", "upload-pack-result"):
                self.assertEqual(call["global_config"], environment_before["GIT_CONFIG_GLOBAL"])
                self.assertEqual(call["parameters"], environment_before["GIT_CONFIG_PARAMETERS"])
                self.assertEqual(call["count"], environment_before["GIT_CONFIG_COUNT"])
        self.assertEqual(list(destination.parent.glob(".efsw.clone-config-*")), [])

    def test_config_construction_ignores_unavailable_foreign_caller_worktree_metadata(self):
        caller = self.root / "foreign-caller"
        caller.mkdir()
        foreign_gitfile = caller / ".git"
        foreign_gitfile.write_text(f"gitdir: {self.root / 'unavailable-host-worktree'}\n")
        foreign_bytes_before = foreign_gitfile.read_bytes()
        # The owned config context is itself below the broken caller. Changing
        # only its cwd must not rediscover the caller's unavailable metadata.
        destination = caller / "build" / "private" / "efsw"
        cache_before = directory_bytes(self.cache)
        global_before = self.global_config.read_bytes()

        result = self.prepare(destination, cwd=caller)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_before)
        self.assertEqual(self.global_config.read_bytes(), global_before)
        self.assertEqual(foreign_gitfile.read_bytes(), foreign_bytes_before)
        self.assertEqual(list(destination.parent.glob(".efsw.clone-config-*")), [])

    def allocation_script(self, suffixes):
        # Intercept only random suffix allocation in a fixture entrypoint. The
        # production helper gets no test parameters or alternate allocation API.
        script = self.root / "allocation-fixture.cmake"
        self.allocation_count = self.root / "allocation-count"
        script.write_text(
            "set(_fixture_allocation_count 0)\n"
            f"set(_fixture_suffixes {';'.join(suffixes)})\n"
            "function(string)\n"
            "  if(\"${ARGV0}\" STREQUAL \"RANDOM\")\n"
            "    list(GET _fixture_suffixes ${_fixture_allocation_count} _suffix)\n"
            "    math(EXPR _count \"${_fixture_allocation_count} + 1\")\n"
            "    set(_fixture_allocation_count ${_count} PARENT_SCOPE)\n"
            f"    file(WRITE [[{self.allocation_count.as_posix()}]] \"${{_count}}\")\n"
            "    list(GET ARGV -1 _output)\n"
            "    set(${_output} ${_suffix} PARENT_SCOPE)\n"
            "  else()\n"
            "    _string(${ARGV})\n"
            "    if(\"${ARGV0}\" STREQUAL \"SHA256\")\n"
            "      set(${ARGV1} \"${${ARGV1}}\" PARENT_SCOPE)\n"
            "    endif()\n"
            "  endif()\n"
            "endfunction()\n"
            f"include([[{PREPARE_SCRIPT.as_posix()}]])\n"
        )
        return script

    def test_allocation_preserves_staging_and_context_collision_sentinels(self):
        for prefix in ("prepare", "clone-config"):
            for kind in ("file", "directory", "dangling-symlink"):
                with self.subTest(prefix=prefix, kind=kind):
                    destination = self.root / f"private-{prefix}-{kind}" / "efsw"
                    destination.parent.mkdir()
                    sentinel = destination.parent / f".efsw.{prefix}-{'a' * 16}"
                    if kind == "directory":
                        sentinel.mkdir()
                        (sentinel / "preserve").write_bytes(b"collision sentinel")
                    elif kind == "file":
                        sentinel.write_bytes(b"collision sentinel")
                    else:
                        sentinel.symlink_to(self.root / "nonexistent-sentinel-target")
                    script = self.allocation_script(["a" * 16, "b" * 16])

                    result = self.prepare(destination, script=script)

                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_prepared_clean_checkout(destination)
                    self.assertEqual(self.allocation_count.read_text(), "2")
                    if kind == "dangling-symlink":
                        self.assertTrue(sentinel.is_symlink())
                        self.assertEqual(os.readlink(sentinel), str(self.root / "nonexistent-sentinel-target"))
                    else:
                        value = sentinel / "preserve" if kind == "directory" else sentinel
                        self.assertEqual(value.read_bytes(), b"collision sentinel")
                    residual = list(destination.parent.glob(".efsw.prepare-*"))
                    residual += list(destination.parent.glob(".efsw.clone-config-*"))
                    self.assertEqual(residual, [sentinel])

    def test_allocation_exhaustion_is_bounded_and_does_not_delete_sentinels(self):
        for prefix in ("prepare", "clone-config"):
            with self.subTest(prefix=prefix):
                destination = self.root / f"exhausted-{prefix}" / "efsw"
                destination.parent.mkdir()
                sentinel = destination.parent / f".efsw.{prefix}-{'a' * 16}"
                sentinel.write_bytes(b"collision sentinel")
                script = self.allocation_script(["a" * 16] * 16)

                result = self.prepare(destination, script=script)

                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Failed to allocate unique", result.stdout + result.stderr)
                self.assertEqual(self.allocation_count.read_text(), "16")
                self.assertEqual(sentinel.read_bytes(), b"collision sentinel")
                self.assertFalse(destination.exists())
                self.assertFalse(self.private_marker(destination).exists())
                residual = list(destination.parent.glob(".efsw.prepare-*"))
                residual += list(destination.parent.glob(".efsw.clone-config-*"))
                self.assertEqual(residual, [sentinel])

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_reuse_allocates_no_temporary_configuration(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.enable_different_owner_clone(fail_config_write=1)

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.clone_calls(), [])
        self.assertFalse(any(call["kind"] == "config-write" for call in self.git_calls()))
        self.assertEqual(list(destination.parent.glob(".efsw.clone-config-*")), [])

    def assert_config_write_failure_cleans_context(self, step):
        self.enable_different_owner_clone(fail_config_write=step)
        cache_before = directory_bytes(self.cache)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Failed to configure", result.stdout + result.stderr)
        self.assertEqual(self.clone_calls(), [])
        writes = [call for call in self.git_calls() if call["kind"] == "config-write"]
        self.assertEqual(len(writes), step)
        self.assertEqual(directory_bytes(self.cache), cache_before)
        self.assert_no_private_source(destination)

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_first_config_write_failure_cleans_owned_context_and_lock(self):
        self.assert_config_write_failure_cleans_context(1)

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_second_config_write_failure_cleans_owned_context_and_lock(self):
        self.assert_config_write_failure_cleans_context(2)

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_clone_failure_cleans_owned_context_and_partial_staging(self):
        self.enable_different_owner_clone(fail_clone=True)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("injected clone failure", " ".join((result.stdout + result.stderr).split()))
        self.assert_clone_context(self.clone_calls()[0], destination)
        self.assert_no_private_source(destination)

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_clone_timeout_closes_descendant_socket_and_cleans_context(self):
        # EOF is the process-lifetime observation. Timeouts are outer safety
        # deadlines, not sleeps, performance budgets or process polling.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)
            self.enable_different_owner_clone(timeout_address=listener.getsockname())
            destination = self.root / "private" / "efsw"
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.prepare, destination, timeout=12)
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(6)
                    try:
                        eof = connection.recv(1) == b""
                    except socket.timeout:
                        eof = False
                    # Closing only this fixture's socket releases a descendant
                    # if production failed to reap it; RED cannot leak it.
                result = future.result(timeout=8)

        self.assertTrue(eof, "clone descendant survived the internal Git timeout")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to clone", result.stdout + result.stderr)
        self.assert_clone_context(self.clone_calls()[0], destination)
        self.assert_no_private_source(destination)

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_dirty_cache_is_rejected_before_different_owner_clone(self):
        (self.cache / "value.txt").write_text("unapproved dirty source\n")
        self.enable_different_owner_clone()
        cache_bytes_before = directory_bytes(self.cache)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source tree SHA-256 mismatch", result.stdout + result.stderr)
        self.assertEqual(self.clone_calls(), [])
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assert_no_private_source(destination)

    def test_rejects_cache_gitfile_without_trusting_external_metadata(self):
        external_git = self.root / "external-git"
        (self.cache / ".git").rename(external_git)
        (self.cache / ".git").write_text(f"gitdir: {external_git}\n")
        self.assert_cache_metadata_rejected(external_git)

    def test_rejects_cache_git_directory_symlink(self):
        external_git = self.root / "external-git"
        (self.cache / ".git").rename(external_git)
        (self.cache / ".git").symlink_to(external_git, target_is_directory=True)
        self.assert_cache_metadata_rejected(external_git)

    def test_rejects_cache_common_directory_metadata(self):
        external_git = self.root / "external-git"
        shutil.copytree(self.cache / ".git", external_git)
        (self.cache / ".git" / "commondir").write_text(str(external_git) + "\n")
        self.assert_cache_metadata_rejected(external_git)

    def test_rejects_dangling_cache_common_directory_symlink(self):
        external_git = self.root / "external-git"
        shutil.copytree(self.cache / ".git", external_git)
        (self.cache / ".git" / "commondir").symlink_to(external_git / "missing-redirect")
        self.assert_cache_metadata_rejected(external_git)

    def assert_cache_metadata_rejected(self, external_git):
        # Keep native Windows metadata controls; observe the clone boundary on
        # POSIX, where the selective executable shim is supported.
        if os.name != "nt":
            self.enable_different_owner_clone()
        cache_bytes_before = directory_bytes(self.cache)
        external_bytes_before = directory_bytes(external_git)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("cache source does not retain ordinary Git metadata", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(directory_bytes(external_git), external_bytes_before)
        if os.name != "nt":
            self.assertEqual(self.clone_calls(), [])
        self.assert_no_private_source(destination)

    @unittest.skipIf(os.name == "nt", "The selective executable Git shim requires POSIX")
    def test_different_owner_clone_still_rejects_symlinked_objects_and_cleans_staging(self):
        external_objects = self.root / "external-objects"
        (self.cache / ".git" / "objects").rename(external_objects)
        (self.cache / ".git" / "objects").symlink_to(external_objects, target_is_directory=True)
        self.enable_different_owner_clone()
        cache_bytes_before = directory_bytes(self.cache)
        external_bytes_before = directory_bytes(external_objects)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to clone", result.stdout + result.stderr)
        self.assertIn("symlink", result.stdout + result.stderr)
        self.assertEqual(len(self.clone_calls()), 1)
        self.assertTrue(self.clone_calls()[0]["different_owner"])
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(directory_bytes(external_objects), external_bytes_before)
        self.assert_no_private_source(destination)

    def private_marker(self, destination):
        return destination.parent / f".{destination.name}.klogg-private-source"

    def assert_prepared_clean_checkout(self, destination):
        self.assertTrue((destination / ".git").exists())
        self.assertNotEqual(destination.resolve(), self.cache.resolve())
        self.assertEqual((destination / "value.txt").read_text(), "clean source\n")
        self.assertEqual(source_tree_digest(destination), self.clean_digest)
        self.assertEqual(self.git_output("rev-parse", "HEAD", cwd=destination), self.revision)
        self.assertEqual(self.private_marker(destination).read_text(), self.revision + "\n")

    def test_prepares_distinct_clean_private_checkout_from_clean_cache(self):
        destination = self.root / "private" / "efsw"
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_uses_pinned_commit_not_approved_patched_cache_worktree(self):
        destination = self.root / "private" / "efsw"
        subprocess.run(
            self.git + ["apply", str(self.patch)],
            cwd=self.cache,
            check=True,
            env=self.environment,
        )
        cache_bytes_before = directory_bytes(self.cache)
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(
            destination,
            approved_cache_digests=[self.clean_digest, self.patched_digest],
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_later_patch_series_preserves_lf_with_global_autocrlf_true(self):
        global_config = self.root / "global-gitconfig"
        global_config.write_text("[core]\n\tautocrlf = true\n")
        self.environment["GIT_CONFIG_GLOBAL"] = str(global_config)
        destination = self.root / "private" / "efsw"
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()
        expected = self.root / "expected-patched"
        shutil.copytree(self.cache, expected)
        (expected / "value.txt").write_bytes(b"patched LF\n")
        patched_digest = source_tree_digest(expected)
        patch = self.root / "later.patch"
        patch.write_bytes(
            b"diff --git a/value.txt b/value.txt\n"
            b"--- a/value.txt\n"
            b"+++ b/value.txt\n"
            b"@@ -1 +1 @@\n"
            b"-clean source\n"
            b"+patched LF\n"
        )

        prepared = self.prepare(destination)
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        stored_autocrlf = subprocess.check_output(
            ["git", "-C", str(destination), "config", "--local", "--get", "core.autocrlf"],
            text=True,
            env=self.environment,
        ).strip()
        self.assertEqual(stored_autocrlf, "false")
        applied = self.apply_series(destination, patch, patched_digest)

        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        self.assertEqual((destination / "value.txt").read_bytes(), b"patched LF\n")
        self.assertEqual(source_tree_digest(destination), patched_digest)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_rerun_reuses_the_verified_private_pin_without_resetting_it(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        destination_bytes_before = directory_bytes(destination)
        destination_index_before = (destination / ".git" / "index").read_bytes()

        second = self.prepare(destination)

        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("already prepared", second.stdout + second.stderr)
        self.assertEqual(directory_bytes(destination), destination_bytes_before)
        self.assertEqual((destination / ".git" / "index").read_bytes(), destination_index_before)

    def test_rerun_reuses_a_user_changed_private_checkout_without_resetting_it(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        value = destination / "value.txt"
        value.write_text("user change\n")
        index_before = (destination / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("already prepared for patch series", result.stdout + result.stderr)
        self.assertEqual(value.read_text(), "user change\n")
        self.assertEqual((destination / ".git" / "index").read_bytes(), index_before)

    def test_rejects_clean_destination_without_usable_git_metadata(self):
        destination = self.root / "private" / "efsw"
        shutil.copytree(self.cache, destination)
        shutil.rmtree(destination / ".git")
        preserved_bytes = directory_bytes(destination)

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), preserved_bytes)

    def test_rejects_unmarked_git_checkout_without_overwriting_it(self):
        destination = self.root / "private" / "efsw"
        shutil.copytree(self.cache, destination)
        preserved_bytes = directory_bytes(destination)

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("was not prepared by this helper", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), preserved_bytes)
        self.assertFalse(self.private_marker(destination).exists())

    def assert_cache_unchanged(self, cache_bytes_before, cache_head_before, cache_index_before):
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_rejects_markered_checkout_with_git_dir_symlink_to_cache(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        shutil.rmtree(destination / ".git")
        (destination / ".git").symlink_to(self.cache / ".git", target_is_directory=True)
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_rejects_markered_checkout_with_redirected_worktree(self):
        destination = self.root / "private" / "efsw"
        first = self.prepare(destination)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        subprocess.run(
            self.git + ["config", "core.worktree", str(self.cache)],
            cwd=destination,
            check=True,
            env=self.environment,
        )
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Git worktree is not private", result.stdout + result.stderr)
        self.assert_cache_unchanged(cache_bytes_before, cache_head_before, cache_index_before)

    def test_cross_volume_relative_path_guard_is_explicit(self):
        source = PREPARE_SCRIPT.read_text()
        self.assertIn('if(IS_ABSOLUTE "${_path_remainder}")', source)
        outer = self.root / "outer"
        child = self.root / "child"
        outer.mkdir()
        child.mkdir()
        probe = self.root / "relative-path-probe.cmake"
        probe.write_text(
            f"file(RELATIVE_PATH relative [[{outer.as_posix()}]] [[{child.as_posix()}]])\n"
            "if(IS_ABSOLUTE \"${relative}\")\n"
            "  message(FATAL_ERROR \"host relative path unexpectedly absolute\")\n"
            "endif()\n"
            "if(NOT \"${relative}\" MATCHES \"^\\\\.\\\\.(/|$)\")\n"
            "  message(FATAL_ERROR \"host sibling did not escape parent\")\n"
            "endif()\n"
        )

        result = subprocess.run(
            ["cmake", "-P", str(probe)],
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_cache_isolation_rejected(self, destination):
        cache_bytes_before = directory_bytes(self.cache)
        cache_head_before = self.git_output("rev-parse", "HEAD")
        cache_index_before = (self.cache / ".git" / "index").read_bytes()

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not overlap", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(self.git_output("rev-parse", "HEAD"), cache_head_before)
        self.assertEqual((self.cache / ".git" / "index").read_bytes(), cache_index_before)

    def test_rejects_destination_equal_to_cache_source(self):
        self.assert_cache_isolation_rejected(self.cache)

    def test_rejects_destination_symlink_alias_to_cache_source(self):
        alias = self.root / "cache-alias"
        alias.symlink_to(self.cache, target_is_directory=True)

        self.assert_cache_isolation_rejected(alias)

    def test_rejects_destination_nested_within_cache_source(self):
        self.assert_cache_isolation_rejected(self.cache / "private-source")

    def test_rejects_destination_within_existing_symlink_to_cache_source(self):
        alias_parent = self.root / "cache-parent-alias"
        alias_parent.symlink_to(self.cache, target_is_directory=True)

        self.assert_cache_isolation_rejected(alias_parent / "private-source")

    def test_rejects_cache_source_nested_within_destination(self):
        self.assert_cache_isolation_rejected(self.root)

    def test_rejects_wrong_pin_without_creating_destination(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, revision="0" * 40)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be pinned", result.stdout + result.stderr)
        self.assertFalse(destination.exists())

    def test_rejects_missing_cache_path_without_creating_destination(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, cache=self.root / "missing-cache")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source directory does not exist", result.stdout + result.stderr)
        self.assertFalse(destination.exists())

    def test_partial_git_cache_fails_within_the_subprocess_bound_and_cleans_staging(self):
        partial = self.root / "partial-cache"
        shutil.copytree(self.cache, partial)
        partial_revision = self.git_output("rev-parse", "HEAD", cwd=partial)
        shutil.rmtree(partial / ".git" / "objects")
        destination = self.root / "private" / "efsw"

        result = self.prepare(
            destination,
            cache=partial,
            revision=partial_revision,
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to clone", result.stdout + result.stderr)
        self.assertFalse(destination.exists())
        self.assertEqual(list(destination.parent.glob(".efsw.prepare-*")), [])

    def test_never_overwrites_an_unknown_existing_destination(self):
        destination = self.root / "private" / "efsw"
        destination.mkdir(parents=True)
        sentinel = destination / "user-file.txt"
        sentinel.write_text("do not replace\n")

        result = self.prepare(destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not retain usable Git metadata", result.stdout + result.stderr)
        self.assertEqual(sentinel.read_text(), "do not replace\n")


if __name__ == "__main__":
    unittest.main()
