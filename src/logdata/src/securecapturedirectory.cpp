#include "securecapturedirectory.h"

#include "log.h"

#include "qtcompat/qtcompat.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <limits>
#include <optional>
#include <vector>

#include <QFileInfo>
#include <QHash>
#include <QUuid>

#if defined( Q_OS_WIN )
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined( Q_OS_MACOS )
#include <sys/stdio.h>
#else
#include <sys/syscall.h>
#endif
#endif

namespace {
QString lexicalPath( const QString& path )
{
    return QDir::cleanPath( QFileInfo( path ).absoluteFilePath() );
}

QString leafNameForPath( const QString& directoryPath, const QString& filePath )
{
    QString fileName;
    if ( QDir::isRelativePath( filePath ) && !filePath.contains( QLatin1Char( '/' ) )
         && !filePath.contains( QLatin1Char( '\\' ) ) ) {
        fileName = filePath;
    } else {
        const QFileInfo fileInfo( lexicalPath( filePath ) );
        if ( fileInfo.dir().absolutePath() != directoryPath ) {
            return {};
        }
        fileName = fileInfo.fileName();
    }
    if ( fileName.isEmpty() || fileName == QStringLiteral( "." )
         || fileName == QStringLiteral( ".." ) || fileName.contains( QLatin1Char( '/' ) )
         || fileName.contains( QLatin1Char( '\\' ) ) ) {
        return {};
    }
    return fileName;
}

void sortEntries( QStringList& entries, QDir::SortFlags sort )
{
    if ( !( sort & QDir::Name ) ) {
        return;
    }

    const auto caseSensitivity
        = ( sort & QDir::IgnoreCase ) ? Qt::CaseInsensitive : Qt::CaseSensitive;
    std::sort( entries.begin(), entries.end(), [ caseSensitivity ]( const QString& lhs,
                                                                    const QString& rhs ) {
        return QString::compare( lhs, rhs, caseSensitivity ) < 0;
    } );
}

#if defined( Q_OS_WIN )
class ScopedHandle {
  public:
    ScopedHandle() = default;
    explicit ScopedHandle( HANDLE handle )
        : value_( handle )
    {
    }
    ~ScopedHandle()
    {
        reset();
    }

    ScopedHandle( const ScopedHandle& ) = delete;
    ScopedHandle& operator=( const ScopedHandle& ) = delete;
    ScopedHandle( ScopedHandle&& other ) noexcept
        : value_( other.release() )
    {
    }
    ScopedHandle& operator=( ScopedHandle&& other ) noexcept
    {
        if ( this != &other ) {
            reset( other.release() );
        }
        return *this;
    }

    bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept
    {
        return value_;
    }
    HANDLE release() noexcept
    {
        const auto result = value_;
        value_ = nullptr;
        return result;
    }
    void reset( HANDLE replacement = nullptr ) noexcept
    {
        if ( valid() ) {
            CloseHandle( value_ );
        }
        value_ = replacement;
    }

  private:
    HANDLE value_ = nullptr;
};

namespace nt {
using Status = LONG;

struct UnicodeString {
    USHORT length;
    USHORT maximumLength;
    PWSTR buffer;
};

struct ObjectAttributes {
    ULONG length;
    HANDLE RootDirectory;
    UnicodeString* objectName;
    ULONG attributes;
    PVOID securityDescriptor;
    PVOID securityQualityOfService;
};

struct IoStatusBlock {
    union {
        Status status;
        PVOID pointer;
    };
    ULONG_PTR information;
};

using ApcRoutine = VOID( NTAPI* )( PVOID, IoStatusBlock*, ULONG );
using CreateFileFn = Status( NTAPI* )(
    HANDLE*, ACCESS_MASK, ObjectAttributes*, IoStatusBlock*, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG );
using QueryDirectoryFileFn = Status( NTAPI* )(
    HANDLE, HANDLE, ApcRoutine, PVOID, IoStatusBlock*, PVOID, ULONG, ULONG,
    BOOLEAN, UnicodeString*, BOOLEAN );

struct DirectoryInformation {
    ULONG nextEntryOffset;
    ULONG fileIndex;
    LARGE_INTEGER creationTime;
    LARGE_INTEGER lastAccessTime;
    LARGE_INTEGER lastWriteTime;
    LARGE_INTEGER changeTime;
    LARGE_INTEGER endOfFile;
    LARGE_INTEGER allocationSize;
    ULONG fileAttributes;
    ULONG fileNameLength;
    WCHAR fileName[ 1 ];
};
static_assert( offsetof( DirectoryInformation, fileName ) == 64,
               "FILE_DIRECTORY_INFORMATION layout mismatch" );

constexpr ULONG ObjCaseInsensitive = 0x00000040UL;
constexpr ULONG FileOpen = 1UL;
constexpr ULONG FileCreate = 2UL;
constexpr ULONG FileDirectoryFile = 0x00000001UL;
constexpr ULONG FileSequentialOnly = 0x00000004UL;
constexpr ULONG FileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG FileNonDirectoryFile = 0x00000040UL;
constexpr ULONG FileOpenReparsePoint = 0x00200000UL;
constexpr ULONG FileDirectoryInformation = 1UL;
constexpr ULONG_PTR FileCreated = 2UL;
constexpr Status StatusSuccess = 0;
constexpr Status StatusNoMoreFiles = static_cast<Status>( 0x80000006UL );
constexpr Status StatusNoSuchFile = static_cast<Status>( 0xC000000FUL );
constexpr Status StatusObjectNameNotFound = static_cast<Status>( 0xC0000034UL );
constexpr Status StatusObjectNameCollision = static_cast<Status>( 0xC0000035UL );
constexpr Status StatusObjectPathNotFound = static_cast<Status>( 0xC000003AUL );
} // namespace nt

struct NativeApi {
    nt::CreateFileFn createFile = nullptr;
    nt::QueryDirectoryFileFn queryDirectoryFile = nullptr;

