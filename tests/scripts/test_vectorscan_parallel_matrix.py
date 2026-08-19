import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
VECTORSCAN_TEST = ROOT / "tests" / "vectorscan" / "vectorscan_tests.cpp"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"


def function_body(source, signature):
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for position in range(opening_brace, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError(f"Unterminated function: {signature}")


class VectorscanParallelMatrixTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = VECTORSCAN_TEST.read_text()

    def test_exhaustive_matrix_preserves_each_allocator_case_and_combination(self):
        builder = function_body(self.source, "buildExhaustiveChildRuns()")

        allocator_loop = builder.index("for ( const auto allocator")
        child_case_loop = builder.index("for ( const auto childCase", allocator_loop)
        combination_loop = builder.index("for ( const auto& indexes", child_case_loop)
        child_append = builder.index("childRuns.push_back", combination_loop)

        self.assertLess(allocator_loop, child_case_loop)
        self.assertLess(child_case_loop, combination_loop)
        self.assertLess(combination_loop, child_append)
        self.assertIn("buildSearchSpace()", builder)
        self.assertIn("AllocatorMode::Crt", builder)
        self.assertIn("AllocatorMode::Mimalloc", builder)
        self.assertIn("ChildCase::DirectSinglePrefilter", builder)
        self.assertIn("ChildCase::DirectMultiPrefilter", builder)
        self.assertIn("ChildCase::HighlighterCompilePrefilter", builder)
        self.assertIn("ChildCase::HighlighterCollectionRestorePrefilter", builder)
        self.assertIn(
            "constexpr size_t kExhaustiveSearchSpaceSize = 42;",
            self.source,
        )
        self.assertIn(
            "constexpr size_t kExhaustiveChildRunCount = 336;",
            self.source,
        )
        self.assertIn(
            "REQUIRE( buildSearchSpace().size() == kExhaustiveSearchSpaceSize );",
            self.source,
        )
        self.assertIn(
            "REQUIRE( buildExhaustiveChildRuns().size() "
            "== kExhaustiveChildRunCount );",
            self.source,
        )

    def test_windows_matrix_dispatches_the_complete_batch_with_bounded_processes(self):
        matrix_start = self.source.index(
            'TEST_CASE( "Windows VectorScan regression search space exits cleanly"'
        )
        matrix = self.source[matrix_start : self.source.index("#else", matrix_start)]
        runner = function_body(self.source, "runChildProcesses(")

        self.assertIn("buildWindowsRegressionChildRuns()", matrix)
        self.assertIn("requireSuccessfulChildRuns", matrix)
        self.assertNotIn("requireSuccessfulChildRun(", matrix)
        self.assertNotIn("runChildProcess(", matrix)
        self.assertNotIn("waitForFinished(", matrix)
        default_match = re.search(
            r"constexpr int kDefaultChildConcurrency = (\d+);", self.source
        )
        max_match = re.search(r"constexpr int kMaxChildConcurrency = (\d+);", self.source)
        self.assertIsNotNone(default_match)
        self.assertIsNotNone(max_match)
        self.assertEqual(int(default_match.group(1)), 4)
        self.assertEqual(int(max_match.group(1)), 8)
        self.assertIn('"KLOGG_VECTORSCAN_CHILD_CONCURRENCY"', self.source)
        self.assertIn("configured < 1 || configured > kMaxChildConcurrency", self.source)
        self.assertIn("childConcurrency()", runner)
        self.assertIn("activeChildCount < maxConcurrentChildren", runner)
        self.assertIn("QEventLoop", runner)
        self.assertIn("process->start()", runner)
        self.assertIn("QProcess::finished", runner)
        self.assertIn("runningChild->timeout.setTimerType( Qt::PreciseTimer )", runner)
        self.assertIn("runningChild->timeout.start( kChildTimeoutMs )", runner)
        self.assertRegex(
            runner,
            re.compile(
                r"QProcess::errorOccurred.*?Qt::QueuedConnection",
                re.DOTALL,
            ),
        )
        self.assertIn("if ( activeChildCount > 0 )", runner)
        self.assertIn("std::optional<size_t> firstFailureIndex;", runner)
        self.assertIn("!childRunSucceeded( child->result )", runner)
        self.assertIn("sibling->cancelledAfterFailure = true;", runner)
        self.assertIn("sibling->process.kill();", runner)
        self.assertIn("if ( !firstFailureIndex )", runner)
        self.assertIn(
            "firstFailureIndex || nextChildIndex == childRuns.size()", runner
        )
        self.assertIn("ChildBatchResult", self.source)
        self.assertIn("if ( batch.failureIndex )", self.source)
        self.assertNotIn("configurationWritingChildActive", runner)
        self.assertNotIn("isConfigurationWritingChild", runner)
        self.assertNotIn("waitForStarted", runner)
        self.assertNotIn("waitForFinished", runner)

    def test_child_configuration_stays_in_memory_for_full_parallelism(self):
        in_memory_setup = function_body(
            self.source, "void configureProductLikeTestState()"
        )
        initializer = function_body(
            self.source, "void initializeProductLikeTestState()"
        )
        runner = function_body(self.source, "runChildProcesses(")

        self.assertIn("Configuration::get()", in_memory_setup)
        self.assertNotIn(".save()", in_memory_setup)
        self.assertIn("Configuration::getDefaultForTests()", initializer)
        self.assertNotIn("Configuration::getSynced()", initializer)
        self.assertNotIn("isConfigurationWritingChild", self.source)
        self.assertNotIn("configurationWritingChildActive", runner)

    def test_matrix_sampling_defaults_to_exhaustive_and_covers_every_case(self):
        # The exhaustive matrix is the default; only an explicit opt-out env
        # value may downgrade it to the sampled set.
        selector = function_body(self.source, "exhaustiveRunsRequested()")
        dispatcher = function_body(self.source, "buildWindowsRegressionChildRuns()")
        sampled = function_body(self.source, "buildSampledChildRuns()")

        self.assertIn('"KLOGG_VECTORSCAN_EXHAUSTIVE"', selector)
        self.assertIn('QStringLiteral( "0" )', selector)
        self.assertIn('QStringLiteral( "false" )', selector)
        self.assertIn("exhaustiveRunsRequested()", dispatcher)
        self.assertIn("buildExhaustiveChildRuns()", dispatcher)
        self.assertIn("buildSampledChildRuns()", dispatcher)

        # The sampled set must keep every (child case x allocator) pair so ASan
        # still executes each code path once, rotating index-set shapes.
        for token in (
            "AllocatorMode::Crt",
            "AllocatorMode::Mimalloc",
            "ChildCase::DirectSinglePrefilter",
            "ChildCase::DirectMultiPrefilter",
            "ChildCase::HighlighterCompilePrefilter",
            "ChildCase::HighlighterCollectionRestorePrefilter",
        ):
            self.assertIn(token, sampled)
        self.assertIn("constexpr size_t kSampledChildRunCount = 8;", self.source)
        self.assertIn("childRuns.reserve( kSampledChildRunCount );", sampled)

    def test_windows_asan_leg_wires_sampling_and_higher_concurrency(self):
        # Guards the CI-side of the 2026-08-19 Windows ASan leg optimization:
        # the asan leg must opt into the sampled matrix and raise child
        # concurrency, while the non-ASan x64-qt6 leg must keep the exhaustive
        # default (no opt-out env anywhere near its matrix entry).
        ci = CI_BUILD.read_text()

        self.assertIn('"KLOGG_VECTORSCAN_EXHAUSTIVE=0"', ci)
        self.assertIn('"KLOGG_VECTORSCAN_CHILD_CONCURRENCY=8"', ci)
        self.assertIn('"${{ matrix.config.sanitizer }}" = "address"', ci)

        # The exhaustive-vs-avx2 leg (package_tag: vs-avx2) must NOT carry a
        # sanitizer key, so it never enters the sampling branch.
        avx2_block = ci[ci.index("package_tag: vs-avx2") : ci.index("package_tag: asan")]
        self.assertNotIn("sanitizer:", avx2_block)

    def test_only_process_scheduler_helpers_are_windows_msvc_specific(self):
        platform_guard = "#if defined(Q_OS_WIN) && defined(_MSC_VER)"

        self.assertIn(
            "\nstd::vector<ChildOptions> buildExhaustiveChildRuns()",
            self.source,
        )
        self.assertNotIn(
            platform_guard + "\nstd::vector<ChildOptions> buildExhaustiveChildRuns()",
            self.source,
        )
        self.assertIn(
            platform_guard
            + "\nstd::vector<ChildRunResult> runChildProcesses(",
            self.source,
        )
        self.assertIn(
            platform_guard + "\nvoid requireSuccessfulChildRuns(", self.source
        )


if __name__ == "__main__":
    unittest.main()
