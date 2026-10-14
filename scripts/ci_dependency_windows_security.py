#!/usr/bin/env python3
"""Private Windows staging under an audited, leased local NTFS namespace.

The current identity and privileged SYSTEM/Administrators are trusted, as root
and the owning uid are on POSIX. A stable platform drive mapping and ancestor
security are prerequisites; this does not defend against those principals or
foreign mutation handles retained from an earlier insecure host configuration.
Tokens exercising filesystem/security override privileges are also outside this
unprivileged-adversary boundary. Sharing restrictions pin names, not metadata. Ancestor ACLs independently deny
unprivileged existing-object and security-descriptor mutation throughout the
transaction. Unknown ACL semantics and unsupported host paths fail closed.
"""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import ntpath
import os
import pathlib
import re
import sys

FILE_ALL_ACCESS = 0x001F01FF
FILE_GENERIC_READ = 0x00120089
FILE_GENERIC_WRITE = 0x00120116
FILE_GENERIC_EXECUTE = 0x001200A0
MUTATION_RIGHTS = 0x000D0152  # DELETE, WRITE_DAC/OWNER, DELETE_CHILD, DATA, EA, attributes.
# FILE_ADD_SUBDIRECTORY alone creates a new child; collisions never replace existing names.
# https://learn.microsoft.com/en-us/windows/win32/fileio/file-access-rights-constants
MAX_DIRECTORY_CHAIN = 64
DIRECTORY_ATTRIBUTE = 0x10
REPARSE_ATTRIBUTE = 0x400
OBJECT_INHERIT_ACE = 0x01
CONTAINER_INHERIT_ACE = 0x02
INHERIT_ONLY_ACE = 0x08
ACCESS_ALLOWED_ACE_TYPE = 0
ACCESS_DENIED_ACE_TYPE = 1
SYSTEM_SID = "S-1-5-18"
ADMINISTRATORS_SID = "S-1-5-32-544"
SID = re.compile(r"S-1-(?:[0-9]+-)*[0-9]+\Z")
DEVICE_NAMES = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {
    prefix + str(index) for prefix in ("COM", "LPT") for index in range(1, 10)
} | {prefix + index for prefix in ("COM", "LPT") for index in ("¹", "²", "³")}


class WindowsSecurityError(ValueError):
    """A Windows directory cannot establish the private staging contract."""


@dataclasses.dataclass(frozen=True)
class AccessEntry:
    kind: int
    flags: int
    mask: int
    sid: str


@dataclasses.dataclass(frozen=True)
class DirectorySecurity:
    owner: str
    dacl_present: bool
    protected: bool
    entries: tuple


@dataclasses.dataclass(frozen=True)
class DirectoryRecord:
    identity: tuple
    attributes: int
    path: str
    security: DirectorySecurity


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise WindowsSecurityError(message)


def _access_mask(mask: int) -> int:
    _require(type(mask) is int and 0 <= mask <= 0xffffffff, "invalid directory access mask")
    result = mask & 0x0fffffff
    for generic, mapped in ((0x80000000, FILE_GENERIC_READ),
                            (0x40000000, FILE_GENERIC_WRITE),
                            (0x20000000, FILE_GENERIC_EXECUTE),
                            (0x10000000, FILE_ALL_ACCESS)):
        if mask & generic:
            result |= mapped
    _require(result & ~FILE_ALL_ACCESS == 0, "unsupported directory access right")
    return result