    bool available() const noexcept
    {
        return createFile != nullptr && queryDirectoryFile != nullptr;
    }
};

const NativeApi& nativeApi()
{
    static const NativeApi api = [] {
        const auto module = GetModuleHandleW( L"ntdll.dll" );
        return NativeApi{
            module == nullptr
                ? nullptr
                : reinterpret_cast<nt::CreateFileFn>(
                      GetProcAddress( module, "NtCreateFile" ) ),
            module == nullptr
                ? nullptr
                : reinterpret_cast<nt::QueryDirectoryFileFn>(
                      GetProcAddress( module, "NtQueryDirectoryFile" ) ),
        };
    }();
    return api;
}

constexpr ULONG ShareAll
    = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr ULONG ShareWithoutDelete = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr ACCESS_MASK ParentAccess
    = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK ParentCreateAccess
    = ParentAccess | FILE_ADD_SUBDIRECTORY;
constexpr ACCESS_MASK CaptureAccess
    = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE
      | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK EnumerationAccess
    = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK TreeDeleteAccess
    = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | DELETE
      | SYNCHRONIZE;
constexpr ACCESS_MASK ReadAccess
    = FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK TemporaryAccess
    = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE
      | SYNCHRONIZE;
constexpr ACCESS_MASK PublishRenameAccess
    = DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK FileDeleteAccess
    = DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr auto FileDispositionInfoExClass
    = static_cast<FILE_INFO_BY_HANDLE_CLASS>( 21 );
constexpr DWORD FileDispositionFlagDelete = 0x00000001UL;
constexpr DWORD FileDispositionFlagIgnoreReadonlyAttribute = 0x00000010UL;

struct ExtendedFileDispositionInfo {
    DWORD flags;
};

bool initializeUnicodeString( const QString& text, nt::UnicodeString& result )
{
    const auto bytes = static_cast<quint64>( text.size() ) * sizeof( WCHAR );
    if ( bytes > std::numeric_limits<USHORT>::max() ) {
        return false;
    }
    result.length = static_cast<USHORT>( bytes );
    result.maximumLength = result.length;
    result.buffer = reinterpret_cast<PWSTR>(
        const_cast<ushort*>( text.utf16() ) );
    return true;
}

bool statusSucceeded( nt::Status status )
{
    return status >= 0;
}

bool isMissingStatus( nt::Status status )
{
    return status == nt::StatusNoSuchFile
           || status == nt::StatusObjectNameNotFound
           || status == nt::StatusObjectPathNotFound;
}

ScopedHandle ntOpenRelative( HANDLE root, const QString& name,
                             ACCESS_MASK access, ULONG disposition,
                             ULONG fileAttributes, ULONG createOptions,
                             nt::Status* resultStatus = nullptr,
                             ULONG_PTR* resultInformation = nullptr,
                             ULONG shareAccess = ShareAll )
{
    nt::UnicodeString unicodeName{};
    if ( !nativeApi().available()
         || !initializeUnicodeString( name, unicodeName ) ) {
        return {};
    }
    nt::ObjectAttributes attributes{ sizeof( attributes ), root, &unicodeName,
                                     nt::ObjCaseInsensitive, nullptr, nullptr };
    nt::IoStatusBlock io{};
    HANDLE handle = nullptr;
    const auto status = nativeApi().createFile(
        &handle, access, &attributes, &io, nullptr, fileAttributes, shareAccess,
        disposition, createOptions, nullptr, 0 );
    if ( resultStatus != nullptr ) {
        *resultStatus = status;
    }
    if ( resultInformation != nullptr ) {
        *resultInformation = io.information;
    }
    if ( !statusSucceeded( status ) ) {
        if ( handle != nullptr && handle != INVALID_HANDLE_VALUE ) {
            CloseHandle( handle );
        }
        return {};
    }
    return ScopedHandle( handle );
}

bool queryTagAndStandard( HANDLE handle, FILE_ATTRIBUTE_TAG_INFO& tags,
                          FILE_STANDARD_INFO& standard )
{
    return GetFileInformationByHandleEx( handle, FileAttributeTagInfo, &tags,
                                         sizeof( tags ) )
           && GetFileInformationByHandleEx( handle, FileStandardInfo, &standard,
                                            sizeof( standard ) );
}

bool isNonReparseDirectory( HANDLE handle )
{
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_STANDARD_INFO standard{};
    return queryTagAndStandard( handle, tags, standard )
           && ( tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) == 0
           && standard.Directory;
}

bool isRegularDiskFile( HANDLE handle )
{
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_STANDARD_INFO standard{};
    return queryTagAndStandard( handle, tags, standard )
           && ( tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) == 0
           && !standard.Directory && GetFileType( handle ) == FILE_TYPE_DISK;
}

QString identityKeyForHandle( HANDLE handle )
{
    FILE_ID_INFO extendedInfo{};
    if ( GetFileInformationByHandleEx(
             handle, FileIdInfo, &extendedInfo,
             sizeof( extendedInfo ) ) ) {
        const auto fileId = QByteArray(
            reinterpret_cast<const char*>( extendedInfo.FileId.Identifier ),
            sizeof( extendedInfo.FileId.Identifier ) )
                                .toHex();
        return QStringLiteral( "win:%1:%2" )
            .arg( extendedInfo.VolumeSerialNumber, 0, 16 )
            .arg( QString::fromLatin1( fileId ) );
    }

    std::array<WCHAR, MAX_PATH + 1> filesystemName{};
    if ( !GetVolumeInformationByHandleW(
             handle, nullptr, 0, nullptr, nullptr, nullptr,
             filesystemName.data(),
             static_cast<DWORD>( filesystemName.size() ) ) ) {
        // The 128-bit identity query failed and the filesystem is unknown
        // (for example a legacy SMB client). The legacy 64-bit index is not
        // authoritative on ReFS, so without a filesystem name we cannot tell
        // whether it is safe to trust. Fail closed.
        return {};
    }
    if ( QString::fromWCharArray( filesystemName.data() )
             .compare( QStringLiteral( "ReFS" ), Qt::CaseInsensitive )
         == 0 ) {
        // The legacy 64-bit file index is not authoritative on ReFS.
        return {};
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if ( !GetFileInformationByHandle( handle, &info ) ) {
        return {};
    }
    const auto fileIndex = ( static_cast<quint64>( info.nFileIndexHigh ) << 32U )
                           | info.nFileIndexLow;
    return QStringLiteral( "win-legacy:%1:%2" )
        .arg( info.dwVolumeSerialNumber, 0, 16 )
        .arg( fileIndex, 0, 16 );
}

bool sameFileIdentity( HANDLE lhs, HANDLE rhs )
{
    const auto lhsIdentity = identityKeyForHandle( lhs );
    const auto rhsIdentity = identityKeyForHandle( rhs );
    return !lhsIdentity.isEmpty() && lhsIdentity == rhsIdentity;
}

ScopedHandle openExistingDirectoryNoFollow( HANDLE parent,
                                            const QString& name,
                                            ACCESS_MASK access,
                                            nt::Status* status = nullptr,
                                            ULONG shareAccess = ShareAll )
{
    auto handle = ntOpenRelative(
        parent, name, access, nt::FileOpen, FILE_ATTRIBUTE_NORMAL,
        nt::FileSynchronousIoNonAlert | nt::FileOpenReparsePoint, status,
        nullptr, shareAccess );
    if ( !handle.valid() || !isNonReparseDirectory( handle.get() ) ) {
        return {};
    }
    return handle;
}

ScopedHandle openOrCreateDirectoryNoFollow( HANDLE parent,
                                            const QString& name,
                                            ACCESS_MASK access )
{
    nt::Status status = nt::StatusSuccess;
    auto existing = openExistingDirectoryNoFollow( parent, name, access,
                                                   &status );
    if ( existing.valid() ) {
        return existing;
    }
    if ( !isMissingStatus( status ) ) {
        return {};
    }

    ScopedHandle creationParent( ReOpenFile(
        parent, ParentCreateAccess, ShareAll,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT ) );
    if ( !creationParent.valid() ) {
        return {};
    }
    auto created = ntOpenRelative(
        creationParent.get(), name, access, nt::FileCreate,
        FILE_ATTRIBUTE_DIRECTORY,
        nt::FileDirectoryFile | nt::FileSynchronousIoNonAlert, &status );
    if ( created.valid() ) {
        return isNonReparseDirectory( created.get() ) ? std::move( created )
                                                       : ScopedHandle{};
    }
    if ( status == nt::StatusObjectNameCollision ) {
        return openExistingDirectoryNoFollow( parent, name, access );
    }
    return {};
}

std::optional<std::pair<QString, QStringList>> splitWindowsPath(
    const QString& path )
{
    const auto extendedPathPrefix = QStringLiteral( "\\\\?\\" );
    const auto extendedUncPrefix = QStringLiteral( "\\\\?\\UNC\\" );
    auto nativePath = QDir::toNativeSeparators( path );
    if ( nativePath.startsWith( extendedUncPrefix, Qt::CaseInsensitive ) ) {
        nativePath = QStringLiteral( "\\\\" )
                     + nativePath.mid( extendedUncPrefix.size() );
    } else if ( nativePath.startsWith( extendedPathPrefix,
                                       Qt::CaseInsensitive ) ) {
        nativePath = nativePath.mid( extendedPathPrefix.size() );
        if ( nativePath.size() < 3
             || nativePath.at( 1 ) != QLatin1Char( ':' )
             || nativePath.at( 2 ) != QLatin1Char( '\\' ) ) {
            return std::nullopt;
        }
    } else if ( nativePath.startsWith( QStringLiteral( "\\\\.\\" ) ) ) {
        return std::nullopt;
    }
    nativePath = QDir::toNativeSeparators( QDir::cleanPath( nativePath ) );
    if ( nativePath.size() >= 3 && nativePath.at( 1 ) == QLatin1Char( ':' )
         && nativePath.at( 2 ) == QLatin1Char( '\\' ) ) {
        const auto anchor = QStringLiteral( "\\??\\" ) + nativePath.left( 3 );
        return std::make_pair(
            anchor,
            nativePath.mid( 3 ).split(
                QLatin1Char( '\\' ), klogg::qtcompat::skipEmptyParts() ) );
    }
    if ( nativePath.startsWith( QStringLiteral( "\\\\" ) ) ) {
        auto components = nativePath.mid( 2 ).split(
            QLatin1Char( '\\' ), klogg::qtcompat::skipEmptyParts() );
        if ( components.size() < 2 ) {
            return std::nullopt;
        }
        const auto anchor = QStringLiteral( "\\??\\UNC\\" )
                            + components.takeFirst() + QLatin1Char( '\\' )
                            + components.takeFirst() + QLatin1Char( '\\' );
        return std::make_pair( anchor, components );
    }
    return std::nullopt;
}

ScopedHandle bindParentPath( const QString& parentPath,
                             bool createMissingParents )
{
    const auto splitPath = splitWindowsPath( parentPath );
    if ( !splitPath ) {
        return {};
    }
    auto current = openExistingDirectoryNoFollow(
        nullptr, splitPath->first, ParentAccess );
    if ( !current.valid() ) {
        return {};
    }
    for ( const auto& component : splitPath->second ) {
        auto next = createMissingParents
                        ? openOrCreateDirectoryNoFollow(
                              current.get(), component, ParentAccess )
                        : openExistingDirectoryNoFollow(
                              current.get(), component, ParentAccess );
        if ( !next.valid() ) {
            return {};
        }
        current = std::move( next );
    }
    return current;
}

ScopedHandle openExistingRegularFileNoFollow(
    HANDLE parent, const QString& name, ACCESS_MASK access,
    ULONG extraOptions = 0, nt::Status* status = nullptr,
    ULONG_PTR* information = nullptr, ULONG shareAccess = ShareAll )
{
    auto handle = ntOpenRelative(
        parent, name, access, nt::FileOpen, FILE_ATTRIBUTE_NORMAL,
        nt::FileNonDirectoryFile | nt::FileOpenReparsePoint
            | nt::FileSynchronousIoNonAlert | extraOptions,
        status, information, shareAccess );
    if ( !handle.valid() || !isRegularDiskFile( handle.get() ) ) {
        return {};
    }
    return handle;
}

ScopedHandle openExistingObjectNoFollow( HANDLE parent,
                                         const QString& name,
                                         ACCESS_MASK access,
                                         nt::Status* status = nullptr,
                                         ULONG shareAccess = ShareAll )
{
    return ntOpenRelative(
        parent, name, access, nt::FileOpen, FILE_ATTRIBUTE_NORMAL,
        nt::FileOpenReparsePoint | nt::FileSynchronousIoNonAlert, status,
        nullptr, shareAccess );
}

bool isNamedChildStill( HANDLE parent, const QString& name,
                        HANDLE expectedHandle )
{
    auto current = openExistingObjectNoFollow(
        parent, name, FILE_READ_ATTRIBUTES | SYNCHRONIZE );
    return current.valid() && sameFileIdentity( current.get(), expectedHandle );
}

ScopedHandle openCurrentDirectoryForDeletion( HANDLE parent,
                                              const QString& name,
                                              HANDLE expectedHandle )
{
    auto guarded = openExistingDirectoryNoFollow(
        parent, name, TreeDeleteAccess, nullptr, ShareWithoutDelete );
    if ( !guarded.valid()
         || !sameFileIdentity( guarded.get(), expectedHandle ) ) {
        return {};
    }
    return guarded;
}

bool setFileAttributesByHandle( HANDLE handle, DWORD attributes )
{
    FILE_BASIC_INFO update{};
    update.FileAttributes
        = attributes == 0 ? FILE_ATTRIBUTE_NORMAL : attributes;
    return SetFileInformationByHandle( handle, FileBasicInfo, &update,
                                       sizeof( update ) );
}

bool markDeleteByHandle( HANDLE handle, HANDLE attributeRoot = nullptr,
                         const QString& attributeName = {} )
{
    FILE_BASIC_INFO original{};
    if ( !GetFileInformationByHandleEx( handle, FileBasicInfo, &original,
                                        sizeof( original ) ) ) {
        return false;
    }

    ExtendedFileDispositionInfo extendedDisposition{
        FileDispositionFlagDelete
        | FileDispositionFlagIgnoreReadonlyAttribute };
    if ( SetFileInformationByHandle(
             handle, FileDispositionInfoExClass, &extendedDisposition,
             sizeof( extendedDisposition ) ) ) {
        return true;
    }

    const auto extendedError = GetLastError();
    const auto isReadOnly
        = ( original.FileAttributes & FILE_ATTRIBUTE_READONLY ) != 0;
    if ( isReadOnly ) {
        // The legacy disposition path cannot delete a read-only file. The
        // attribute belongs to the shared file record, so clearing it is
        // only safe when this object has a single link; with surviving
        // hard-link aliases the attribute must remain read-only.
        if ( ( original.FileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0
             || attributeRoot == nullptr || attributeName.isEmpty() ) {
            SetLastError( extendedError );
            return false;
        }
        // The link count is a point-in-time check: a concurrent actor with
        // write access to this directory could still CreateHardLink before
        // the attribute is cleared below. The window is accepted because the
        // capture directory is klogg-private and the post-open identity check
        // confines any mutation to this exact record either way.
        BY_HANDLE_FILE_INFORMATION linkInfo{};
        if ( !GetFileInformationByHandle( handle, &linkInfo )
             || linkInfo.nNumberOfLinks != 1 ) {
            SetLastError( extendedError );
            return false;
        }

        const auto expectedIdentity = identityKeyForHandle( handle );
        if ( expectedIdentity.isEmpty() ) {
            SetLastError( extendedError );
            return false;
        }

        // The deletion handle shares without DELETE, so ReOpenFile cannot
        // grant FILE_WRITE_ATTRIBUTES through it. Reopen the verified name
        // relative to the bound directory instead; only the attribute grant
        // is requested, which the existing handles' sharing permits, and the
        // post-open identity check proves the same single-link record before
        // its attribute is touched.
        nt::Status attributeStatus = nt::StatusSuccess;
        auto attributeHandle = openExistingRegularFileNoFollow(
            attributeRoot, attributeName,
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0,
            &attributeStatus, nullptr, ShareAll );
        if ( !attributeHandle.valid()
             || identityKeyForHandle( attributeHandle.get() )
                    != expectedIdentity ) {
            SetLastError( extendedError );
            return false;
        }
        if ( !setFileAttributesByHandle(
                 attributeHandle.get(),
                 original.FileAttributes & ~FILE_ATTRIBUTE_READONLY ) ) {
            SetLastError( extendedError );
            return false;
        }

        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if ( SetFileInformationByHandle(
                 handle, FileDispositionInfo, &disposition,
                 sizeof( disposition ) ) ) {
            return true;
        }

        const auto dispositionError = GetLastError();
        if ( !setFileAttributesByHandle( attributeHandle.get(),
                                         original.FileAttributes ) ) {
            // The deletion failed and the original read-only attribute could
            // not be restored; the record stays behind less protected than
            // the owner configured it. The retry sweep still owns the file's
            // lifecycle, but surface the drift instead of hiding it.
            LOG_WARNING << "Failed to restore read-only attribute after a"
                           " failed legacy disposition on a capture file";
        }
        SetLastError( dispositionError );
        return false;
    }

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle( handle, FileDispositionInfo,
                                       &disposition, sizeof( disposition ) );
}

bool markDescriptorDeleteOnClose( int descriptor )
{
    const auto nativeValue = _get_osfhandle( descriptor );
    return nativeValue != -1
           && markDeleteByHandle( reinterpret_cast<HANDLE>( nativeValue ) );
}

struct WindowsDirectoryEntry {
    QString name;
    ULONG attributes = 0;
    LARGE_INTEGER lastWriteTime{};
};

std::optional<std::vector<WindowsDirectoryEntry>> enumerateDirectory(
    HANDLE directoryHandle )
{
    ScopedHandle enumerationHandle( ReOpenFile(
        directoryHandle, EnumerationAccess, ShareAll,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT ) );
    if ( !enumerationHandle.valid()
         || !sameFileIdentity( directoryHandle, enumerationHandle.get() ) ) {
        return std::nullopt;
    }

    std::vector<WindowsDirectoryEntry> entries;
    QByteArray buffer( 64 * 1024, '\0' );
    BOOLEAN restartScan = TRUE;
    while ( true ) {
        nt::IoStatusBlock io{};
        const auto status = nativeApi().queryDirectoryFile(
            enumerationHandle.get(), nullptr, nullptr, nullptr, &io,
            buffer.data(), static_cast<ULONG>( buffer.size() ),
            nt::FileDirectoryInformation, FALSE, nullptr, restartScan );
        restartScan = FALSE;
        if ( status == nt::StatusNoMoreFiles ) {
            break;
        }
        if ( !statusSucceeded( status ) ) {
            return std::nullopt;
        }

        const auto bytesReturned = static_cast<size_t>( io.information );
        size_t offset = 0;
        while ( offset + offsetof( nt::DirectoryInformation, fileName )
                <= bytesReturned ) {
            const auto* info = reinterpret_cast<const nt::DirectoryInformation*>(
                buffer.constData() + offset );
            const auto nameBytes = static_cast<size_t>( info->fileNameLength );
            if ( ( nameBytes % sizeof( WCHAR ) ) != 0
                 || offset + offsetof( nt::DirectoryInformation, fileName )
                            + nameBytes
                        > bytesReturned ) {
                return std::nullopt;
            }
            const auto name = QString::fromWCharArray(
                info->fileName,
                static_cast<int>( nameBytes / sizeof( WCHAR ) ) );
            if ( name != QStringLiteral( "." )
                 && name != QStringLiteral( ".." ) ) {
                entries.push_back( WindowsDirectoryEntry{
                    name, info->fileAttributes, info->lastWriteTime } );
            }
            if ( info->nextEntryOffset == 0 ) {
                break;
            }
            if ( info->nextEntryOffset
                     < offsetof( nt::DirectoryInformation, fileName )
                 || offset + info->nextEntryOffset > bytesReturned ) {
                return std::nullopt;
            }
            offset += info->nextEntryOffset;
        }
    }
    return entries;
}

bool removeWindowsTreeContents( HANDLE directoryHandle )
{
    const auto entries = enumerateDirectory( directoryHandle );
    if ( !entries ) {
        return false;
    }
    for ( const auto& entry : *entries ) {
        // FILE_LIST_DIRECTORY and FILE_TRAVERSE alias FILE_READ_DATA and
        // FILE_EXECUTE on regular files; an orphan file can be deletable
        // without either. Ask for directory access only for directories.
        const auto childAccess
            = ( entry.attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0
                  ? TreeDeleteAccess
                  : FileDeleteAccess;
        nt::Status status = nt::StatusSuccess;
        auto child = openExistingObjectNoFollow(
            directoryHandle, entry.name, childAccess, &status,
            ShareWithoutDelete );
        if ( !child.valid() ) {
            if ( isMissingStatus( status ) ) {
                continue;
            }
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tags{};
        FILE_STANDARD_INFO standard{};
        if ( !queryTagAndStandard( child.get(), tags, standard ) ) {
            return false;
        }
        const auto reparsePoint
            = ( tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0;
        if ( standard.Directory && !reparsePoint
             && !removeWindowsTreeContents( child.get() ) ) {
            return false;
        }
        if ( !isNamedChildStill( directoryHandle, entry.name, child.get() )
             || !markDeleteByHandle( child.get(), directoryHandle,
                                     entry.name ) ) {
            return false;
        }
    }
    return true;
}

QDateTime dateTimeForWindowsTicks( LONGLONG ticks )
{
    constexpr LONGLONG WindowsToUnixEpochMilliseconds = 11644473600000LL;
    return QDateTime::fromMSecsSinceEpoch(
               ticks / 10000LL - WindowsToUnixEpochMilliseconds )
        .toUTC();
}

std::optional<QDateTime> latestWindowsModificationTime( HANDLE directoryHandle )
{
    FILE_BASIC_INFO basicInfo{};
    if ( !GetFileInformationByHandleEx( directoryHandle, FileBasicInfo,
                                        &basicInfo, sizeof( basicInfo ) ) ) {
        return std::nullopt;
    }
    auto latest = dateTimeForWindowsTicks( basicInfo.LastWriteTime.QuadPart );
    const auto entries = enumerateDirectory( directoryHandle );
    if ( !entries ) {
        return std::nullopt;
    }
    for ( const auto& entry : *entries ) {
        const auto modified
            = dateTimeForWindowsTicks( entry.lastWriteTime.QuadPart );
        if ( modified > latest ) {
            latest = modified;
        }
        const auto isDirectory
            = ( entry.attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
        const auto reparsePoint
            = ( entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0;
        if ( isDirectory && !reparsePoint ) {
            auto child = openExistingDirectoryNoFollow(
                directoryHandle, entry.name, EnumerationAccess );
            if ( !child.valid() ) {
                continue;
            }
            const auto childLatest
                = latestWindowsModificationTime( child.get() );
            if ( childLatest && *childLatest > latest ) {
                latest = *childLatest;
            }
        }
    }
    return latest;
}
#else
int openPath( const QByteArray& path, int flags )
{
    // POSIX open() is variadic only when O_CREAT/O_TMPFILE is present; this
    // fixed-arity wrapper never permits either flag.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::open( path.constData(), flags );
}

int openRelative( int directoryFd, const char* name, int flags )
{
    // POSIX openat() is variadic only when O_CREAT/O_TMPFILE is present; this
    // fixed-arity wrapper never permits either flag.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::openat( directoryFd, name, flags );
}

int createRelative( int directoryFd, const char* name, int flags,
                    mode_t permissions )
{
    // This is the sole typed boundary for the mode-bearing openat() overload.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::openat( directoryFd, name, flags, permissions );
}

#if !defined( Q_OS_MACOS )
long renameNoReplaceRelative( int oldDirectoryFd, const char* oldName,
                              int newDirectoryFd, const char* newName,
                              unsigned int renameFlags )
{
    // Linux exposes renameat2 through syscall() on the oldest supported libc.
    // Keep the unavoidable variadic boundary isolated behind this fixed API.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::syscall( SYS_renameat2, oldDirectoryFd, oldName, newDirectoryFd,
                      newName, renameFlags );
}
#endif

int openDirectory( const QByteArray& path )
{
    return openPath(
        path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW );
}

bool sameIdentity( const struct stat& lhs, const struct stat& rhs )
{
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino
           && ( lhs.st_mode & S_IFMT ) == ( rhs.st_mode & S_IFMT );
}

QString identityKeyForStat( const struct stat& info )
{
    return QStringLiteral( "posix:%1:%2" )
        .arg( static_cast<qulonglong>( info.st_dev ), 0, 16 )
        .arg( static_cast<qulonglong>( info.st_ino ), 0, 16 );
}

std::optional<qulonglong> mountIdentityForDescriptor( int descriptor )
{
#if defined( Q_OS_MACOS )
    Q_UNUSED( descriptor );
    return std::nullopt;
#else
    QFile descriptorInfo(
        QStringLiteral( "/proc/self/fdinfo/%1" ).arg( descriptor ) );
    if ( !descriptorInfo.open( QIODevice::ReadOnly ) ) {
        return std::nullopt;
    }
    while ( !descriptorInfo.atEnd() ) {
        const auto line = descriptorInfo.readLine().trimmed();
        if ( !line.startsWith( QByteArrayLiteral( "mnt_id:" ) ) ) {
            continue;
        }
        bool valid = false;
        const auto mountId = line.mid( 7 ).trimmed().toULongLong( &valid );
        return valid ? std::optional<qulonglong>{ mountId }
                     : std::nullopt;
    }
    return std::nullopt;
#endif
}

bool isSameMountedFilesystem( int parentFd, int childFd,
                              const struct stat& parentInfo,
                              const struct stat& childInfo )
{
    if ( parentInfo.st_dev != childInfo.st_dev ) {
        return false;
    }
    const auto parentMount = mountIdentityForDescriptor( parentFd );
    const auto childMount = mountIdentityForDescriptor( childFd );
    return !parentMount || !childMount || *parentMount == *childMount;
}

qint64 modificationMilliseconds( const struct stat& info )
{
#if defined( Q_OS_MACOS )
    return static_cast<qint64>( info.st_mtimespec.tv_sec ) * 1000
           + info.st_mtimespec.tv_nsec / 1000000;
#else
    return static_cast<qint64>( info.st_mtim.tv_sec ) * 1000
           + info.st_mtim.tv_nsec / 1000000;
#endif
}

QDateTime latestModificationTimeForDirectory( int directoryFd )
{
    struct stat directoryInfo {};
    if ( ::fstat( directoryFd, &directoryInfo ) != 0 ) {
        return {};
    }
    auto latest = QDateTime::fromMSecsSinceEpoch(
        modificationMilliseconds( directoryInfo ) ).toUTC();

    const auto iteratorFd = openRelative(
        directoryFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if ( iteratorFd < 0 ) {
        return latest;
    }
    auto* directory = ::fdopendir( iteratorFd );
    if ( directory == nullptr ) {
        ::close( iteratorFd );
        return latest;
    }

    while ( const auto* entry = ::readdir( directory ) ) {
        const QByteArray name( entry->d_name );
        if ( name == QByteArrayLiteral( "." ) || name == QByteArrayLiteral( ".." ) ) {
            continue;
        }

        struct stat entryInfo {};
        if ( ::fstatat( directoryFd, name.constData(), &entryInfo,
                        AT_SYMLINK_NOFOLLOW )
             != 0 ) {
            continue;
        }
        const auto modified = QDateTime::fromMSecsSinceEpoch(
                                  modificationMilliseconds( entryInfo ) )
                                  .toUTC();
        if ( modified > latest ) {
            latest = modified;
        }

        if ( S_ISDIR( entryInfo.st_mode )
             && entryInfo.st_dev == directoryInfo.st_dev ) {
            const auto childFd = openRelative(
                directoryFd, name.constData(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW );
            if ( childFd >= 0 ) {
                struct stat openedChildInfo {};
                if ( ::fstat( childFd, &openedChildInfo ) == 0
                     && sameIdentity( entryInfo, openedChildInfo )
                     && isSameMountedFilesystem(
                         directoryFd, childFd, directoryInfo,
                         openedChildInfo ) ) {
                    const auto childLatest
                        = latestModificationTimeForDirectory( childFd );
                    if ( childLatest > latest ) {
                        latest = childLatest;
                    }
                }
                ::close( childFd );
            }
        }
    }
    ::closedir( directory );
    return latest;
}

bool renameNoReplace( int oldDirectoryFd, const QByteArray& oldName,
                      int newDirectoryFd, const QByteArray& newName )
{
#if defined( Q_OS_MACOS )
    return ::renameatx_np( oldDirectoryFd, oldName.constData(), newDirectoryFd,
                           newName.constData(), RENAME_EXCL ) == 0;
#else
    constexpr unsigned int RenameNoReplace = 1U;
    return renameNoReplaceRelative(
               oldDirectoryFd, oldName.constData(), newDirectoryFd,
               newName.constData(), RenameNoReplace )
           == 0;
#endif
}

QByteArray uniqueDeletionName( const QByteArray& prefix )
{
    return prefix
           + QUuid::createUuid().toByteArray( QUuid::WithoutBraces );
}

std::optional<QByteArray> quarantineEntry( int directoryFd,
                                           const QByteArray& entryName,
                                           const struct stat& expectedInfo,
                                           const QByteArray& prefix )
{
    for ( int attempt = 0; attempt < 32; ++attempt ) {
        auto quarantineName = uniqueDeletionName( prefix );
        if ( !renameNoReplace( directoryFd, entryName, directoryFd,
                              quarantineName ) ) {
            if ( errno == EEXIST ) {
                continue;
            }
            return std::nullopt;
        }

        struct stat quarantinedInfo {};
        if ( ::fstatat( directoryFd, quarantineName.constData(),
                        &quarantinedInfo, AT_SYMLINK_NOFOLLOW ) == 0
             && sameIdentity( expectedInfo, quarantinedInfo ) ) {
            return quarantineName;
        }

        // The source changed between validation and the atomic move. Restore it
        // only when the public name is still free, and never delete it.
        renameNoReplace( directoryFd, quarantineName, directoryFd, entryName );
        return std::nullopt;
    }
    return std::nullopt;
}

bool restoreQuarantinedEntry( int directoryFd,
                              const QByteArray& quarantineName,
                              const QByteArray& publicName,
                              const struct stat& expectedInfo )
{
    struct stat quarantinedInfo {};
    if ( ::fstatat( directoryFd, quarantineName.constData(),
                    &quarantinedInfo, AT_SYMLINK_NOFOLLOW ) != 0
         || !sameIdentity( expectedInfo, quarantinedInfo )
         || !renameNoReplace( directoryFd, quarantineName, directoryFd,
                              publicName ) ) {
        return false;
    }

    struct stat restoredInfo {};
    if ( ::fstatat( directoryFd, publicName.constData(), &restoredInfo,
                    AT_SYMLINK_NOFOLLOW ) == 0
         && sameIdentity( expectedInfo, restoredInfo ) ) {
        return true;
    }

    // The public name changed after restore. Never move that unverified live
    // occupant back into an internal quarantine path; leave it untouched and
    // let the old bound generation fail closed.
    return false;
}

bool removeDirectoryContents( int directoryFd );

bool removeQuarantinedEntry( int directoryFd,
                             const QByteArray& quarantineName,
                             const struct stat& expectedInfo )
{
    if ( S_ISDIR( expectedInfo.st_mode ) ) {
        const auto childFd = openRelative(
            directoryFd, quarantineName.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW );
        if ( childFd < 0 ) {
            return false;
        }
        struct stat openedInfo {};
        const auto openedExpectedChild
            = ::fstat( childFd, &openedInfo ) == 0
              && sameIdentity( expectedInfo, openedInfo );
        const auto contentsRemoved
            = openedExpectedChild && removeDirectoryContents( childFd );
        struct stat finalNamedInfo {};
        const auto stillNamesOpenedChild
            = contentsRemoved
              && ::fstatat( directoryFd, quarantineName.constData(),
                            &finalNamedInfo, AT_SYMLINK_NOFOLLOW ) == 0
              && sameIdentity( openedInfo, finalNamedInfo );
        ::close( childFd );
        return stillNamesOpenedChild
               && ::unlinkat( directoryFd, quarantineName.constData(),
                              AT_REMOVEDIR ) == 0;
    }

    struct stat finalNamedInfo {};
    return ::fstatat( directoryFd, quarantineName.constData(),
                      &finalNamedInfo, AT_SYMLINK_NOFOLLOW ) == 0
           && sameIdentity( expectedInfo, finalNamedInfo )
           && ::unlinkat( directoryFd, quarantineName.constData(), 0 ) == 0;
}

bool removeDirectoryContents( int directoryFd )
{
    struct stat directoryInfo {};
    if ( ::fstat( directoryFd, &directoryInfo ) != 0 ) {
        return false;
    }

    std::vector<QByteArray> names;
    const auto iteratorFd = openRelative(
        directoryFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if ( iteratorFd < 0 ) {
        return false;
    }
    auto* directory = ::fdopendir( iteratorFd );
    if ( directory == nullptr ) {
        ::close( iteratorFd );
        return false;
    }
    while ( const auto* entry = ::readdir( directory ) ) {
        const QByteArray name( entry->d_name );
        if ( name != QByteArrayLiteral( "." ) && name != QByteArrayLiteral( ".." ) ) {
            names.push_back( name );
        }
    }
    ::closedir( directory );

    for ( const auto& name : names ) {
        struct stat entryInfo {};
        if ( ::fstatat( directoryFd, name.constData(), &entryInfo,
                        AT_SYMLINK_NOFOLLOW ) != 0 ) {
            if ( errno == ENOENT ) {
                continue;
            }
            return false;
        }
        if ( S_ISDIR( entryInfo.st_mode ) ) {
            const auto childFd = openRelative(
                directoryFd, name.constData(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW );
            struct stat openedChildInfo {};
            const auto sameMount
                = childFd >= 0
                  && ::fstat( childFd, &openedChildInfo ) == 0
                  && sameIdentity( entryInfo, openedChildInfo )
                  && isSameMountedFilesystem(
                      directoryFd, childFd, directoryInfo,
                      openedChildInfo );
            if ( childFd >= 0 ) {
                ::close( childFd );
            }
            if ( !sameMount ) {
                // Do not recurse through bind, FUSE, or other mounts.
                return false;
            }
        }
        const auto quarantineName = quarantineEntry(
            directoryFd, name, entryInfo,
            QByteArrayLiteral( ".klogg-delete-" ) );
        if ( !quarantineName ) {
            return false;
        }
        if ( !removeQuarantinedEntry( directoryFd, *quarantineName,
                                      entryInfo ) ) {
            restoreQuarantinedEntry( directoryFd, *quarantineName, name,
                                     entryInfo );
            return false;
        }
    }
    return true;
}

std::optional<QByteArray> quarantineBoundDirectory(
    int parentFd, int directoryFd, const QByteArray& leafName )
{
    struct stat openedInfo {};
    struct stat namedInfo {};
    if ( ::fstat( directoryFd, &openedInfo ) != 0
         || ::fstatat( parentFd, leafName.constData(), &namedInfo,
                       AT_SYMLINK_NOFOLLOW ) != 0
         || !sameIdentity( openedInfo, namedInfo ) ) {
        return std::nullopt;
    }
    return quarantineEntry( parentFd, leafName, openedInfo,
                            QByteArrayLiteral( ".klogg-capture-delete-" ) );
}

bool restoreQuarantinedDirectory( int parentFd,
                                  const QByteArray& quarantineName,
                                  const QByteArray& publicName,
                                  int directoryFd )
{
    struct stat openedInfo {};
    return ::fstat( directoryFd, &openedInfo ) == 0
           && restoreQuarantinedEntry( parentFd, quarantineName, publicName,
                                       openedInfo );
}
#endif
} // namespace

struct SecureCaptureDirectory::Impl {
    explicit Impl( QString directoryPath )
        : path( lexicalPath( directoryPath ) )
        , parentPath( QFileInfo( path ).dir().absolutePath() )
        , leafName( QFileInfo( path ).fileName() )
    {
    }

    QString path;
    QString parentPath;
    QString leafName;
    QString identityKey;
    void invalidateBoundDirectory()
    {
#if defined( Q_OS_WIN )
        directoryHandle.reset();
#else
        if ( directoryFd >= 0 ) {
            ::close( directoryFd );
            directoryFd = -1;
        }
#endif
        removed = true;
    }

    bool removed = false;
    bool failNextRecursiveRemovalForTesting = false;
    std::function<void()> afterRecursiveRemovalQuarantineCallbackForTesting;
#if defined( Q_OS_WIN )
    ScopedHandle parentHandle;
    ScopedHandle directoryHandle;
    mutable QHash<QString, QString> pendingFileDeletions;
#else
    int parentFd = -1;
    int directoryFd = -1;
#endif
};

SecureCaptureDirectory::SecureCaptureDirectory( QString path )
    : impl_( std::make_unique<Impl>( std::move( path ) ) )
{
}

SecureCaptureDirectory::SecureCaptureDirectory( SecureCaptureDirectory&& ) noexcept = default;
SecureCaptureDirectory&
SecureCaptureDirectory::operator=( SecureCaptureDirectory&& ) noexcept = default;

SecureCaptureDirectory::~SecureCaptureDirectory()
{
    if ( !impl_ ) {
        return;
    }
#if defined( Q_OS_WIN )
    impl_->directoryHandle.reset();
    impl_->parentHandle.reset();
#else
    if ( impl_->directoryFd >= 0 ) {
        ::close( impl_->directoryFd );
    }
    if ( impl_->parentFd >= 0 ) {
        ::close( impl_->parentFd );
    }
#endif
}

bool SecureCaptureDirectory::ensureExists()
{
    if ( impl_->removed ) {
        return false;
    }
#if defined( Q_OS_WIN )
    if ( impl_->directoryHandle.valid() ) {
        return isCurrentPath();
    }
    if ( !impl_->parentHandle.valid() ) {
        impl_->parentHandle = bindParentPath( impl_->parentPath, true );
        if ( !impl_->parentHandle.valid() ) {
            return false;
        }
    }
    auto directory = openOrCreateDirectoryNoFollow(
        impl_->parentHandle.get(), impl_->leafName, CaptureAccess );
    if ( !directory.valid() ) {
        return false;
    }
    const auto identityKey = identityKeyForHandle( directory.get() );
    if ( identityKey.isEmpty() ) {
        return false;
    }
    impl_->directoryHandle = std::move( directory );
    impl_->identityKey = identityKey;
    return true;
#else
    if ( impl_->directoryFd >= 0 ) {
        return isCurrentPath();
    }
    if ( impl_->parentFd < 0 ) {
        impl_->parentFd = openDirectory( QFile::encodeName( impl_->parentPath ) );
        if ( impl_->parentFd < 0 ) {
            return false;
        }
    }
    const auto leafName = QFile::encodeName( impl_->leafName );
    if ( ::mkdirat( impl_->parentFd, leafName.constData(), 0700 ) != 0
         && errno != EEXIST ) {
        return false;
    }
    return bindExisting();
#endif
}

bool SecureCaptureDirectory::bindExisting()
{
    if ( impl_->removed ) {
        return false;
    }
#if defined( Q_OS_WIN )
    if ( impl_->directoryHandle.valid() ) {
        return isCurrentPath();
    }
    if ( !impl_->parentHandle.valid() ) {
        impl_->parentHandle = bindParentPath( impl_->parentPath, false );
        if ( !impl_->parentHandle.valid() ) {
            return false;
        }
    }
    auto directory = openExistingDirectoryNoFollow(
        impl_->parentHandle.get(), impl_->leafName, CaptureAccess );
    if ( !directory.valid() ) {
        return false;
    }
    const auto identityKey = identityKeyForHandle( directory.get() );
    if ( identityKey.isEmpty() ) {
        return false;
    }
    impl_->directoryHandle = std::move( directory );
    impl_->identityKey = identityKey;
    return true;
#else
    if ( impl_->parentFd < 0 ) {
        impl_->parentFd = openDirectory( QFile::encodeName( impl_->parentPath ) );
        if ( impl_->parentFd < 0 ) {
            return false;
        }
    }

    const auto leafName = QFile::encodeName( impl_->leafName );
    struct stat namedInfo {};
    if ( ::fstatat( impl_->parentFd, leafName.constData(), &namedInfo,
                    AT_SYMLINK_NOFOLLOW )
             != 0
         || !S_ISDIR( namedInfo.st_mode ) ) {
        return false;
    }
    const auto directoryFd = openRelative(
        impl_->parentFd, leafName.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW );
    if ( directoryFd < 0 ) {
        return false;
    }
    struct stat openedInfo {};
    struct stat currentNamedInfo {};
    const auto valid = ::fstat( directoryFd, &openedInfo ) == 0
                       && ::fstatat( impl_->parentFd, leafName.constData(),
                                     &currentNamedInfo, AT_SYMLINK_NOFOLLOW )
                              == 0
                       && S_ISDIR( currentNamedInfo.st_mode )
                       && sameIdentity( openedInfo, currentNamedInfo );
    if ( !valid ) {
        ::close( directoryFd );
        return false;
    }
    const auto identityKey = identityKeyForStat( openedInfo );
    if ( identityKey.isEmpty() ) {
        ::close( directoryFd );
        return false;
    }
    impl_->directoryFd = directoryFd;
    impl_->identityKey = identityKey;
    return true;
#endif
}

bool SecureCaptureDirectory::isCurrentPath() const
{
    if ( impl_->removed ) {
        return false;
    }
#if defined( Q_OS_WIN )
    if ( !impl_->parentHandle.valid() || !impl_->directoryHandle.valid() ) {
        return false;
    }
    auto current = openExistingDirectoryNoFollow(
        impl_->parentHandle.get(), impl_->leafName, FILE_READ_ATTRIBUTES | SYNCHRONIZE );
    return current.valid()
           && sameFileIdentity( impl_->directoryHandle.get(), current.get() );
#else
    if ( impl_->parentFd < 0 || impl_->directoryFd < 0 ) {
        return false;
    }
    const auto leafName = QFile::encodeName( impl_->leafName );
    struct stat openedInfo {};
    struct stat namedInfo {};
    return ::fstat( impl_->directoryFd, &openedInfo ) == 0
           && ::fstatat( impl_->parentFd, leafName.constData(), &namedInfo,
                         AT_SYMLINK_NOFOLLOW )
                  == 0
           && S_ISDIR( namedInfo.st_mode ) && sameIdentity( openedInfo, namedInfo );
#endif
}

bool SecureCaptureDirectory::isRemoved() const
{
    return impl_->removed;
}

QString SecureCaptureDirectory::identityKey() const
{
    return impl_->identityKey;
}

QString SecureCaptureDirectory::fileIdentity( const QString& filePath ) const
{
    const auto fileName = leafNameForPath( impl_->path, filePath );
    if ( fileName.isEmpty() || !isCurrentPath() ) {
        return {};
    }
#if defined( Q_OS_WIN )
    auto handle = openExistingRegularFileNoFollow(
        impl_->directoryHandle.get(), fileName,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE );
    return handle.valid() ? identityKeyForHandle( handle.get() ) : QString{};
#else
    const auto encodedName = QFile::encodeName( fileName );
    const auto descriptor = openRelative(
        impl_->directoryFd, encodedName.constData(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW );
    if ( descriptor < 0 ) {
        return {};
    }
    struct stat fileInfo {};
    const auto identity
        = ::fstat( descriptor, &fileInfo ) == 0 && S_ISREG( fileInfo.st_mode )
              ? identityKeyForStat( fileInfo )
              : QString{};
    ::close( descriptor );
    return identity;
#endif
}

QString SecureCaptureDirectory::path() const
{
    return impl_->path;
}

bool SecureCaptureDirectory::hasEntries() const
{
    if ( !isCurrentPath() ) {
        return true;
    }
#if defined( Q_OS_WIN )
    const auto entries = enumerateDirectory( impl_->directoryHandle.get() );
    return !entries || !entries->empty();
#else
    const auto iteratorFd = openRelative(
        impl_->directoryFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if ( iteratorFd < 0 ) {
        return true;
    }
    auto* directory = ::fdopendir( iteratorFd );
    if ( directory == nullptr ) {
        ::close( iteratorFd );
        return true;
    }

    bool hasNamedEntry = false;
    while ( const auto* entry = ::readdir( directory ) ) {
        const QByteArray encodedName( entry->d_name );
        if ( encodedName != QByteArrayLiteral( "." )
             && encodedName != QByteArrayLiteral( ".." ) ) {
            hasNamedEntry = true;
            break;
        }
    }
    ::closedir( directory );
    return hasNamedEntry;
#endif
}

QStringList SecureCaptureDirectory::entryList( const QStringList& nameFilters,
                                               QDir::Filters filters,
                                               QDir::SortFlags sort ) const
{
    QStringList entries;
    if ( !isCurrentPath() ) {
        return entries;
    }
#if defined( Q_OS_WIN )
    const auto directoryEntries = enumerateDirectory( impl_->directoryHandle.get() );
    if ( !directoryEntries ) {
        return entries;
    }
    for ( const auto& entry : *directoryEntries ) {
        const auto reparsePoint
            = ( entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0;
        if ( reparsePoint ) {
            continue;
        }
        const auto isDirectory
            = ( entry.attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
        const auto hidden = entry.name.startsWith( QLatin1Char( '.' ) )
                            || ( entry.attributes & FILE_ATTRIBUTE_HIDDEN ) != 0;
        const auto system = ( entry.attributes & FILE_ATTRIBUTE_SYSTEM ) != 0;
        if ( ( hidden && !( filters & QDir::Hidden ) )
             || ( system && !( filters & QDir::System ) )
             || ( !nameFilters.isEmpty()
                  && !QDir::match( nameFilters, entry.name ) ) ) {
            continue;
        }
        if ( ( isDirectory && ( filters & QDir::Dirs ) )
             || ( !isDirectory && ( filters & QDir::Files ) ) ) {
            entries.append( entry.name );
        }
    }
#else
    const auto iteratorFd = openRelative(
        impl_->directoryFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if ( iteratorFd < 0 ) {
        return entries;
    }
    auto* directory = ::fdopendir( iteratorFd );
    if ( directory == nullptr ) {
        ::close( iteratorFd );
        return entries;
    }
    while ( const auto* entry = ::readdir( directory ) ) {
        const QByteArray encodedName( entry->d_name );
        if ( encodedName == QByteArrayLiteral( "." )
             || encodedName == QByteArrayLiteral( ".." ) ) {
            continue;
        }
        const auto fileName = QFile::decodeName( encodedName );
        if ( fileName.startsWith( QLatin1Char( '.' ) ) && !( filters & QDir::Hidden ) ) {
            continue;
        }
        if ( !nameFilters.isEmpty() && !QDir::match( nameFilters, fileName ) ) {
            continue;
        }

        struct stat entryInfo {};
        if ( ::fstatat( impl_->directoryFd, encodedName.constData(), &entryInfo,
                        AT_SYMLINK_NOFOLLOW )
             != 0 ) {
            continue;
        }
        const auto includeFile = ( filters & QDir::Files ) && S_ISREG( entryInfo.st_mode );
        const auto includeDirectory
            = ( filters & QDir::Dirs ) && S_ISDIR( entryInfo.st_mode );
        if ( includeFile || includeDirectory ) {
            entries.append( fileName );
        }
    }
    ::closedir( directory );
#endif
    sortEntries( entries, sort );
    return entries;
}

QStringList SecureCaptureDirectory::entryList( QDir::Filters filters,
                                               QDir::SortFlags sort ) const
{
    return entryList( {}, filters, sort );
}

std::unique_ptr<QFile> SecureCaptureDirectory::openReadFile(
    const QString& filePath, const QString& expectedIdentity ) const
{
    const auto fileName = leafNameForPath( impl_->path, filePath );
    if ( fileName.isEmpty() ) {
        return {};
    }
#if defined( Q_OS_WIN )
    if ( !impl_->directoryHandle.valid() ) {
        return {};
    }
    auto handle = openExistingRegularFileNoFollow(
        impl_->directoryHandle.get(), fileName, ReadAccess,
        nt::FileSequentialOnly );
    if ( !handle.valid() || !isCurrentPath()
         || ( !expectedIdentity.isEmpty()
              && identityKeyForHandle( handle.get() ) != expectedIdentity ) ) {
        return {};
    }
    const auto nativeHandle = handle.release();
    const auto descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>( nativeHandle ), _O_BINARY | _O_RDONLY );
    if ( descriptor < 0 ) {
        CloseHandle( nativeHandle );
        return {};
    }
#else
    if ( impl_->directoryFd < 0 ) {
        return {};
    }
    const auto encodedName = QFile::encodeName( fileName );
    const auto descriptor = openRelative(
        impl_->directoryFd, encodedName.constData(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW );
    if ( descriptor < 0 ) {
        return {};
    }
#endif

    auto file = std::make_unique<QFile>();
    // QFile adopts this already-open native capability; it does not resolve a
    // path again. Validate the adopted object through the same descriptor.
    if ( !file->open( descriptor, QIODevice::ReadOnly,
                      QFileDevice::AutoCloseHandle ) ) {
#if defined( Q_OS_WIN )
        _close( descriptor );
#else
        ::close( descriptor );
#endif
        return {};
    }
#if !defined( Q_OS_WIN )
    struct stat fileInfo {};
    if ( ::fstat( file->handle(), &fileInfo ) != 0
         || !S_ISREG( fileInfo.st_mode )
         || ( !expectedIdentity.isEmpty()
              && identityKeyForStat( fileInfo ) != expectedIdentity ) ) {
        return {};
    }
#endif
    return file;
}

std::unique_ptr<QFile>
SecureCaptureDirectory::createTemporaryFile( QString& filePath ) const
{
    if ( !isCurrentPath() ) {
        return {};
    }
    for ( int attempt = 0; attempt < 32; ++attempt ) {
        const auto fileName
            = QStringLiteral( ".klogg-segment-%1.tmp" )
                  .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
        filePath = fileName;
#if defined( Q_OS_WIN )
        nt::Status status = nt::StatusSuccess;
        ULONG_PTR information = 0;
        auto handle = ntOpenRelative(
            impl_->directoryHandle.get(), fileName, TemporaryAccess,
            nt::FileCreate, FILE_ATTRIBUTE_TEMPORARY,
            nt::FileNonDirectoryFile | nt::FileOpenReparsePoint
                | nt::FileSynchronousIoNonAlert,
            &status, &information );
        if ( !handle.valid() ) {
            if ( status == nt::StatusObjectNameCollision ) {
                continue;
            }
            return {};
        }
        if ( information != nt::FileCreated || !isRegularDiskFile( handle.get() )
             || !isCurrentPath() ) {
            markDeleteByHandle( handle.get() );
            return {};
        }
        const auto descriptor = _open_osfhandle(
            reinterpret_cast<intptr_t>( handle.get() ), _O_BINARY | _O_RDWR );
        if ( descriptor < 0 ) {
            markDeleteByHandle( handle.get() );
            return {};
        }
        handle.release();
#else
        const auto encodedName = QFile::encodeName( fileName );
        const auto descriptor = createRelative(
            impl_->directoryFd, encodedName.constData(),
            O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600 );
        if ( descriptor < 0 ) {
            if ( errno == EEXIST ) {
                continue;
            }
            return {};
        }
#endif
        auto file = std::make_unique<QFile>();
        if ( !file->open( descriptor, QIODevice::ReadWrite,
                          QFileDevice::AutoCloseHandle ) ) {
#if defined( Q_OS_WIN )
            markDescriptorDeleteOnClose( descriptor );
            _close( descriptor );
#else
            ::close( descriptor );
#endif
            return {};
        }
        return file;
    }
    return {};
}

SecureCaptureDirectory::PublishResult SecureCaptureDirectory::publishTemporaryFile(
    const QString& temporaryPath, const QString& targetPath ) const
{
    const auto temporaryName = leafNameForPath( impl_->path, temporaryPath );
    const auto targetName = leafNameForPath( impl_->path, targetPath );
    if ( temporaryName.isEmpty() || targetName.isEmpty() || !isCurrentPath() ) {
        return PublishResult::Error;
    }
#if defined( Q_OS_WIN )
    auto source = openExistingRegularFileNoFollow(
        impl_->directoryHandle.get(), temporaryName, PublishRenameAccess );
    if ( !source.valid() || !isCurrentPath() ) {
        return PublishResult::Error;
    }
    const auto nameBytes
        = static_cast<size_t>( targetName.size() ) * sizeof( WCHAR );
    const auto bufferBytes = offsetof( FILE_RENAME_INFO, FileName ) + nameBytes;
    if ( nameBytes > std::numeric_limits<DWORD>::max()
         || bufferBytes > static_cast<size_t>( std::numeric_limits<int>::max() )
         || bufferBytes > std::numeric_limits<DWORD>::max() ) {
        return PublishResult::Error;
    }
    QByteArray buffer( static_cast<int>( bufferBytes ), '\0' );
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>( buffer.data() );
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = impl_->directoryHandle.get();
    rename->FileNameLength = static_cast<DWORD>( nameBytes );
    std::memcpy( rename->FileName, targetName.utf16(), nameBytes );
    if ( SetFileInformationByHandle(
             source.get(), FileRenameInfo, rename,
             static_cast<DWORD>( bufferBytes ) ) ) {
        // The source handle now owns the published target even if the public
        // capture pathname changed concurrently. Report the completed atomic
        // rename so CaptureStore transfers ownership to the target name.
        return PublishResult::Success;
    }
    const auto error = GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
               ? PublishResult::AlreadyExists
               : PublishResult::Error;
#else
    const auto encodedTemporary = QFile::encodeName( temporaryName );
    const auto encodedTarget = QFile::encodeName( targetName );
    if ( ::linkat( impl_->directoryFd, encodedTemporary.constData(), impl_->directoryFd,
                   encodedTarget.constData(), 0 )
         != 0 ) {
        return errno == EEXIST ? PublishResult::AlreadyExists : PublishResult::Error;
    }
    if ( ::unlinkat( impl_->directoryFd, encodedTemporary.constData(), 0 ) != 0 ) {
        ::unlinkat( impl_->directoryFd, encodedTarget.constData(), 0 );
        return PublishResult::Error;
    }
    if ( !isCurrentPath() ) {
        ::unlinkat( impl_->directoryFd, encodedTarget.constData(), 0 );
        return PublishResult::Error;
    }
    return PublishResult::Success;
#endif
}

bool SecureCaptureDirectory::removeFile(
    const QString& filePath, const QString& expectedIdentity ) const
{
    const auto fileName = leafNameForPath( impl_->path, filePath );
    if ( fileName.isEmpty() ) {
        return false;
    }
#if defined( Q_OS_WIN )
    if ( !impl_->directoryHandle.valid() ) {
        return false;
    }
    // Prefer the public name so the common case also proves the directory is
    // still where callers expect it. If the name is missing or now identifies
    // a successor directory, fall back to the bound handle: it pins the
    // original directory object, so child operations relative to it still
    // target the generation this object owns (POSIX descriptor parity) and
    // can never reach the successor.
    auto publicRoot = openExistingDirectoryNoFollow(
        impl_->parentHandle.get(), impl_->leafName, TreeDeleteAccess, nullptr,
        ShareWithoutDelete );
    HANDLE rootHandle = impl_->directoryHandle.get();
    ScopedHandle rootGuard;
    if ( publicRoot.valid()
         && sameFileIdentity( publicRoot.get(),
                              impl_->directoryHandle.get() ) ) {
        rootGuard = std::move( publicRoot );
        rootHandle = rootGuard.get();
    } else {
        // Do not pin a successor directory (or linger on a failed open)
        // while the bound generation is serviced through its own handle.
        publicRoot.reset();
    }
    const auto pendingDeletion
        = impl_->pendingFileDeletions.find( fileName );
    if ( pendingDeletion != impl_->pendingFileDeletions.end() ) {
        nt::Status currentStatus = nt::StatusSuccess;
        auto current = openExistingRegularFileNoFollow(
            rootHandle, fileName,
            FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0, &currentStatus,
            nullptr, ShareAll );
        if ( !current.valid() ) {
            if ( isMissingStatus( currentStatus ) ) {
                impl_->pendingFileDeletions.erase( pendingDeletion );
                return true;
            }
            return false;
        }
        const auto currentIdentity = identityKeyForHandle( current.get() );
        if ( currentIdentity.isEmpty() ) {
            return false;
        }
        if ( currentIdentity != pendingDeletion.value() ) {
            // The disposition-bound original is gone and this name now owns a
            // successor, so the old retirement is complete. Do not stop here:
            // the caller may have asked to retire exactly this successor
            // (expectedIdentity matches the current occupant). Fall through
            // to the regular path so it applies the same expectedIdentity
            // checks as POSIX instead of leaving a requested file behind.
            impl_->pendingFileDeletions.erase( pendingDeletion );
        } else {
            // A successful open proves the object is not delete-pending: the
            // original link disappeared and the same file record was linked
            // back under this name. The retirement is identity-bound, so
            // reapply the disposition instead of leaving the tombstone stuck
            // forever.
            current.reset();
            nt::Status deleteStatus = nt::StatusSuccess;
            auto deleteHandle = openExistingRegularFileNoFollow(
                rootHandle, fileName, FileDeleteAccess, 0, &deleteStatus,
                nullptr, ShareWithoutDelete );
            if ( !deleteHandle.valid()
                 || identityKeyForHandle( deleteHandle.get() )
                        != pendingDeletion.value()
                 || !isNamedChildStill( rootHandle, fileName,
                                        deleteHandle.get() )
                 || !markDeleteByHandle( deleteHandle.get(), rootHandle,
                                        fileName ) ) {
                return false;
            }
            deleteHandle.reset();

            nt::Status remainingStatus = nt::StatusSuccess;
            auto remaining = openExistingRegularFileNoFollow(
                rootHandle, fileName,
                FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0, &remainingStatus,
                nullptr, ShareAll );
            if ( !remaining.valid() ) {
                if ( isMissingStatus( remainingStatus ) ) {
                    impl_->pendingFileDeletions.erase( pendingDeletion );
                    return true;
                }
                return false;
            }
            const auto remainingIdentity
                = identityKeyForHandle( remaining.get() );
            if ( remainingIdentity.isEmpty() ) {
                return false;
            }
            if ( remainingIdentity != pendingDeletion.value() ) {
                impl_->pendingFileDeletions.erase( pendingDeletion );
                return true;
            }
            return false;
        }
    }

    nt::Status status = nt::StatusSuccess;
    auto handle = openExistingRegularFileNoFollow(
        rootHandle, fileName, FileDeleteAccess, 0, &status, nullptr,
        ShareWithoutDelete );
    if ( !handle.valid() ) {
        return isMissingStatus( status );
    }
    const auto deletedIdentity = identityKeyForHandle( handle.get() );
    if ( deletedIdentity.isEmpty() ) {
        return false;
    }
    if ( !expectedIdentity.isEmpty()
         && deletedIdentity != expectedIdentity ) {
        return true;
    }
    if ( !isNamedChildStill( rootHandle, fileName, handle.get() )
         || !markDeleteByHandle( handle.get(), rootHandle, fileName ) ) {
        return false;
    }
    handle.reset();

    nt::Status remainingStatus = nt::StatusSuccess;
    auto remaining = openExistingRegularFileNoFollow(
        rootHandle, fileName,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0, &remainingStatus,
        nullptr, ShareAll );
    if ( !remaining.valid() ) {
        if ( isMissingStatus( remainingStatus ) ) {
            return true;
        }
        impl_->pendingFileDeletions.insert( fileName, deletedIdentity );
        return false;
    }
    const auto remainingIdentity = identityKeyForHandle( remaining.get() );
    if ( remainingIdentity.isEmpty() ) {
        impl_->pendingFileDeletions.insert( fileName, deletedIdentity );
        return false;
    }
    if ( remainingIdentity != deletedIdentity ) {
        return true;
    }
    impl_->pendingFileDeletions.insert( fileName, deletedIdentity );
    return false;
#else
    if ( impl_->directoryFd < 0 ) {
        return false;
    }
    const auto encodedName = QFile::encodeName( fileName );
    struct stat namedInfo {};
    if ( ::fstatat( impl_->directoryFd, encodedName.constData(), &namedInfo,
                    AT_SYMLINK_NOFOLLOW ) != 0 ) {
        return errno == ENOENT;
    }
    if ( !S_ISREG( namedInfo.st_mode ) ) {
        return false;
    }
    if ( !expectedIdentity.isEmpty()
         && identityKeyForStat( namedInfo ) != expectedIdentity ) {
        return true;
    }

    const auto quarantineName = quarantineEntry(
        impl_->directoryFd, encodedName, namedInfo,
        QByteArrayLiteral( ".klogg-delete-" ) );
    if ( !quarantineName ) {
        return false;
    }
    if ( removeQuarantinedEntry( impl_->directoryFd, *quarantineName,
                                 namedInfo ) ) {
        return true;
    }
    restoreQuarantinedEntry( impl_->directoryFd, *quarantineName,
                             encodedName, namedInfo );
    return false;
#endif
}

bool SecureCaptureDirectory::removeIfEmpty()
{
    if ( !isCurrentPath() ) {
        return false;
    }
#if defined( Q_OS_WIN )
    auto guardedRoot = openCurrentDirectoryForDeletion(
        impl_->parentHandle.get(), impl_->leafName,
        impl_->directoryHandle.get() );
    const auto entries = guardedRoot.valid()
                             ? enumerateDirectory( guardedRoot.get() )
                             : std::nullopt;
    if ( !entries || !entries->empty()
         || !markDeleteByHandle( guardedRoot.get() ) ) {
        return false;
    }
    guardedRoot.reset();
    impl_->directoryHandle.reset();
    impl_->removed = true;
    return true;
#else
    if ( !entryList( QDir::AllEntries | QDir::Hidden | QDir::System,
                     QDir::NoSort )
              .isEmpty() ) {
        return false;
    }
    const auto publicName = QFile::encodeName( impl_->leafName );
    const auto quarantineName = quarantineBoundDirectory(
        impl_->parentFd, impl_->directoryFd, publicName );
    if ( !quarantineName ) {
        return false;
    }
    impl_->leafName = QFile::decodeName( *quarantineName );
    impl_->path = QDir( impl_->parentPath ).filePath( impl_->leafName );
    const auto restorePublicName = [ & ] {
        if ( !restoreQuarantinedDirectory(
                 impl_->parentFd, *quarantineName, publicName,
                 impl_->directoryFd ) ) {
            impl_->invalidateBoundDirectory();
            return false;
        }
        impl_->leafName = QFile::decodeName( publicName );
        impl_->path = QDir( impl_->parentPath ).filePath( impl_->leafName );
        return true;
    };

    if ( !entryList( QDir::AllEntries | QDir::Hidden | QDir::System,
                     QDir::NoSort ).isEmpty() ) {
        restorePublicName();
        return false;
    }

    struct stat openedInfo {};
    struct stat namedInfo {};
    if ( ::fstat( impl_->directoryFd, &openedInfo ) != 0
         || ::fstatat( impl_->parentFd, quarantineName->constData(),
                       &namedInfo, AT_SYMLINK_NOFOLLOW ) != 0
         || !sameIdentity( openedInfo, namedInfo )
         || ::unlinkat( impl_->parentFd, quarantineName->constData(),
                        AT_REMOVEDIR ) != 0 ) {
        restorePublicName();
        return false;
    }
    ::close( impl_->directoryFd );
    impl_->directoryFd = -1;
    impl_->removed = true;
    return true;
#endif
}

void SecureCaptureDirectory::failNextRecursiveRemovalForTesting()
{
    impl_->failNextRecursiveRemovalForTesting = true;
}

void SecureCaptureDirectory::setAfterRecursiveRemovalQuarantineCallbackForTesting(
    std::function<void()> callback )
{
    impl_->afterRecursiveRemovalQuarantineCallbackForTesting
        = std::move( callback );
}

bool SecureCaptureDirectory::removeRecursively()
{
    if ( !isCurrentPath() ) {
        return false;
    }
#if defined( Q_OS_WIN )
    auto guardedRoot = openCurrentDirectoryForDeletion(
        impl_->parentHandle.get(), impl_->leafName,
        impl_->directoryHandle.get() );
    if ( !guardedRoot.valid()
         || !removeWindowsTreeContents( guardedRoot.get() )
         || !markDeleteByHandle( guardedRoot.get() ) ) {
        return false;
    }
    guardedRoot.reset();
    impl_->directoryHandle.reset();
    impl_->removed = true;
    return true;
#else
    const auto publicName = QFile::encodeName( impl_->leafName );
    const auto quarantineName = quarantineBoundDirectory(
        impl_->parentFd, impl_->directoryFd, publicName );
    if ( !quarantineName ) {
        return false;
    }
    impl_->leafName = QFile::decodeName( *quarantineName );
    impl_->path = QDir( impl_->parentPath ).filePath( impl_->leafName );
    const auto restorePublicName = [ & ] {
        if ( !restoreQuarantinedDirectory(
                 impl_->parentFd, *quarantineName, publicName,
                 impl_->directoryFd ) ) {
            impl_->invalidateBoundDirectory();
            return false;
        }
        impl_->leafName = QFile::decodeName( publicName );
        impl_->path = QDir( impl_->parentPath ).filePath( impl_->leafName );
        return true;
    };

    auto afterQuarantineCallback = std::move(
        impl_->afterRecursiveRemovalQuarantineCallbackForTesting );
    impl_->afterRecursiveRemovalQuarantineCallbackForTesting = {};
    if ( afterQuarantineCallback ) {
        afterQuarantineCallback();
    }

    const auto failsRecursiveRemoval
        = impl_->failNextRecursiveRemovalForTesting;
    impl_->failNextRecursiveRemovalForTesting = false;
    if ( failsRecursiveRemoval
         || !removeDirectoryContents( impl_->directoryFd ) ) {
        restorePublicName();
        return false;
    }
    struct stat openedInfo {};
    struct stat namedInfo {};
    if ( ::fstat( impl_->directoryFd, &openedInfo ) != 0
         || ::fstatat( impl_->parentFd, quarantineName->constData(),
                       &namedInfo, AT_SYMLINK_NOFOLLOW ) != 0
         || !sameIdentity( openedInfo, namedInfo )
         || ::unlinkat( impl_->parentFd, quarantineName->constData(),
                        AT_REMOVEDIR ) != 0 ) {
        restorePublicName();
        return false;
    }
    ::close( impl_->directoryFd );
    impl_->directoryFd = -1;
    impl_->removed = true;
    return true;
#endif
}

QDateTime SecureCaptureDirectory::latestModificationTime() const
{
#if defined( Q_OS_WIN )
    if ( !isCurrentPath() ) {
        return {};
    }
    const auto latest
        = latestWindowsModificationTime( impl_->directoryHandle.get() );
    return latest ? *latest : QDateTime{};
#else
    if ( impl_->directoryFd < 0 ) {
        return {};
    }
    return latestModificationTimeForDirectory( impl_->directoryFd );
#endif
}
