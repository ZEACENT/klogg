import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
STREAM_SOURCE = ROOT / "src" / "livecapture" / "src" / "iosnativestream.cpp"
STREAM_TEST = ROOT / "tests" / "unit" / "ios_native_stream_worker_test.cpp"
PROTOCOL_TEST = ROOT / "tests" / "unit" / "ios_ostrace_protocol_test.cpp"
UNIT_CMAKE = ROOT / "tests" / "unit" / "CMakeLists.txt"


class IosCompilerPortabilityContractTest(unittest.TestCase):
    def test_syslog_byte_copy_avoids_gcc13_iterator_provenance_warning(self):
        source = STREAM_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn(
            "completed.assign( state->syslogRecord.begin(), state->syslogRecord.end() )",
            source,
        )
        self.assertIn(
            "std::memcpy( completed.data(), completedRecord.data(), completed.size() )",
            source,
        )

    def test_admission_constructors_do_not_shadow_members_under_gcc(self):
        source = STREAM_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn(
            "AdmissionState( std::size_t maximumConcurrentSessions )", source
        )
        self.assertNotIn(
            "Lease( std::shared_ptr<AdmissionState> owner, IosEndpointKey endpoint )",
            source,
        )

    def test_native_contract_target_links_the_platform_thread_runtime(self):
        cmake = UNIT_CMAKE.read_text(encoding="utf-8")
        target = cmake.split("add_executable(\n  klogg_ios_native_contract_tests", 1)[1]
        target = target.split("klogg_configure_test_target", 1)[0]
        self.assertIn("find_package(Threads REQUIRED)", cmake)
        self.assertIn("Threads::Threads", target)

    def test_protocol_bounds_check_uses_a_named_fixture_for_gcc15(self):
        source = PROTOCOL_TEST.read_text(encoding="utf-8")
        self.assertNotIn("PacketFixture{}.message.size()", source)
        self.assertIn("const PacketFixture fixture;", source)

    def test_byte_fill_values_are_explicitly_typed_for_msvc(self):
        sources = "\n".join(
            path.read_text(encoding="utf-8") for path in (STREAM_TEST, PROTOCOL_TEST)
        )
        for unsafe_fill in (
            "std::fill( source.begin(), source.end(), 0xa5u )",
            "std::fill( source.begin(), source.end(), 0x5au )",
            "std::fill( borrowed.begin(), borrowed.end(), 0xa5u )",
        ):
            self.assertNotIn(unsafe_fill, sources)
        self.assertIn("std::uint8_t{ 0xa5 }", sources)
        self.assertIn("std::uint8_t{ 0x5a }", sources)


if __name__ == "__main__":
    unittest.main()
