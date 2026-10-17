import hashlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))

import fetch_codeql_bundle

ROOT = pathlib.Path(__file__).resolve().parents[2]

PAYLOAD = b"pinned codeql bundle payload"


def write_materials(root, materials):
    document = {"codeql": materials}
    path = root / "ci" / "environments" / "role-materials.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def pinned_material(**overrides):
    material = {
        "name": fetch_codeql_bundle.BUNDLE_NAME,
        "url": (
            "https://github.com/github/codeql-action/releases/download/"
            "codeql-bundle-v9.9.9/codeql-bundle-linux64.tar.gz"
        ),
        "sha256": hashlib.sha256(PAYLOAD).hexdigest(),
    }
    material.update(overrides)
    return material


def fake_downloader(payload):
    def download(url, destination, *, max_bytes):
        assert max_bytes == fetch_codeql_bundle.MAX_BUNDLE_BYTES
        # Deliberately ignore the budget here to retain post-download defenses.
        pathlib.Path(destination).write_bytes(payload)

    return download


class FetchCodeqlBundleTest(unittest.TestCase):
    def test_repository_pin_names_the_official_release_asset(self):
        pin = fetch_codeql_bundle.load_pin(ROOT)
        self.assertTrue(pin["url"].startswith(fetch_codeql_bundle.BUNDLE_URL_PREFIX))
        self.assertTrue(pin["url"].endswith(fetch_codeql_bundle.BUNDLE_URL_SUFFIX))
        self.assertRegex(pin["sha256"], r"^[0-9a-f]{64}$")

    def test_load_pin_requires_exactly_one_bundle_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            write_materials(root, [])
            with self.assertRaises(fetch_codeql_bundle.BundleError):
                fetch_codeql_bundle.load_pin(root)

            write_materials(root, [pinned_material(), pinned_material()])
            with self.assertRaises(fetch_codeql_bundle.BundleError):
                fetch_codeql_bundle.load_pin(root)

    def test_load_pin_rejects_unofficial_or_malformed_identities(self):
        for override in (
            {"url": "https://mirror.invalid/codeql-bundle-linux64.tar.gz"},
            {"url": "http://github.com/github/codeql-action/releases/download/codeql-bundle-v9.9.9/codeql-bundle-linux64.tar.gz"},
            {"sha256": "ABCDEF"},
            {"sha256": ""},
        ):
            with self.subTest(override=override), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                write_materials(root, [pinned_material(**override)])
                with self.assertRaises(fetch_codeql_bundle.BundleError):
                    fetch_codeql_bundle.load_pin(root)

    def test_fetch_bundle_returns_the_verified_tarball(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            bundle = fetch_codeql_bundle.fetch_bundle(
                root, output, downloader=fake_downloader(PAYLOAD)
            )
            self.assertEqual(bundle, output / "codeql-bundle-linux64.tar.gz")
            self.assertEqual(bundle.read_bytes(), PAYLOAD)

    def test_fetch_bundle_rejects_a_checksum_mismatch_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            with self.assertRaises(fetch_codeql_bundle.BundleError):
                fetch_codeql_bundle.fetch_bundle(
                    root, output, downloader=fake_downloader(b"tampered")
                )
            self.assertFalse((output / "codeql-bundle-linux64.tar.gz").exists())

    def test_fetch_bundle_rejects_oversized_payloads(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"

            original_limit = fetch_codeql_bundle.MAX_BUNDLE_BYTES
            fetch_codeql_bundle.MAX_BUNDLE_BYTES = len(PAYLOAD) - 1
            try:
                with self.assertRaises(fetch_codeql_bundle.BundleError):
                    fetch_codeql_bundle.fetch_bundle(
                        root, output, downloader=fake_downloader(PAYLOAD)
                    )
            finally:
                fetch_codeql_bundle.MAX_BUNDLE_BYTES = original_limit
            self.assertFalse((output / "codeql-bundle-linux64.tar.gz").exists())

    def test_default_transport_stops_at_the_first_overflow_byte(self):
        class Response:
            def __init__(self):
                self.read_sizes = []
                self.closed = False

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                self.closed = True
                self.partial_payloads = [path.read_bytes() for path in output.iterdir()]

            def read(self, size):
                raise AssertionError("read1 must be preferred")

            def read1(self, size):
                self.read_sizes.append(size)
                if len(self.read_sizes) == 1:
                    return b"12345678"
                if len(self.read_sizes) == 2:
                    return b"9"
                raise AssertionError("transport continued reading after the first overflow byte")

        response = Response()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            with mock.patch.object(fetch_codeql_bundle, "MAX_BUNDLE_BYTES", 8), mock.patch.object(
                fetch_codeql_bundle.transport.urllib.request, "urlopen", return_value=response
            ) as opened, mock.patch.object(fetch_codeql_bundle.transport, "sha256") as digest:
                with self.assertRaisesRegex(fetch_codeql_bundle.BundleError, "size bound") as caught:
                    fetch_codeql_bundle.fetch_bundle(root, output)
            self.assertIsInstance(caught.exception.__cause__, fetch_codeql_bundle.transport.DownloadSizeError)
            opened.assert_called_once()
            digest.assert_not_called()
            self.assertEqual(response.read_sizes, [9, 1])
            self.assertEqual(response.partial_payloads, [b"12345678"])
            self.assertTrue(response.closed)
            self.assertEqual(list(output.iterdir()), [])

    def test_default_transport_accepts_exact_bound_then_verifies_checksum(self):
        response = io.BytesIO(PAYLOAD)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            with mock.patch.object(fetch_codeql_bundle, "MAX_BUNDLE_BYTES", len(PAYLOAD)), mock.patch.object(
                fetch_codeql_bundle.transport.urllib.request, "urlopen", return_value=response
            ):
                bundle = fetch_codeql_bundle.fetch_bundle(root, output)
            self.assertEqual(bundle.read_bytes(), PAYLOAD)
            self.assertTrue(response.closed)
            self.assertEqual(list(output.iterdir()), [bundle])

    def test_downloader_type_error_does_not_fall_back_to_an_unbounded_call(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            downloader = mock.Mock(side_effect=TypeError("unsupported byte budget"))
            with self.assertRaisesRegex(TypeError, "unsupported byte budget"):
                fetch_codeql_bundle.fetch_bundle(root, output, downloader=downloader)
            downloader.assert_called_once_with(pinned_material()["url"], output / "codeql-bundle-linux64.tar.gz",
                                               max_bytes=fetch_codeql_bundle.MAX_BUNDLE_BYTES)
            self.assertEqual(list(output.iterdir()), [])

    def test_main_exposes_the_bundle_path_through_the_github_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "repo"
            write_materials(root, [pinned_material()])
            output = pathlib.Path(temporary) / "out"
            github_output = pathlib.Path(temporary) / "github-output"
            github_output.write_text("", encoding="ascii")

            original = fetch_codeql_bundle.transport.download
            fetch_codeql_bundle.transport.download = fake_downloader(PAYLOAD)
            try:
                result = fetch_codeql_bundle.main(
                    [
                        "--repo-root", str(root),
                        "--output-dir", str(output),
                        "--github-output", str(github_output),
                    ]
                )
            finally:
                fetch_codeql_bundle.transport.download = original

            self.assertEqual(result, 0)
            bundle = output / "codeql-bundle-linux64.tar.gz"
            self.assertEqual(
                github_output.read_text(encoding="ascii"), f"bundle={bundle}\n"
            )


if __name__ == "__main__":
    unittest.main()
