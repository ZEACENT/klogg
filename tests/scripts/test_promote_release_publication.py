from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PROMOTE_SCRIPT = ROOT / "scripts" / "promote_release_publication.py"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_source_publication_manifest.py"
FIXTURE_TEST = ROOT / "tests" / "scripts" / "test_external_source_asset_package_contract.py"
MANIFEST_NAME = "klogg-source-publication-manifest.json"
CHECKSUMS_NAME = "SHA256SUMS"


def load_fixture_module():
    spec = importlib.util.spec_from_file_location("publication_fixture", FIXTURE_TEST)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load publication fixture helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FIXTURES = load_fixture_module()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PromoteReleasePublicationTest(unittest.TestCase):
    SOURCE_RELEASE_ID = 987654321

    def setUp(self):
        self.fixture = FIXTURES.ConsolidatedReleasePublicationContractTest(
            methodName="runTest"
        )
        self.fixture.setUp()
        self.base = self.fixture.root
        self.source = self.base / "continuous"
        self.source.mkdir()
        self.fixture.root = self.source
        self.source_manifest, self.source_document = self.fixture.make_publication(
            channel="continuous", evidence_level="validation"
        )
        self.output = self.base / "stable"
        self.metadata_path = self.base / "source-release-metadata.json"
        self.metadata = self.make_metadata()
        self.write_metadata()

    def tearDown(self):
        self.fixture.tearDown()

    def make_metadata(self) -> dict:
        records = []
        for offset, path in enumerate(sorted(self.source.iterdir()), start=1):
            records.append(
                {
                    "id": 100000 + offset,
                    "name": path.name,
                    "sha256": sha256(path),
                }
            )
        return {
            "schema_version": 1,
            "metadata_kind": "klogg-continuous-publication-source",
            "source_release_id": self.SOURCE_RELEASE_ID,
            "source_tag_sha": self.source_document["release"]["commit"],
            "source_assets": records,
        }

    def write_metadata(self) -> None:
        self.metadata_path.write_text(
            json.dumps(self.metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def run_promoter(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(PROMOTE_SCRIPT),
                "--source-manifest",
                str(self.source_manifest),
                "--source-assets-root",
                str(self.source),
                "--source-metadata",
                str(self.metadata_path),
                "--output",
                str(self.output),
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def run_verifier(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(VERIFY_SCRIPT),
                "--manifest",
                str(self.output / MANIFEST_NAME),
                "--assets-root",
                str(self.output),
                "--expected-channel",
                "stable",
                "--expected-evidence-level",
                "validation",
                "--expected-tag",
                f"v{self.fixture.VERSION}",
                "--expected-version",
                self.fixture.VERSION,
                "--expected-commit",
                self.fixture.COMMIT,
                "--expected-base-url",
                self.fixture.BASE_URL,
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def refresh_output_checksums(self) -> None:
        names = sorted(
            path.name
            for path in self.output.iterdir()
            if path.is_file() and path.name != CHECKSUMS_NAME
        )
        (self.output / CHECKSUMS_NAME).write_text(
            "".join(f"{sha256(self.output / name)} *{name}\n" for name in names),
            encoding="utf-8",
        )

    def rewrite_output_manifest(self, document: dict) -> None:
        (self.output / MANIFEST_NAME).write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        self.refresh_output_checksums()

    def promote_successfully(self) -> dict:
        result = self.run_promoter()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.output / MANIFEST_NAME).read_text(encoding="utf-8"))

    def test_promotes_verified_continuous_publication_without_mutating_payloads(self):
        source_bytes = {
            path.name: path.read_bytes()
            for path in self.source.iterdir()
            if path.name not in {MANIFEST_NAME, CHECKSUMS_NAME}
        }
        source_manifest_hash = sha256(self.source_manifest)

        document = self.promote_successfully()

        self.assertEqual(len(list(self.output.iterdir())), 23)
        self.assertEqual(document["schema_version"], 3)
        self.assertEqual(document["channel"], "stable")
        self.assertEqual(document["evidence_level"], "validation")
        self.assertEqual(
            document["release"],
            {
                "tag": f"v{self.fixture.VERSION}",
                "version": self.fixture.VERSION,
                "commit": self.fixture.COMMIT,
                "mutable": False,
                "public_name": f"Release v{self.fixture.VERSION}",
                "page_url": f"{self.fixture.BASE_URL}/releases/tag/v{self.fixture.VERSION}",
                "direct_asset_urls_are_archival": True,
            },
        )
        for component in document["components"].values():
            archive = component["source_archive"]
            self.assertEqual(
                archive["url"],
                f"{self.fixture.BASE_URL}/releases/download/v{self.fixture.VERSION}/{archive['file_name']}",
            )
        promotion = document["promotion"]
        self.assertEqual(
            set(promotion),
            {
                "source_release_id",
                "source_tag_sha",
                "source_manifest_sha256",
                "source_commit",
                "source_assets",
            },
        )
        self.assertEqual(promotion["source_release_id"], self.SOURCE_RELEASE_ID)
        self.assertEqual(promotion["source_tag_sha"], self.fixture.COMMIT)
        self.assertEqual(promotion["source_manifest_sha256"], source_manifest_hash)
        self.assertEqual(promotion["source_commit"], self.fixture.COMMIT)
        self.assertEqual(
            promotion["source_assets"],
            sorted(self.metadata["source_assets"], key=lambda item: item["name"]),
        )
        for name, content in source_bytes.items():
            self.assertEqual((self.output / name).read_bytes(), content, name)
        self.assertEqual(
            self.source_manifest.read_bytes(),
            json.dumps(self.source_document, indent=2, sort_keys=True).encode("utf-8")
            + b"\n",
        )
        verified = self.run_verifier()
        self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)

    def test_rejects_malformed_duplicate_missing_and_extra_source_metadata(self):
        mutations = {
            "missing field": lambda value: value.pop("source_release_id"),
            "extra field": lambda value: value.__setitem__("unexpected", True),
            "boolean release id": lambda value: value.__setitem__(
                "source_release_id", True
            ),
            "duplicate asset id": lambda value: value["source_assets"][1].__setitem__(
                "id", value["source_assets"][0]["id"]
            ),
            "duplicate asset name": lambda value: value["source_assets"][1].__setitem__(
                "name", value["source_assets"][0]["name"]
            ),
            "missing asset": lambda value: value["source_assets"].pop(),
            "extra asset": lambda value: value["source_assets"].append(
                {"id": 999999, "name": "extra.bin", "sha256": "0" * 64}
            ),
            "extra asset field": lambda value: value["source_assets"][0].__setitem__(
                "size", 1
            ),
        }
        original = json.loads(json.dumps(self.metadata))
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                self.metadata = json.loads(json.dumps(original))
                mutate(self.metadata)
                self.write_metadata()
                result = self.run_promoter()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_rejects_malformed_json_unsafe_names_and_non_asset_entries(self):
        self.metadata_path.write_text("{", encoding="utf-8")
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

        self.metadata = self.make_metadata()
        self.metadata["source_assets"][0]["name"] = "../escape"
        self.write_metadata()
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

        self.metadata = self.make_metadata()
        self.write_metadata()
        (self.source / "unexpected-directory").mkdir()
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_rejects_wrong_source_identity_and_source_asset_hash_mismatch(self):
        for label, mutate in (
            (
                "wrong tag sha",
                lambda value: value.__setitem__("source_tag_sha", "b" * 40),
            ),
            (
                "wrong manifest digest",
                lambda value: next(
                    item
                    for item in value["source_assets"]
                    if item["name"] == MANIFEST_NAME
                ).__setitem__("sha256", "b" * 64),
            ),
            (
                "wrong payload digest",
                lambda value: next(
                    item
                    for item in value["source_assets"]
                    if item["name"].endswith(".deb")
                ).__setitem__("sha256", "b" * 64),
            ),
        ):
            with self.subTest(label=label):
                self.metadata = self.make_metadata()
                mutate(self.metadata)
                self.write_metadata()
                result = self.run_promoter()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_rejects_mutated_source_payload_and_symlink(self):
        payload = next(path for path in self.source.iterdir() if path.name.endswith(".deb"))
        payload.write_bytes(payload.read_bytes() + b"mutation")
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

        self.fixture.tempdir.cleanup()
        self.fixture.setUp()
        self.base = self.fixture.root
        self.source = self.base / "continuous"
        self.source.mkdir()
        self.fixture.root = self.source
        self.source_manifest, self.source_document = self.fixture.make_publication(
            channel="continuous", evidence_level="validation"
        )
        self.output = self.base / "stable"
        self.metadata_path = self.base / "source-release-metadata.json"
        self.metadata = self.make_metadata()
        self.write_metadata()
        payload = next(path for path in self.source.iterdir() if path.name.endswith(".deb"))
        content = payload.read_bytes()
        target = self.base / "payload-target"
        target.write_bytes(content)
        payload.unlink()
        try:
            os.symlink(target, payload)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks are unavailable")
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_rejects_existing_output_without_modifying_it(self):
        self.output.mkdir()
        sentinel = self.output / "sentinel"
        sentinel.write_bytes(b"preserve me")
        result = self.run_promoter()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"preserve me")

    def test_v3_verifier_rejects_invalid_promotion_lineage(self):
        original = self.promote_successfully()
        mutations = {
            "extra promotion field": lambda value: value["promotion"].__setitem__(
                "unexpected", True
            ),
            "boolean source release id": lambda value: value["promotion"].__setitem__(
                "source_release_id", True
            ),
            "extra source asset field": lambda value: value["promotion"][
                "source_assets"
            ][0].__setitem__("unexpected", True),
            "duplicate source asset name": lambda value: value["promotion"][
                "source_assets"
            ][1].__setitem__(
                "name", value["promotion"]["source_assets"][0]["name"]
            ),
            "wrong source commit": lambda value: value["promotion"].__setitem__(
                "source_commit", "b" * 40
            ),
            "wrong tag sha": lambda value: value["promotion"].__setitem__(
                "source_tag_sha", "b" * 40
            ),
            "wrong source manifest hash": lambda value: value["promotion"].__setitem__(
                "source_manifest_sha256", "b" * 64
            ),
            "unsorted source assets": lambda value: value["promotion"][
                "source_assets"
            ].reverse(),
            "missing source asset": lambda value: value["promotion"][
                "source_assets"
            ].pop(),
            "duplicate source asset id": lambda value: value["promotion"][
                "source_assets"
            ][1].__setitem__(
                "id", value["promotion"]["source_assets"][0]["id"]
            ),
            "payload lineage mismatch": lambda value: next(
                item
                for item in value["promotion"]["source_assets"]
                if item["name"].endswith(".deb")
            ).__setitem__("sha256", "b" * 64),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                document = json.loads(json.dumps(original))
                mutate(document)
                self.rewrite_output_manifest(document)
                result = self.run_verifier()
                self.assertNotEqual(result.returncode, 0, label)


if __name__ == "__main__":
    unittest.main()
