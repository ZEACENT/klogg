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
import os
import re
import subprocess
import sys
import unicodedata
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

# Non-Latin writing systems, rejected on sight. Symbol/punctuation blocks
# (arrows, box drawing, geometric shapes, dingbats, emoji, ...) stay allowed:
# they legitimately appear in English docs and diagrams. Latin script
# extensions (Vietnamese diacritics U+1E00-1EFF, U+2C60+, U+A720+) stay
# allowed; Greek extended (U+1F00+) and every other script below do not.
NON_LATIN_SCRIPT_RE = re.compile(
    "["
    "\u0370-\u03ff\u1f00-\u1fff"  # Greek, Coptic, Greek extended
    "\u0400-\u052f\u2de0-\u2dff\ua640-\ua69f"  # Cyrillic + supplements
    "\u0530-\u058f\ufb13-\ufb17"  # Armenian + ligatures
    "\u0590-\u05ff"  # Hebrew
    "\u0600-\u06ff\u0750-\u077f\u08a0-\u08ff\ufb50-\ufdff"  # Arabic family
    "\ufe70-\ufeff\u0700-\u074f\u0780-\u07bf"  # Arabic forms, Syriac, Thaana
    "\u07c0-\u07ff\u0800-\u083f\u0840-\u085f\u0860-\u086f"  # NKo, Samaritan, Mandaic
    "\u0900-\u097f\u0980-\u09ff\u0a00-\u0a7f\u0a80-\u0aff"  # Indic N-C
    "\u0b00-\u0b7f\u0b80-\u0bff\u0c00-\u0c7f\u0c80-\u0cff"  # Indic O-K
    "\u0d00-\u0d7f\u0d80-\u0dff"  # Malayalam, Sinhala
    "\u0e00-\u0e7f\u0e80-\u0eff\u0f00-\u0fff"  # Thai, Lao, Tibetan
    "\u1000-\u109f\u1780-\u17ff\u19e0-\u19ff"  # Myanmar, Khmer
    "\u1950-\u197f\u1980-\u19df\uaa60-\uaa7f\uaa80-\uaadf"  # Tai scripts
    "\u1700-\u177f\u1a20-\u1a6f\ua980-\ua9df"  # Philippine, Tai Tham, Javanese
    "\ua930-\ua95f\u1900-\u194f"  # Rejang, Limbu
    "\u1800-\u18af\u18b0-\u18ff"  # Mongolian, Canadian Aboriginal
    "\u1680-\u169f\u16a0-\u16ff\u13a0-\u13ff"  # Ogham, Runic, Cherokee
    "\ua500-\ua63f\ua000-\ua4cf"  # Vai, Yi
    "\u10a0-\u10ff\u2d00-\u2d2f\u1200-\u139f\uab00-\uab2f"  # Georgian, Ethiopic
    "\u1100-\u11ff\u3130-\u318f\ua960-\ua97f\ud7b0-\ud7ff\uac00-\ud7af"  # Hangul
    "\u2e80-\u2fdf\u3000-\u303f\u3100-\u312f\u31a0-\u31bf"  # CJK radicals..Bopomofo
    "\u3190-\u319f\u31c0-\u31ef\u3200-\u32ff\u3300-\u33ff"  # Kanbun..compat
    "\U0001b000-\U0001b16f"  # Kana supplement
    "\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"  # CJK ideographs
    "\U00020000-\U0002fa1f\U00030000-\U0003234f"  # CJK ext planes
    "\ufe10-\ufe6f\uff00-\uffdc"  # CJK compat forms, small/halfwidth/fullwidth
    "]"
)

