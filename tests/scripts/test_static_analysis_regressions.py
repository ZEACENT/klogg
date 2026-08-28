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
ADB_LOGCAT_SESSION_DATA_SOURCE = (
    ROOT / "src" / "ui" / "src" / "adblogcatsessiondata.cpp"
)
SESSION_SOURCE = ROOT / "src" / "ui" / "src" / "session.cpp"
QUICKFIND_HEADER = ROOT / "src" / "ui" / "include" / "quickfind.h"
QUICKFIND_SOURCE = ROOT / "src" / "ui" / "src" / "quickfind.cpp"
FILEWATCHER_SOURCE = ROOT / "src" / "filewatch" / "src" / "filewatcher.cpp"
EFSW_INOTIFY_TEST = ROOT / "tests" / "unit" / "efsw_inotify_test.cpp"
APP_MAIN = ROOT / "src" / "app" / "main.cpp"
UNIT_TEST_MAIN = ROOT / "tests" / "unit" / "tests_main.cpp"
UI_TEST_MAIN = ROOT / "tests" / "ui" / "qtests_main.cpp"


def function_body(source, signature):
    start = source.index(signature)
    # The body opens at the first "{" that is either alone on its line
    # (Allman style) or directly follows the parameter list (K&R). Brace
    # pairs inside the declaration (for example "= {}" default arguments)
    # satisfy neither condition.
    opening_brace = start
    while True:
        opening_brace = source.index("{", opening_brace)
        line_start = source.rfind("\n", 0, opening_brace) + 1
        if (
            source[line_start:opening_brace].strip() == ""
            or source[start:opening_brace].rstrip().endswith(")")
        ):
            break
        opening_brace += 1
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
    def test_filewatch_executor_contains_callback_exceptions(self):
        source = FILEWATCHER_SOURCE.read_text()
        runner = function_body(source, "    void run()")
        callback = function_body(source, "    void handleFileAction(")
        deleter = function_body(
            source, "void EfswFileWatcherDeleter::operator()"
        )

        self.assertRegex(
            runner,
            re.compile(
                r"try\s*\{\s*task\(\);\s*\}\s*"
                r"catch \( const std::exception& error \)"
            ),
        )
        self.assertIn("catch ( ... )", runner)
        self.assertIn(
            "NOLINTNEXTLINE(bugprone-exception-escape)", callback
        )
        self.assertIn(
            "NOLINTNEXTLINE(cppcoreguidelines-owning-memory)", deleter
        )

    def test_efsw_listener_outlives_native_watcher_on_assertion_failure(self):
        source = EFSW_INOTIFY_TEST.read_text()

        guard = source[
            source.index("class WatcherResetGuard") :
            source.index("class RemovingListener")
        ]
        self.assertIn("~WatcherResetGuard()", guard)
        self.assertIn("watcher_.reset();", guard)
        self.assertEqual(
            source.count("WatcherResetGuard stopWatcher{ watcher };"), 2
        )

    def test_capturestore_retries_terminal_gate_timeouts_and_foreign_readers(self):
        source = CAPTURESTORE_SOURCE.read_text()
        activation = function_body(source, "std::optional<ActivationResult> activate()")
        deactivation = function_body(
            source, "    void deactivate( const QByteArray& activationToken )"
        )
        release = function_body(
            source, "void CaptureStore::CapturePathState::releaseRetiredFile"
        )
        retry = function_body(
            source,
            "void CaptureStore::CapturePathState::retryRetiredFilesAndReleaseRegistryGateHeld",
        )
        cleanup = function_body(
            source, "void CaptureStore::cleanupCaptureCandidates("
        )
        scheduler = function_body(
            source,
            "void CaptureStore::CapturePathState::scheduleRetry( bool force )",
        )
        retryable = function_body(
            source,
            "bool CaptureStore::CapturePathState::hasRetryableMaintenance() const",
        )
        directory_removal = function_body(
            source, "    void removeDirectoryIfRequestedAndEmptyGateHeld()"
        )

        self.assertIn("QFile::remove( activeMarkerPath() )", activation)
        self.assertIn("activeStoreTokens_.remove( activationToken )", deactivation)
        self.assertIn("processMarker_.reset()", deactivation)
        self.assertIn("scheduleRetry()", deactivation)
        self.assertIn("scheduleRetry()", release)
        self.assertIn("hasActiveProcessMarker()", release)
        self.assertIn("hasActiveProcessMarker()", retry)
        self.assertIn("directory_.isRemoved()", cleanup)
        self.assertIn("fileLeases_.value( retired.key() ).expired()", retryable)
        self.assertIn("activeStoreTokens_.isEmpty()", retryable)
        self.assertLess(
            scheduler.index("!state->hasPendingGateRetry()"),
            scheduler.index("std::this_thread::sleep_for"),
        )
        self.assertIn("gateRetryRequestEpoch_.fetch_add", scheduler)
        self.assertIn("gateRetryCompletedEpoch_.store", scheduler)
        self.assertIn("CaptureRetryAttemptLimit", scheduler)
        self.assertIn("CaptureRetryInitialDelay", scheduler)
        self.assertIn("CaptureRetryMaximumDelay", scheduler)
        self.assertIn("retryDelay * 2", scheduler)
        self.assertIn("newRetryRequestArrived", scheduler)
        self.assertIn("registerCaptureBackgroundThread()", scheduler)
        self.assertNotIn("QCoreApplication::closingDown()", scheduler)
        self.assertIn("qAddPostRoutine( stopCaptureBackgroundThreads )", source)
        self.assertIn("directory_.hasEntries()", directory_removal)
        self.assertIn("cancelDirectoryDeletionLocked()", directory_removal)
        self.assertNotIn("hasPendingMaintenance", source)
        self.assertNotIn("pendingDeactivationTokens_", source)

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
            "auto file = read.spilledFile->openForRead()"
        )
        self.assertNotIn("openForRead()", method[:unlock_position])
        self.assertNotIn(
            "QFile file( read.spilledFile->path() )", method
        )
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
        self.assertRegex(
            loader,
            re.compile(
                r"std::sort\(\s*segmentFiles\.begin\(\),\s*"
                r"segmentFiles\.end\(\),"
            ),
        )
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
        self.assertIn("ownedFiles", source)
        self.assertNotIn("ownedFilePaths", source)
        self.assertIn("trackedFileKey", source)
        self.assertIn("struct TrackedFile", source)
        self.assertIn("pendingRetirementFiles_", source)
        self.assertIn("retiredFiles_", source)
        self.assertNotIn("pendingRetirementIdentities_", source)
        self.assertNotIn("retiredFileIdentities_", source)
        self.assertIn("registerCreatedFile", source)
        self.assertIn("transferCreatedFile", source)
        self.assertIn("tryRetireOwnedFile", source)
        self.assertNotIn("ownedFiles", loader)
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
        self.assertIn("finalizeRemovedGenerationGateHeldLocked", source)
        self.assertIn("directoryDeletionGeneration_", source)
        self.assertIn("directoryDeletionRequesterToken_", source)
        self.assertIn("activeStoreTokens_", source)
        self.assertIn("pendingRetirementRequesters_", source)
        self.assertIn("RetireResult::Deferred", source)
        self.assertIn("notifyOnRelease_", source)
        self.assertIn("retiredFiles_", source)
        self.assertIn("pendingRetirementFiles_", source)
        self.assertIn("fileIdentity", source)
        self.assertIn("identity_", source)
        self.assertIn("promotePendingRetirementsLocked", source)
        self.assertNotIn("pendingDeactivationTokens_", source)
        self.assertNotIn("applyPendingDeactivationsLocked", source)
        self.assertNotIn(
            "activeStoreTokens_.remove( activationToken ) > 0", source
        )
        self.assertIn("processMarker_.reset();", source)
        self.assertIn("removeProcessGeneration();", source)
        self.assertNotIn("lockUntilAcquired", source)
        self.assertIn("removeDirectoryIfRequestedAndEmptyGateHeld", source)
        self.assertIn("retryRetiredFilesAndReleaseRegistryGateHeld", source)
        self.assertIn(
            "processGeneration() != directoryDeletionGeneration_", source
        )
        self.assertIn("ownedFiles.insert( fileName, fileIdentity )", source)
        self.assertNotIn("rebindOrCreate", source)
        self.assertNotIn("pathRegistry.entries[ path ]", source)
        self.assertNotIn("QTemporaryFile temporaryFile", source)
        self.assertNotIn("QFile::rename( temporaryPath, segment.filePath )", source)
        self.assertIn("catch ( const std::exception& error )", destructor)
        self.assertNotIn(
            "retryRetiredFilesAndReleaseRegistry();", destructor
        )
        self.assertNotIn(
            "retryRetiredFilesAndReleaseRegistry();", delete_files
        )
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

    def test_marker_scan_rechecks_incoherent_record_before_removal(self):
        source = CAPTURESTORE_SOURCE.read_text()
        marker_scan = function_body(source, "bool hasActiveProcessMarker")

        # QLockFile creates the marker O_EXCL and writes the pid record in a
        # separate step, so a live sibling can briefly own a marker whose
        # record is unreadable or names another pid. The scan must re-read
        # the record through a bounded grace window before removing; only a
        # marker that stays incoherent is treated as abandoned. Removing on
        # first sight orphans a mid-creation sibling, while keeping it
        # unconditionally lets pid-reuse leftovers block cleanup forever
        # (pinned by the unlocked-foreign-pid unit tests).
        self.assertIn("lockInfoAvailable", marker_scan)
        grace = marker_scan.index("MarkerRecordGraceMs")
        self.assertIn("getLockInfo", marker_scan[grace:])
        self.assertLess(grace, marker_scan.index("QFile::remove", grace))

    def test_async_capture_cleanup_collects_and_handles_failures_off_thread(self):
        source = CAPTURESTORE_SOURCE.read_text()
        scheduler = function_body(
            source, "void CaptureStore::scheduleCleanupUnusedCaptures("
        )

        self.assertLess(
            scheduler.index("std::thread"),
            scheduler.index("collectUnusedCaptureCandidates"),
        )
        self.assertGreaterEqual(
            scheduler.count("catch ( const std::exception& error )"), 2
        )
        self.assertGreaterEqual(scheduler.count("catch ( ... )"), 2)
        self.assertIn("registerCaptureBackgroundThread()", scheduler)
        self.assertIn("CaptureBackgroundThreadRegistration registration", scheduler)
        self.assertIn("captureBackgroundThreadsStopping()", scheduler)
        self.assertIn("rootPath, 0, shouldStop", scheduler)
        self.assertIn("preserveModifiedAfter, {}, 0,", scheduler)

    def test_capture_background_workers_stop_before_qt_teardown(self):
        header = CAPTURESTORE_HEADER.read_text()
        source = CAPTURESTORE_SOURCE.read_text()

        self.assertIn("static void shutdownBackgroundWorkers();", header)
        self.assertIn("std::condition_variable stopped;", source)
        self.assertIn("tracker.stopped.wait", source)
        self.assertIn("stopCaptureBackgroundThreads();", source)
        for entry_point in (APP_MAIN, UNIT_TEST_MAIN, UI_TEST_MAIN):
            self.assertIn(
                "CaptureStore::shutdownBackgroundWorkers();",
                entry_point.read_text(),
            )

    def test_live_capture_ids_are_validated_and_fail_closed(self):
        header = CAPTURESTORE_HEADER.read_text()
        capture_source = CAPTURESTORE_SOURCE.read_text()
        adb_session_data = ADB_LOGCAT_SESSION_DATA_SOURCE.read_text()
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
            adb_session_data, "bool AdbLogcatSessionData::isValid"
        )
        self.assertIn("CaptureStore::isValidCaptureId", validity)

        opener = function_body(session_source, "ViewInterface* Session::openAdbAlways")
        self.assertIn("if ( !restoredSessionData.isValid() )", opener)
        self.assertIn("catch ( const std::exception& error )", opener)
        self.assertGreaterEqual(opener.count("return nullptr;"), 2)

    def test_windows_tombstone_mismatch_falls_through_to_expected_identity(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        remove_file = function_body(
            source, "bool SecureCaptureDirectory::removeFile"
        )

        # A tombstone whose recorded identity no longer matches the current
        # occupant is complete, but the caller may have asked to retire
        # exactly that occupant. The mismatch branch must erase the tombstone
        # and fall through to the regular expectedIdentity path (POSIX
        # parity) instead of returning without deleting a requested file.
        mismatch = remove_file.index(
            "currentIdentity != pendingDeletion.value()"
        )
        branch = remove_file[mismatch : mismatch + 900]
        self.assertIn("} else {", branch)
        self.assertNotIn("return true", branch.split("} else {")[0])

    def test_windows_directory_opens_request_the_directory_create_option(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        open_directory = function_body(
            source, "ScopedHandle openExistingDirectoryNoFollow("
        )
        open_object = function_body(
            source, "ScopedHandle openExistingObjectNoFollow("
        )

        # NTFS rejects opening a directory unless CreateOptions carries
        # FILE_DIRECTORY_FILE (STATUS_FILE_IS_A_DIRECTORY). Missing the flag
        # broke every capture bind on Windows (PR #50 CI). Type-agnostic
        # object opens must retry with the flag on that exact status so tree
        # walks can descend into subdirectories.
        self.assertIn("nt::FileDirectoryFile", open_directory)
        self.assertIn("nt::StatusFileIsADirectory", open_object)
        self.assertIn("nt::FileDirectoryFile", open_object)
        self.assertLess(
            open_object.index("nt::StatusFileIsADirectory"),
            open_object.index("nt::FileDirectoryFile"),
        )

    def test_capture_cleanup_ignores_marker_application_name(self):
        source = CAPTURESTORE_SOURCE.read_text()
        marker_check = function_body(source, "bool hasActiveProcessMarker() const")

        # A live foreign process marker must be honored regardless of the
        # application name recorded in it: renamed executables and compatible
        # builds write different names into the same locked marker, and the
        # marker's name line is the OS process name (Qt 6 QLockFile uses
        # processNameByPid, not QCoreApplication::applicationName()), so it
        # cannot be customized in-process. The ownership decision may only
        # use lock coherence and pid liveness; comparing the recorded
        # application name would delete captures a live process still owns.
        self.assertIn("getLockInfo(", marker_check)
        self.assertNotIn("applicationName ==", marker_check)
        self.assertNotIn("applicationName !=", marker_check)
        self.assertNotIn("applicationName.contains", marker_check)
        self.assertNotIn("applicationName.startsWith", marker_check)

    def test_windows_creation_and_enumeration_do_not_reopen_handles(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        create_directory = function_body(
            source, "ScopedHandle openOrCreateDirectoryNoFollow("
        )
        enumerate_directory = function_body(
            source,
            "std::optional<std::vector<WindowsDirectoryEntry>> "
            "enumerateDirectory(",
        )
        bind_parent = function_body(source, "ScopedHandle bindParentPath(")

        # ReOpenFile fails deterministically (NULL with GetLastError()==0) on
        # the NT-native directory handles used by this PAL; every capture
        # bind died at the creation-parent reopen on Windows CI (PR #50).
        # Creation capability is threaded through the walk instead: when
        # parents may be created, bindParentPath opens the anchor and every
        # component with FILE_ADD_SUBDIRECTORY (ParentCreateAccess), and
        # openOrCreateDirectoryNoFollow creates directly on that bound
        # parent. Enumeration runs on the bound handle itself, whose access
        # already covers FILE_LIST_DIRECTORY.
        self.assertNotIn("ReOpenFile(", create_directory)
        self.assertNotIn("ReOpenFile(", enumerate_directory)
        self.assertIn("ParentCreateAccess", bind_parent)

    def test_windows_publish_rename_uses_native_set_information(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        publish = function_body(
            source,
            "SecureCaptureDirectory::PublishResult "
            "SecureCaptureDirectory::publishTemporaryFile(",
        )
        native_api = function_body(source, "const NativeApi& nativeApi(")

        # SetFileInformationByHandle(FileRenameInfo) fails deterministically
        # with ERROR_INVALID_PARAMETER (0x57) for the NT-native handles used
        # by this PAL — 27,394/27,394 publish renames on Windows CI (PR #50)
        # even though every parameter satisfies [MS-FSA] FileRenameInformation
        # validation. The PAL is NT-native end to end (NtCreateFile opens,
        # NtQueryDirectoryFile enumeration), so the publish rename must use
        # NtSetInformationFile(FileRenameInformation) as well: it bypasses
        # whatever the Win32 wrapper rejects and surfaces the raw NT status
        # (STATUS_OBJECT_NAME_COLLISION maps to AlreadyExists) instead of a
        # lossy GetLastError translation.
        self.assertNotIn("SetFileInformationByHandle(", publish)
        self.assertIn("setInformationFile(", publish)
        self.assertIn("NtSetInformationFile", native_api)

    def test_secure_capture_windows_cleanup_uses_guarded_delete_handles(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        split_path = function_body(
            source,
            "std::optional<std::pair<QString, QStringList>> splitWindowsPath",
        )
        native_open = function_body(source, "ScopedHandle ntOpenRelative(")
        recursive_cleanup = function_body(
            source, "bool removeWindowsTreeContents( HANDLE directoryHandle )"
        )
        remove_file = function_body(
            source, "bool SecureCaptureDirectory::removeFile("
        )
        remove_if_empty = function_body(
            source, "bool SecureCaptureDirectory::removeIfEmpty()"
        )
        remove_recursively = function_body(
            source, "bool SecureCaptureDirectory::removeRecursively()"
        )
        identity_for_handle = function_body(
            source, "QString identityKeyForHandle( HANDLE handle )"
        )

        self.assertIn('#include "qtcompat/qtcompat.h"', source)
        self.assertNotIn("Qt::SkipEmptyParts", split_path)
        self.assertEqual(
            split_path.count("klogg::qtcompat::skipEmptyParts()"), 2
        )
        self.assertIn("constexpr ULONG ShareWithoutDelete", source)
        capture_access = source[
            source.index("constexpr ACCESS_MASK CaptureAccess") :
            source.index("constexpr ACCESS_MASK EnumerationAccess")
        ]
        self.assertNotIn("DELETE", capture_access)
        self.assertIn("ULONG shareAccess = ShareAll", native_open)
        self.assertIn("fileAttributes, shareAccess", native_open)
        self.assertIn("disposition, createOptions", native_open)
        self.assertIn("ShareWithoutDelete", recursive_cleanup)
        # Regular files must not require read-data or execute access merely
        # to be deleted; only directories need the wider traversal mask.
        self.assertIn("entry.attributes & FILE_ATTRIBUTE_DIRECTORY", recursive_cleanup)
        self.assertIn("FileDeleteAccess", recursive_cleanup)
        self.assertIn("ShareWithoutDelete", remove_file)
        self.assertIn("pendingFileDeletions", remove_file)
        self.assertIn("expectedIdentity", remove_file)
        self.assertIn("identityKeyForHandle", remove_file)
        self.assertIn("quarantineEntry", remove_file)
        # Tombstone retirement must follow the bound directory object when
        # the public name was displaced (POSIX descriptor parity), and a
        # relinked same-identity name must have its disposition reapplied
        # rather than wedging the tombstone.
        self.assertIn("HANDLE rootHandle = impl_->directoryHandle.get();", remove_file)
        self.assertIn("sameFileIdentity( publicRoot.get(),", remove_file)
        self.assertIn(
            "markDeleteByHandle( deleteHandle.get(), rootHandle,", remove_file
        )
        self.assertIn("openCurrentDirectoryForDeletion", remove_if_empty)
        self.assertIn("openCurrentDirectoryForDeletion", remove_recursively)
        delete_access = source[
            source.index("constexpr ACCESS_MASK TreeDeleteAccess") :
            source.index("bool initializeUnicodeString")
        ]
        self.assertNotIn("FILE_WRITE_ATTRIBUTES", delete_access)
        # Identity must fail closed when neither the 128-bit identity nor the
        # filesystem name is available (legacy SMB + ReFS), prefer FILE_ID_INFO
        # otherwise, and refuse the 64-bit fallback on ReFS.
        self.assertIn("FileIdInfo", identity_for_handle)
        self.assertIn("!GetVolumeInformationByHandleW(", identity_for_handle)
        self.assertIn('"ReFS"', identity_for_handle)
        self.assertIn("win-legacy:", identity_for_handle)
        self.assertGreaterEqual(identity_for_handle.count("return {};"), 3)

    def test_secure_capture_temp_adoption_and_readonly_delete_are_fail_closed(self):
        source = SECURE_CAPTURE_DIRECTORY_SOURCE.read_text()
        create = function_body(
            source, "SecureCaptureDirectory::createTemporaryFile("
        )
        delete_by_handle = function_body(
            source,
            "bool markDeleteByHandle( HANDLE handle, HANDLE attributeRoot = nullptr,\n"
            "                         const QString& attributeName = {} )",
        )

        self.assertIn("_get_osfhandle", source)
        self.assertIn("markDescriptorDeleteOnClose", source)
        self.assertLess(
            create.index("_open_osfhandle("),
            create.index("handle.release()"),
        )
        self.assertIn("markDeleteByHandle( handle.get() )", create)
        self.assertIn("markDescriptorDeleteOnClose( descriptor )", create)
        self.assertIn("FILE_ATTRIBUTE_READONLY", delete_by_handle)
        self.assertIn("FileBasicInfo", delete_by_handle)
        self.assertIn("FileDispositionInfoEx", delete_by_handle)
        self.assertIn("FileDispositionFlagIgnoreReadonlyAttribute", delete_by_handle)
        # Pre-1709 fallback: only a single-link read-only record may have its
        # attribute cleared, and only through an identity-verified reopen.
        self.assertIn("nNumberOfLinks != 1", delete_by_handle)
        self.assertIn("identityKeyForHandle", delete_by_handle)
        self.assertIn("openExistingRegularFileNoFollow", delete_by_handle)
        self.assertIn("FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE", delete_by_handle)
        self.assertIn("setFileAttributesByHandle", delete_by_handle)
        self.assertIn("FILE_ATTRIBUTE_READONLY )", delete_by_handle)
        self.assertNotIn("ReOpenFile(", delete_by_handle)

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
            "openPath",
            "openRelative",
            "createRelative",
            "renameNoReplaceRelative",
            "O_NONBLOCK",
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
        restore_entry = function_body(
            source, "bool restoreQuarantinedEntry("
        )
        self.assertNotIn(
            "renameNoReplace( directoryFd, publicName, directoryFd, quarantineName )",
            restore_entry,
        )
        self.assertEqual(
            source.count("impl_->invalidateBoundDirectory();"), 2
        )
        recursive_cleanup = function_body(
            source, "bool removeDirectoryContents( int directoryFd )\n{"
        )
        latest_modification = function_body(
            source, "QDateTime latestModificationTimeForDirectory( int directoryFd )"
        )
        self.assertIn("restoreQuarantinedEntry", recursive_cleanup)
        self.assertIn("isSameMountedFilesystem(", recursive_cleanup)
        self.assertIn("isSameMountedFilesystem(", latest_modification)
        self.assertIn("mountIdentityForDescriptor(", source)
        self.assertNotIn("entryInfo.st_dev != directoryInfo.st_dev", recursive_cleanup)
        for signature in (
            "bool SecureCaptureDirectory::removeIfEmpty()",
            "bool SecureCaptureDirectory::removeRecursively()",
        ):
            cleanup = function_body(source, signature)
            self.assertGreaterEqual(cleanup.count("restorePublicName();"), 1)

        has_entries = function_body(
            source, "bool SecureCaptureDirectory::hasEntries() const"
        )
        self.assertIn("enumerateDirectory", has_entries)
        self.assertIn("entries->empty()", has_entries)
        self.assertNotIn("entries->isEmpty()", has_entries)
        self.assertIn("::readdir", has_entries)
        self.assertIn("return true;", has_entries)

        file_identity = function_body(
            source, "QString SecureCaptureDirectory::fileIdentity("
        )
        self.assertIn("identityKeyForHandle", file_identity)
        self.assertIn("identityKeyForStat", file_identity)

        secure_reader = function_body(
            source, "SecureCaptureDirectory::openReadFile("
        )
        self.assertIn("expectedIdentity", secure_reader)
        self.assertIn("std::make_unique<QFile>()", secure_reader)
        self.assertIn("QFileDevice::AutoCloseHandle", secure_reader)
        self.assertIn("::fstat( file->handle()", secure_reader)
        self.assertEqual(source.count("::openat("), 2)
        self.assertEqual(source.count("::open("), 1)
        self.assertEqual(source.count("::syscall("), 1)

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
