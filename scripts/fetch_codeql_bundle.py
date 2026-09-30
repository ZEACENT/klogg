"""Fetch the pinned official CodeQL bundle for the traced analysis job.

The bundle identity lives in the reviewed ci/environments/role-materials.json
pin. This script downloads exactly that release asset, verifies its SHA-256,
and exposes the verified tarball path through the GitHub step output so the
pinned codeql-action init step can consume it via its `tools` input. The
bundle stays in the job's private directory and is never republished: the
CodeQL CLI license forbids redistribution.
"""

import argparse
import json
import pathlib
import re
import sys

import prefetch_adb_helper_sources as transport

BUNDLE_NAME = "codeql-bundle"
BUNDLE_URL_PREFIX = (
    "https://github.com/github/codeql-action/releases/download/codeql-bundle-v"
)
BUNDLE_URL_SUFFIX = "/codeql-bundle-linux64.tar.gz"
# The official linux64 bundle is well under 2 GiB; a larger payload means the
# pin or the upstream asset changed and must be reviewed, not consumed.
MAX_BUNDLE_BYTES = 2 * 1024**3
SHA256_RE = re.compile(r"[0-9a-f]{64}")


class BundleError(RuntimeError):
    """Raised when the pinned bundle cannot be validated or verified."""


def load_pin(root: pathlib.Path) -> dict:
    document = json.loads(
        (root / "ci" / "environments" / "role-materials.json").read_text(
            encoding="utf-8"
        )
    )
    entries = [
        material
        for material in document.get("codeql", [])
        if isinstance(material, dict) and material.get("name") == BUNDLE_NAME
    ]
    if len(entries) != 1:
        raise BundleError("role materials must pin exactly one CodeQL bundle")
    material = entries[0]
    url = material.get("url")
    sha256 = material.get("sha256")
    if (
        not isinstance(url, str)
        or not url.startswith(BUNDLE_URL_PREFIX)
        or not url.endswith(BUNDLE_URL_SUFFIX)
    ):
        raise BundleError("CodeQL bundle pin must name the official release asset")
    if not isinstance(sha256, str) or SHA256_RE.fullmatch(sha256) is None:
        raise BundleError("CodeQL bundle pin must carry a lowercase SHA-256")
    return {"url": url, "sha256": sha256}


def fetch_bundle(root, output_dir, *, downloader=None):
    if downloader is None:
        downloader = transport.download
    pin = load_pin(root)
    output_dir = pathlib.Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    destination = output_dir / "codeql-bundle-linux64.tar.gz"
    try:
        downloader(pin["url"], destination)
        if not destination.is_file() or destination.is_symlink():
            raise BundleError("CodeQL bundle download did not produce a regular file")
        if destination.stat().st_size > MAX_BUNDLE_BYTES:
            raise BundleError("CodeQL bundle exceeds the reviewed size bound")
        digest = transport.sha256(destination)
        if digest != pin["sha256"]:
            raise BundleError("CodeQL bundle checksum does not match the reviewed pin")
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    return destination


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--github-output", type=pathlib.Path)
    args = parser.parse_args(argv)

    destination = fetch_bundle(args.repo_root, args.output_dir)
    if args.github_output is not None:
        with args.github_output.open("a", encoding="ascii") as stream:
            stream.write(f"bundle={destination}\n")
    print(f"verified CodeQL bundle: {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
