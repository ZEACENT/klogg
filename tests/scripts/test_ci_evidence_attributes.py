import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("git"), "Git is required for checkout attribute verification")
class EvidenceCheckoutAttributesTest(unittest.TestCase):
    def test_signed_evidence_preserves_exact_bytes_on_every_checkout(self):
        paths = [
            "ci/environments/evidence/jammy-qt5/verification.json",
            "ci/environments/evidence/jammy-qt5/image.sigstore.json",
            "ci/dependencies/evidence/adb-windows-x64/verification.json",
        ]
        result = subprocess.run(
            ["git", "check-attr", "-z", "text", "--", *paths], cwd=ROOT,
            check=True, capture_output=True, timeout=20,
        )
        fields = result.stdout.decode("utf-8").split("\0")
        actual = {fields[index]: fields[index + 2] for index in range(0, len(fields) - 1, 3)}
        self.assertEqual(actual, {path: "unset" for path in paths},
                         "Signed provenance is byte-addressed and must not undergo CRLF conversion")


if __name__ == "__main__":
    unittest.main()
