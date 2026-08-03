import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
CAPTURESTORE_HEADER = ROOT / "src" / "logdata" / "include" / "capturestore.h"
CAPTURESTORE_SOURCE = ROOT / "src" / "logdata" / "src" / "capturestore.cpp"
SECURE_CAPTURE_DIRECTORY_SOURCE = (
    ROOT / "src" / "logdata" / "src" / "securecapturedirectory.cpp"
)
LIVE_SOURCE_TRANSPORT = ROOT / "src" / "ui" / "src" / "livesourcetransport.cpp"
ADB_LOGCAT_SOURCE = ROOT / "src" / "ui" / "src" / "adblogcatsource.cpp"
SESSION_SOURCE = ROOT / "src" / "ui" / "src" / "session.cpp"
QUICKFIND_HEADER = ROOT / "src" / "ui" / "include" / "quickfind.h"
QUICKFIND_SOURCE = ROOT / "src" / "ui" / "src" / "quickfind.cpp"


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


class StaticAnalysisRegressionTest(unittest.TestCase):
    def test_capturestore_widens_before_next_index_arithmetic(self):
        source = CAPTURESTORE_SOURCE.read_text()
        self.assertNotIn(
            "static_cast<size_t>( lastLocalLine + 1 )",
            source,
        )
        self.assertIn(
            "static_cast<size_t>( lastLocalLine ) + 1U",
            source,
        )

    def test_capturestore_reinitializes_one_shot_callback_after_move(self):
        source = CAPTURESTORE_SOURCE.read_text()
        self.assertRegex(
            source,
            re.compile(
                r"auto callback = std::move\( beforeRawSnapshotCopyCallback \);\s*"
                r"beforeRawSnapshotCopyCallback = \{\};\s*callback\(\);"
            ),
        )

    def test_capturestore_reads_spilled_snapshots_after_unlock(self):
        source = CAPTURESTORE_SOURCE.read_text()
        method = source[
            source.index("SearchableLogData::RawLines CaptureStore::buildRawLines") :
            source.index("QString CaptureStore::lineAt")
        ]
        unlock_position = method.index("// mutex released")
        file_read_position = method.index(
            "QFile file( read.spilledFile->path() )"
        )
        self.assertNotIn("QFile file(", method[:unlock_position])
        self.assertIn(
            "read.spilledFile = segIt->spilledFile;",
            method[:unlock_position],
        )
        self.assertGreater(file_read_position, unlock_position)

    def test_capturestore_reload_reuses_leases_and_maintenance_scans_disk(self):
        header = CAPTURESTORE_HEADER.read_text()
        source = CAPTURESTORE_SOURCE.read_text()
        destructor = function_body(source, "CaptureStore::~CaptureStore")
        appender = function_body(source, "CaptureStore::AppendResult CaptureStore::appendUtf8")
        loader = function_body(source, "bool CaptureStore::loadFromDisk")
        retirement = function_body(source, "CaptureStore::retireCaptureFiles()")
        trim = function_body(source, "CaptureStore::TrimResult CaptureStore::trimToLimits")
        id_capacity = function_body(
            source, "void CaptureStore::ensureSegmentIdsAvailable"
        )
        clear = function_body(source, "void CaptureStore::clear")
        delete_files = function_body(
            source, "void CaptureStore::deleteCaptureFiles"
        )

        self.assertIn(
            "std::shared_ptr<CapturePathState> capturePathState_;", header
        )
        self.assertIn("std::deque<qint64> reservedSegmentIds_;", header)
        self.assertIn("struct CaptureStore::CapturePathState", source)
        self.assertIn(
            "QHash<QString, std::weak_ptr<SpilledSegmentFile>> fileLeases_;",
            source,
        )
        self.assertIn("std::atomic<qint64> nextSegmentId", source)
        self.assertIn("compare_exchange_weak", source)
        self.assertNotIn("nextSegmentId_ = 0", loader)
        self.assertIn("capturePathState_->mutex_", loader)
        self.assertIn("std::sort( segmentFiles.begin()", loader)
        self.assertIn("scanSegment( segment )", loader)
        self.assertIn(
            "spilledFile->retire( capturePathActivationToken_ );",
            retirement,
        )
        self.assertIn("retiredLeases.push_back", retirement)
        self.assertIn("inheritedCaptureFiles_", retirement)
        self.assertIn("struct ActivationResult", source)
        self.assertIn("std::optional<ActivationResult> activate()", source)
        self.assertIn("localProcessGeneration_", source)
        self.assertIn("CaptureProcessFileOwnership", source)
        self.assertIn("processFileOwnership_", source)
        self.assertIn("ownedFilePaths", source)
        self.assertIn("registerCreatedFile", source)
        self.assertIn("transferCreatedFile", source)
        self.assertIn("tryRetireOwnedFile", source)
        self.assertNotIn("ownedFilePaths", loader)
        self.assertNotIn("snapshotInheritedCaptureFiles", source)
        self.assertNotIn("relocateRetiredFilesForReplacement", source)
        self.assertIn("QLockFile", source)
        self.assertIn("hasActiveProcessMarker", source)
        self.assertIn("retryRetiredFilesAndReleaseRegistry", source)
        self.assertIn("capturePathForId", source)
        self.assertIn("entry.isSymLink()", source)
        self.assertIn("SecureCaptureDirectory directory_", source)
        self.assertIn("directory_.createTemporaryFile", source)
        self.assertIn("directory_.publishTemporaryFile", source)
        self.assertIn("directory_.removeRecursively()", source)
        self.assertIn("directory.identityKey()", source)
        self.assertIn("registryKey_", source)
        self.assertIn("registrySlotEpoch_", source)
        self.assertIn("captureCoordinationRoot", source)
        self.assertIn("captureCoordinationStem( registryKey_ )", source)
        self.assertIn("terminallyRemoved_", source)
        self.assertIn("retainedRegistryKeys", source)
        self.assertIn("finalizeRemovedGenerationLocked", source)
        self.assertIn("directoryDeletionGeneration_", source)
        self.assertIn("directoryDeletionRequesterToken_", source)
        self.assertIn("activeStoreTokens_", source)
        self.assertIn("pendingRetirementRequesters_", source)
        self.assertIn("RetireResult::Deferred", source)
        self.assertIn("notifyOnRelease_", source)
        self.assertIn("promotePendingRetirementsLocked", source)
        self.assertIn("pendingDeactivationTokens_", source)
        self.assertIn("applyPendingDeactivationsLocked", source)
        self.assertNotIn("lockUntilAcquired", source)
        self.assertIn("removeDirectoryIfRequestedAndEmptyGateHeld", source)
        self.assertIn("retryRetiredFilesAndReleaseRegistryGateHeld", source)
        self.assertIn(
            "processGeneration() != directoryDeletionGeneration_", source
        )
        self.assertIn("ownedFilePaths.insert( fileName )", source)
        self.assertNotIn("rebindOrCreate", source)
        self.assertNotIn("pathRegistry.entries[ path ]", source)
        self.assertNotIn("QTemporaryFile temporaryFile", source)
        self.assertNotIn("QFile::rename( temporaryPath, segment.filePath )", source)
        self.assertIn("catch ( const std::exception& error )", destructor)
        self.assertLess(
            appender.index("ensureSegmentIdsAvailable"),
            appender.index("persistBufferedSegmentsOnDestroy_ = true"),
        )
        self.assertIn("requiredIds > alreadyReserved", id_capacity)
        self.assertIn("capturePathState_->reserveSegmentIds", id_capacity)
        self.assertIn("while ( !segments_.empty() )", trim)
        self.assertIn("preserveTailDuringTrim_ || !needsNewSegment()", trim)
        self.assertIn("retireCaptureFiles()", clear)
        self.assertIn("retireCaptureFiles()", delete_files)
        for method in ( clear, trim, delete_files ):
            self.assertNotIn("capturePathState_->mutex_", method)
            self.assertIn("retiredLeases", method)

    def test_live_capture_ids_are_validated_and_fail_closed(self):
        header = CAPTURESTORE_HEADER.read_text()
        capture_source = CAPTURESTORE_SOURCE.read_text()
        adb_source = ADB_LOGCAT_SOURCE.read_text()
        session_source = SESSION_SOURCE.read_text()

        self.assertIn(
            "static bool isValidCaptureId( const QString& captureId );",
            header,
        )
        validator = function_body(
            capture_source, "bool CaptureStore::isValidCaptureId"
        )
        self.assertIn("isSafeCaptureId", validator)
        path_builder = function_body(capture_source, "QString capturePathForId")
        self.assertIn("std::invalid_argument", path_builder)
        self.assertIn("QFileInfo( capturePath ).isSymLink()", path_builder)

        validity = function_body(
            adb_source, "bool AdbLogcatSessionData::isValid"
        )
        self.assertIn("CaptureStore::isValidCaptureId", validity)

        opener = function_body(session_source, "ViewInterface* Session::openAdbAlways")
        self.assertIn("if ( !restoredSessionData.isValid() )", opener)
        self.assertIn("catch ( const std::exception& error )", opener)
        self.assertGreaterEqual(opener.count("return nullptr;"), 2)

    def test_secure_capture_cleanup_quarantines_validated_posix_identities(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()

        for token in (
            "renameNoReplace",
            "quarantineEntry",
            "quarantineBoundDirectory",
            "removeQuarantinedEntry",
            "restoreQuarantinedEntry",
            "restoreQuarantinedDirectory",
            "restorePublicName",
            "AT_SYMLINK_NOFOLLOW",
            "sameIdentity( expectedInfo, quarantinedInfo )",
            "sameIdentity( openedInfo, finalNamedInfo )",
            "RENAME_EXCL",
            "SYS_renameat2",
        ):
            self.assertIn(token, source)
        self.assertNotIn(
            "!removed\n"
            "                 || ::unlinkat( directoryFd, name.constData(), "
            "AT_REMOVEDIR )",
            source,
        )
        recursive_cleanup = function_body(
            source, "bool removeDirectoryContents( int directoryFd )\n{"
        )
        self.assertIn("restoreQuarantinedEntry", recursive_cleanup)
        for signature in (
            "bool SecureCaptureDirectory::removeIfEmpty()",
            "bool SecureCaptureDirectory::removeRecursively()",
        ):
            cleanup = function_body(source, signature)
            self.assertGreaterEqual(cleanup.count("restorePublicName();"), 1)

    def test_secure_capture_windows_operations_are_handle_relative(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()

        for token in (
            "NtCreateFile",
            "NtQueryDirectoryFile",
            "GetProcAddress",
            "RootDirectory",
            "FileOpenReparsePoint",
            "parentHandle",
            "directoryHandle",
            "SetFileInformationByHandle",
            "FileRenameInfo",
            "FileDispositionInfo",
            "ReplaceIfExists = FALSE",
            "extendedPathPrefix",
            "extendedUncPrefix",
            "BOOLEAN restartScan",
        ):
            self.assertIn(token, source)
        for obsolete in (
            "CreateFileW(",
            "CreateDirectoryW(",
            "GetFileAttributesW(",
            "MoveFileExW(",
            "DeleteFileW(",
            "QDirIterator",
            "removeWindowsTreeContents( const QString&",
            "QDir( impl_->path ).entryInfoList(",
        ):
            self.assertNotIn(obsolete, source)

    def test_qt_ownership_transfer_consumes_unique_ptr_release_result(self):
        source = LIVE_SOURCE_TRANSPORT.read_text()
        self.assertNotRegex(
            source,
            re.compile(r"^\s*dyingProcess\.release\(\);", re.MULTILINE),
        )
        self.assertEqual(
            source.count(
                "auto* const qtOwnedProcess = dyingProcess.release();"
            ),
            2,
        )

    def test_quickfind_notifications_are_bound_to_receiver_lifetime(self):
        source = QUICKFIND_SOURCE.read_text()
        method = function_body(
            source, "void QuickFind::sendNotification"
        )

        self.assertNotIn("dispatchToMainThread", method)
        self.assertRegex(
            method,
            re.compile(
                r"QMetaObject::invokeMethod\(\s*this,\s*"
                r"\[ this, notification \]\s*"
                r"\{\s*Q_EMIT notify\( notification \);\s*\},\s*"
                r"Qt::QueuedConnection\s*\);"
            ),
        )

    def test_quickfind_clears_interrupt_before_submission_not_worker_entry(self):
        header = QUICKFIND_HEADER.read_text()
        source = QUICKFIND_SOURCE.read_text()

        quickfind_class = header.index("class QuickFind : public QObject")
        private_position = header.index("  private:", quickfind_class)
        private_section = header[private_position:]
        self.assertNotIn(
            "pauseBeforeLineReadForTesting",
            header[quickfind_class:private_position],
        )

        for token in (
            "friend class AbstractLogView;",
            "pauseBeforeLineReadForTesting",
            "runBeforeLineReadCallbackForTesting",
            "beforeLineReadCallbackForTesting_",
        ):
            token_position = private_section.index(token)
            guard_position = private_section.rfind(
                "#if defined( KLOGG_ASAN_BUILD )", 0, token_position
            )
            self.assertGreaterEqual(guard_position, 0)
            guard_end = private_section.index("#endif", guard_position)
            self.assertLess(token_position, guard_end)

        self.assertIn(
            "void runBeforeLineReadCallbackForTesting();", private_section
        )

        for signature in (
            "void QuickFind::incrementallySearchForward",
            "void QuickFind::incrementallySearchBackward",
            "void QuickFind::searchForward",
            "void QuickFind::searchBackward",
        ):
            method = function_body(source, signature)
            self.assertLess(
                method.index("interruptRequested_.clear();"),
                method.index("operationFuture_ ="),
            )

        for signature in (
            "Portion QuickFind::doSearchForward( const FilePosition&",
            "Portion QuickFind::doSearchBackward( const FilePosition&",
        ):
            method = function_body(source, signature)
            self.assertNotIn("interruptRequested_.clear();", method)
            barrier_position = method.index(
                "runBeforeLineReadCallbackForTesting();"
            )
            first_read_position = method.index(
                "logData_.getExpandedLineString", barrier_position
            )
            self.assertLess(barrier_position, first_read_position)
            self.assertNotIn(
                "if ( runBeforeLineReadCallbackForTesting", method
            )


if __name__ == "__main__":
    unittest.main()
