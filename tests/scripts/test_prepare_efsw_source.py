import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import shutil
import shlex
import socket
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


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


def directory_namespace(source: pathlib.Path) -> dict:
    # Include directories, links and locks; a file-only manifest misses mkdir
    # side effects in an authenticated shared source cache.
    result = {}
    for path in [source] + list(source.rglob("*")):
        info = path.lstat()
        content = (os.readlink(path) if path.is_symlink()
                   else hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file()
                   else None)
        result[path.relative_to(source).as_posix()] = (
            info.st_mode, info.st_uid, info.st_gid, info.st_nlink, content,
        )
    return result


def directory_ownership(source: pathlib.Path) -> dict:
    return {
        path.relative_to(source).as_posix(): (path.lstat().st_uid, path.lstat().st_gid)
        for path in [source] + list(source.rglob("*"))
    }


def read_startup_identity(peer):
    identity = bytearray()
    while not identity.endswith(b"\n"):
        chunk = peer.recv(1)
        if not chunk:
            raise EOFError(f"Startup child closed before completing identity: {bytes(identity)!r}")
        identity.extend(chunk)
    return json.loads(identity)


def startup_channel_is_closed(peer):
    """Observe only the channel; callers must establish owner completion first."""
    return peer.recv(1) == b""


class StartupChannelObservationTest(unittest.TestCase):
    def test_closed_peer_is_observed_without_a_post_teardown_write(self):
        class ClosedPeer:
            reset = False

            def sendall(self, data):
                self.reset = True

            def recv(self, size):
                if self.reset:
                    raise ConnectionResetError(54, "post-close write induced reset")
                return b""

        peer = ClosedPeer()
        self.assertTrue(startup_channel_is_closed(peer))
        self.assertFalse(peer.reset)

    def test_eof_and_data_are_distinct_channel_observations(self):
        for data, closed in ((b"", True), (b"A", False)):
            with self.subTest(data=data):
                peer = mock.Mock()
                peer.recv.return_value = data
                self.assertIs(startup_channel_is_closed(peer), closed)
                peer.recv.assert_called_once_with(1)

    def test_inconclusive_receive_errors_are_not_closed_channel_evidence(self):
        for error in (socket.timeout("bounded observation expired"),
                      ConnectionResetError(54, "connection reset")):
            with self.subTest(error=type(error).__name__):
                peer = mock.Mock()
                peer.recv.side_effect = error
                with self.assertRaises(type(error)):
                    startup_channel_is_closed(peer)


class StartupIdentityTest(unittest.TestCase):
    def test_complete_identity_is_read_through_its_newline(self):
        record = b'{"pid":1,"ppid":2,"group":3}\n'
        peer = mock.Mock()
        peer.recv.side_effect = [bytes([value]) for value in record]

        self.assertEqual(read_startup_identity(peer), {"pid": 1, "ppid": 2, "group": 3})
        self.assertEqual(peer.recv.call_count, len(record))
        peer.recv.assert_called_with(1)

    def test_eof_before_newline_reports_partial_identity_without_reading_again(self):
        for partial in (b"", b'{"pid":1'):
            with self.subTest(partial=partial):
                peer = mock.Mock()
                peer.recv.side_effect = [bytes([value]) for value in partial] + [
                    b"", AssertionError("Startup observer read again after EOF"),
                ]

                with self.assertRaises(EOFError) as failure:
                    read_startup_identity(peer)

                self.assertIn(repr(partial), str(failure.exception))
                self.assertEqual(peer.recv.call_count, len(partial) + 1)


class PrepareEfswSourceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name).resolve()
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
        if not getattr(self, "_preserve_uncertain_fixture", False):
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
        python=None,
        final_digest=None,
        child_umask=None,
        controlled_phase=False,
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
            f"-DPATCHED_TREE_HASH={final_digest or self.patched_digest}",
            "-DAPPROVED_CACHE_TREE_HASHES="
            + ";".join(approved_cache_digests),
            "-DLOCK_TIMEOUT=2",
            "-DGIT_TIMEOUT=2",
            "-P",
            str(script),
        ]
        if python is not None:
            command.insert(1, f"-DPREPARE_PYTHON_EXECUTABLE={python}")
        if child_umask is not None:
            launcher = self.root / "prepare-child-mask.py"
            launcher.write_text(
                "import os, sys\n"
                "os.umask(int(sys.argv[1], 8))\n"
                "os.execvp(sys.argv[2], sys.argv[2:])\n"
            )
            command = [sys.executable, "-I", "-S", str(launcher), f"{child_umask:o}"] + command
        if controlled_phase:
            # Only the synthetic caller-boundary phase promises no nested
            # sessions. Generic/native prepare below is not universally family
            # contained: real Git owners reserve their own separate sessions.
            sys.path.insert(0, str(ROOT / "scripts"))
            import prepare_efsw_git as owner
            result = owner._run(command, timeout=timeout, env=self.environment, cwd=cwd)
            if result.kind == "cleanup":
                self._preserve_uncertain_fixture = True
                self.temporary._finalizer.detach()
                raise RuntimeError("Controlled CMake family cleanup incomplete: " + result.diagnostic)
            if result.kind == "timeout":
                raise subprocess.TimeoutExpired(command, timeout, output=result.stdout, stderr=result.stderr)
            if result.kind not in ("ok", "git"):
                raise RuntimeError("Controlled CMake family failed: " + result.diagnostic)
            return subprocess.CompletedProcess(command, result.returncode,
                                               result.stdout.decode("utf-8"), result.stderr.decode("utf-8"))
        # Inherited pipes can hide CMake completion while an orphan remains.
        # Owned files let the result and socket lifetime be observed separately.
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as output, \
                tempfile.TemporaryFile(mode="w+", encoding="utf-8") as error:
            result = subprocess.run(
                command, check=False, stdout=output, stderr=error, text=True,
                env=self.environment, timeout=timeout, cwd=cwd,
            )
            output.seek(0)
            error.seek(0)
            return subprocess.CompletedProcess(command, result.returncode, output.read(), error.read())

    def enable_different_owner_clone(
        self, scrub_upload_pack=False, fail_config_write=0, fail_clone=False,
        timeout_address=None, timeout_step="clone", timeout_query_path=None,
    ):
        # POSIX executable shim: only the local clone and its transport child
        # see the ownership flag. Ordinary forwarding replaces the shim process.
        real_git = shutil.which("git", path=self.environment["PATH"])
        self.assertIsNotNone(real_git)
        shim_directory = self.root / "git-shim"
        shim_directory.mkdir()
        self.git_log = self.root / "git-log.jsonl"
        self.git_log.write_text("")
        # Keep Python isolation in one shebang argument for POSIX kernels.
        common = (
            f"#!{sys.executable} -IS\n"
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
                    # Inspection must not discover a broken caller repository;
                    # never mutate the command's recorded/forwarded environment
                    # or its production cwd to make the test pass.
                    inspection_environment = environment.copy()
                    for selector in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_COMMON_DIR'):
                        inspection_environment.pop(selector, None)
                    inspection_environment['GIT_CEILING_DIRECTORIES'] = str(config.parent.parent)
                    values = subprocess.check_output(
                        [real_git, 'config', '--file', str(config), '--null', '--get-all', 'safe.directory'],
                        env=inspection_environment, cwd=config.parent,
                    ) if config.exists() and config.stat().st_size else b''
                    item['trust_entries'] = values.decode().split('\\0')[:-1]
                    item['config_keys'] = subprocess.check_output(
                        [real_git, 'config', '--file', str(config), '--name-only', '--list'],
                        env=inspection_environment, cwd=config.parent,
                    ).decode().splitlines() if config.exists() else []
                    if kind == 'upload-pack':
                        effective = subprocess.run(
                            [real_git, 'config', '--null', '--get-all', 'safe.directory'],
                            capture_output=True, env=inspection_environment, cwd=config.parent,
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
            f"timeout_step = {timeout_step!r}\n"
            f"timeout_query_path = {str(timeout_query_path) if timeout_query_path else None!r}\n"
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
            with open(log_path) as log:
                previous = [json.loads(line) for line in log]
            config_writes = sum(call['kind'] == 'config-write' for call in previous)
            query_matches = (
                arguments[-2:] == ['rev-parse', 'HEAD'] and '-C' in arguments
                and timeout_query_path is not None
                and pathlib.Path(arguments[arguments.index('-C') + 1]).resolve() == pathlib.Path(timeout_query_path)
            )
            query_count = sum(
                call['arguments'][-2:] == ['rev-parse', 'HEAD']
                and '-C' in call['arguments']
                and pathlib.Path(call['arguments'][call['arguments'].index('-C') + 1]).resolve() == pathlib.Path(timeout_query_path)
                for call in previous
            ) if query_matches else 0
            block = (
                (kind == 'clone' and timeout_step == 'clone')
                or (kind == 'config-write' and timeout_step == 'config-' + str(config_writes))
                or (query_matches and timeout_step == 'initial-pin')
                or (query_matches and timeout_step == 'reuse-pin' and query_count == 2)
            )
            if timeout_address and block:
                child_code = (
                    "import json, os, socket, sys; "
                    "connection = socket.create_connection(tuple(json.loads(sys.argv[1])), timeout=8); "
                    "connection.settimeout(None); "
                    "connection.sendall((json.dumps({'role': 'child', 'pid': os.getpid(), 'ppid': os.getppid(), "
                    "'group': os.getpgrp()}) + '\\\\n').encode()); "
                    "connection.recv(1); connection.close()"
                )
                child = subprocess.Popen([sys.executable, '-c', child_code, json.dumps(timeout_address)])
                with socket.create_connection(timeout_address, timeout=8) as connection:
                    connection.settimeout(None)
                    connection.sendall((json.dumps({
                        'role': 'command', 'pid': os.getpid(), 'ppid': os.getppid(), 'group': os.getpgrp(),
                    }) + '\\n').encode())
                    connection.recv(1)
                child.wait(timeout=8)
                sys.exit(0)
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

    def apply_series(self, destination, patch, patched_digest, python=None):
        return subprocess.run(
            [
                "cmake",
                "-DDEPENDENCY=efsw",
                f"-DPREPARE_PYTHON_EXECUTABLE={python or sys.executable}",
                "-DGIT_TIMEOUT=2",
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

    @unittest.skipIf(os.name == "nt", "The observational executable Git shim requires POSIX")
    def test_git_shim_startup_ignores_ambient_python_imports_and_preserves_call_context(self):
        self.enable_different_owner_clone()
        imports = self.root / "ambient-python"
        imports.mkdir()
        marker = self.root / "ambient-site-imported"
        (imports / "sitecustomize.py").write_text(
            f"with open({str(marker)!r}, 'w') as output: output.write('ambient site imported')\n"
        )
        (imports / "json.py").write_text(
            "raise RuntimeError('ambient json import must not run in the Git shim')\n"
        )
        caller = self.root / "shim-caller"
        caller.mkdir()
        environment = self.environment.copy()
        environment.update({
            "PYTHONPATH": str(imports),
            "GIT_CONFIG_PARAMETERS": "'fixture.parameters=unchanged'",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "fixture.number",
            "GIT_CONFIG_VALUE_0": "unchanged",
            "GIT_DIR": str(self.cache / ".git"),
            "GIT_WORK_TREE": str(self.cache),
            "GIT_COMMON_DIR": str(self.cache / ".git"),
            "GIT_CEILING_DIRECTORIES": str(self.root),
        })
        fixture_environment_before = self.environment.copy()
        environment_before = environment.copy()
        arguments = self.git[1:] + ["--version"]

        result = subprocess.run(
            [self.git[0]] + arguments, cwd=caller, env=environment,
            capture_output=True, text=True, timeout=10,
        )

        self.assertEqual(
            result.returncode, 0,
            result.stdout + result.stderr + f"\nAmbient site marker exists: {marker.exists()}",
        )
        self.assertTrue(result.stdout.startswith("git version "), result.stdout)
        self.assertFalse(marker.exists(), "Git shim imported ambient site customization")
        calls = self.git_calls()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["arguments"], arguments)
        self.assertEqual(calls[0]["working_directory"], str(caller.resolve()))
        for field, selector in (
            ("global_config", "GIT_CONFIG_GLOBAL"),
            ("parameters", "GIT_CONFIG_PARAMETERS"),
            ("count", "GIT_CONFIG_COUNT"),
            ("git_dir", "GIT_DIR"),
            ("git_work_tree", "GIT_WORK_TREE"),
            ("git_common_dir", "GIT_COMMON_DIR"),
            ("ceiling", "GIT_CEILING_DIRECTORIES"),
        ):
            with self.subTest(selector=selector):
                self.assertEqual(calls[0][field], environment_before[selector])
        self.assertEqual(environment, environment_before)
        self.assertEqual(self.environment, fixture_environment_before)

    @unittest.skipIf(os.name == "nt", "The observational executable Git shim requires POSIX")
    def test_logging_inspection_does_not_relocate_an_unavailable_caller(self):
        caller = self.root / "foreign-observed-caller"
        caller.mkdir()
        gitfile = caller / ".git"
        gitfile.write_text(f"gitdir: {self.root / 'unavailable-host-worktree'}\n")
        original_gitfile = gitfile.read_bytes()
        destination = caller / "build" / "private" / "efsw"
        self.enable_different_owner_clone(scrub_upload_pack=True)

        result = self.prepare(destination, cwd=caller)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(self.clone_calls()[0]["working_directory"], str(caller.resolve()))
        self.assertEqual(gitfile.read_bytes(), original_gitfile)

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
            "  elseif(\"${ARGV0}\" STREQUAL \"REPLACE\")\n"
            "    if(NOT ARGC EQUAL 5)\n"
            "      message(FATAL_ERROR \"allocation fixture expects one REPLACE input\")\n"
            "    endif()\n"
            "    _string(REPLACE \"${ARGV1}\" \"${ARGV2}\" \"${ARGV3}\" \"${ARGV4}\")\n"
            "    set(${ARGV3} \"${${ARGV3}}\" PARENT_SCOPE)\n"
            "  elseif(\"${ARGV0}\" STREQUAL \"SHA256\")\n"
            "    _string(SHA256 \"${ARGV1}\" \"${ARGV2}\")\n"
            "    set(${ARGV1} \"${${ARGV1}}\" PARENT_SCOPE)\n"
            "  else()\n"
            "    message(FATAL_ERROR \"unexpected string operation in allocation fixture\")\n"
            "  endif()\n"
            "endfunction()\n"
            f"include([[{PREPARE_SCRIPT.as_posix()}]])\n"
        )
        return script

    def test_allocation_forwards_present_locale_replace_and_its_output(self):
        for index, locale in enumerate(("C.UTF-8", "present;locale=with space", "")):
            with self.subTest(locale=locale):
                self.environment["LC_CTYPE"] = locale
                destination = self.root / f"allocation-locale-{index}" / "efsw"
                destination.parent.mkdir()
                sentinel = destination.parent / f".efsw.clone-config-{'a' * 16}"
                sentinel.write_bytes(b"preserve locale collision")
                script = self.allocation_script(["a" * 16, "b" * 16])
                with script.open("a") as fixture:
                    fixture.write(
                        "_klogg_owned_git_startup_arguments(_observed_locale_arguments)\n"
                        "list(GET _observed_locale_arguments 2 _observed_locale_value)\n"
                        "if(NOT \"${_observed_locale_value}\" STREQUAL \"--startup-lc-ctype=$ENV{LC_CTYPE}\")\n"
                        "  message(FATAL_ERROR \"allocation fixture lost the REPLACE output\")\n"
                        "endif()\n"
                    )

                result = self.prepare(destination, script=script, python=sys.executable)

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_prepared_clean_checkout(destination)
                self.assertEqual(self.allocation_count.read_text(), "2")
                self.assertEqual(sentinel.read_bytes(), b"preserve locale collision")
                self.assertEqual(list(destination.parent.glob(".efsw.clone-config-*")), [sentinel])

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

                    result = self.prepare(destination, script=script, python=sys.executable)

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

                result = self.prepare(destination, script=script, python=sys.executable)

                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Failed to allocate unique", result.stdout + result.stderr)
                self.assertEqual(self.allocation_count.read_text(), "16")
                self.assertEqual(sentinel.read_bytes(), b"collision sentinel")
                self.assertFalse(destination.exists())
                self.assertFalse(self.private_marker(destination).exists())
                residual = list(destination.parent.glob(".efsw.prepare-*"))
                residual += list(destination.parent.glob(".efsw.clone-config-*"))
                self.assertEqual(residual, [sentinel])

    @unittest.skipIf(os.name == "nt", "The temporary-root alias fixture uses a POSIX directory link")
    def test_temporary_root_alias_uses_physical_fixture_paths(self):
        physical_parent = self.root.resolve() / "physical-temporary-parent"
        physical_parent.mkdir()
        alias_parent = self.root.resolve() / "aliased-temporary-parent"
        alias_parent.symlink_to(physical_parent, target_is_directory=True)
        temporary = tempfile.TemporaryDirectory(dir=alias_parent)
        self.addCleanup(temporary.cleanup)
        self.assertNotEqual(pathlib.Path(temporary.name), pathlib.Path(temporary.name).resolve())
        fixture = PrepareEfswSourceTest("test_reuse_allocates_no_temporary_configuration")
        with mock.patch.object(tempfile, "TemporaryDirectory", return_value=temporary):
            fixture.setUp()
        try:
            self.assertEqual(fixture.root, pathlib.Path(temporary.name).resolve())
            fixture.test_reuse_allocates_no_temporary_configuration()
        finally:
            fixture.tearDown()

    @unittest.skipIf(os.name == "nt", "The controlled owner protocol fixture requires POSIX exec")
    def test_reuse_allocates_no_temporary_configuration(self):
        # Allocation is the contract here, not observational-shim/bootstrap
        # latency. Native reuse identity and owner timeouts have separate tests.
        destination = self.root / "private" / "efsw"
        shutil.copytree(self.cache, destination)
        self.private_marker(destination).write_bytes((self.revision + "\n").encode())
        interpreter = self.root / "reuse-protocol-python"
        calls = self.root / "reuse-protocol-calls"
        interpreter.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            "arguments = sys.argv[1:]\n"
            "if any(pathlib.Path(arg).name == 'prepare_efsw_git.py' for arg in arguments):\n"
            "    if '--check-interpreter' in arguments: sys.exit(0)\n"
            "    command = arguments[arguments.index('--') + 1:]\n"
            f"    with open({str(calls)!r}, 'a') as output: output.write(json.dumps(command) + '\\n')\n"
            "    if arguments[arguments.index('--operation') + 1] != 'inherit':\n"
            "        sys.stderr.write('reuse attempted a constructor operation\\n'); sys.exit(3)\n"
            "    path = command[command.index('-C') + 1]\n"
            f"    if path not in ({str(self.cache)!r}, {str(destination)!r}): sys.exit(3)\n"
            "    query = command[-1]\n"
            f"    if query == 'HEAD': print({self.revision!r})\n"
            "    elif query == '--show-toplevel': print(path)\n"
            "    elif query == '--absolute-git-dir': print(str(pathlib.Path(path) / '.git'))\n"
            "    else: sys.exit(3)\n"
            "    sys.exit(0)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}] + arguments)\n"
        )
        interpreter.chmod(0o755)
        script = self.allocation_script(["a" * 16])

        result = self.prepare(destination, script=script, python=interpreter)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.allocation_count.exists(), "Reuse reached production random allocation")
        commands = [json.loads(line) for line in calls.read_text().splitlines()]
        self.assertEqual(len(commands), 5)
        for command in commands:
            self.assertNotIn("config", command)
            self.assertNotIn("clone", command)
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

    def assert_owned_operation_timeout(self, step):
        destination = self.root / "private" / "efsw"
        if step == "reuse-pin":
            first = self.prepare(destination)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        destination_before = directory_bytes(destination) if destination.exists() else None
        cache_before = directory_bytes(self.cache)
        query_path = destination if step == "reuse-pin" else self.cache
        connections = []
        ready = []
        eof = []
        result = None
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            listener.settimeout(8)
            self.enable_different_owner_clone(
                timeout_address=listener.getsockname(), timeout_step=step,
                timeout_query_path=query_path.resolve(),
            )
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.prepare, destination, timeout=16)
                try:
                    for _ in range(2):
                        connection, _ = listener.accept()
                        connections.append(connection)
                        connection.settimeout(8)
                        frame = b""
                        while b"\n" not in frame and len(frame) < 1024:
                            chunk = connection.recv(256)
                            if not chunk:
                                break
                            frame += chunk
                        ready.append(json.loads(frame.decode()))
                    try:
                        # Observe the operation failure WHILE both sockets are
                        # held; inherited stdout pipes cannot obscure this.
                        result = future.result(timeout=8)
                    except concurrent.futures.TimeoutError:
                        pass
                    if result is not None:
                        for connection in connections:
                            connection.settimeout(4)
                            try:
                                eof.append(connection.recv(1) == b"")
                            except socket.timeout:
                                eof.append(False)
                finally:
                    # Release only fixture resources after the observation,
                    # including on RED, so blocked ordinary children cannot leak.
                    for connection in connections:
                        connection.close()
                    future.result(timeout=8)

        self.assertIsNotNone(result, "operation did not report its deadline before fixture release")
        roles = {item["role"]: item for item in ready}
        self.assertEqual(set(roles), {"command", "child"})
        self.assertEqual(roles["child"]["ppid"], roles["command"]["pid"])
        self.assertEqual(roles["child"]["group"], roles["command"]["group"])
        self.assertEqual(eof, [True, True], "owned command/descendant survived the internal Git timeout")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        diagnostic = " ".join((result.stdout + result.stderr).split())
        if step in ("initial-pin", "reuse-pin"):
            self.assertIn("owned Git revision verification failed", diagnostic)
            self.assertIn("status 2", diagnostic)
            self.assertNotIn("Git metadata is incomplete", diagnostic)
        elif step.startswith("config-"):
            self.assertIn("Failed to configure", diagnostic)
        else:
            self.assertIn("Failed to clone", diagnostic)
            self.assert_clone_context(self.clone_calls()[0], destination)
        self.assertEqual(directory_bytes(self.cache), cache_before)
        if destination_before is None:
            self.assert_no_private_source(destination)
        else:
            self.assertEqual(directory_bytes(destination), destination_before)
            self.assertEqual(list(destination.parent.glob(".efsw.clone-config-*")), [])
            self.assertEqual(list(destination.parent.glob(".efsw.prepare-*")), [])

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_clone_timeout_closes_descendant_socket_and_cleans_context(self):
        self.assert_owned_operation_timeout("clone")

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_first_config_timeout_closes_real_child_and_cleans_context(self):
        self.assert_owned_operation_timeout("config-1")

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_second_config_timeout_closes_real_child_and_cleans_context(self):
        self.assert_owned_operation_timeout("config-2")

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_initial_pin_query_timeout_is_fatal_and_closes_real_child(self):
        self.assert_owned_operation_timeout("initial-pin")

    @unittest.skipIf(os.name == "nt", "The socket lifetime and executable Git shim require POSIX")
    def test_reused_pin_query_timeout_is_fatal_and_preserves_destination(self):
        self.assert_owned_operation_timeout("reuse-pin")

    def test_missing_explicit_interpreter_fails_before_git_or_staging(self):
        destination = self.root / "private" / "efsw"
        cache_before = directory_bytes(self.cache)

        result = self.prepare(destination, python=self.root / "missing-python")

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Python interpreter", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(self.cache), cache_before)
        self.assert_no_private_source(destination)

    def test_relative_explicit_interpreter_is_rejected(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, python="python3")

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("absolute", result.stdout + result.stderr)
        self.assert_no_private_source(destination)

    @unittest.skipIf(os.name == "nt", "The rejecting executable interpreter fixture requires POSIX")
    def test_rejected_interpreter_check_fails_before_git_or_staging(self):
        rejected = self.root / "rejected-python"
        rejected.write_text(f"#!{sys.executable}\nimport sys\nsys.exit('incompatible interpreter fixture')\n")
        rejected.chmod(0o755)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, python=rejected)

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Python interpreter", result.stdout + result.stderr)
        self.assertIn("incompatible interpreter fixture", " ".join((result.stdout + result.stderr).split()))
        self.assert_no_private_source(destination)

    def test_explicit_host_interpreter_prepares_clean_private_source(self):
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, python=sys.executable)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_prepared_clean_checkout(destination)

    def test_third_party_scope_discovers_and_passes_supported_absolute_interpreter(self):
        source = (ROOT / "3rdparty" / "CMakeLists.txt").read_text()
        self.assertIn("find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)", source)
        self.assertIn('"-DPREPARE_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"', source)

    @unittest.skipIf(os.name == "nt", "Pre-Python locale observation uses a POSIX shell shim")
    def test_owned_git_receives_exact_prestartup_locale_state(self):
        real_git = shutil.which("git", path=self.environment["PATH"])
        shim_directory = self.root / "locale-git-shim"
        shim_directory.mkdir()
        observation = self.root / "locale-observation"
        shim = shim_directory / "git"
        shim.write_text(
            "#!/bin/sh\n"
            f"if [ \"${{LC_CTYPE+x}}\" = x ]; then printf 'present\\000%s\\000' \"$LC_CTYPE\" >> {shlex.quote(str(observation))}; "
            f"else printf 'absent\\000\\000' >> {shlex.quote(str(observation))}; fi\n"
            f"exec {shlex.quote(real_git)} \"$@\"\n"
        )
        shim.chmod(0o755)
        self.environment["PATH"] = str(shim_directory) + os.pathsep + self.environment["PATH"]
        self.environment["LANG"] = "C"
        self.environment.pop("LC_ALL", None)
        cache_before = directory_bytes(self.cache)
        values = (None, "", "C", "with space", "-dash=equals;semicolon", "back\\slash;tail")
        for index, value in enumerate(values):
            with self.subTest(value=value):
                if value is None:
                    self.environment.pop("LC_CTYPE", None)
                    expected = [b"absent", b""]
                else:
                    self.environment["LC_CTYPE"] = value
                    expected = [b"present", value.encode()]
                observation.write_bytes(b"")
                destination = self.root / f"locale-private-{index}" / "efsw"

                result = self.prepare(destination, python=sys.executable)

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                entries = observation.read_bytes().split(b"\0")[:-1]
                self.assertGreaterEqual(len(entries), 12)
                self.assertEqual(entries, expected * (len(entries) // 2))
                self.assert_prepared_clean_checkout(destination)
        self.assertEqual(directory_bytes(self.cache), cache_before)

    def cleanup_failure_interpreter(self, failure_step, observation=None):
        interpreter = self.root / "cleanup-failure-python"
        protocol = self.root / "cleanup-failure-protocol.py"
        write_count = self.root / "cleanup-config-write-count"
        protocol.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, shutil, sys\n"
            f"target = {failure_step!r}\n"
            f"cache = pathlib.Path({str(self.cache)!r})\n"
            f"revision = {self.revision!r}\n"
            f"count_file = pathlib.Path({str(write_count)!r})\n"
            "arguments = sys.argv[1:]\n"
            "if any(pathlib.Path(argument).name == 'prepare_efsw_git.py' for argument in arguments) and '--operation' in arguments:\n"
            "    operation = arguments[arguments.index('--operation') + 1]\n"
            "    git_arguments = arguments[arguments.index('--') + 1:]\n"
            "    step = operation\n"
            "    config = None\n"
            "    staging = None\n"
            "    if operation == 'config':\n"
            "        count = int(count_file.read_text()) + 1 if count_file.exists() else 1\n"
            "        count_file.write_text(str(count))\n"
            "        step = 'config-' + str(count)\n"
            "        config = pathlib.Path(git_arguments[git_arguments.index('--file') + 1])\n"
            "        staging = config.parent.with_name(config.parent.name.replace('.clone-config-', '.prepare-'))\n"
            "    elif operation == 'clone':\n"
            "        config = pathlib.Path(arguments[arguments.index('--global-config') + 1])\n"
            "        staging = pathlib.Path(git_arguments[-1])\n"
            "    elif 'checkout' in git_arguments:\n"
            "        step = 'checkout'\n"
            "        staging = pathlib.Path(git_arguments[git_arguments.index('-C') + 1])\n"
            "    elif '-C' in git_arguments and git_arguments[-2:] == ['rev-parse', 'HEAD']:\n"
            "        candidate = pathlib.Path(git_arguments[git_arguments.index('-C') + 1])\n"
            "        if '.prepare-' in candidate.name:\n"
            "            step = 'staged-revision'\n"
            "            staging = candidate\n"
            "    if step == target:\n"
            "        staging.mkdir(exist_ok=True)\n"
            "        (staging / 'uncertain-family').write_bytes(b'preserve while quiescence is unknown')\n"
            "        if config is not None:\n"
            "            pathlib.Path(str(config) + '.lock').write_bytes(b'preserve uncertain config lock')\n"
            "        sys.stderr.write('primary fixture Git cause; cleanup incomplete\\n')\n"
            "        sys.exit(5)\n"
            "    # Controlled caller-boundary model: no native Git or nested\n"
            "    # owner sessions in this injected phase. Values come from the\n"
            "    # real fixture; native ownership is covered separately.\n"
            "    if operation == 'config':\n"
            "        if git_arguments[:1] != ['config'] or git_arguments[-3:-1] != ['--add', 'safe.directory']: sys.exit(3)\n"
            "        value = git_arguments[-1]\n"
            "        expected = '' if step == 'config-1' else str(cache / '.git')\n"
            "        if value != expected: sys.exit(3)\n"
            "        with config.open('a') as output: output.write('[safe]\\n\\tdirectory = ' + value + '\\n')\n"
            "    elif operation == 'clone':\n"
            "        if '--no-hardlinks' not in git_arguments or '--no-checkout' not in git_arguments or git_arguments[-2] != str(cache): sys.exit(3)\n"
            "        shutil.copytree(cache, staging)\n"
            "    elif step == 'checkout':\n"
            "        if git_arguments[-2:] != ['--detach', revision]: sys.exit(3)\n"
            "    elif '-C' in git_arguments and git_arguments[-2:] == ['rev-parse', 'HEAD']:\n"
            "        candidate = pathlib.Path(git_arguments[git_arguments.index('-C') + 1])\n"
            "        if candidate != cache and '.prepare-' not in candidate.name: sys.exit(3)\n"
            "        print(revision)\n"
            "    else: sys.exit(3)\n"
            "    sys.exit(0)\n"
            "if any(pathlib.Path(argument).name == 'prepare_efsw_git.py' for argument in arguments) and '--check-interpreter' in arguments: sys.exit(0)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}, '-B'] + arguments)\n"
        )
        if observation is not None:
            source = protocol.read_text().replace(
                "arguments = sys.argv[1:]\n",
                "import json\n"
                f"pathlib.Path({str(observation)!r}).write_text(json.dumps({{'isolated': sys.flags.isolated, "
                "'no_site': sys.flags.no_site, 'dont_write_bytecode': sys.dont_write_bytecode}))\n"
                "arguments = sys.argv[1:]\n",
            )
            protocol.write_text(source)
        # Flags after a shebang script become its argv, not Python startup
        # options. Isolate the actual protocol interpreter before its filename.
        interpreter.write_text(
            "#!/bin/sh\n"
            f"exec {shlex.quote(sys.executable)} -B -I -S {shlex.quote(str(protocol))} \"$@\"\n"
        )
        interpreter.chmod(0o755)
        return interpreter

    def assert_cleanup_failure_preserves_owned_residue(self, failure_step):
        interpreter = self.cleanup_failure_interpreter(failure_step)
        destination = self.root / "private" / "efsw"
        script = self.allocation_script(["a" * 16, "b" * 16])
        cache_before = directory_bytes(self.cache)
        cache_namespace_before = directory_namespace(self.cache)
        trap_directory = self.root / "controlled-git-trap"
        trap_directory.mkdir()
        trap_log = self.root / "unexpected-native-git"
        trap = trap_directory / "git"
        trap.write_text("#!/bin/sh\n"
                        f"printf 'unexpected native Git\\n' >> {shlex.quote(str(trap_log))}\n"
                        "exit 97\n")
        trap.chmod(0o755)
        native_path = self.environment["PATH"]
        self.environment["PATH"] = str(trap_directory) + os.pathsep + native_path
        try:
            result = self.prepare(destination, python=interpreter, script=script, controlled_phase=True)
        finally:
            self.environment["PATH"] = native_path
        self.assertFalse(trap_log.exists(), "Synthetic caller-boundary phase invoked native Git")
        self.assertEqual(directory_namespace(self.cache), cache_namespace_before)

        self.assertNotEqual(result.returncode, 0)
        diagnostic = " ".join((result.stdout + result.stderr).split())
        self.assertIn("primary fixture Git cause", diagnostic)
        self.assertIn("cleanup incomplete", diagnostic)
        self.assertFalse(destination.exists())
        self.assertFalse(self.private_marker(destination).exists())
        contexts = list(destination.parent.glob(".efsw.clone-config-*"))
        stages = list(destination.parent.glob(".efsw.prepare-*"))
        self.assertEqual(len(stages), 1)
        self.assertEqual(len(contexts), 1 if failure_step.startswith("config-") or failure_step == "clone" else 0)
        residue = {path: directory_bytes(path) for path in contexts + stages}
        self.assertEqual(directory_bytes(self.cache), cache_before)

        retry = self.prepare(destination, python=sys.executable, script=script)

        self.assertEqual(retry.returncode, 0, retry.stdout + retry.stderr)
        self.assert_prepared_clean_checkout(destination)
        self.assertEqual(self.allocation_count.read_text(), "2")
        for path, preserved in residue.items():
            self.assertEqual(directory_bytes(path), preserved)

    @unittest.skipIf(os.name == "nt", "The controlled startup interpreter uses POSIX exec")
    def test_controlled_outer_timeout_closes_startup_family_before_teardown(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        interpreter = self.root / "blocked-startup-python"
        protocol = self.root / "blocked-startup-protocol.py"
        protocol.write_text(
            "import json, os, socket\n"
            f"peer = socket.create_connection({listener.getsockname()!r})\n"
            "peer.sendall((json.dumps({'pid': os.getpid(), 'ppid': os.getppid(), 'group': os.getpgrp()}) + '\\n').encode())\n"
            "while True:\n"
            "    command = peer.recv(1)\n"
            "    if command == b'?': peer.sendall(b'A')\n"
            "    elif command == b'x' or not command: break\n"
            "peer.close()\n"
            "os._exit(0)\n"
        )
        interpreter.write_text("#!/bin/sh\n"
                               f"exec {shlex.quote(sys.executable)} -B -I -S {shlex.quote(str(protocol))} \"$@\"\n")
        interpreter.chmod(0o755)
        peer = None
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.prepare, self.root / "private/efsw", python=interpreter,
                                         timeout=2, controlled_phase=True)
                peer, _ = listener.accept()
                peer.settimeout(5)
                observed = read_startup_identity(peer)
                self.assertGreater(observed["pid"], 0)
                with self.assertRaises(subprocess.TimeoutExpired):
                    future.result(timeout=10)
                # Timeout is reported only after owned reap/group disappearance;
                # this extra channel observation must not provoke a TCP reset.
                self.assertTrue(startup_channel_is_closed(peer),
                                f"Startup channel remained open after owned timeout: {observed}")
        finally:
            # RED cleanup releases only this socket-bound fixture child. Never
            # signal an unreserved numeric PID/group or another user's process.
            if peer is not None:
                try:
                    peer.sendall(b"x")
                    self.assertEqual(peer.recv(1), b"")
                except (BrokenPipeError, ConnectionResetError):
                    pass
                peer.close()
            listener.close()

    @unittest.skipIf(os.name == "nt", "The interpreter protocol bootstrap uses POSIX exec")
    def test_cleanup_protocol_bootstrap_is_isolated_and_does_not_write_home_bytecode(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        import prepare_efsw_git as owner
        observation = self.root / "cleanup-bootstrap.json"
        interpreter = self.cleanup_failure_interpreter("clone", observation=observation)
        home = self.root / "protocol-home"
        environment = dict(self.environment, HOME=str(home))
        result = owner._run(
            [str(interpreter), "-I", "-S", str(ROOT / "scripts/prepare_efsw_git.py"), "--check-interpreter"],
            timeout=5, env=environment,
        )
        self.assertEqual(result.kind, "ok", result.stderr.decode())
        flags = json.loads(observation.read_text())
        self.assertFalse(home.exists(), f"Protocol bootstrap wrote HOME cache; flags={flags}")
        self.assertEqual(flags, {"isolated": 1, "no_site": 1, "dont_write_bytecode": True})

    @unittest.skipIf(os.name == "nt", "The fake interpreter status protocol uses a POSIX executable")
    def test_first_config_cleanup_failure_preserves_uncertain_owned_state(self):
        self.assert_cleanup_failure_preserves_owned_residue("config-1")

    @unittest.skipIf(os.name == "nt", "The fake interpreter status protocol uses a POSIX executable")
    def test_second_config_cleanup_failure_preserves_uncertain_owned_state(self):
        self.assert_cleanup_failure_preserves_owned_residue("config-2")

    @unittest.skipIf(os.name == "nt", "The fake interpreter status protocol uses a POSIX executable")
    def test_clone_cleanup_failure_preserves_uncertain_owned_state(self):
        self.assert_cleanup_failure_preserves_owned_residue("clone")

    @unittest.skipIf(os.name == "nt", "The fake interpreter status protocol uses a POSIX executable")
    def test_checkout_cleanup_failure_preserves_uncertain_owned_state(self):
        self.assert_cleanup_failure_preserves_owned_residue("checkout")

    @unittest.skipIf(os.name == "nt", "The fake interpreter status protocol uses a POSIX executable")
    def test_staged_query_cleanup_failure_preserves_uncertain_owned_state(self):
        self.assert_cleanup_failure_preserves_owned_residue("staged-revision")

    def assert_owned_git_routes(self, source):
        self.assertNotIn("COMMAND git", source)
        git_commands = [block for block in re.findall(r"execute_process\((.*?)\n\s*\)", source, re.S)
                        if '"${_git_owner_script}"' in block]
        self.assertTrue(git_commands)
        for command in git_commands:
            self.assertNotIn("TIMEOUT ${GIT_TIMEOUT}", command)
        self.assertNotIn("-E env", source)
        self.assertEqual(source.count("--operation inherit"), 5)
        self.assertEqual(source.count("--operation config"), 2)
        self.assertEqual(source.count("--operation clone"), 1)
        self.assertEqual(source.count('GIT_OWNER "${_git_owner_script}"'), 2)
        self.assertIn("--check-interpreter", source)

    def test_preparation_has_one_owned_route_for_every_git_operation(self):
        self.assert_owned_git_routes(PREPARE_SCRIPT.read_text())

    def test_owned_git_route_guard_rejects_bare_git_and_cmake_timeout_mutants(self):
        source = PREPARE_SCRIPT.read_text()
        clone = next(block for block in re.findall(r"execute_process\((.*?)\n\s*\)", source, re.S)
                     if "--operation clone" in block)
        bare = clone.replace('COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"',
                             "COMMAND git")
        timeout = clone.replace("RESULT_VARIABLE _clone_result",
                                "TIMEOUT ${GIT_TIMEOUT}\n  RESULT_VARIABLE _clone_result")
        for mutant in (bare, timeout):
            self.assertNotEqual(mutant, clone)
            with self.assertRaises(AssertionError):
                self.assert_owned_git_routes(source.replace(clone, mutant))

    def test_shared_verifier_falls_back_only_for_ordinary_owned_git_failure(self):
        owner = self.root / "status-owner.py"
        script = self.root / "supervised-verifier.cmake"
        script.write_text(
            f"include([[{(ROOT / 'cmake' / 'verify_pinned_source.cmake').as_posix()}]])\n"
            f"klogg_require_pinned_source(efsw [[{self.cache.as_posix()}]] {self.revision}\n"
            f"  GIT_OWNER [[{owner.as_posix()}]] PYTHON_EXECUTABLE [[{pathlib.Path(sys.executable).as_posix()}]]\n"
            f"  GIT_TIMEOUT 2 {self.clean_digest})\n"
        )
        for status in (1, 2, 3, 4, 5):
            with self.subTest(status=status):
                owner.write_text(
                    "import sys\nsys.stderr.write('original owner failure\\n')\n"
                    f"sys.exit({status})\n"
                )
                result = subprocess.run(
                    ["cmake", "-P", str(script)], capture_output=True, text=True,
                    env=self.environment, check=False,
                )
                diagnostic = " ".join((result.stdout + result.stderr).split())
                if status == 1:
                    self.assertEqual(result.returncode, 0, diagnostic)
                    self.assertIn("Git metadata is incomplete", diagnostic)
                else:
                    self.assertNotEqual(result.returncode, 0, diagnostic)
                    self.assertIn(f"status {status}", diagnostic)
                    self.assertIn("original owner failure", diagnostic)
                    self.assertNotIn("Git metadata is incomplete", diagnostic)

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
        namespace_before = directory_namespace(self.cache)

        result = self.prepare(destination)

        self.assertEqual(directory_namespace(self.cache), namespace_before)
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

    def assert_dangling_alias_cannot_mutate_cache_namespace(self, metadata_free):
        if metadata_free:
            shutil.rmtree(self.cache / ".git")
        outside = self.root / "outside-witness"
        outside.mkdir()
        (outside / "sentinel").write_bytes(b"do not modify\n")
        for index, suffix in enumerate(("", "efsw", "nested/further/efsw")):
            with self.subTest(suffix=suffix):
                target = self.cache / f"new-parent-{index}"
                alias = self.root / f"dangling-alias-{index}"
                alias.symlink_to(target, target_is_directory=True)
                self.assertFalse(alias.exists())
                self.assertTrue(alias.is_symlink())
                cache_before = directory_namespace(self.cache)
                root_before = directory_namespace(self.root)
                destination = alias / suffix if suffix else alias

                result = self.prepare(destination, python=sys.executable)

                self.assertEqual(directory_namespace(self.cache), cache_before)
                root_after = directory_namespace(self.root)
                self.assertEqual(root_after, root_before,
                                 f"Added namespace: {sorted(set(root_after) - set(root_before))}; "
                                 f"preparation: {result.stdout + result.stderr}")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(target.exists())
                self.assertEqual(os.readlink(alias), str(target))

    @unittest.skipUnless(os.name == "posix", "POSIX dangling directory alias contract")
    def test_dangling_alias_into_cache_preserves_complete_snapshot_cache_namespace(self):
        self.assert_dangling_alias_cannot_mutate_cache_namespace(metadata_free=True)

    @unittest.skipUnless(os.name == "posix", "POSIX dangling directory alias contract")
    def test_dangling_alias_into_cache_preserves_complete_pinned_git_cache_namespace(self):
        self.assert_dangling_alias_cannot_mutate_cache_namespace(metadata_free=False)

    @unittest.skipUnless(os.name == "posix", "POSIX existing directory alias contract")
    def test_existing_destination_parent_alias_preserves_both_materialization_routes(self):
        for metadata_free in (False, True):
            with self.subTest(metadata_free=metadata_free):
                if metadata_free:
                    shutil.rmtree(self.cache / ".git")
                target = self.root / f"resolved-parent-{metadata_free}"
                target.mkdir(mode=0o700)
                alias = self.root / f"resolved-alias-{metadata_free}"
                alias.symlink_to(target, target_is_directory=True)
                cache_before = directory_namespace(self.cache)
                result = self.prepare(alias / "new-parent" / "efsw")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(source_tree_digest(target / "new-parent" / "efsw"), self.clean_digest)
                self.assertEqual(directory_namespace(self.cache), cache_before)
                self.assertTrue(alias.is_symlink())

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

    def assert_verified_clean_content_materializes(self, metadata_kind):
        # This is the approved-content contract, not a foreign-owner Git model.
        self.environment["GIT_CEILING_DIRECTORIES"] = str(self.root)
        if metadata_kind == "absent":
            shutil.rmtree(self.cache / ".git")
        else:
            (self.cache / ".git" / "HEAD").unlink()
        witness = self.root / "external-witness"
        witness.write_bytes(b"independent fixture bytes\n")
        cache_bytes_before = directory_bytes(self.cache)
        cache_ownership_before = directory_ownership(self.cache)
        cache_modes_before = {
            path.relative_to(self.cache).as_posix(): path.lstat().st_mode
            for path in [self.cache] + list(self.cache.rglob("*"))
        }
        verifier = self.root / "verify-clean-content.cmake"
        verifier.write_text(
            f"include([[{(ROOT / 'cmake' / 'verify_pinned_source.cmake').as_posix()}]])\n"
            f"klogg_require_pinned_source(efsw [[{self.cache.as_posix()}]] {self.revision}\n"
            f"  GIT_OWNER [[{(ROOT / 'scripts' / 'prepare_efsw_git.py').as_posix()}]]\n"
            f"  PYTHON_EXECUTABLE [[{pathlib.Path(sys.executable).as_posix()}]]\n"
            f"  GIT_TIMEOUT 2 {self.clean_digest})\n"
            'message(STATUS "Independent approved clean content verification succeeded")\n'
        )
        verified = subprocess.run(
            ["cmake", "-P", str(verifier)], capture_output=True, text=True,
            env=self.environment, cwd=self.root, timeout=10, check=False,
        )
        self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
        self.assertIn("Independent approved clean content verification succeeded", verified.stdout)
        if metadata_kind == "unusable":
            self.assertIn("Git metadata is incomplete", verified.stdout)
        print(f"Approved clean content independently verified ({metadata_kind}): {self.clean_digest}", flush=True)
        destination = self.root / "private" / "efsw"

        result = self.prepare(destination, cwd=self.root)

        self.assertEqual(directory_bytes(self.cache), cache_bytes_before)
        self.assertEqual(directory_ownership(self.cache), cache_ownership_before)
        self.assertEqual(
            {
                path.relative_to(self.cache).as_posix(): path.lstat().st_mode
                for path in [self.cache] + list(self.cache.rglob("*"))
            },
            cache_modes_before,
        )
        self.assertEqual(witness.read_bytes(), b"independent fixture bytes\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((destination / "value.txt").read_bytes(), b"clean source\n")
        self.assertEqual(source_tree_digest(destination), self.clean_digest)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_verified_clean_cache_without_git_metadata_materializes_private_source(self):
        self.assert_verified_clean_content_materializes("absent")

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_verified_clean_cache_with_unusable_git_metadata_materializes_private_source(self):
        self.assert_verified_clean_content_materializes("unusable")

    def make_typed_snapshot(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        import prepare_efsw_snapshot as snapshot
        self.environment["GIT_CEILING_DIRECTORIES"] = str(self.root)
        shutil.rmtree(self.cache / ".git")
        destination = self.root / "private" / "efsw"
        destination.parent.mkdir()
        snapshot.materialize(self.cache, destination, self.clean_digest)
        self.private_marker(destination).write_bytes(
            snapshot.marker_bytes(self.revision, self.clean_digest, self.patched_digest)
        )
        return destination

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_missing_cache_head_never_reads_the_enclosing_repository_revision(self):
        subprocess.run(self.git + ["init", "-q", str(self.root)], env=self.environment, check=True)
        subprocess.run(self.git + ["-C", str(self.root), "-c", "user.name=Fixture",
                                  "-c", "user.email=fixture@example.invalid", "commit", "-q",
                                  "--allow-empty", "-m", "unrelated enclosing revision"],
                       env=self.environment, check=True)
        parent_before = directory_bytes(self.root / ".git")
        (self.cache / ".git" / "HEAD").unlink()
        destination = self.root / "private" / "efsw"
        result = self.prepare(destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source_tree_digest(destination), self.clean_digest)
        self.assertEqual(directory_bytes(self.root / ".git"), parent_before)
        self.assertFalse((destination / ".git").exists())

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_typed_clean_snapshot_reuses_without_inventing_pinned_git_head(self):
        destination = self.make_typed_snapshot()
        before = directory_bytes(destination)
        result = self.prepare(destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((destination / ".git").exists())
        self.assertEqual(directory_bytes(destination), before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_patch_and_reconfigure_preserve_parent_repository_and_hostile_selectors(self):
        destination = self.make_typed_snapshot()
        subprocess.run(self.git + ["init", "-q", str(self.root)], env=self.environment, check=True)
        (self.root / "value.txt").write_bytes(b"clean source\n")
        parent_before = directory_bytes(self.root / ".git")
        config_before = self.global_config.read_bytes()
        self.environment.update({
            "GIT_DIR": str(self.root / ".git"), "GIT_COMMON_DIR": str(self.root / ".git"),
            "GIT_WORK_TREE": str(self.root), "GIT_INDEX_FILE": str(self.root / ".git" / "index"),
            "GIT_OBJECT_DIRECTORY": str(self.root / ".git" / "objects"),
            "GIT_ALTERNATE_OBJECT_DIRECTORIES": str(self.root / ".git" / "objects"),
        })
        selectors_before = dict(self.environment)
        for operation in (lambda: self.apply_series(destination, self.patch, self.patched_digest),
                          lambda: self.prepare(destination),
                          lambda: self.apply_series(destination, self.patch, self.patched_digest)):
            result = operation()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source_tree_digest(destination), self.patched_digest)
        self.assertFalse((destination / ".git").exists())
        self.assertEqual(directory_bytes(self.root / ".git"), parent_before)
        self.assertEqual((self.root / "value.txt").read_bytes(), b"clean source\n")
        self.assertEqual((self.cache / "value.txt").read_bytes(), b"clean source\n")
        self.assertEqual(self.global_config.read_bytes(), config_before)
        self.assertEqual(self.environment, selectors_before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_rejects_forged_final_digest_in_marker(self):
        destination = self.make_typed_snapshot()
        (destination / "value.txt").write_bytes(b"unapproved source\n")
        forged = self.private_marker(destination).read_bytes().replace(
            self.patched_digest.encode(), source_tree_digest(destination).encode()
        )
        self.private_marker(destination).write_bytes(forged)
        result = self.prepare(destination)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("independently supplied approval", result.stdout + result.stderr)
        self.assertEqual((destination / "value.txt").read_bytes(), b"unapproved source\n")

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_rejects_git_commit_added_under_snapshot_provenance(self):
        metadata = self.root / "saved-metadata"
        shutil.copytree(self.cache / ".git", metadata)
        destination = self.make_typed_snapshot()
        shutil.copytree(metadata, destination / ".git")
        result = self.prepare(destination)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("metadata/commit to be absent", result.stdout + result.stderr)
        self.assertEqual(self.git_output("rev-parse", "HEAD", cwd=destination), self.revision)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_interrupted_known_patch_state_fails_closed_without_marker_authority(self):
        destination = self.make_typed_snapshot()
        first = self.root / "first.patch"
        first.write_bytes(self.patch.read_bytes().replace(b"approved patched cache", b"intermediate source"))
        subprocess.run(self.git + ["apply", str(first)], cwd=destination,
                       env=self.environment, check=True)
        before = directory_bytes(destination)
        for operation in (lambda: self.prepare(destination),
                          lambda: self.apply_series(destination, self.patch, self.patched_digest)):
            result = operation()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("dirty/interrupted state rejected", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), before)

    @unittest.skipUnless(os.name == "posix", "Child-only POSIX umask and descriptor namespace contract")
    def test_snapshot_creates_private_parents_before_lock_under_permissive_child_umasks(self):
        shutil.rmtree(self.cache / ".git")
        source_before = directory_bytes(self.cache)
        probe_before = self.root / "caller-mode-before"
        probe_before.write_bytes(b"caller probe\n")
        caller_mode = probe_before.stat().st_mode & 0o777
        for mask in (0o002, 0o000):
            with self.subTest(mask=mask):
                ancestor = self.root / f"mask-{mask:o}"
                ancestor.mkdir(mode=0o700)
                destination = ancestor / "first" / "second" / "efsw"
                result = self.prepare(destination, child_umask=mask)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for parent in (ancestor / "first", destination.parent):
                    self.assertEqual(parent.stat().st_mode & 0o777, 0o700)
                self.assertEqual(source_tree_digest(destination), self.clean_digest)
                repeated = self.prepare(destination, child_umask=mask)
                self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
        probe_after = self.root / "caller-mode-after"
        probe_after.write_bytes(b"caller probe\n")
        self.assertEqual(probe_after.stat().st_mode & 0o777, caller_mode)
        self.assertEqual(directory_bytes(self.cache), source_before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_counterfeit_pinned_marker_cannot_downgrade_to_unowned_patch_route(self):
        destination = self.make_typed_snapshot()
        self.private_marker(destination).write_bytes((self.revision + "\n").encode())
        before = directory_bytes(destination)
        result = self.apply_series(destination, self.patch, self.patched_digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(directory_bytes(destination), before)
        self.assertFalse((destination / ".git").exists())

    def test_marked_private_git_rejects_marker_pin_mismatch_before_patching(self):
        destination = self.root / "private" / "efsw"
        prepared = self.prepare(destination)
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        self.private_marker(destination).write_bytes(b"0" * 40 + b"\n")
        before = directory_bytes(destination)
        result = self.apply_series(destination, self.patch, self.patched_digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("independently supplied pin", " ".join((result.stdout + result.stderr).split()))
        self.assertEqual(directory_bytes(destination), before)

    def test_marked_private_git_rejects_shared_common_directory_selector(self):
        destination = self.root / "private" / "efsw"
        prepared = self.prepare(destination)
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        cache_before = directory_bytes(self.cache)
        before = directory_bytes(destination)
        self.environment["GIT_COMMON_DIR"] = str(self.cache / ".git")
        result = self.apply_series(destination, self.patch, self.patched_digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--git-common-dir", result.stdout + result.stderr)
        self.assertEqual(directory_bytes(destination), before)
        self.assertEqual(directory_bytes(self.cache), cache_before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_ordinary_git_failure_keeps_bad_config_primary_diagnostic(self):
        destination = self.make_typed_snapshot()
        self.global_config.write_bytes(b"[unterminated section\n")
        before = directory_bytes(destination)
        result = self.apply_series(destination, self.patch, self.patched_digest)
        self.assertNotEqual(result.returncode, 0)
        diagnostic = " ".join((result.stdout + result.stderr).split())
        self.assertIn("bad config", diagnostic)
        self.assertIn(str(self.global_config), diagnostic)
        self.assertEqual(directory_bytes(destination), before)

    @unittest.skipUnless(os.name == "posix", "Owned driver protocol fixture requires POSIX exec")
    def test_snapshot_primary_error_precedes_secondary_rollback_failure(self):
        destination = self.make_typed_snapshot()
        interpreter = self.root / "ordinary-patch-python"
        second = self.root / "second.patch"
        second.write_bytes(self.patch.read_bytes())
        interpreter.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, sys\n"
            "arguments = sys.argv[1:]\n"
            "if any(pathlib.Path(arg).name == 'prepare_efsw_git.py' for arg in arguments) and '--' in arguments:\n"
            "    command = arguments[arguments.index('--') + 1:]\n"
            "    if '--reverse' in command:\n"
            "        sys.stderr.write('secondary rollback fixture failure\\n'); sys.exit(1)\n"
            f"    if '--check' in command and {str(second)!r} in command:\n"
            "        sys.stderr.write('primary malformed-config fixture failure\\n'); sys.exit(1)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}] + arguments)\n"
        )
        interpreter.chmod(0o755)
        result = self.apply_series(destination, f"{self.patch};{second}", self.patched_digest,
                                   python=interpreter)
        self.assertNotEqual(result.returncode, 0)
        diagnostic = " ".join(result.stderr.split())
        self.assertIn("primary malformed-config fixture failure", diagnostic)
        self.assertIn("Cannot cleanly roll back patch", diagnostic)
        self.assertLess(diagnostic.index("primary malformed-config fixture failure"),
                        diagnostic.index("Cannot cleanly roll back patch"))

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_namespace_rejection_precedes_lockfile_writes(self):
        destination = self.make_typed_snapshot()
        self.root.chmod(0o777)
        before = directory_bytes(destination)
        for operation in (lambda: self.prepare(destination),
                          lambda: self.apply_series(destination, self.patch, self.patched_digest)):
            result = operation()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("namespace ancestor", " ".join((result.stdout + result.stderr).split()))
        self.assertFalse((destination.parent / "klogg-efsw-prepare.lock").exists())
        self.assertFalse((destination.parent / "klogg-patch.lock").exists())
        self.assertEqual(directory_bytes(destination), before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_rejects_dangling_marker_redirect_without_writing_external_target(self):
        shutil.rmtree(self.cache / ".git")
        destination = self.root / "private" / "efsw"
        destination.parent.mkdir()
        witness = self.root / "external-marker-witness"
        self.private_marker(destination).symlink_to(witness)
        result = self.prepare(destination)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(witness.exists())
        self.assertFalse(destination.exists())
        self.assertTrue(self.private_marker(destination).is_symlink())

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_patch_owner_fatal_statuses_never_retry_or_rollback_uncertain_state(self):
        destination = self.make_typed_snapshot()
        interpreter = self.root / "fatal-patch-python"
        calls = self.root / "owned-patch-calls"
        interpreter.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, sys\n"
            "arguments = sys.argv[1:]\n"
            "if any(pathlib.Path(arg).name == 'prepare_efsw_git.py' for arg in arguments) and 'snapshot-patch' in arguments:\n"
            f"    with open({str(calls)!r}, 'a') as output: output.write('owned-patch\\n')\n"
            "    sys.stderr.write('primary owned patch fixture failure\\n')\n"
            "    sys.exit(int(os.environ['KLOGG_FIXTURE_PATCH_FAILURE']))\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}] + arguments)\n"
        )
        interpreter.chmod(0o755)
        before = directory_bytes(destination)
        for status in (2, 3, 4, 5):
            with self.subTest(status=status):
                calls.write_bytes(b"")
                self.environment["KLOGG_FIXTURE_PATCH_FAILURE"] = str(status)
                result = self.apply_series(destination, self.patch, self.patched_digest, python=interpreter)
                self.assertNotEqual(result.returncode, 0)
                diagnostic = " ".join((result.stdout + result.stderr).split())
                self.assertIn(f"status {status}", diagnostic)
                self.assertIn("primary owned patch fixture failure", diagnostic)
                self.assertEqual(calls.read_bytes(), b"owned-patch\n")
                self.assertEqual(directory_bytes(destination), before)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_snapshot_patch_failure_rolls_back_to_authenticated_clean_bytes(self):
        destination = self.make_typed_snapshot()
        missing = self.root / "missing.patch"
        result = self.apply_series(destination, f"{self.patch};{missing}", self.patched_digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Patch file does not exist", result.stdout + result.stderr)
        self.assertEqual(source_tree_digest(destination), self.clean_digest)
        reused = self.prepare(destination)
        self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)

    @unittest.skipUnless(os.name == "posix", "Snapshot capture requires descriptor-relative no-follow IO")
    def test_metadata_free_approved_patched_cache_is_not_reconstructed_as_clean(self):
        subprocess.run(self.git + ["apply", str(self.patch)], cwd=self.cache,
                       env=self.environment, check=True)
        shutil.rmtree(self.cache / ".git")
        destination = self.root / "private" / "efsw"
        result = self.prepare(destination, approved_cache_digests=[self.clean_digest, self.patched_digest])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Clean source tree SHA-256 mismatch", " ".join((result.stdout + result.stderr).split()))
        self.assert_no_private_source(destination)

    def test_partial_git_cache_fails_within_the_subprocess_bound_and_cleans_staging(self):
        partial = self.root / "partial-cache"
        shutil.copytree(self.cache, partial)
        partial_revision = self.git_output("rev-parse", "HEAD", cwd=partial)
        shutil.rmtree(partial / ".git" / "objects")
        # Keep an invalid fixture from discovering this worktree's real HEAD.
        self.environment["GIT_CEILING_DIRECTORIES"] = str(self.root)
        destination = self.root / "private" / "efsw"

        result = self.prepare(
            destination,
            cache=partial,
            revision=partial_revision,
            timeout=5,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("retaining HEAD", result.stdout + result.stderr)
        self.assertFalse(destination.exists())
        self.assertEqual(list(destination.parent.glob(".efsw.prepare-*")), [])

    def test_readable_pin_with_missing_commit_objects_never_falls_back_after_clone(self):
        partial = self.root / "partial-cache"
        shutil.copytree(self.cache, partial)
        shutil.rmtree(partial / ".git" / "objects")
        (partial / ".git" / "objects").mkdir()
        self.assertEqual(self.git_output("rev-parse", "HEAD", cwd=partial), self.revision)
        destination = self.root / "private" / "efsw"
        result = self.prepare(destination, cache=partial)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to clone", result.stdout + result.stderr)
        self.assert_no_private_source(destination)

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