# Letters outside the Latin blocks are non-English prose no matter whether
# the specific script block was enumerated above (Adlam, Deseret, Osage,
# Cherokee Supplement, and every future Unicode addition). This
# category-based catch-all makes the denylist above belt-and-braces: it is
# still needed for non-letter codepoints such as CJK punctuation, which
# carry no letter category. Symbols, emoji, and math are unaffected because
# they are not letters.
_LATIN_LETTER_RANGES = (
    (0x0000, 0x024F),  # Basic Latin, Latin-1, Latin Extended-A/B
    (0x1E00, 0x1EFF),  # Latin Extended Additional
    (0x2C60, 0x2C7F),  # Latin Extended-C
    (0xA720, 0xA7FF),  # Latin Extended-D
    (0xAB30, 0xAB6F),  # Latin Extended-E
    (0x10780, 0x107BF),  # Latin Extended-F
    (0x1DF00, 0x1DF1F),  # Latin Extended-G
)


def _is_non_latin_letter(char: str) -> bool:
    if not unicodedata.category(char).startswith("L"):
        return False
    code = ord(char)
    return not any(start <= code <= end for start, end in _LATIN_LETTER_RANGES)

_UTF8_BOM = b"\xef\xbb\xbf"


def is_binary(data: bytes) -> bool:
    """NUL byte anywhere in the payload. Git itself samples only the first
    8 KiB, but a prohibited blob could hide its first NUL behind a longer
    textual header and slip past a sampled check; the payload is already in
    memory, so scan it all."""
    return b"\0" in data


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


def strip_utf8_bom(data: bytes) -> bytes:
    return data[len(_UTF8_BOM) :] if data.startswith(_UTF8_BOM) else data


def utf8_issue(relative_path: str, data: bytes) -> str | None:
    if is_binary(data):
        return None
    payload = strip_utf8_bom(data)
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
        offenders = sorted(
            set(NON_LATIN_SCRIPT_RE.findall(line))
            | {ch for ch in line if _is_non_latin_letter(ch)}
        )
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


def _git_entries(repo_root: Path, args: list[str]) -> list[str]:
    # -z: NUL-delimited and never C-style quoted, so filenames with non-ASCII
    # or control characters arrive literally. Line-based output under the
    # default core.quotePath would quote those names, making read_bytes() fail
    # and silently skipping the file (an OSError used to read as "no issue").
    completed = subprocess.run(
        ["git", *args], cwd=repo_root, capture_output=True, check=True
    )
    return [os.fsdecode(entry) for entry in completed.stdout.split(b"\0") if entry]


def tracked_files(repo_root: Path) -> list[str]:
    return _git_entries(repo_root, ["ls-files", "-z"])


def staged_files(repo_root: Path) -> list[str]:
    return _git_entries(
        repo_root, ["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"]
    )


def _read_index_blob(repo_root: Path, relative_path: str) -> bytes:
    completed = subprocess.run(
        ["git", "show", f":{relative_path}"],
        cwd=repo_root,
        capture_output=True,
        check=True,
    )
    return completed.stdout


def check_file(repo_root: Path, relative_path: str, from_index: bool = False) -> int:
    if from_index:
        # Staged mode audits the blob selected in the index, not the working
        # tree: reading the worktree copy lets a developer stage prohibited
        # content and pass the pre-commit check after cleaning the worktree
        # (and produces false failures the other way round). Fail closed when
        # the blob cannot be read.
        try:
            data = _read_index_blob(repo_root, relative_path)
        except (OSError, subprocess.CalledProcessError):
            print(
                "[repo-hygiene] unreadable-staged-entry\n"
                f"  at {relative_path}\n"
                "  the index blob could not be read; failing closed.\n"
            )
            return 1
    else:
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
        relative_path, strip_utf8_bom(data).decode("utf-8")
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

    if args.check_staged:
        files = staged_files(repo_root)
        issues = sum(check_file(repo_root, path, from_index=True) for path in files)
    else:
        files = tracked_files(repo_root)
        issues = sum(check_file(repo_root, path) for path in files)

    if issues:
        print(f"Found {issues} repo-hygiene issue(s).")
        print(f"Append `{ALLOW_MARKER}` on a specific line to override an intentional use.")
        return 1
    print("OK: no repo-hygiene issues found.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
