"""Descriptor-anchored, clean-only source snapshots; never copy cache Git metadata."""
import argparse
import ctypes
import errno
import hashlib
import os
import re
import stat
import sys


class SnapshotError(RuntimeError):
    pass


class CleanupError(SnapshotError):
    pass


_MAX_ENTRIES = 100000
_MAX_BYTES = 256 * 1024 * 1024
_DIRECTORY_FLAGS = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
_FILE_FLAGS = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)


def _require_descriptors():
    if (os.name != "posix" or not hasattr(os, "O_NOFOLLOW")
            or os.open not in os.supports_dir_fd or os.stat not in os.supports_dir_fd):
        raise SnapshotError("Approved content snapshots require POSIX descriptor-relative no-follow IO")


def _fingerprint(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_nlink, info.st_uid,
            info.st_gid, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _identity(info):
    return info.st_dev, info.st_ino


def _private(info):
    if info.st_uid != os.geteuid() or info.st_mode & (0o022 | 0o7000):
        raise SnapshotError("Snapshot output must be privately owned without unsafe write/setid modes")


def _darwin_acl_api():
    native = ctypes.CDLL(None, use_errno=True)
    pointer = ctypes.c_void_p
    signatures = {
        "acl_init": ([ctypes.c_int], pointer),
        "acl_get_fd": ([ctypes.c_int], pointer),
        "acl_set_fd": ([ctypes.c_int, pointer], ctypes.c_int),
        "acl_free": ([pointer], ctypes.c_int),
        "acl_valid": ([pointer], ctypes.c_int),
        "acl_get_entry": ([pointer, ctypes.c_int, ctypes.POINTER(pointer)], ctypes.c_int),
        "acl_get_tag_type": ([pointer, ctypes.POINTER(ctypes.c_int)], ctypes.c_int),
        "acl_get_qualifier": ([pointer], pointer),
        "mbr_uid_to_uuid": ([ctypes.c_uint32, pointer], ctypes.c_int),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(native, name)
        function.argtypes, function.restype = arguments, result
    return native


def _check_acl(descriptor, parent=False):
    if sys.platform != "darwin":
        return
    native = _darwin_acl_api()
    ctypes.set_errno(0)
    acl = native.acl_get_fd(descriptor)
    if not acl:
        if ctypes.get_errno() == errno.ENOENT:
            return
        raise SnapshotError("Could not verify private Darwin ACL")
    try:
        if native.acl_valid(acl) != 0:
            raise SnapshotError("Private Darwin ACL is invalid")
        entry = ctypes.c_void_p()
        entry_id = 0  # ACL_FIRST_ENTRY; subsequent Darwin entries use -1.
        while True:
            ctypes.set_errno(0)
            if native.acl_get_entry(acl, entry_id, ctypes.byref(entry)) != 0:
                # Darwin returns EINVAL when the cursor reaches the end of an
                # already validated ACL, rather than POSIX's end-of-list value.
                if ctypes.get_errno() not in (0, errno.ENOENT, errno.EINVAL):
                    raise SnapshotError("Could not verify private Darwin ACL entry (errno "
                                        + str(ctypes.get_errno()) + ")")
                break
            entry_id = -1
            if not parent:
                raise SnapshotError("Private snapshot output must not retain extended ACL entries")
            tag = ctypes.c_int()
            if native.acl_get_tag_type(entry, ctypes.byref(tag)) != 0:
                raise SnapshotError("Could not verify private parent ACL tag")
            if tag.value == 2:  # ACL_EXTENDED_DENY grants no additional access.
                continue
            qualifier = native.acl_get_qualifier(entry)
            try:
                owner = (ctypes.c_ubyte * 16)()
                if (tag.value != 1 or not qualifier
                        or native.mbr_uid_to_uuid(os.geteuid(), ctypes.byref(owner)) != 0
                        or ctypes.string_at(qualifier, 16) != bytes(owner)):
                    raise SnapshotError("Private parent ACL must not grant access to another identity")
            finally:
                if qualifier:
                    native.acl_free(qualifier)
    finally:
        native.acl_free(acl)


def _canonical_output(descriptor, mode):
    if sys.platform == "darwin":
        native = _darwin_acl_api()
        empty = native.acl_init(0)
        if not empty:
            raise SnapshotError("Could not allocate an empty private output ACL")
        try:
            if native.acl_set_fd(descriptor, empty) != 0:
                raise SnapshotError("Could not clear inherited private output ACL")
        finally:
            native.acl_free(empty)
    elif hasattr(os, "listxattr"):
        for name in os.listxattr(descriptor):
            if name in ("system.posix_acl_access", "system.posix_acl_default"):
                os.removexattr(descriptor, name)
    os.fchmod(descriptor, mode)
    _private(os.fstat(descriptor))
    _check_acl(descriptor)


def _open_root(source, private=False):
    info = os.lstat(source)
    if stat.S_ISLNK(info.st_mode):
        raise SnapshotError("Source root symlink is not allowed")
    if not stat.S_ISDIR(info.st_mode):
        raise SnapshotError("Source directory does not exist or is not an ordinary directory")
    descriptor = os.open(source, _DIRECTORY_FLAGS)
    try:
        if _fingerprint(os.fstat(descriptor)) != _fingerprint(info):
            raise SnapshotError("Source root changed during capture")
        if private:
            _private(info)
            _check_acl(descriptor)
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def _open_private_parent(path):
    # Pin every component of the canonical namespace, not just its leaf. Same
    # UID actors and root override privileges are outside this isolation model.
    path = os.path.realpath(path)
    descriptor = os.open(os.path.sep, _DIRECTORY_FLAGS)
    try:
        for name in path.split(os.path.sep):
            if not name:
                continue
            ancestor = os.fstat(descriptor)
            child_info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            if ancestor.st_uid not in (0, os.geteuid()):
                raise SnapshotError("Private namespace ancestor has an untrusted owner")
            if ancestor.st_mode & 0o022:
                if not ancestor.st_mode & stat.S_ISVTX or child_info.st_uid not in (0, os.geteuid()):
                    raise SnapshotError("Private namespace ancestor permits foreign rename/write")
            _check_acl(descriptor, parent=True)
            child = os.open(name, _DIRECTORY_FLAGS, dir_fd=descriptor)
            if _identity(os.fstat(child)) != _identity(child_info):
                os.close(child)
                raise SnapshotError("Private namespace ancestor identity changed")
            os.close(descriptor)
            descriptor = child
        _private(os.fstat(descriptor))
        _check_acl(descriptor, parent=True)
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def prepare_parent(path):
    _require_descriptors()
    path = os.path.realpath(path)
    existing = path
    missing = []
    while not os.path.exists(existing):
        missing.append(os.path.basename(existing))
        parent = os.path.dirname(existing)
        if parent == existing:
            raise SnapshotError("Could not resolve private namespace ancestor")
        existing = parent
    descriptor = _open_private_parent(existing)
    try:
        for name in reversed(missing):
            _validate_name(name)
            created = False
            try:
                os.mkdir(name, 0o700, dir_fd=descriptor)
                created = True
            except FileExistsError:
                pass
            child = os.open(name, _DIRECTORY_FLAGS, dir_fd=descriptor)
            try:
                if created:
                    _canonical_output(child, 0o700)
                else:
                    _private(os.fstat(child))
                    _check_acl(child, parent=True)
                current = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
                if _identity(current) != _identity(os.fstat(child)):
                    raise SnapshotError("Private parent identity changed during creation")
            except BaseException:
                os.close(child)
                raise
            os.close(descriptor)
            descriptor = child
    finally:
        os.close(descriptor)
    verified = _open_private_parent(path)
    os.close(verified)


def _validate_name(name):
    if any(ord(character) < 32 for character in name) or any(c in name for c in "\\;"):
        raise SnapshotError("Source contains an unsafe manifest path")
    try:
        name.encode("utf-8")
    except UnicodeError as error:
        raise SnapshotError("Source manifest paths must be UTF-8") from error


def _walk(descriptor, relative="", output=None, metadata=False, private=False, budget=None):
    if budget is None:
        budget = [0, 0]
    before = os.fstat(descriptor)
    names = sorted(os.listdir(descriptor))
    records = {relative: _fingerprint(before)}
    manifest = {}
    for name in names:
        _validate_name(name)
        if not relative and name == ".git" and not metadata:
            continue
        budget[0] += 1
        if budget[0] > _MAX_ENTRIES or relative.count("/") > 64:
            raise SnapshotError("Source exceeds bounded snapshot entry/depth limits")
        path = relative + "/" + name if relative else name
        info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
        if stat.S_ISLNK(info.st_mode):
            raise SnapshotError("Source symlink is not allowed: " + path)
        if not stat.S_ISDIR(info.st_mode) and not stat.S_ISREG(info.st_mode):
            raise SnapshotError("Source special file is not allowed: " + path)
        if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
            raise SnapshotError("Source hardlink is not allowed: " + path)
        if private:
            _private(info)
        flags = _DIRECTORY_FLAGS if stat.S_ISDIR(info.st_mode) else _FILE_FLAGS
        entry = os.open(name, flags, dir_fd=descriptor)
        target = None
        try:
            if _fingerprint(os.fstat(entry)) != _fingerprint(info):
                raise SnapshotError("Source entry changed during capture: " + path)
            if private:
                _check_acl(entry)
            records[path] = _fingerprint(info)
            if stat.S_ISDIR(info.st_mode):
                if output is not None:
                    os.mkdir(name, 0o700, dir_fd=output)
                    target = os.open(name, _DIRECTORY_FLAGS, dir_fd=output)
                    _canonical_output(target, 0o700)
                children, child_records = _walk(entry, path, target, metadata, private, budget)
                manifest.update(children)
                records.update(child_records)
            elif not metadata:
                digest = hashlib.sha256()
                if output is not None:
                    target = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                     0o600, dir_fd=output)
                    _canonical_output(target, 0o600)
                while True:
                    block = os.read(entry, 1024 * 1024)
                    if not block:
                        break
                    budget[1] += len(block)
                    if budget[1] > _MAX_BYTES:
                        raise SnapshotError("Source exceeds bounded snapshot byte limit")
                    digest.update(block)
                    if target is not None:
                        view = memoryview(block)
                        while view:
                            written = os.write(target, view)
                            if written <= 0:
                                raise SnapshotError("Incomplete snapshot write")
                            view = view[written:]
                manifest[path] = digest.hexdigest()
            if _fingerprint(os.fstat(entry)) != _fingerprint(info):
                raise SnapshotError("Source entry changed during capture: " + path)
        finally:
            if target is not None:
                os.close(target)
            os.close(entry)
    if names != sorted(os.listdir(descriptor)) or _fingerprint(os.fstat(descriptor)) != _fingerprint(before):
        raise SnapshotError("Source directory changed during capture: " + relative)
    return manifest, records


def _tree_digest(manifest):
    content = "".join(path + ":" + manifest[path] + "\n" for path in sorted(manifest))
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


def _validate_retained_head(git):
    names = os.listdir(git)
    if "HEAD" in names:
        if "objects" not in names:
            raise SnapshotError("Incomplete cache Git metadata retaining HEAD is not snapshot eligible")
        objects = os.stat("objects", dir_fd=git, follow_symlinks=False)
        if not stat.S_ISDIR(objects.st_mode) and not stat.S_ISLNK(objects.st_mode):
            raise SnapshotError("Incomplete cache Git metadata retaining HEAD is not snapshot eligible")


def _validate_cache_metadata(root):
    names = os.listdir(root)
    if ".git" not in names:
        return
    info = os.stat(".git", dir_fd=root, follow_symlinks=False)
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise SnapshotError("Cache Git metadata redirect is not allowed")
    git = os.open(".git", _DIRECTORY_FLAGS, dir_fd=root)
    try:
        if _fingerprint(os.fstat(git)) != _fingerprint(info):
            raise SnapshotError("Cache Git metadata changed during capture")
        names = os.listdir(git)
        if "commondir" in names:
            raise SnapshotError("Cache Git metadata redirect is not allowed")
        _validate_retained_head(git)
        _walk(git, metadata=True)
    finally:
        os.close(git)


def preflight(source):
    _require_descriptors()
    root = _open_root(source)
    try:
        if ".git" in os.listdir(root):
            info = os.stat(".git", dir_fd=root, follow_symlinks=False)
            if stat.S_ISDIR(info.st_mode):
                git = os.open(".git", _DIRECTORY_FLAGS, dir_fd=root)
                try:
                    _validate_retained_head(git)
                finally:
                    os.close(git)
        manifest, _ = _walk(root)
        return _tree_digest(manifest)
    finally:
        os.close(root)


def _remove_contents(root):
    for name in os.listdir(root):
        info = os.stat(name, dir_fd=root, follow_symlinks=False)
        if stat.S_ISDIR(info.st_mode):
            child = os.open(name, _DIRECTORY_FLAGS, dir_fd=root)
            try:
                _remove_contents(child)
            finally:
                os.close(child)
            os.rmdir(name, dir_fd=root)
        else:
            os.unlink(name, dir_fd=root)


def materialize(source, destination, expected_tree):
    _require_descriptors()
    source_root = _open_root(source)
    parent = None
    output = None
    created = None
    name = os.path.basename(os.fspath(destination))
    try:
        _validate_cache_metadata(source_root)
        manifest, original = _walk(source_root)
        if _tree_digest(manifest) != expected_tree:
            raise SnapshotError("Clean source tree SHA-256 mismatch")
        parent = _open_private_parent(os.path.dirname(os.fspath(destination)))
        try:
            os.mkdir(name, 0o700, dir_fd=parent)
        except FileExistsError as error:
            raise SnapshotError("Snapshot destination already exists") from error
        output = os.open(name, _DIRECTORY_FLAGS, dir_fd=parent)
        created = _identity(os.fstat(output))
        _canonical_output(output, 0o700)
        copied, captured = _walk(source_root, output=output)
        after, final_records = _walk(source_root)
        _validate_cache_metadata(source_root)
        if captured != original or final_records != original or after != manifest:
            raise SnapshotError("Source changed during snapshot capture")
        if _tree_digest(copied) != expected_tree:
            raise SnapshotError("Captured source tree SHA-256 mismatch")
        output_manifest, _ = _walk(output, private=True)
        if _tree_digest(output_manifest) != expected_tree:
            raise SnapshotError("Private source tree SHA-256 mismatch")
    except BaseException as primary:
        if created is not None:
            try:
                current = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if _identity(current) != created or not stat.S_ISDIR(current.st_mode):
                    raise SnapshotError("Owned staging identity changed; preserving unknown state")
                _remove_contents(output)
                os.rmdir(name, dir_fd=parent)
            except (OSError, SnapshotError) as cleanup:
                raise CleanupError(str(primary) + "; snapshot cleanup incomplete: " + str(cleanup)) from primary
        raise
    finally:
        if output is not None:
            os.close(output)
        if parent is not None:
            os.close(parent)
        os.close(source_root)


def marker_bytes(revision, clean, final):
    if not re.fullmatch(r"[0-9a-f]{40}", revision) or not re.fullmatch(r"[0-9a-f]{64}", clean):
        raise SnapshotError("Snapshot approval requires an exact revision and clean tree digest")
    if final and not re.fullmatch(r"[0-9a-f]{64}", final):
        raise SnapshotError("Snapshot final tree digest is invalid")
    return ("klogg-private-source-v1\nkind=approved-content-snapshot\napproval-revision=" + revision
            + "\nactual-git-commit=absent\nclean-tree-sha256=" + clean
            + "\nfinal-tree-sha256=" + final + "\n").encode("ascii")


def write_marker(marker, revision, clean, final, kind="approved-content-snapshot"):
    _require_descriptors()
    if kind == "pinned-git":
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise SnapshotError("Private Git marker requires an exact approved revision")
        content = (revision + "\n").encode("ascii")
    else:
        content = marker_bytes(revision, clean, final)
    parent = _open_private_parent(os.path.dirname(os.fspath(marker)))
    descriptor = None
    name = os.path.basename(os.fspath(marker))
    try:
        descriptor = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600, dir_fd=parent)
        _canonical_output(descriptor, 0o600)
        view = memoryview(content)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise SnapshotError("Incomplete private marker write")
            view = view[written:]
    finally:
        if descriptor is not None:
            os.close(descriptor)
        os.close(parent)


