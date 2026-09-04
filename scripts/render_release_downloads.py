#!/usr/bin/env python3
"""Render deterministic release-download Markdown from a verified publication manifest."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

from source_publication_identity import normalize_base_url
from verify_source_publication_manifest import (
    PublicationError,
    read_json,
    supported_package_display,
    verify_manifest,
)


def infer_base_url(page_url: object, tag: str) -> str:
    if not isinstance(page_url, str):
        raise PublicationError("release page URL is missing")
    suffix = f"/releases/tag/{tag}"
    if not page_url.endswith(suffix):
        raise PublicationError("release page URL does not match the release tag")
    return normalize_base_url(page_url[: -len(suffix)])


def render_body(
    document: dict,
    base_url: str,
    changelog: str,
    evidence_description: str | None,
) -> str:
    release = document["release"]
    channel = document["channel"]
    tag = release["tag"]
    version = release["version"]
    expected_packages = supported_package_display(version)
    packages = document["packages"]
    if not isinstance(packages, list):
        raise PublicationError("release package mapping is malformed")
    by_name: dict[str, dict] = {}
    for package in packages:
        if not isinstance(package, dict) or not isinstance(package.get("file_name"), str):
            raise PublicationError("release package mapping is malformed")
        name = package["file_name"]
        if name in by_name:
            raise PublicationError(f"ambiguous release package mapping: {name}")
        by_name[name] = package
    if set(by_name) != set(expected_packages):
        missing = sorted(set(expected_packages) - set(by_name))
        extra = sorted(set(by_name) - set(expected_packages))
        raise PublicationError(
            f"release package mapping is incomplete or extended; missing={missing}, extra={extra}"
        )
    for name, expected_display in expected_packages.items():
        display = by_name[name].get("display")
        if not isinstance(display, dict) or (
            display.get("section"), display.get("label")
        ) != expected_display:
            raise PublicationError(f"ambiguous release package display mapping: {name}")

    def link(label: str, name: str) -> str:
        return f"- [{label}]({base_url}/releases/download/{tag}/{name})"

    if channel == "stable":
        introduction = "This immutable release was verified from its publication manifest."
    elif channel == "continuous":
        introduction = (
            f"The [continuous release page]({base_url}/releases/tag/continuous) is durable. "
            "The versioned asset links below identify only the current build; they are replaced "
            "by the next successful publication and are not archival permalinks."
        )
    else:
        raise PublicationError("release channel is unsupported")

    lines = [
        release["public_name"],
        "",
        introduction,
        "",
        "## Changes",
        changelog.rstrip("\n"),
        "",
        "## Downloads",
    ]
    for group in ("Windows", "Linux", "macOS"):
        lines.extend(["", f"### {group}"])
        if group == "macOS":
            description = evidence_description
            if description is None:
                description = (
                    "Signed evidence includes signing and notarization receipts."
                    if document["evidence_level"] == "signed"
                    else "These disk images are unsigned validation artifacts, not signed or notarized releases."
                )
            if not description.strip():
                raise PublicationError("release evidence description is empty")
            lines.append(description.strip())
        for name, (section, label) in expected_packages.items():
            if section == group:
                lines.append(link(label, name))

    lines.extend(["", "### Corresponding source and legal materials"])
    components = document["components"]
    for component in ("adb-helper", "ios-native"):
        record = components[component]
        lines.append(link(record["display_name"], record["source_archive"]["file_name"]))
        receipt = record["source_set_receipt"]
        lines.append(link(receipt["display_name"], receipt["file_name"]))
    for asset in document["support_assets"]:
        lines.append(link(asset["display_name"], asset["file_name"]))
    lines.extend(
        [
            "",
            "### Verification",
            link("Publication manifest", pathlib.Path(document["manifest_path"]).name),
            link("Release evidence", document["evidence_archive"]["file_name"]),
            link("SHA-256 checksums", document["checksums"]["file_name"]),
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--assets-root", required=True, type=pathlib.Path)
    parser.add_argument("--changelog-file", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--base-url")
    parser.add_argument("--tag")
    parser.add_argument("--channel", choices=("stable", "continuous"))
    parser.add_argument("--version")
    parser.add_argument("--evidence-description")
    args = parser.parse_args()

    try:
        document = read_json(args.manifest, "release publication manifest")
        release = document.get("release")
        if not isinstance(release, dict):
            raise PublicationError("release identity is missing from manifest")
        channel = args.channel or document.get("channel")
        tag = args.tag or release.get("tag")
        version = args.version or release.get("version")
        evidence_level = document.get("evidence_level")
        commit = release.get("commit")
        if not all(isinstance(value, str) and value for value in (channel, tag, version, evidence_level, commit)):
            raise PublicationError("release manifest identity is malformed")
        base_url = normalize_base_url(args.base_url) if args.base_url else infer_base_url(release.get("page_url"), tag)
        if args.channel is not None and document.get("channel") != args.channel:
            raise PublicationError("release channel mismatch")
        if args.tag is not None and release.get("tag") != args.tag:
            raise PublicationError("release tag mismatch")
        if args.version is not None and release.get("version") != args.version:
            raise PublicationError("release version mismatch")
        expected_tag = "continuous" if channel == "continuous" else f"v{version}"
        if tag != expected_tag:
            raise PublicationError("release channel/tag mismatch")
        verify_manifest(
            args.manifest,
            args.assets_root,
            channel,
            evidence_level,
            tag,
            version,
            commit,
            base_url,
        )
        changelog = args.changelog_file.read_text(encoding="utf-8")
        render_document = dict(document)
        render_document["manifest_path"] = str(args.manifest)
        rendered = render_body(
            render_document,
            base_url,
            changelog,
            args.evidence_description,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        return 0
    except (OSError, PublicationError, json.JSONDecodeError) as error:
        if args.output.exists():
            args.output.unlink()
        print(f"release download rendering failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
