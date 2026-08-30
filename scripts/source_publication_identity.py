#!/usr/bin/env python3
"""Canonical names and URLs for external corresponding-source assets."""

from __future__ import annotations

import pathlib
import re
import urllib.parse


class SourcePublicationIdentityError(ValueError):
    pass


def validate_version(version: str) -> str:
    if re.fullmatch(r"\d{2}\.\d{2}\.\d{2}(?:\.\d+)?", version) is None:
        raise SourcePublicationIdentityError(f"invalid klogg release version: {version}")
    return version


def normalize_base_url(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url)
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or parsed.query
        or parsed.fragment
        or not parsed.path.strip("/")
    ):
        raise SourcePublicationIdentityError(f"invalid source publication base URL: {base_url}")
    normalized = urllib.parse.urlunsplit(
        (parsed.scheme, parsed.netloc, "/" + parsed.path.strip("/"), "", "")
    )
    return normalized


def validate_component(component: str) -> str:
    if component not in ("adb-helper", "ios-native"):
        raise SourcePublicationIdentityError(f"unsupported source component: {component}")
    return component


def validate_sha256(value: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise SourcePublicationIdentityError(f"invalid source archive sha256: {value}")
    return value


def validate_asset_file_name(file_name: str) -> str:
    if (
        not isinstance(file_name, str)
        or not file_name
        or file_name in (".", "..")
        or pathlib.PurePosixPath(file_name).name != file_name
        or pathlib.PureWindowsPath(file_name).name != file_name
    ):
        raise SourcePublicationIdentityError(
            f"invalid source publication file name: {file_name}"
        )
    return file_name


def published_source_name(version: str, component: str, archive_sha256: str) -> str:
    return (
        f"klogg-v{validate_version(version)}-{validate_component(component)}-source-"
        f"{validate_sha256(archive_sha256)[:12]}.tar.gz"
    )


def source_asset_url(base_url: str, tag: str, file_name: str) -> str:
    version = tag[1:] if tag.startswith("v") else tag
    if tag != "continuous" and tag != f"v{validate_version(version)}":
        raise SourcePublicationIdentityError(f"invalid source publication tag: {tag}")
    return (
        f"{normalize_base_url(base_url)}/releases/download/{tag}/"
        f"{validate_asset_file_name(file_name)}"
    )