def validate_directory_security(security: DirectorySecurity, user_sid: str,
                                *, private: bool) -> None:
    """Reject any unprivileged ancestor mutation or non-owner private grant."""
    _require(isinstance(user_sid, str) and SID.fullmatch(user_sid) is not None,
             "invalid current Windows identity")
    _require(isinstance(security, DirectorySecurity) and security.dacl_present is True
             and type(security.protected) is bool, "missing, NULL or invalid directory DACL")
    trusted = {user_sid, SYSTEM_SID, ADMINISTRATORS_SID}
    _require(security.owner in trusted, "untrusted directory owner can change its DACL")
    _require(isinstance(security.entries, tuple) and len(security.entries) <= 1024,
             "invalid directory ACL entries")
    for entry in security.entries:
        _require(isinstance(entry, AccessEntry)
                 and type(entry.kind) is int
                 and entry.kind in (ACCESS_ALLOWED_ACE_TYPE, ACCESS_DENIED_ACE_TYPE)
                 and type(entry.flags) is int and entry.flags & ~0x1f == 0
                 and isinstance(entry.sid, str) and SID.fullmatch(entry.sid) is not None,
                 "unsupported directory ACE semantics")
        mask = _access_mask(entry.mask)
        if entry.kind == ACCESS_ALLOWED_ACE_TYPE and not entry.flags & INHERIT_ONLY_ACE:
            _require(entry.sid in trusted or mask & MUTATION_RIGHTS == 0,
                     "unprivileged ancestor mutation grant")
    if private:
        _require(security.owner == user_sid and security.protected is True
                 and len(security.entries) == 1, "private directory must be protected and owner-only")
        entry = security.entries[0]
        _require(entry.kind == ACCESS_ALLOWED_ACE_TYPE and entry.sid == user_sid
                 and entry.flags == OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE
                 and _access_mask(entry.mask) == FILE_ALL_ACCESS,
                 "private directory must inherit only the owner's complete access")


def _component(name: str) -> None:
    _require(isinstance(name, str) and name not in ("", ".", "..")
             and not name.endswith((".", " "))
             and not any(ord(character) < 32 or character in '<>:"/\\|?*' for character in name)
             and name.split(".", 1)[0].upper() not in DEVICE_NAMES,
             "unsafe Windows directory component")
    try:
        units = len(name.encode("utf-16-le")) // 2
    except UnicodeError as error:
        raise WindowsSecurityError("invalid Windows directory encoding") from error
    _require(units <= 255, "oversized Windows directory component")


def directory_chain(path) -> tuple:
    """Accept canonical drive-absolute names, not devices, UNC, ADS or aliases."""
    text = os.fspath(path)
    _require(isinstance(text, str) and "/" not in text and "\x00" not in text,
             "noncanonical Windows directory path")
    drive, tail = ntpath.splitdrive(text)
    _require(re.fullmatch(r"[A-Za-z]:", drive) is not None and tail.startswith("\\")
             and not tail.startswith("\\\\"), "private staging requires a local drive-absolute path")
    parts = tail[1:].split("\\") if tail != "\\" else []
    _require(len(parts) < MAX_DIRECTORY_CHAIN, "oversized Windows directory chain")
    for part in parts:
        _component(part)
    current = drive.upper() + "\\"
    paths = [current]
    for part in parts:
        current = ntpath.join(current, part)
        paths.append(current)
    return tuple(paths)


def _diagnostic_text(value, maximum: int) -> str:
    text = ascii(value) if isinstance(value, (str, int, bool)) else "<unavailable>"
    marker = "<truncated>"
    return text if len(text) <= maximum else text[:maximum - len(marker)] + marker


def _rejection_context(error, record: DirectoryRecord, path: str, user_sid: str,
                       *, private: bool) -> None:
    """Describe inspected values without changing refusal or claiming effective access."""
    descriptor = record.security
    owner = getattr(descriptor, "owner", None)
    entries = getattr(descriptor, "entries", ())
    mutations = []
    omitted = 0
    if isinstance(entries, tuple):
        for entry in entries[:1024]:
            if not isinstance(entry, AccessEntry):
                continue
            try:
                mask = _access_mask(entry.mask) & MUTATION_RIGHTS
            except WindowsSecurityError:
                continue
            if not mask:
                continue
            if len(mutations) == 4:
                omitted += 1
                continue
            mutations.append({
                "sid": _diagnostic_text(entry.sid, 192),
                "kind": entry.kind,
                "flags": entry.flags,
                "raw_mask": entry.mask,
                "mutation_mask": mask,
                "applicable_allow": entry.kind == ACCESS_ALLOWED_ACE_TYPE
                    and type(entry.flags) is int and not entry.flags & INHERIT_ONLY_ACE,
                "trusted_principal": entry.sid in (user_sid, SYSTEM_SID, ADMINISTRATORS_SID),
            })
        omitted += max(0, len(entries) - 1024)
    reason = str(error)
    error.windows_namespace_context = {
        "path": path, "owner_sid": owner, "user_sid": user_sid, "private": private,
        "decision": reason, "mutation_entries": mutations, "omitted_entries": omitted,
    }
    detail = (f"path={_diagnostic_text(path, 512)} owner={_diagnostic_text(owner, 192)} "
              f"user={_diagnostic_text(user_sid, 192)} private={private} "
              f"observed_mutation_entries={mutations!r} omitted={omitted}")
    marker = "<truncated>"
    if len(detail) > 2048:
        detail = detail[:2048 - len(marker)] + marker
    error.args = (reason + "\nWindows namespace:" + detail,) + error.args[1:]


