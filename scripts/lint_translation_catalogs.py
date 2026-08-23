#!/usr/bin/env python3
"""Check app-language build parity and guarded translation coverage.

Run locally with: python3 scripts/lint_translation_catalogs.py
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
GUARDED_SOURCE_CONTEXTS = {
    Path("src/ui/src/predefinedfilterscombobox.cpp"): "PredefinedFiltersComboBox",
    Path("src/ui/src/mainwindow.cpp"): None,
}
DIRECT_TR_RE = re.compile(
    r'(?<![\w:])tr\s*\(\s*"(?P<literal>(?:\\.|[^"\\])*)"', re.DOTALL
)
EXPLICIT_TRANSLATE_NOOP_RE = re.compile(
    r'QT_TRANSLATE_NOOP\s*\(\s*"(?P<context>(?:\\.|[^"\\])*)"\s*,\s*'
    r'"(?P<literal>(?:\\.|[^"\\])*)"',
    re.DOTALL,
)


def supported_languages(xml_text: str) -> list[str]:
    root = ET.fromstring(xml_text)
    return [
        language.attrib["ietfCode"]
        for language in root.findall("language")
        if language.attrib.get("ietfCode")
    ]


def _cmake_call_body(cmake_text: str, command: str) -> str:
    match = re.search(
        rf"\b{re.escape(command)}\s*\((?P<body>.*?)\)",
        cmake_text,
        flags=re.DOTALL | re.IGNORECASE,
    )
    return match.group("body") if match is not None else ""


def generated_catalog_languages(cmake_text: str) -> set[str]:
    body = _cmake_call_body(cmake_text, "set")
    if not re.match(r"\s*TS_FILES(?:\s|$)", body):
        match = re.search(
            r"\bset\s*\(\s*TS_FILES(?P<body>.*?)\)",
            cmake_text,
            flags=re.DOTALL | re.IGNORECASE,
        )
        body = match.group("body") if match is not None else ""
    return set(re.findall(r"/i18n/([A-Za-z][A-Za-z0-9_-]*)\.ts\b", body))


def packaged_qt_translation_languages(cmake_text: str) -> set[str]:
    body = _cmake_call_body(cmake_text, "add_qt_translations_resource")
    tokens = re.findall(r"[A-Za-z][A-Za-z0-9_-]*", body)
    return set(tokens[1:]) if tokens else set()


def _decode_cpp_string(content: str) -> str | None:
    try:
        return ast.literal_eval(f'"{content}"')
    except (SyntaxError, ValueError):
        return None


def direct_translation_literals(source_text: str) -> set[str]:
    return {
        literal
        for match in DIRECT_TR_RE.finditer(source_text)
        if (literal := _decode_cpp_string(match.group("literal"))) is not None
    }


def explicit_translation_messages(source_text: str) -> set[tuple[str, str]]:
    messages: set[tuple[str, str]] = set()
    for match in EXPLICIT_TRANSLATE_NOOP_RE.finditer(source_text):
        context = _decode_cpp_string(match.group("context"))
        literal = _decode_cpp_string(match.group("literal"))
        if context is not None and literal is not None:
            messages.add((context, literal))
    return messages


def source_translation_messages(
    repo_root: Path, source_contexts: dict[Path, str | None]
) -> set[tuple[str, str]]:
    messages: set[tuple[str, str]] = set()
    for source_path, direct_context in source_contexts.items():
        source_text = (repo_root / source_path).read_text(encoding="utf-8")
        messages.update(explicit_translation_messages(source_text))
        if direct_context is not None:
            messages.update(
                (direct_context, literal)
                for literal in direct_translation_literals(source_text)
            )
    return messages


def catalog_messages(ts_text: str) -> set[tuple[str, str]]:
    root = ET.fromstring(ts_text)
    messages: set[tuple[str, str]] = set()
    for context in root.findall("context"):
        context_name = context.findtext("name")
        if context_name is None:
            continue
        for message in context.findall("message"):
            source = message.find("source")
            translation = message.find("translation")
            if (
                source is not None
                and translation is not None
                and translation.attrib.get("type") != "obsolete"
            ):
                messages.add((context_name, source.text or ""))
    return messages


def build_parity_issues(repo_root: Path) -> list[str]:
    i18n_dir = repo_root / "src" / "app" / "i18n"
    cmake_path = repo_root / "src" / "app" / "CMakeLists.txt"
    languages_path = i18n_dir / "Languages.xml"

    supported_list = supported_languages(languages_path.read_text(encoding="utf-8"))
    supported = set(supported_list)
    generated = generated_catalog_languages(cmake_path.read_text(encoding="utf-8"))
    packaged_qt = packaged_qt_translation_languages(cmake_path.read_text(encoding="utf-8"))
    available_catalogs = {path.stem for path in i18n_dir.glob("*.ts")}
    expected_qt_translations = supported - {"en"}

    issues: list[str] = []
    for language in supported_list:
        if language not in available_catalogs:
            issues.append(f"{language}: missing src/app/i18n/{language}.ts catalog")
        if language not in generated:
            issues.append(
                f"{language}: missing from TS_FILES translation generation/app resource packaging"
            )
        if language in expected_qt_translations and language not in packaged_qt:
            issues.append(
                f"{language}: missing from add_qt_translations_resource Qt resource packaging"
            )

    for language in sorted(available_catalogs - supported):
        issues.append(
            f"{language}: catalog is not listed in src/app/i18n/Languages.xml"
        )
    for language in sorted(generated - supported):
        issues.append(
            f"{language}: TS_FILES entry is not listed in src/app/i18n/Languages.xml"
        )
    for language in sorted(packaged_qt - expected_qt_translations):
        issues.append(
            f"{language}: Qt resource entry is not required by src/app/i18n/Languages.xml"
        )
    return issues


def catalog_coverage_issues(
    repo_root: Path,
    source_contexts: dict[Path, str | None] = GUARDED_SOURCE_CONTEXTS,
) -> list[str]:
    i18n_dir = repo_root / "src" / "app" / "i18n"
    languages_path = i18n_dir / "Languages.xml"
    required_messages = source_translation_messages(repo_root, source_contexts)

    issues: list[str] = []
    for language in supported_languages(languages_path.read_text(encoding="utf-8")):
        catalog_path = i18n_dir / f"{language}.ts"
        if not catalog_path.exists():
            continue
        messages = catalog_messages(catalog_path.read_text(encoding="utf-8"))
        for context, source in sorted(required_messages - messages):
            issues.append(
                f"src/app/i18n/{language}.ts: missing guarded translation "
                f"{context}::{source!r}"
            )
    return issues


def repository_issues(repo_root: Path) -> list[str]:
    return build_parity_issues(repo_root) + catalog_coverage_issues(repo_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)

    issues = repository_issues(args.repo_root.resolve())
    for issue in issues:
        print(issue)
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
