"""Windows core-directory policy models and separate native acceptance."""

import ctypes
import dataclasses
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_windows_security as security

USER = "S-1-5-21-100-200-300-1001"
FOREIGN = "S-1-5-21-100-200-300-1002"
INSTALLER = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"
PARENT = "C:\\Users\\runner"
PRIVATE = PARENT + "\\private"


def private_security():
    return security.DirectorySecurity(
        USER, True, True,
        (security.AccessEntry(0, 3, security.FILE_ALL_ACCESS, USER),),
    )


def ancestor_security():
    return security.DirectorySecurity(
        security.SYSTEM_SID, True, False,
        (security.AccessEntry(0, 0, security.FILE_ALL_ACCESS, security.SYSTEM_SID),
         security.AccessEntry(0, 0, 0x00120089, "S-1-1-0")),
    )


class FakeNativeSecurity:
    """Native API model, not Windows ACL or namespace execution evidence."""

    def __init__(self):
        self.records = {
            path: security.DirectoryRecord((1, index + 1), 0x10, path,
                                           private_security() if path == PRIVATE else ancestor_security())
            for index, path in enumerate(security.directory_chain(PRIVATE))
        }
        self.opened = []
        self.closed = []
        self.created = []
        self.removed = []
        self.local_ntfs = True
        self.open_failure = None
        self.close_failure = None
        self.inspect_hook = None

    def current_user_sid(self):
        return USER

    def open_directory(self, path):
        if path == self.open_failure:
            raise security.WindowsSecurityError("open failed")
        if path not in self.records:
            raise security.WindowsSecurityError("missing directory")
        self.opened.append(path)
        return path

    def close_directory(self, handle):
        self.closed.append(handle)
        if handle == self.close_failure:
            raise security.WindowsSecurityError("close failed")

    def inspect_directory(self, handle):
        if self.inspect_hook:
            self.inspect_hook(handle)
        return self.records[handle]

    def paths_equal(self, left, right):
        return left == right

    def is_local_ntfs(self, path, handle):
        return self.local_ntfs

    def create_private_directory(self, path, user_sid):
        if path in self.records:
            raise security.WindowsSecurityError("destination already exists")
        self.created.append((path, user_sid))
        self.records[path] = security.DirectoryRecord((1, 99), 0x10, path, private_security())

    def remove_created_directory(self, path):
        self.removed.append(path)
        del self.records[path]


