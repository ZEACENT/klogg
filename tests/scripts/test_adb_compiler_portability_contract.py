import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PROTOCOL_TEST = ROOT / "tests" / "unit" / "adb_protocol_test.cpp"


class AdbCompilerPortabilityContractTest(unittest.TestCase):
    def test_host_frame_uses_explicit_byte_conversion(self):
        source = PROTOCOL_TEST.read_text(encoding="utf-8")
        host_frame = source.split("ByteVector hostFrame", 1)[1]
        host_frame = host_frame.split("ByteVector shellFrame", 1)[0]

        self.assertNotIn(
            "frame.insert( frame.end(), payload.begin(), payload.end() )", host_frame
        )
        self.assertIn("static_cast<std::uint8_t>( byte )", host_frame)


if __name__ == "__main__":
    unittest.main()
