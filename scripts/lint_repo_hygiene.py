#!/usr/bin/env python3
"""Reject committed binary blobs and non-English text in the repository.

Two repository-hygiene rules, checked against every tracked file:

1. Binary files are not allowed outside the designated asset locations.
   Binary blobs bloat the git history permanently and cannot be reviewed in
   a diff. The only binaries this repository intentionally carries are the
   application icons/fonts and the website static assets, so a new binary
   anywhere else is almost certainly a mistake (a build product, a core
   dump, a screenshot that belongs in an issue, ...).

2. Code, comments, and documentation must be written in English (plus
   ordinary typographic punctuation). Non-Latin scripts (CJK, Cyrillic,
   Greek, Arabic, ...) in source or docs fragment the contributor audience
   and break reviewers who cannot read them. The translation catalogs under
   src/app/i18n/ are the single sanctioned home for non-English UI text;
   vendored third-party trees (3rdparty/, website/themes/) are exempt.
   Test files that embed non-Latin text as *fixture data* (encoding
   detection, CJK path handling) opt in per line with a trailing
   ``// lint-allow: repo-hygiene`` comment.

Non-binary files must also be valid UTF-8: a GBK/latin-1 source file is a
non-English file the compiler accepts silently until someone edits it.

Usage:
    python3 scripts/lint_repo_hygiene.py
    python3 scripts/lint_repo_hygiene.py --check-staged   # pre-commit mode

Exit codes:
    0   No findings.
    1   At least one finding (printed to stdout).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ALLOW_MARKER = "lint-allow: repo-hygiene"

# Locations that legitimately hold binary assets (application icons/fonts,
# packaging artwork, website static files, the vendored website theme).
ASSET_ROOTS = (
    "Resources/",
    "src/app/images/",
    "src/app/fonts/",
    "packaging/",
    "website/static/",
    "website/themes/",
)
ASSET_EXTENSIONS = {
    ".png",
    ".gif",
    ".jpg",
    ".jpeg",
    ".ico",
    ".icns",
    ".tif",
    ".tiff",
    ".ttf",
    ".otf",
    ".woff",
    ".woff2",
    ".eot",
}

# Directories exempt from the English-only rule: translation catalogs are
# the sanctioned home for localized UI text; vendored third-party trees are
# not our prose.
NON_ENGLISH_EXEMPT_PREFIXES = (
    "3rdparty/",
    "src/app/i18n/",
    "website/themes/",
)

# Scripts other than Latin. Everything not matched here (ASCII, Latin
# extended, general punctuation, arrows, box drawing, ...) is allowed, so
# English prose with typographic dashes/quotes or names like "café" pass.
NON_LATIN_SCRIPT_RE = re.compile(
    "["
    "\u0370-\u03ff"  # Greek and Coptic
    "\u0400-\u052f"  # Cyrillic + supplement
    "\u0590-\u05ff"  # Hebrew
    "\u0600-\u06ff"  # Arabic
    "\u0750-\u077f"  # Arabic supplement
    "\u0900-\u097f"  # Devanagari
    "\u0e00-\u0e7f"  # Thai
    "\u1100-\u11ff"  # Hangul Jamo
    "\u2e80-\u2fdf"  # CJK radicals / Kangxi
    "\u3000-\u303f"  # CJK symbols and punctuation
    "\u3040-\u30ff"  # Hiragana + Katakana
    "\u31f0-\u31ff"  # Katakana phonetic extensions
    "\u3400-\u4dbf"  # CJK ext A
    "\u4e00-\u9fff"  # CJK unified ideographs
    "\uac00-\ud7af"  # Hangul syllables
    "\uf900-\ufaff"  # CJK compatibility ideographs
    "\uff00-\uff65"  # fullwidth forms (fullwidth Latin, CJK brackets, ...)
    "\uff66-\uff9f"  # halfwidth katakana
    "\U00020000-\U0002a6df"  # CJK ext B
    "]"
)

_UTF8_BOM = b"\xef\xbb\xbf"


def is_binary(data: bytes) -> bool:
    """NUL byte in the first 8 KiB: the same heuristic git itself uses."""
    return b"\0" in data[:8192]


def binary_issue(relative_path: str, data: bytes) -> str | None:
    if not is_binary(data):
        return None
    path = Path(relative_path)
    under_asset_root = relative_path.startswith(ASSET_ROOTS)
    allowed_extension = path.suffix.lower() in ASSET_EXTENSIONS
    if under_asset_root and allowed_extension:
        return None
    return (
        f"binary file committed outside the designated asset locations "
        f"(asset roots: {', '.join(ASSET_ROOTS)}; asset extensions: "
        f"{', '.join(sorted(ASSET_EXTENSIONS))}). Binary blobs cannot be "
        f"reviewed in a diff and bloat the git history permanently. Do not "
        f"commit it; if it is a genuine new asset, extend ASSET_ROOTS / "
        f"ASSET_EXTENSIONS in scripts/lint_repo_hygiene.py with the "
        f"rationale in the commit message."
    )


def utf8_issue(relative_path: str, data: bytes) -> str | None:
    if is_binary(data):
        return None
    payload = data[len(_UTF8_BOM) :] if data.startswith(_UTF8_BOM) else data
    try:
        payload.decode("utf-8")
    except UnicodeDecodeError as error:
        return (
            f"text file is not valid UTF-8 ({error.reason} near byte "
            f"{error.start}). Source, comments, and docs must be UTF-8; a "
            f"legacy encoding (GBK, latin-1, ...) usually means non-English "
            f"text was pasted in. Re-save the file as UTF-8."
        )
    return None


def non_english_issues(relative_path: str, text: str) -> list[tuple[int, str]]:
    if relative_path.startswith(NON_ENGLISH_EXEMPT_PREFIXES):
        return []
    findings: list[tuple[int, str]] = []
    for line_num, line in enumerate(text.splitlines(), start=1):
        offenders = sorted(set(NON_LATIN_SCRIPT_RE.findall(line)))
        if not offenders or ALLOW_MARKER in line:
            continue
        rendered = " ".join(f"U+{ord(ch):04X} {ch}" for ch in offenders)
        findings.append(
            (
                line_num,
                f"non-English (non-Latin script) character(s): {rendered}. "
                f"Code, comments, and docs must be written in English so "
                f"every contributor can review them. Localized UI text "
                f"belongs in src/app/i18n/*.ts. If this line is intentional "
                f"fixture data (encoding/CJK handling test), append "
                f"`{ALLOW_MARKER}` to it.",
            )
        )
    return findings


def tracked_files(repo_root: Path) -> list[str]:
    completed = subprocess.run(
        ["git", "ls-files"],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=True,
    )
    return completed.stdout.splitlines()


def staged_files(repo_root: Path) -> list[str]:
    completed = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=True,
    )
    return completed.stdout.splitlines()


def check_file(repo_root: Path, relative_path: str) -> int:
    try:
        data = (repo_root / relative_path).read_bytes()
    except OSError:
        return 0

    issues = 0
    issue = binary_issue(relative_path, data)
    if issue is not None:
        print(f"[repo-hygiene] binary-file\n  at {relative_path}\n  {issue}\n")
        return 1
    if is_binary(data):
        # An allowlisted binary asset: text checks do not apply.
        return 0

    issue = utf8_issue(relative_path, data)
    if issue is not None:
        print(f"[repo-hygiene] non-utf8-text\n  at {relative_path}\n  {issue}\n")
        return 1

    for line_num, message in non_english_issues(
        relative_path, data.decode("utf-8")
    ):
        print(f"[repo-hygiene] non-english-text")
        print(f"  at {relative_path}:{line_num}")
        print(f"  {message}")
        print()
        issues += 1
    return issues


def main(argv: list[str]) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-staged",
        action="store_true",
        help="Only scan files staged for commit (pre-commit hook mode).",
    )
    args = parser.parse_args(argv)

    files = staged_files(repo_root) if args.check_staged else tracked_files(repo_root)
    issues = sum(check_file(repo_root, path) for path in files)

    if issues:
        print(f"Found {issues} repo-hygiene issue(s).")
        print(f"Append `{ALLOW_MARKER}` on a specific line to override an intentional use.")
        return 1
    print("OK: no repo-hygiene issues found.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