def run_native_acl_probe(path, command):
    # Keep the built-in Windows PowerShell runtime and module namespace coherent.
    try:
        chain = security.directory_chain(os.environ.get("SystemRoot", ""))
    except security.WindowsSecurityError as error:
        raise ValueError("independent ACL probe requires a canonical Windows SystemRoot") from error
    if len(chain) < 2:
        raise ValueError("independent ACL probe requires a Windows SystemRoot directory")
    runtime = pathlib.PureWindowsPath(chain[-1]) / "System32" / "WindowsPowerShell" / "v1.0"
    environment = {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}
    environment["PSModulePath"] = str(runtime / "Modules")
    environment["KLOGG_NATIVE_ACL_TEST_PATH"] = str(path)
    return subprocess.run(
        [str(runtime / "powershell.exe"), "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
        env=environment, text=True, capture_output=True, check=True, timeout=30)


def create_native_fixture_parent(parent, name):
    try:
        return security.create_private_parent(parent, name)
    except security.WindowsSecurityError as error:
        context = getattr(error, "windows_namespace_context", None)
        if context is not None:
            comparison = {"available": False, "sampled_after_rejection": True}
            try:
                inspect = (
                    "$a=Get-Acl -LiteralPath $env:KLOGG_NATIVE_ACL_TEST_PATH;"
                    "[pscustomobject]@{owner=$a.GetOwner([System.Security.Principal.SecurityIdentifier]).Value;"
                    "user=[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value}"
                    "|ConvertTo-Json -Compress"
                )
                completed = run_native_acl_probe(context["path"], inspect)
                observed = json.loads(completed.stdout)
                if (not isinstance(observed, dict)
                        or any(not isinstance(observed.get(key), str)
                               or len(observed[key]) > 192 for key in ("owner", "user"))):
                    raise ValueError("invalid independent identity response")
                comparison.update(
                    available=True, owner_sid=observed["owner"], user_sid=observed["user"],
                    owner_matches=observed["owner"] == context["owner_sid"],
                    user_matches=observed["user"] == context["user_sid"],
                )
            except Exception as failure:
                comparison["failure_type"] = type(failure).__name__
                if isinstance(failure, subprocess.CalledProcessError):
                    comparison["returncode"] = failure.returncode
                    comparison["stdout"] = security._diagnostic_text(failure.stdout, 512)
                    comparison["stderr"] = security._diagnostic_text(failure.stderr, 512)
                else:
                    comparison["failure"] = security._diagnostic_text(str(failure), 192)
            error.windows_namespace_independent = comparison
            try:
                print("Independent native namespace snapshot: " + json.dumps(comparison), file=sys.stderr)
            except Exception:
                pass
        raise


class DirectoryPolicyModelTest(unittest.TestCase):
    def test_protected_owner_only_private_directory_is_accepted(self):
        security.validate_directory_security(private_security(), USER, private=True)

    def installer_root_api(self):
        api = FakeNativeSecurity()
        descriptor = security.DirectorySecurity(INSTALLER, True, False, (
            security.AccessEntry(0, 11, 0xe0010000, "S-1-5-11"),
            security.AccessEntry(0, 3, security.FILE_ALL_ACCESS, security.SYSTEM_SID),
            security.AccessEntry(0, 3, security.FILE_ALL_ACCESS, security.ADMINISTRATORS_SID),
        ))
        api.records["C:\\"] = dataclasses.replace(api.records["C:\\"], security=descriptor)
        return api

    def test_documented_servicing_owner_is_supported_only_for_leased_ntfs_root(self):
        api = self.installer_root_api()
        created = security.create_private_parent(PARENT, "new-private", api=api)
        self.assertEqual(str(created), PARENT + "\\new-private")
        self.assertEqual(api.created, [(str(created), USER)])
        with security.private_parent_lease(str(created), api=api):
            pass
        self.assertCountEqual(api.closed, api.opened)

    def test_servicing_root_owner_does_not_trust_descendant_private_or_unknown_owners(self):
        for path, owner in ((PARENT, INSTALLER), (PRIVATE, INSTALLER),
                            ("C:\\", FOREIGN), ("C:\\", INSTALLER + "1")):
            with self.subTest(path=path, owner=owner):
                api = self.installer_root_api()
                api.records[path] = dataclasses.replace(
                    api.records[path], security=dataclasses.replace(api.records[path].security, owner=owner))
                with self.assertRaisesRegex(security.WindowsSecurityError, "owner"):
                    with security.private_parent_lease(PRIVATE, api=api):
                        self.fail("untrusted owner entered lease")
                self.assertEqual(api.created, [])
                self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_servicing_root_owner_never_expands_mutation_grant_trust(self):
        for sid in (INSTALLER, FOREIGN):
            with self.subTest(sid=sid):
                api = self.installer_root_api()
                record = api.records["C:\\"]
                api.records["C:\\"] = dataclasses.replace(record, security=dataclasses.replace(
                    record.security, entries=record.security.entries + (
                        security.AccessEntry(0, 0, security.FILE_ALL_ACCESS, sid),)))
                with self.assertRaisesRegex(security.WindowsSecurityError, "mutation grant"):
                    security.create_private_parent(PARENT, "new-private", api=api)
                self.assertEqual(api.created, [])
                self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_servicing_root_exception_cannot_bypass_namespace_checks_or_revalidation(self):
        for violation in ("filesystem", "reparse", "alias", "identity", "volume"):
            with self.subTest(violation=violation):
                api = self.installer_root_api()
                root = api.records["C:\\"]
                if violation == "filesystem":
                    api.local_ntfs = False
                elif violation == "reparse":
                    api.records["C:\\"] = dataclasses.replace(root, attributes=0x410)
                elif violation == "alias":
                    api.records["C:\\"] = dataclasses.replace(root, path="D:\\")
                elif violation == "identity":
                    api.records["C:\\"] = dataclasses.replace(root, identity=(1, 0))
                else:
                    api.records[PARENT] = dataclasses.replace(api.records[PARENT], identity=(2, 10))
                with self.assertRaises(security.WindowsSecurityError):
                    security.create_private_parent(PARENT, "new-private", api=api)
                self.assertEqual(api.created, [])
                self.assertEqual(api.closed, list(reversed(api.opened)))
        api = self.installer_root_api()
        with self.assertRaisesRegex(security.WindowsSecurityError, "identity changed"):
            with security.private_parent_lease(PRIVATE, api=api):
                api.records["C:\\"] = dataclasses.replace(api.records["C:\\"], identity=(1, 999))
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_platform_ancestor_with_no_foreign_mutation_grant_is_accepted(self):
        security.validate_directory_security(ancestor_security(), USER, private=False)

    def test_each_unprivileged_mutation_right_and_generic_write_is_rejected(self):
        for mask in (0x2, 0x10, 0x40, 0x100, 0x10000, 0x40000, 0x80000,
                     0x40000000, 0x10000000):
            changed = dataclasses.replace(ancestor_security(), entries=(
                security.AccessEntry(0, 0, mask, FOREIGN),))
            with self.subTest(mask=mask), self.assertRaises(security.WindowsSecurityError):
                security.validate_directory_security(changed, USER, private=False)

    def test_unprivileged_owner_null_dacl_unknown_ace_and_unknown_right_are_rejected(self):
        for changed in (
            dataclasses.replace(ancestor_security(), owner=FOREIGN),
            dataclasses.replace(ancestor_security(), dacl_present=False),
            dataclasses.replace(ancestor_security(), entries=(security.AccessEntry(5, 0, 0, USER),)),
            dataclasses.replace(ancestor_security(), entries=(security.AccessEntry(0, 0x40, 0, USER),)),
            dataclasses.replace(ancestor_security(), entries=(security.AccessEntry(0, 0, 0x02000000, USER),)),
        ):
            with self.subTest(changed=changed), self.assertRaises(security.WindowsSecurityError):
                security.validate_directory_security(changed, USER, private=False)

    def test_private_root_rejects_extra_trusted_grants_unprotected_and_inherited_aces(self):
        for changed in (
            dataclasses.replace(private_security(), owner=security.SYSTEM_SID),
            dataclasses.replace(private_security(), protected=False),
            dataclasses.replace(private_security(), entries=(
                security.AccessEntry(0, 3, security.FILE_ALL_ACCESS, USER),
                security.AccessEntry(0, 3, security.FILE_ALL_ACCESS, security.SYSTEM_SID))),
            dataclasses.replace(private_security(), entries=(
                security.AccessEntry(0, 0x13, security.FILE_ALL_ACCESS, USER),)),
            dataclasses.replace(private_security(), entries=(security.AccessEntry(0, 3, 0x120089, USER),)),
        ):
            with self.subTest(changed=changed), self.assertRaises(security.WindowsSecurityError):
                security.validate_directory_security(changed, USER, private=True)

    def test_deny_ace_cannot_hide_a_foreign_mutation_allow_grant(self):
        changed = dataclasses.replace(ancestor_security(), entries=(
            security.AccessEntry(1, 0, security.FILE_ALL_ACCESS, FOREIGN),
            security.AccessEntry(0, 0, 0x100, FOREIGN)))
        with self.assertRaises(security.WindowsSecurityError):
            security.validate_directory_security(changed, USER, private=False)

    def test_inherit_only_grants_do_not_apply_to_existing_ancestor(self):
        changed = dataclasses.replace(ancestor_security(), entries=(
            security.AccessEntry(0, 0x0b, security.FILE_ALL_ACCESS, FOREIGN),))
        security.validate_directory_security(changed, USER, private=False)

    def test_child_creation_without_existing_namespace_mutation_is_not_delete_child(self):
        changed = dataclasses.replace(ancestor_security(), entries=(
            security.AccessEntry(0, 0, 0x4, FOREIGN),))
        security.validate_directory_security(changed, USER, private=False)

    def test_device_unc_drive_relative_ads_alias_and_reserved_components_are_rejected(self):
        for path in ("C:private", "\\\\host\\share\\private", "\\\\?\\C:\\private",
                     "C:\\private:stream", "C:\\private.", "C:\\private ", "C:\\..\\private",
                     "C:\\CON.txt", "C:\\COM\u00b9", "C:\\private\\", "C:/private", "C:\\bad\x00name"):
            with self.subTest(path=path), self.assertRaises(security.WindowsSecurityError):
                security.directory_chain(path)
        self.assertEqual(security.directory_chain(PRIVATE),
                         ("C:\\", "C:\\Users", PARENT, PRIVATE))

    def test_all_ancestors_are_audited_and_handles_cover_body_and_close_in_reverse(self):
        api = FakeNativeSecurity()
        with security.private_parent_lease(PRIVATE, api=api) as lease:
            self.assertEqual(api.opened, list(security.directory_chain(PRIVATE)))
            self.assertEqual(api.closed, [])
            lease.verify()
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_unsafe_ancestor_is_rejected_even_when_private_leaf_is_valid(self):
        api = FakeNativeSecurity()
        api.records[PARENT] = dataclasses.replace(api.records[PARENT], security=
            dataclasses.replace(ancestor_security(), entries=(security.AccessEntry(0, 0, 0x100, FOREIGN),)))
        with self.assertRaisesRegex(security.WindowsSecurityError, "mutation"):
            with security.private_parent_lease(PRIVATE, api=api):
                self.fail("unsafe ancestor entered")
        self.assertEqual(api.closed, list(reversed(api.opened)))
        self.assertNotIn(PRIVATE, api.opened)

    def test_owner_rejection_identifies_ancestor_without_creating_or_repairing_it(self):
        api = FakeNativeSecurity()
        api.records[PARENT] = dataclasses.replace(
            api.records[PARENT], security=dataclasses.replace(ancestor_security(), owner=FOREIGN))

        with self.assertRaises(security.WindowsSecurityError) as caught:
            security.create_private_parent(PARENT, "new-private", api=api)

        self.assertTrue(str(caught.exception).startswith("untrusted directory owner can change its DACL"))
        context = getattr(caught.exception, "windows_namespace_context", None)
        self.assertIsNotNone(context, "rejection omits its inspected namespace context")
        self.assertEqual(context["path"], PARENT)
        self.assertEqual(context["owner_sid"], FOREIGN)
        self.assertEqual(context["user_sid"], USER)
        self.assertFalse(context["private"])
        self.assertIn("Windows namespace:", str(caught.exception))
        self.assertEqual(api.created, [])
        self.assertEqual(api.removed, [])
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_namespace_rejections_report_requested_and_handle_observed_paths(self):
        original = FakeNativeSecurity().records[PARENT]
        for reason, changed in (
            ("path aliases", dataclasses.replace(original, path=PARENT + "\\other")),
            ("non-directory or reparse", dataclasses.replace(original, attributes=0x410)),
            ("native directory identity", dataclasses.replace(original, identity=(1, 0))),
        ):
            with self.subTest(reason=reason):
                api = FakeNativeSecurity()
                api.records[PARENT] = changed
                with self.assertRaisesRegex(security.WindowsSecurityError, reason) as caught:
                    security.create_private_parent(PARENT, "new-private", api=api)
                context = getattr(caught.exception, "windows_namespace_context", None)
                self.assertIsNotNone(context, "namespace rejection omits inspected paths")
                self.assertEqual(context["path"], PARENT)
                self.assertEqual(context["observed_path"], changed.path)
                self.assertIn("observed_path=", str(caught.exception))
                self.assertTrue(str(caught.exception).startswith("directory lease rejects")
                                or str(caught.exception).startswith("invalid native directory identity"))
                self.assertEqual(api.created, [])
                self.assertEqual(api.removed, [])
                self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_bounded_rejection_context_preserves_primary_and_secondary_errors(self):
        path = "C:\\" + "\\".join(["a" * 200] * 4)
        api = FakeNativeSecurity()
        api.records = {
            name: security.DirectoryRecord((1, index + 1), 0x10, name, ancestor_security())
            for index, name in enumerate(security.directory_chain(path))
        }
        entries = (
            security.AccessEntry(0, 0, 0x40000000, FOREIGN),
            security.AccessEntry(1, 0, security.FILE_ALL_ACCESS, FOREIGN),
            security.AccessEntry(0, 8, security.FILE_ALL_ACCESS, FOREIGN),
        ) * 4
        api.records[path] = dataclasses.replace(
            api.records[path], security=dataclasses.replace(ancestor_security(), owner=FOREIGN, entries=entries))
        api.close_failure = path
        primary = security.WindowsSecurityError("original refusal")
        primary.__cause__ = ValueError("original cause")
        validation = security.validate_directory_security

        def refuse_leaf(descriptor, user, *, private, servicing_root_owner=False):
            if descriptor.owner == FOREIGN:
                raise primary
            validation(descriptor, user, private=private, servicing_root_owner=servicing_root_owner)

        with mock.patch.object(security, "validate_directory_security", side_effect=refuse_leaf):
            with self.assertRaises(security.WindowsSecurityError) as caught:
                with security.private_parent_lease(path, api=api):
                    self.fail("refused namespace entered")

        self.assertIs(caught.exception, primary)
        self.assertEqual(str(primary.__cause__), "original cause")
        self.assertEqual(primary.windows_security_cleanup_errors, ("close failed",))
        context = getattr(primary, "windows_namespace_context", None)
        self.assertIsNotNone(context, "primary error has no bounded namespace diagnosis")
        self.assertEqual(context["path"], path)
        self.assertEqual(len(context["mutation_entries"]), 4)
        self.assertEqual(context["omitted_entries"], 8)
        self.assertEqual(context["mutation_entries"][0]["mutation_mask"],
                         security._access_mask(0x40000000) & security.MUTATION_RIGHTS)
        self.assertFalse(context["mutation_entries"][1]["applicable_allow"])
        self.assertFalse(context["mutation_entries"][2]["applicable_allow"])
        suffix = str(primary).split("Windows namespace:", 1)[1]
        self.assertLessEqual(len(suffix), 2048)
        self.assertIn("truncated", suffix)
        self.assertTrue(str(primary).startswith("original refusal"))
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_native_refusal_is_independently_observed_and_never_becomes_success(self):
        original_path = os.environ.get("KLOGG_NATIVE_ACL_TEST_PATH")
        for probe_fails in (False, True):
            with self.subTest(probe_fails=probe_fails):
                primary = security.WindowsSecurityError("original native refusal")
                primary.windows_namespace_context = {
                    "path": PARENT, "owner_sid": FOREIGN, "user_sid": USER,
                }
                independent = mock.Mock(stdout=json.dumps({"owner": FOREIGN, "user": USER}))
                with mock.patch.dict(os.environ, {"SystemRoot": "Z:\\Windows"}), \
                        mock.patch.object(security, "create_private_parent", side_effect=primary):
                    with mock.patch.object(subprocess, "run", return_value=independent,
                                           side_effect=OSError("probe unavailable") if probe_fails else None) as run:
                        with mock.patch.object(sys, "stderr"):
                            with self.assertRaises(security.WindowsSecurityError) as caught:
                                create_native_fixture_parent(PARENT, "native-private")
                self.assertIs(caught.exception, primary)
                run.assert_called_once()
                self.assertEqual(run.call_args.kwargs["env"]["KLOGG_NATIVE_ACL_TEST_PATH"], PARENT)
                self.assertEqual(primary.args, ("original native refusal",))
                comparison = getattr(primary, "windows_namespace_independent", None)
                self.assertIsNotNone(comparison, "native rejection has no independent diagnostic")
                self.assertEqual(comparison["available"], not probe_fails)
                if not probe_fails:
                    self.assertTrue(comparison["owner_matches"])
                    self.assertTrue(comparison["user_matches"])
                self.assertEqual(os.environ.get("KLOGG_NATIVE_ACL_TEST_PATH"), original_path)

    def test_independent_probe_error_keeps_exit_and_stderr_before_long_command_text(self):
        primary = security.WindowsSecurityError("original native refusal")
        primary.windows_namespace_context = {
            "path": PARENT, "owner_sid": FOREIGN, "user_sid": USER,
        }
        failure = subprocess.CalledProcessError(
            17, ["powershell.exe", "-Command", "command-prefix" * 1000],
            output="probe-output " * 1000, stderr="ParserError: actual reason " * 1000,
        )
        with mock.patch.dict(os.environ, {"SystemRoot": "Z:\\Windows"}), \
                mock.patch.object(security, "create_private_parent", side_effect=primary):
            with mock.patch.object(subprocess, "run", side_effect=failure):
                with mock.patch.object(sys, "stderr"):
                    with self.assertRaises(security.WindowsSecurityError) as caught:
                        create_native_fixture_parent(PARENT, "native-private")
        self.assertIs(caught.exception, primary)
        self.assertEqual(primary.args, ("original native refusal",))
        comparison = primary.windows_namespace_independent
        self.assertFalse(comparison["available"])
        self.assertEqual(comparison.get("failure_type"), "CalledProcessError")
        self.assertEqual(comparison.get("returncode"), 17)
        self.assertIn("ParserError: actual reason", comparison.get("stderr", ""))
        self.assertLessEqual(len(comparison["stderr"]), 512)
        self.assertLessEqual(len(comparison["stdout"]), 512)
        self.assertIn("truncated", comparison["stderr"])

    def test_independent_probe_uses_only_builtin_modules_for_its_windows_runtime(self):
        # A child-environment model; Get-Acl itself still requires native Windows.
        primary = security.WindowsSecurityError("original native refusal")
        primary.windows_namespace_context = {
            "path": PARENT, "owner_sid": FOREIGN, "user_sid": USER,
        }
        environment = {
            "SystemRoot": "Z:\\Windows",
            "PSModulePath": "Q:\\PowerShell\\7\\Modules;Q:\\UserModules",
            "PSMODULEPATH": "Q:\\InheritedModules",
            "KLOGG_NATIVE_ACL_TEST_PATH": "unchanged caller path",
            "UNRELATED_PROBE_TEST": "preserved",
        }
        independent = mock.Mock(stdout=json.dumps({"owner": FOREIGN, "user": USER}))
        with mock.patch.dict(os.environ, environment, clear=True):
            before = dict(os.environ)
            with mock.patch.object(security, "create_private_parent", side_effect=primary), \
                    mock.patch.object(subprocess, "run", return_value=independent) as run, \
                    mock.patch.object(sys, "stderr"):
                with self.assertRaises(security.WindowsSecurityError) as caught:
                    create_native_fixture_parent(PARENT, "native-private")
            self.assertEqual(dict(os.environ), before)
        self.assertIs(caught.exception, primary)
        run.assert_called_once()
        arguments, options = run.call_args
        runtime = "Z:\\Windows\\System32\\WindowsPowerShell\\v1.0"
        self.assertEqual(arguments[0][:5], [
            runtime + "\\powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
        ])
        self.assertIn("Get-Acl -LiteralPath", arguments[0][5])
        child = options["env"]
        module_keys = [key for key in child if key.upper() == "PSMODULEPATH"]
        self.assertEqual(len(module_keys), 1)
        self.assertEqual(child[module_keys[0]], runtime + "\\Modules")
        self.assertEqual(child["KLOGG_NATIVE_ACL_TEST_PATH"], PARENT)
        self.assertEqual(child["UNRELATED_PROBE_TEST"], "preserved")
        self.assertEqual({key: value for key, value in options.items() if key != "env"}, {
            "text": True, "capture_output": True, "check": True, "timeout": 30,
        })
        self.assertTrue(primary.windows_namespace_independent["available"])

    def test_missing_or_unsafe_system_root_never_launches_another_runtime(self):
        for root in (None, "", "Windows", "Z:Windows", "Z:\\", "Z:\\Windows\\..",
                     "Z:\\Windows.", "Z:\\Windows ", "Z:/Windows", "\\\\host\\Windows",
                     "\\\\?\\Z:\\Windows", "Z:\\Windows:stream"):
            with self.subTest(root=root):
                primary = security.WindowsSecurityError("original native refusal")
                primary.windows_namespace_context = {
                    "path": PARENT, "owner_sid": FOREIGN, "user_sid": USER,
                }
                environment = {} if root is None else {"SystemRoot": root}
                with mock.patch.dict(os.environ, environment, clear=True), \
                        mock.patch.object(security, "create_private_parent", side_effect=primary), \
                        mock.patch.object(subprocess, "run") as run, \
                        mock.patch.object(sys, "stderr"):
                    with self.assertRaises(security.WindowsSecurityError) as caught:
                        create_native_fixture_parent(PARENT, "native-private")
                self.assertIs(caught.exception, primary)
                self.assertEqual(primary.args, ("original native refusal",))
                run.assert_not_called()
                comparison = primary.windows_namespace_independent
                self.assertFalse(comparison["available"])
                self.assertTrue(comparison["sampled_after_rejection"])
                self.assertIn("SystemRoot", comparison["failure"])
                self.assertLessEqual(len(comparison["failure"]), 192)

    def test_filesystem_reparse_volume_path_alias_and_identity_changes_fail_closed(self):
        for changed in (
            dataclasses.replace(FakeNativeSecurity().records[PRIVATE], attributes=0x410),
            dataclasses.replace(FakeNativeSecurity().records[PRIVATE], identity=(2, 4)),
            dataclasses.replace(FakeNativeSecurity().records[PRIVATE], path=PARENT + "\\other"),
        ):
            api = FakeNativeSecurity()
            api.records[PRIVATE] = changed
            with self.subTest(changed=changed), self.assertRaises(security.WindowsSecurityError):
                with security.private_parent_lease(PRIVATE, api=api):
                    self.fail("unsafe namespace entered")
            self.assertEqual(api.closed, list(reversed(api.opened)))
        api = FakeNativeSecurity()
        api.local_ntfs = False
        with self.assertRaisesRegex(security.WindowsSecurityError, "NTFS"):
            with security.private_parent_lease(PRIVATE, api=api):
                self.fail("unsupported filesystem entered")

    def test_exit_revalidation_detects_mutation_and_preserves_prior_error(self):
        api = FakeNativeSecurity()
        with self.assertRaisesRegex(security.WindowsSecurityError, "identity changed"):
            with security.private_parent_lease(PRIVATE, api=api):
                api.records[PRIVATE] = dataclasses.replace(api.records[PRIVATE], identity=(1, 500))
        self.assertEqual(api.closed, list(reversed(api.opened)))
        api = FakeNativeSecurity()
        api.close_failure = PRIVATE
        with self.assertRaisesRegex(ValueError, "primary") as caught:
            with security.private_parent_lease(PRIVATE, api=api):
                raise ValueError("primary")
        self.assertEqual(caught.exception.windows_security_cleanup_errors, ("close failed",))
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_acquisition_failure_closes_only_owned_handles(self):
        api = FakeNativeSecurity()
        api.open_failure = PARENT
        with self.assertRaisesRegex(security.WindowsSecurityError, "open failed"):
            with security.private_parent_lease(PRIVATE, api=api):
                self.fail("failed acquisition entered")
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_successful_body_cannot_hide_close_failure(self):
        api = FakeNativeSecurity()
        api.close_failure = PRIVATE
        with self.assertRaisesRegex(security.WindowsSecurityError, "cleanup failed"):
            with security.private_parent_lease(PRIVATE, api=api):
                pass
        self.assertEqual(api.closed, list(reversed(api.opened)))

    def test_creation_is_fresh_owner_only_and_never_repairs_existing_paths(self):
        api = FakeNativeSecurity()
        created = security.create_private_parent(PARENT, "new-private", api=api)
        self.assertEqual(str(created), PARENT + "\\new-private")
        self.assertEqual(api.created, [(str(created), USER)])
        self.assertEqual(api.removed, [])
        with self.assertRaisesRegex(security.WindowsSecurityError, "already exists"):
            security.create_private_parent(PARENT, "private", api=api)
        self.assertEqual(api.records[PRIVATE].security, private_security())

    def test_created_root_validation_failure_removes_only_new_owned_empty_root(self):
        api = FakeNativeSecurity()
        original = api.create_private_directory

        def bad_creation(path, user):
            original(path, user)
            api.records[path] = dataclasses.replace(api.records[path], security=
                dataclasses.replace(private_security(), protected=False))

        api.create_private_directory = bad_creation
        with self.assertRaises(security.WindowsSecurityError):
            security.create_private_parent(PARENT, "bad-private", api=api)
        self.assertEqual(api.removed, [PARENT + "\\bad-private"])
        self.assertNotIn(PARENT + "\\bad-private", api.records)
        self.assertEqual(api.records[PRIVATE].security, private_security())

    def test_native_name_comparison_distinguishes_api_error_from_unequal_names(self):
        for result in (0, 1, 2, 3):
            with self.subTest(result=result):
                api = security.NativeSecurityApi.__new__(security.NativeSecurityApi)
                api.compare = mock.Mock(return_value=result)
                failure = security.WindowsSecurityError("comparison failed (model Win32 87)")
                api._error = mock.Mock(return_value=failure)
                if result == 0:
                    with self.assertRaises(security.WindowsSecurityError) as caught:
                        api.paths_equal(PARENT, PARENT.lower())
                    self.assertIs(caught.exception, failure)
                    api._error.assert_called_once()
                else:
                    self.assertEqual(api.paths_equal(PARENT, PARENT.lower()), result == 2)
                    api._error.assert_not_called()
                api.compare.assert_called_once_with(PARENT, -1, PARENT.lower(), -1, True)

    def test_real_native_backend_is_unavailable_on_non_windows(self):
        if os.name != "nt":
            with self.assertRaisesRegex(security.WindowsSecurityError, "unavailable"):
                security.NativeSecurityApi()


class NativeDescriptorBufferModelTest(unittest.TestCase):
    """Production pointer/bounds parser over owned model bytes, not Win32 calls."""

    def make_api(self, *, owner_offset=20, ace_kind=0, ace_flags=3, ace_size=36,
                 acl_size=44, ace_count=1, get_ace_succeeds=True):
        raw = bytearray(92)
        raw[48:56] = bytes((2, 0, acl_size & 255, acl_size >> 8, ace_count, 0, 0, 0))
        raw[56:64] = bytes((ace_kind, ace_flags, ace_size & 255, ace_size >> 8)) + \
            security.FILE_ALL_ACCESS.to_bytes(4, "little")
        api = security.NativeSecurityApi.__new__(security.NativeSecurityApi)
        api.HANDLE, api.DWORD = ctypes.c_void_p, ctypes.c_uint32
        api.buffer = ctypes.create_string_buffer(bytes(raw))
        base = ctypes.addressof(api.buffer)
        api.freed = []
        api._error = lambda label: security.WindowsSecurityError(label + " (model API failure)")
        api.free = lambda pointer: api.freed.append(pointer.value)
        api.valid_descriptor = lambda pointer: True
        api.descriptor_length = lambda pointer: 92
        api._sid_text = lambda pointer, maximum: USER
        api.sid_length = lambda pointer: 28

        def fetch(handle, kind, flags, owner, group, dacl, sacl, descriptor):
            owner._obj.value = base + owner_offset
            dacl._obj.value = base + 48
            descriptor._obj.value = base
            return 0

        def control(descriptor, value, revision):
            value._obj.value = 0x1004
            revision._obj.value = 1
            return True

        def ace(acl, index, pointer):
            if get_ace_succeeds:
                pointer._obj.value = base + 56
            return get_ace_succeeds

        api.security, api.control, api.get_ace = fetch, control, ace
        return api

    def test_native_parser_accepts_bounded_owner_only_model_and_frees_descriptor(self):
        api = self.make_api()
        self.assertEqual(api._security(1), private_security())
        self.assertEqual(api.freed, [ctypes.addressof(api.buffer)])

    def test_native_parser_rejects_owner_acl_ace_bounds_and_unknown_semantics(self):
        for arguments in (
            {"owner_offset": 100}, {"owner_offset": 88}, {"acl_size": 48},
            {"ace_kind": 5}, {"ace_size": 40}, {"ace_size": 12},
            {"get_ace_succeeds": False},
        ):
            api = self.make_api(**arguments)
            with self.subTest(arguments=arguments), self.assertRaises(security.WindowsSecurityError):
                api._security(1)
            self.assertEqual(api.freed, [ctypes.addressof(api.buffer)])


class NativeFixturePortabilityModelTest(unittest.TestCase):
    def test_owned_temporary_parent_is_canonical_before_native_backend_selection(self):
        # Exercise setup only: no alias privilege or native Windows call is needed.
        case = NativeDirectorySecurityTest("test_atomic_owner_only_creation_and_native_lease")
        with tempfile.TemporaryDirectory() as owned:
            canonical = pathlib.Path(owned).resolve(strict=True)
            (canonical / "alias-child").mkdir()
            alias = canonical / "alias-child" / ".."
            temporary = mock.Mock()
            temporary.name = str(alias)

            def select_backend():
                self.assertEqual(case.parent, canonical)
                return mock.sentinel.native_api

            resolve = pathlib.Path.resolve
            try:
                with mock.patch.object(tempfile, "TemporaryDirectory", return_value=temporary), \
                        mock.patch.object(security, "NativeSecurityApi", side_effect=select_backend), \
                        mock.patch.object(pathlib.Path, "resolve", autospec=True,
                                          side_effect=resolve) as canonicalize:
                    case.setUp()
                    canonicalize.assert_any_call(alias, strict=True)
                self.assertIs(case.api, mock.sentinel.native_api)
            finally:
                case.doCleanups()
            temporary.cleanup.assert_called_once_with()


@unittest.skipUnless(os.name == "nt", "requires native Windows ACL and token APIs")
class NativeDirectorySecurityTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.parent = pathlib.Path(self.temporary.name).resolve(strict=True)
        self.api = security.NativeSecurityApi()

    def test_atomic_owner_only_creation_and_native_lease(self):
        root = create_native_fixture_parent(self.parent, "native-private")
        with security.private_parent_lease(root):
            self.assertTrue(root.is_dir())
        handle = self.api.open_directory(str(root))
        try:
            observed = self.api.inspect_directory(handle)
            self.assertEqual(observed.security.owner, self.api.current_user_sid())
            self.assertTrue(observed.security.protected)
            self.assertEqual(len(observed.security.entries), 1)
            self.assertEqual(observed.security.entries[0].mask, security.FILE_ALL_ACCESS)
            self.assertEqual(observed.security.entries[0].flags, 3)
        finally:
            self.api.close_directory(handle)
        # Independent .NET ACL/token inspection, not the production ctypes parser.
        inspect = (
            "$a=Get-Acl -LiteralPath $env:KLOGG_NATIVE_ACL_TEST_PATH;"
            "$r=@($a.GetAccessRules($true,$true,[System.Security.Principal.SecurityIdentifier]) | "
            "ForEach-Object { [pscustomobject]@{sid=$_.IdentityReference.Value;"
            "rights=[int]$_.FileSystemRights;inheritance=[int]$_.InheritanceFlags;"
            "propagation=[int]$_.PropagationFlags;inherited=$_.IsInherited;"
            "kind=[int]$_.AccessControlType} });"
            "[pscustomobject]@{owner=$a.GetOwner([System.Security.Principal.SecurityIdentifier]).Value;"
            "user=[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value;"
            "protected=$a.AreAccessRulesProtected;rules=$r}|ConvertTo-Json -Depth 4 -Compress"
        )
        completed = run_native_acl_probe(root, inspect)
        independent = json.loads(completed.stdout)
        self.assertEqual(independent["owner"], independent["user"])
        self.assertTrue(independent["protected"])
        self.assertEqual(independent["rules"], [{
            "sid": independent["user"], "rights": security.FILE_ALL_ACCESS,
            "inheritance": 3, "propagation": 0, "inherited": False, "kind": 0,
        }])
        with self.assertRaises(security.WindowsSecurityError):
            security.create_private_parent(self.parent, "native-private")

    def test_native_mutable_ancestor_is_rejected_without_acl_repair(self):
        ancestor = create_native_fixture_parent(self.parent, "owned-ancestor")
        root = create_native_fixture_parent(ancestor, "native-private")
        descriptor = self.api.HANDLE()
        size = self.api.DWORD()
        user = self.api.current_user_sid()
        sddl = "O:{}D:P(A;OICI;FA;;;{})(A;;0x00000100;;;WD)".format(user, user)
        self.assertTrue(self.api.convert_descriptor(sddl, 1, ctypes.byref(descriptor), ctypes.byref(size)))
        get_dacl = self.api.advapi.GetSecurityDescriptorDacl
        get_dacl.argtypes = [self.api.HANDLE, ctypes.POINTER(self.api.BOOL),
                            ctypes.POINTER(self.api.HANDLE), ctypes.POINTER(self.api.BOOL)]
        get_dacl.restype = self.api.BOOL
        set_security = self.api.advapi.SetNamedSecurityInfoW
        set_security.argtypes = [ctypes.c_wchar_p, self.api.DWORD, self.api.DWORD,
                                self.api.HANDLE, self.api.HANDLE, self.api.HANDLE, self.api.HANDLE]
        set_security.restype = self.api.DWORD
        with self.api._allocation(descriptor):
            present, defaulted, dacl = self.api.BOOL(), self.api.BOOL(), self.api.HANDLE()
            self.assertTrue(get_dacl(descriptor, ctypes.byref(present), ctypes.byref(dacl),
                                     ctypes.byref(defaulted)))
            self.assertTrue(present.value)
            self.assertEqual(set_security(str(ancestor), 1, 0x80000004,
                                          None, None, dacl, None), 0)
        with self.assertRaisesRegex(security.WindowsSecurityError, "mutation"):
            with security.private_parent_lease(root):
                self.fail("hostile ancestor entered")
        handle = self.api.open_directory(str(ancestor))
        try:
            self.assertEqual(len(self.api.inspect_directory(handle).security.entries), 2)
        finally:
            self.api.close_directory(handle)

    def test_native_reparse_directory_is_rejected(self):
        root = create_native_fixture_parent(self.parent, "native-private")
        link = self.parent / "junction"
        # mklink creates only this test-owned alias; no payload or external path.
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(root)],
                       check=True, capture_output=True, timeout=30)
        self.addCleanup(link.rmdir)
        with self.assertRaises(security.WindowsSecurityError):
            with security.private_parent_lease(link):
                self.fail("reparse root entered")

    def test_distinct_restricted_token_cannot_read_private_child(self):
        root = create_native_fixture_parent(self.parent, "native-private")
        child = root / "protected.bin"
        child.write_bytes(b"private native core fixture")
        advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        H, D, B = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32
        P = ctypes.POINTER
        token, restricted, foreign_sid = H(), H(), H()
        open_token = advapi.OpenProcessToken
        open_token.argtypes, open_token.restype = [H, D, P(H)], B
        restrict = advapi.CreateRestrictedToken
        restrict.restype = B
        impersonate = advapi.ImpersonateLoggedOnUser
        impersonate.argtypes, impersonate.restype = [H], B
        revert = advapi.RevertToSelf
        revert.argtypes, revert.restype = [], B
        convert = advapi.ConvertStringSidToSidW
        convert.argtypes, convert.restype = [ctypes.c_wchar_p, P(H)], B

        class SidAndAttributes(ctypes.Structure):
            _fields_ = [("sid", H), ("attributes", D)]

        restrict.argtypes = [H, D, D, H, D, H, D, P(SidAndAttributes), P(H)]
        self.assertTrue(open_token(self.api.process(), 0x000e, ctypes.byref(token)))
        impersonating = False
        try:
            self.assertTrue(convert("S-1-5-12", ctypes.byref(foreign_sid)))
            entry = SidAndAttributes(foreign_sid, 0)
            self.assertTrue(restrict(token, 1, 0, None, 0, None, 1,
                                     ctypes.byref(entry), ctypes.byref(restricted)))
            self.assertTrue(impersonate(restricted))
            impersonating = True
            with self.assertRaises(PermissionError):
                child.read_bytes()
        finally:
            if impersonating:
                self.assertTrue(revert())
            if restricted.value:
                self.assertTrue(self.api.close(restricted))
            if token.value:
                self.assertTrue(self.api.close(token))
            if foreign_sid.value:
                self.assertFalse(self.api.free(foreign_sid))


if __name__ == "__main__":
    unittest.main()
