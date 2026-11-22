#!/usr/bin/env python3
"""Check the directly invoked control CLI in the final, unmodified AppImage."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def bundled_file(root, path):
    path = Path(path).resolve(strict=True)
    if root not in path.parents or not path.is_file():
        raise ValueError("expected a file inside the extracted AppImage: " + str(path))
    return path


def verify(package):
    package = Path(package).resolve(strict=True)
    with tempfile.TemporaryDirectory() as directory:
        # Do not inherit AppRun/SDK loader overrides or the user's settings.
        options = dict(cwd=directory, env={"HOME": directory, "LC_ALL": "C", "PATH": os.environ["PATH"]},
                       stdin=subprocess.DEVNULL, check=True, timeout=90, text=True)
        subprocess.run([str(package), "--appimage-extract"], stdout=subprocess.DEVNULL, **options)
        root = Path(directory).resolve() / "squashfs-root"
        cli = bundled_file(root, root / "usr/bin/kloggctl")
        # Starting successfully in a build image is not proof of portability:
        # ldd must resolve the Qt closure to bundled files, not the build SDK.
        linked = subprocess.run(["ldd", str(cli)], stdout=subprocess.PIPE, **options).stdout
        modules = set()
        for line in linked.splitlines():
            if line.lstrip().startswith("libQt"):
                match = re.fullmatch(r"\s*(libQt\S+)\s+=>\s+(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*", line)
                if not match:
                    raise ValueError("unresolved Qt dependency: " + line)
                bundled_file(root, match[2])
                modules.add(match[1])
        missing = {"libQt5Core.so.5", "libQt5Network.so.5"} - modules
        if missing:
            raise ValueError("missing Qt dependencies: " + ", ".join(sorted(missing)))
        version = subprocess.run([str(cli), "--version"], stdout=subprocess.PIPE, **options).stdout
        if not json.loads(version)["result"]["version"]:
            raise ValueError("empty kloggctl version")
        help_text = subprocess.run([str(cli), "--help"], stdout=subprocess.PIPE, **options).stdout
        if "Usage: kloggctl" not in help_text:
            raise ValueError("missing kloggctl usage")


if __name__ == "__main__":
    verify(sys.argv[1])