def _record(api, handle, path: str, user_sid: str, *, private: bool) -> DirectoryRecord:
    record = api.inspect_directory(handle)
    _require(isinstance(record, DirectoryRecord)
             and type(record.attributes) is int
             and record.attributes & DIRECTORY_ATTRIBUTE != 0
             and record.attributes & REPARSE_ATTRIBUTE == 0,
             "directory lease rejects non-directory or reparse objects")
    _require(isinstance(record.identity, tuple) and len(record.identity) == 2
             and all(type(value) is int and value >= 0 for value in record.identity)
             and record.identity[1] != 0, "invalid native directory identity")
    _require(api.paths_equal(record.path, path), "directory lease rejects path aliases")
    try:
        validate_directory_security(record.security, user_sid, private=private)
    except WindowsSecurityError as error:
        try:
            _rejection_context(error, record, path, user_sid, private=private)
        except Exception:
            # Diagnostic formatting must never replace the original refusal.
            pass
        raise
    return record


def _secondary(primary, failures) -> None:
    if not failures:
        return
    if primary is None:
        raise WindowsSecurityError("directory lease cleanup failed: " + "; ".join(failures))
    previous = getattr(primary, "windows_security_cleanup_errors", ())
    primary.windows_security_cleanup_errors = previous + tuple(failures)


class DirectoryLease:
    def __init__(self, api, user_sid: str, records):
        self.api = api
        self.user_sid = user_sid
        self.records = records

    def verify(self) -> None:
        for handle, path, before, private in self.records:
            current = _record(self.api, handle, path, self.user_sid, private=private)
            _require(current.identity == before.identity, "leased directory identity changed")


@contextlib.contextmanager
def _directory_lease(parent, *, private: bool, api=None):
    paths = directory_chain(parent)
    api = NativeSecurityApi() if api is None else api
    user_sid = api.current_user_sid()
    records = []
    primary = None
    lease = None
    try:
        for index, path in enumerate(paths):
            handle = api.open_directory(path)
            records.append((handle, path, None, private and index == len(paths) - 1))
            observed = _record(api, handle, path, user_sid,
                               private=private and index == len(paths) - 1)
            records[-1] = (handle, path, observed, private and index == len(paths) - 1)
            if index == 0:
                _require(api.is_local_ntfs(path, handle), "private staging requires a fixed local NTFS volume")
            else:
                _require(observed.identity[0] == records[0][2].identity[0],
                         "directory lease crossed a volume boundary")
        lease = DirectoryLease(api, user_sid, records)
        lease.verify()
        yield lease
    except BaseException as error:
        primary = error
        raise
    finally:
        failures = []
        if lease is not None:
            try:
                lease.verify()
            except Exception as error:
                failures.append(str(error))
        for handle, _path, _record_value, _private in reversed(records):
            try:
                api.close_directory(handle)
            except Exception as error:
                failures.append(str(error))
        _secondary(primary, failures)


def private_parent_lease(parent, *, api=None):
    """Hold audited ancestors and the private leaf through staging and cleanup."""
    return _directory_lease(parent, private=True, api=api)