def _read_marker(marker):
    _require_descriptors()
    info = os.lstat(marker)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > 4096:
        raise SnapshotError("Private source marker is not a bounded ordinary owned file")
    _private(info)
    descriptor = os.open(marker, _FILE_FLAGS)
    try:
        if _fingerprint(os.fstat(descriptor)) != _fingerprint(info):
            raise SnapshotError("Private source marker changed")
        _check_acl(descriptor, parent=True)
        content = os.read(descriptor, 4097)
        if _fingerprint(os.fstat(descriptor)) != _fingerprint(info):
            raise SnapshotError("Private source marker changed")
        return content
    finally:
        os.close(descriptor)


def classify_marker(marker):
    content = _read_marker(marker)
    if content.startswith(b"klogg-private-source-v1\n"):
        return "approved-content-snapshot"
    if re.fullmatch(b"[0-9a-f]{40}\n", content):
        return "pinned-git"
    raise SnapshotError("Private source marker does not match an explicit provenance kind")


def verify_snapshot(source, marker, revision, clean, final):
    parent = _open_private_parent(os.path.dirname(os.fspath(source)))
    os.close(parent)
    if _read_marker(marker) != marker_bytes(revision, clean, final):
        raise SnapshotError("Snapshot marker does not match independently supplied approval")
    root = _open_root(source, private=True)
    try:
        if ".git" in os.listdir(root):
            raise SnapshotError("Snapshot provenance requires actual Git metadata/commit to be absent")
        manifest, _ = _walk(root, private=True)
        digest = _tree_digest(manifest)
        if digest != clean and (not final or digest != final):
            raise SnapshotError("Snapshot must match trusted clean or final patched tree; dirty/interrupted state rejected")
        return digest
    finally:
        os.close(root)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("preflight", "namespace", "prepare-parent", "materialize", "write-marker", "classify", "verify"), required=True)
    parser.add_argument("--kind", choices=("approved-content-snapshot", "pinned-git"), default="approved-content-snapshot")
    parser.add_argument("--source")
    parser.add_argument("--destination")
    parser.add_argument("--marker")
    parser.add_argument("--revision")
    parser.add_argument("--clean-tree")
    parser.add_argument("--final-tree", default="")
    options = parser.parse_args(argv)
    try:
        if options.operation == "preflight":
            preflight(options.source)
        elif options.operation == "namespace":
            _require_descriptors()
            descriptor = _open_private_parent(options.source)
            os.close(descriptor)
        elif options.operation == "prepare-parent":
            prepare_parent(options.source)
        elif options.operation == "write-marker":
            write_marker(options.marker, options.revision, options.clean_tree,
                         options.final_tree, options.kind)
        elif options.operation == "materialize":
            marker = marker_bytes(options.revision, options.clean_tree, options.final_tree)
            materialize(options.source, options.destination, options.clean_tree)
            sys.stdout.buffer.write(marker)
        elif options.operation == "classify":
            print(classify_marker(options.marker))
        else:
            verify_snapshot(options.source, options.marker, options.revision,
                            options.clean_tree, options.final_tree)
        return 0
    except (OSError, SnapshotError, TypeError) as error:
        sys.stderr.write("efsw snapshot: " + str(error) + "\n")
        return 5 if isinstance(error, CleanupError) else 1


if __name__ == "__main__":
    sys.exit(main())