def create_private_parent(parent, name: str, *, api=None) -> pathlib.Path:
    """Create a fresh empty owner-only root; never repair an existing directory."""
    _component(name)
    with _directory_lease(parent, private=False, api=api) as lease:
        path = ntpath.join(directory_chain(parent)[-1], name)
        lease.verify()
        lease.api.create_private_directory(path, lease.user_sid)
        try:
            with private_parent_lease(path, api=lease.api):
                pass
        except BaseException as primary:
            try:
                lease.api.remove_created_directory(path)
            except Exception as error:
                _secondary(primary, [str(error)])
            raise
        return pathlib.Path(path)


class NativeSecurityApi:
    """Windows APIs only; portable tests inject a model without native claims."""

    def __init__(self):
        _require(os.name == "nt", "Windows native directory security is unavailable on this host")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        self.DWORD = ctypes.c_uint32
        self.BOOL = ctypes.c_int32
        self.HANDLE = ctypes.c_void_p
        self._declare()

    def _function(self, library, name, result, arguments):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments
        return function

    def _declare(self):
        D, B, H = self.DWORD, self.BOOL, self.HANDLE
        P = ctypes.POINTER
        self.close = self._function(self.kernel, "CloseHandle", B, [H])
        self.free = self._function(self.kernel, "LocalFree", H, [H])
        self.open = self._function(self.kernel, "CreateFileW", H,
                                  [ctypes.c_wchar_p, D, D, H, D, D, H])
        self.drive_type = self._function(self.kernel, "GetDriveTypeW", D, [ctypes.c_wchar_p])
        self.final_name = self._function(self.kernel, "GetFinalPathNameByHandleW", D,
                                        [H, ctypes.c_wchar_p, D, D])
        self.compare = self._function(self.kernel, "CompareStringOrdinal", ctypes.c_int32,
                                     [ctypes.c_wchar_p, ctypes.c_int32,
                                      ctypes.c_wchar_p, ctypes.c_int32, B])
        self.volume = self._function(self.kernel, "GetVolumeInformationByHandleW", B,
                                     [H, ctypes.c_wchar_p, D, P(D), P(D), P(D), ctypes.c_wchar_p, D])
        self.process = self._function(self.kernel, "GetCurrentProcess", H, [])
        self.thread = self._function(self.kernel, "GetCurrentThread", H, [])
        self.open_process_token = self._function(self.advapi, "OpenProcessToken", B, [H, D, P(H)])
        self.open_thread_token = self._function(self.advapi, "OpenThreadToken", B, [H, D, B, P(H)])
        self.token_info = self._function(self.advapi, "GetTokenInformation", B, [H, D, H, D, P(D)])
        self.sid_string = self._function(self.advapi, "ConvertSidToStringSidW", B, [H, P(H)])
        self.valid_sid = self._function(self.advapi, "IsValidSid", B, [H])
        self.sid_length = self._function(self.advapi, "GetLengthSid", D, [H])
        self.security = self._function(self.advapi, "GetSecurityInfo", D,
                                      [H, D, D, P(H), P(H), P(H), P(H), P(H)])
        self.valid_descriptor = self._function(self.advapi, "IsValidSecurityDescriptor", B, [H])
        self.descriptor_length = self._function(self.advapi, "GetSecurityDescriptorLength", D, [H])
        self.control = self._function(self.advapi, "GetSecurityDescriptorControl", B,
                                      [H, P(ctypes.c_uint16), P(D)])
        self.get_ace = self._function(self.advapi, "GetAce", B, [H, D, P(H)])
        self.convert_descriptor = self._function(
            self.advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW", B,
            [ctypes.c_wchar_p, D, P(H), P(D)])
        self.create_directory = self._function(self.kernel, "CreateDirectoryW", B,
                                               [ctypes.c_wchar_p, H])
        self.remove_directory = self._function(self.kernel, "RemoveDirectoryW", B, [ctypes.c_wchar_p])

        class FileTime(ctypes.Structure):
            _fields_ = [("low", D), ("high", D)]

        class FileInformation(ctypes.Structure):
            _fields_ = [("attributes", D), ("creation", FileTime), ("access", FileTime),
                        ("write", FileTime), ("volume", D), ("size_high", D), ("size_low", D),
                        ("links", D), ("index_high", D), ("index_low", D)]

        class SecurityAttributes(ctypes.Structure):
            _fields_ = [("length", D), ("descriptor", H), ("inherit_handle", B)]

        self.FileInformation = FileInformation
        self.SecurityAttributes = SecurityAttributes
        self.file_info = self._function(self.kernel, "GetFileInformationByHandle", B,
                                        [H, P(FileInformation)])

    @staticmethod
    def _error(label: str):
        return WindowsSecurityError("{} (Win32 {})".format(label, ctypes.get_last_error()))

    @contextlib.contextmanager
    def _allocation(self, pointer):
        try:
            yield
        finally:
            primary = sys.exc_info()[1]
            if self.free(pointer):
                _secondary(primary, ["LocalFree failed"])

    def _sid_text(self, pointer: int, maximum: int) -> str:
        _require(maximum >= 8, "truncated native SID")
        count = ctypes.c_ubyte.from_address(pointer + 1).value
        _require(count <= 15 and 8 + 4 * count <= maximum and self.valid_sid(pointer),
                 "invalid native SID")
        _require(self.sid_length(pointer) == 8 + 4 * count, "invalid native SID length")
        text = self.HANDLE()
        if not self.sid_string(pointer, ctypes.byref(text)):
            raise self._error("cannot format native SID")
        with self._allocation(text):
            return ctypes.wstring_at(text.value)

    def current_user_sid(self) -> str:
        token = self.HANDLE()
        if not self.open_thread_token(self.thread(), 0x0008, True, ctypes.byref(token)):
            _require(ctypes.get_last_error() == 1008, "cannot inspect effective Windows token")
            if not self.open_process_token(self.process(), 0x0008, ctypes.byref(token)):
                raise self._error("cannot inspect process token")
        try:
            needed = self.DWORD()
            self.token_info(token, 1, None, 0, ctypes.byref(needed))
            _require(ctypes.get_last_error() == 122 and 0 < needed.value <= 65536,
                     "invalid current token identity size")
            buffer = ctypes.create_string_buffer(needed.value)
            if not self.token_info(token, 1, buffer, needed.value, ctypes.byref(needed)):
                raise self._error("cannot read current token identity")
            pointer = self.HANDLE.from_buffer(buffer).value
            base = ctypes.addressof(buffer)
            _require(pointer is not None and base <= pointer <= base + len(buffer) - 8,
                     "current token SID escapes native buffer")
            return self._sid_text(pointer, base + len(buffer) - pointer)
        finally:
            primary = sys.exc_info()[1]
            if not self.close(token):
                _secondary(primary, ["CloseHandle token failed"])

    def open_directory(self, path: str):
        # READ_CONTROL/LIST/ATTRIBUTES, shared READ/WRITE but never DELETE.
        handle = self.open(path, 0x00020081, 0x00000003, None, 3, 0x02200000, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            raise self._error("cannot lease directory")
        return handle

    def close_directory(self, handle) -> None:
        if not self.close(handle):
            raise self._error("cannot close directory lease")

    def paths_equal(self, left: str, right: str) -> bool:
        return self.compare(left, -1, right, -1, True) == 2

    def is_local_ntfs(self, path: str, handle) -> bool:
        if self.drive_type(path) != 3:
            return False
        name = ctypes.create_unicode_buffer(256)
        if not self.volume(handle, None, 0, None, None, None, name, len(name)):
            raise self._error("cannot inspect native filesystem")
        return self.paths_equal(name.value, "NTFS")

    def _security(self, handle) -> DirectorySecurity:
        owner, dacl, descriptor = self.HANDLE(), self.HANDLE(), self.HANDLE()
        status = self.security(handle, 1, 0x00000005, ctypes.byref(owner), None,
                               ctypes.byref(dacl), None, ctypes.byref(descriptor))
        _require(status == 0 and descriptor.value is not None,
                 "cannot inspect directory security (Win32 {})".format(status))
        with self._allocation(descriptor):
            _require(self.valid_descriptor(descriptor), "invalid native security descriptor")
            length = self.descriptor_length(descriptor)
            _require(20 <= length <= 65536, "invalid native security descriptor length")
            base, end = descriptor.value, descriptor.value + length
            _require(owner.value is not None and base <= owner.value <= end - 8,
                     "directory owner escapes security descriptor")
            owner_sid = self._sid_text(owner.value, end - owner.value)
            control, revision = ctypes.c_uint16(), self.DWORD()
            if not self.control(descriptor, ctypes.byref(control), ctypes.byref(revision)):
                raise self._error("cannot inspect security descriptor control")
            _require(control.value & 0x0004 and dacl.value is not None,
                     "missing or NULL native directory DACL")
            _require(base <= dacl.value <= end - 8, "DACL escapes security descriptor")
            raw = (ctypes.c_ubyte * 8).from_address(dacl.value)
            acl_size = raw[2] | raw[3] << 8
            ace_count = raw[4] | raw[5] << 8
            _require(raw[0] in (2, 4) and raw[1] == 0 and raw[6] == raw[7] == 0
                     and 8 <= acl_size <= end - dacl.value and acl_size % 4 == 0
                     and ace_count <= 1024, "invalid native ACL bounds")
            entries = []
            for index in range(ace_count):
                pointer = self.HANDLE()
                if not self.get_ace(dacl, index, ctypes.byref(pointer)):
                    raise self._error("cannot inspect native ACE")
                _require(pointer.value is not None and dacl.value + 8 <= pointer.value
                         <= dacl.value + acl_size - 16, "ACE escapes native ACL")
                header = (ctypes.c_ubyte * 8).from_address(pointer.value)
                size = header[2] | header[3] << 8
                _require(header[0] in (0, 1) and 16 <= size <= dacl.value + acl_size - pointer.value,
                         "unsupported or invalid native ACE")
                sid_pointer = pointer.value + 8
                sid_text = self._sid_text(sid_pointer, size - 8)
                _require(size == 8 + self.sid_length(sid_pointer), "noncanonical native ACE size")
                mask = int.from_bytes(bytes(header[4:8]), "little")
                entries.append(AccessEntry(header[0], header[1], mask, sid_text))
            return DirectorySecurity(owner_sid, True, bool(control.value & 0x1000), tuple(entries))

    def inspect_directory(self, handle) -> DirectoryRecord:
        information = self.FileInformation()
        if not self.file_info(handle, ctypes.byref(information)):
            raise self._error("cannot inspect directory identity")
        path = ctypes.create_unicode_buffer(32768)
        length = self.final_name(handle, path, len(path), 0)
        _require(0 < length < len(path), "cannot obtain bounded native directory name")
        name = path.value
        _require(name.startswith("\\\\?\\") and not name.startswith("\\\\?\\UNC\\"),
                 "unsupported native directory namespace")
        return DirectoryRecord((information.volume,
                                information.index_high << 32 | information.index_low),
                               information.attributes, name[4:], self._security(handle))

    def create_private_directory(self, path: str, user_sid: str) -> None:
        _require(SID.fullmatch(user_sid) is not None, "invalid private directory owner")
        descriptor = self.HANDLE()
        size = self.DWORD()
        sddl = "O:{}D:P(A;OICI;FA;;;{})".format(user_sid, user_sid)
        if not self.convert_descriptor(sddl, 1, ctypes.byref(descriptor), ctypes.byref(size)):
            raise self._error("cannot construct owner-only security descriptor")
        with self._allocation(descriptor):
            attributes = self.SecurityAttributes(ctypes.sizeof(self.SecurityAttributes),
                                                  descriptor, False)
            if not self.create_directory(path, ctypes.byref(attributes)):
                raise self._error("cannot create fresh private directory")

    def remove_created_directory(self, path: str) -> None:
        if not self.remove_directory(path):
            raise self._error("cannot remove created private directory")
