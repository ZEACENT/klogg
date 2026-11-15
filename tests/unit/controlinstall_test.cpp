#include <catch2/catch.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>

#include "controlinstall.h"
#include "../../src/control/src/controlinstall_pal.h"

using namespace klogg::control;

namespace {
void put( const QString& path, const QByteArray& bytes, bool executable = false )
{
    REQUIRE( QDir().mkpath( QFileInfo( path ).absolutePath() ) );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( bytes ) == bytes.size() );
    file.close();
    if ( executable ) REQUIRE( file.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );
}
QByteArray get( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    return file.readAll();
}
struct Fixture {
    QTemporaryDir directory;
    InstallOptions options;
    Fixture()
    {
        REQUIRE( directory.isValid() );
        const auto root = QFileInfo( directory.path() ).canonicalFilePath();
        options.home = root + "/home space's";
        options.executable = root + "/app space's/bin/kloggctl";
        options.shell = InstallShell::Zsh;
        REQUIRE( QDir().mkpath( options.home ) );
        put( options.executable, "#!/bin/sh\nexit 0\n", true );
    }
    QString shim() const { return options.home + "/.local/bin/kloggctl"; }
    QString config() const { return options.home + "/.zshrc"; }
    InstallResult run( InstallAction action = InstallAction::Install )
    {
        return configureControlInstallation( options, action );
    }
};
}

TEST_CASE( "Control installation checks without writing and installs idempotently", "[controlinstall]" )
{
    Fixture f;
    const auto preview = f.run( InstallAction::Check );
    CHECK( preview.status == InstallStatus::NotInstalled );
    CHECK( preview.plannedPaths == QStringList{ f.shim(), f.config() } );
    CHECK( preview.changedPaths.isEmpty() );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    const auto installed = f.run();
    REQUIRE( installed.status == InstallStatus::Installed );
    CHECK( installed.commandPath == f.shim() );
    CHECK( installed.restartRequired );
    CHECK( installed.changedPaths.size() == 2 );
    CHECK( QFileInfo( f.shim() ).isExecutable() );
    CHECK( get( f.shim() ).contains( "exec '" ) );
    CHECK( get( f.shim() ).contains( "space'\\''s" ) );
    CHECK( get( f.config() ).contains( "case" ) );
    const auto original = get( f.config() );
    const auto again = f.run();
    CHECK( again.status == InstallStatus::Installed );
    CHECK( again.changedPaths.isEmpty() );
    CHECK( again.backupPaths.isEmpty() );
    CHECK( get( f.config() ) == original );
    CHECK( f.run( InstallAction::Check ).status == InstallStatus::Installed );
}

TEST_CASE( "Control managed launcher preserves literal arguments and exit status", "[controlinstall]" )
{
    Fixture f;
    put( f.options.executable, "#!/bin/sh\nprintf '%s\\n' \"$@\"\nexit 37\n", true );
    REQUIRE( f.run().status == InstallStatus::Installed );
    const auto sentinel = f.options.home + "/must-not-be-created";
    const QStringList arguments{ "with spaces", "single' and double\" quotes", "", "* ? ; | &",
        "$(touch \"" + sentinel + "\")" };
    QStringList processArguments{ f.shim() };
    processArguments.append( arguments );
    QProcess process;
    // No inherited shell startup variables or real startup files participate.
    process.setProcessEnvironment( QProcessEnvironment{} );
    process.start( "/bin/sh", processArguments );
    REQUIRE( process.waitForStarted( 5000 ) );
    REQUIRE( process.waitForFinished( 5000 ) );
    CHECK( process.exitStatus() == QProcess::NormalExit );
    CHECK( process.exitCode() == 37 );
    CHECK( process.readAllStandardError().isEmpty() );
    CHECK( process.readAllStandardOutput() == ( arguments.join( '\n' ) + '\n' ).toUtf8() );
    CHECK_FALSE( QFileInfo::exists( sentinel ) );
}

TEST_CASE( "Control installation preserves shell content endings and reversible separators", "[controlinstall]" )
{
    for ( const QByteArray& original : { QByteArray(), QByteArray( "# user\nexport OTHER=value\n" ),
                                       QByteArray( "# user\r\nexport OTHER=value\r\n" ),
                                       QByteArray( "# no trailing newline" ) } ) {
        Fixture f;
        put( f.config(), original );
        const auto installed = f.run();
        REQUIRE( installed.status == InstallStatus::Installed );
        REQUIRE( installed.backupPaths.size() == 1 );
        CHECK( get( installed.backupPaths.first() ) == original );
        CHECK( get( f.config() ).startsWith( original ) );
        if ( original.contains( "\r\n" ) ) {
            auto withoutCrLf = get( f.config() );
            withoutCrLf.replace( "\r\n", "" );
            CHECK_FALSE( withoutCrLf.contains( '\n' ) );
        }
        const auto removed = f.run( InstallAction::Remove );
        CHECK( removed.status == InstallStatus::Removed );
        CHECK_FALSE( QFileInfo::exists( f.shim() ) );
        CHECK( get( f.config() ) == original );
        CHECK( f.run( InstallAction::Remove ).changedPaths.isEmpty() );
    }
}

TEST_CASE( "Control installation preserves mixed user line endings around an owned block", "[controlinstall]" )
{
    Fixture f;
    put( f.config(), "# original\r\n" );
    REQUIRE( f.run().status == InstallStatus::Installed );
    put( f.config(), QByteArray( "# new prefix\n" ) + get( f.config() ) + "# suffix\n" );
    CHECK( f.run( InstallAction::Remove ).status == InstallStatus::Removed );
    CHECK( get( f.config() ) == "# new prefix\n# original\r\n# suffix\n" );
}

TEST_CASE( "Control installation repairs moved owned executable without changing user content", "[controlinstall]" )
{
    Fixture f;
    REQUIRE( f.run().status == InstallStatus::Installed );
    const auto oldShim = get( f.shim() );
    f.options.executable = QFileInfo( f.options.executable ).absolutePath() + "/new tool $;\\'";
    put( f.options.executable, "#!/bin/sh\nexit 0\n", true );
    put( f.config(), QByteArray( "# user prefix\n" ) + get( f.config() ) + "# user suffix\n" );
    CHECK( f.run( InstallAction::Check ).status == InstallStatus::NeedsRepair );
    const auto repaired = f.run();
    CHECK( repaired.status == InstallStatus::Installed );
    CHECK( repaired.changedPaths == QStringList{ f.shim() } );
    REQUIRE( repaired.backupPaths.size() == 1 );
    CHECK( get( repaired.backupPaths.first() ) == oldShim );
    CHECK( get( f.config() ).startsWith( "# user prefix\n" ) );
    CHECK( get( f.config() ).endsWith( "# user suffix\n" ) );
}

TEST_CASE( "Control installation refuses unmanaged or edited launcher collisions", "[controlinstall]" )
{
    Fixture f;
    SECTION( "unmanaged" ) { put( f.shim(), "#!/bin/sh\n# unrelated\n", true ); }
    SECTION( "edited managed shim" ) {
        REQUIRE( f.run().status == InstallStatus::Installed );
        put( f.shim(), get( f.shim() ) + "# user edit\n", true );
    }
    const auto before = get( f.shim() );
    CHECK( f.run().status == InstallStatus::Collision );
    CHECK( f.run( InstallAction::Remove ).status == InstallStatus::Collision );
    CHECK( get( f.shim() ) == before );
}

TEST_CASE( "Control installation rejects malformed duplicate and ambiguous blocks before writes", "[controlinstall]" )
{
    Fixture f;
    const QByteArray begin = "# >>> kloggctl shell integration v1 >>>\n";
    const QByteArray end = "# <<< kloggctl shell integration v1 <<<\n";
    SECTION( "missing end" ) { put( f.config(), begin ); }
    SECTION( "duplicate" ) { put( f.config(), begin + end + begin + end ); }
    SECTION( "reversed" ) { put( f.config(), end + begin ); }
    SECTION( "embedded marker" ) { put( f.config(), "echo '" + begin + end ); }
    SECTION( "additional unknown version" ) {
        REQUIRE( f.run().status == InstallStatus::Installed );
        REQUIRE( QFile::remove( f.shim() ) );
        put( f.config(), get( f.config() ) + "# >>> kloggctl shell integration v2 >>>\n# <<< kloggctl shell integration v2 <<<\n" );
    }
    const auto before = get( f.config() );
    CHECK( f.run().status == InstallStatus::Collision );
    CHECK( get( f.config() ) == before );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
}

TEST_CASE( "Control installation validates executable and destinations before writes", "[controlinstall]" )
{
    Fixture f;
    SECTION( "missing executable" ) { f.options.executable += ".missing"; }
    SECTION( "non-executable" ) {
        REQUIRE( QFile::setPermissions( f.options.executable, QFile::ReadOwner | QFile::WriteOwner ) );
    }
    SECTION( "translocated executable" ) {
        f.options.executable = f.options.home + "/AppTranslocation/id/kloggctl";
        put( f.options.executable, "#!/bin/sh\n", true );
    }
    SECTION( "relative executable" ) { f.options.executable = "relative/kloggctl"; }
    SECTION( "newline in executable" ) {
        f.options.executable = f.options.home + "/tool\nname";
        put( f.options.executable, "#!/bin/sh\n", true );
    }
    SECTION( "PATH delimiter in directory" ) { f.options.binDirectory = f.options.home + "/bin:other"; }
    const auto result = f.run();
    CHECK( result.status == InstallStatus::InvalidInput );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    CHECK_FALSE( QFileInfo::exists( f.config() ) );
}

TEST_CASE( "Control installation never follows symlink destinations or parents", "[controlinstall]" )
{
    Fixture f;
    const auto victim = f.options.home + "/victim";
    put( victim, "untouched" );
    SECTION( "config symlink" ) { REQUIRE( QFile::link( victim, f.config() ) ); }
    SECTION( "launcher symlink" ) {
        REQUIRE( QDir().mkpath( QFileInfo( f.shim() ).absolutePath() ) );
        REQUIRE( QFile::link( victim, f.shim() ) );
    }
    SECTION( "config parent symlink" ) {
        const auto other = f.options.home + "/other";
        REQUIRE( QDir().mkpath( other ) );
        REQUIRE( QFile::link( other, f.options.home + "/linked" ) );
        f.options.configFiles = QStringList{ f.options.home + "/linked/config" };
    }
    const auto result = f.run();
    CHECK( result.status == InstallStatus::UnsafePath );
    CHECK( get( victim ) == "untouched" );
}

TEST_CASE( "Control installation respects shell custom configuration locations", "[controlinstall]" )
{
    Fixture f;
    SECTION( "zsh ZDOTDIR" ) {
        f.options.environment.insert( "ZDOTDIR", f.options.home + "/custom zsh" );
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Installed );
        CHECK( result.configFiles == QStringList{ f.options.home + "/custom zsh/.zshrc" } );
        CHECK_FALSE( QFileInfo::exists( f.config() ) );
    }
    SECTION( "bash login selection and nonlogin startup" ) {
        f.options.shell = InstallShell::Bash;
        put( f.options.home + "/.bash_login", "# login user\n" );
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Installed );
        CHECK( result.configFiles.contains( f.options.home + "/.bash_login" ) );
        CHECK( result.configFiles.contains( f.options.home + "/.bashrc" ) );
        CHECK_FALSE( QFileInfo::exists( f.options.home + "/.bash_profile" ) );
    }
    SECTION( "fish XDG config" ) {
        f.options.shell = InstallShell::Fish;
        f.options.environment.insert( "XDG_CONFIG_HOME", f.options.home + "/config space's" );
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Installed );
        REQUIRE( result.configFiles.size() == 1 );
        CHECK( result.configFiles.first() == f.options.home + "/config space's/fish/config.fish" );
        CHECK( get( result.configFiles.first() ).contains( "contains -- '" ) );
        CHECK( get( result.configFiles.first() ).contains( "space\\'s" ) );
    }
    SECTION( "explicit custom files" ) {
        f.options.configFiles = QStringList{ f.options.home + "/custom.rc" };
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Installed );
        CHECK( result.configFiles == f.options.configFiles );
        CHECK_FALSE( QFileInfo::exists( f.config() ) );
    }
}

TEST_CASE( "Control installation recognizes PATH availability without adding or shadowing commands", "[controlinstall]" )
{
    Fixture f;
    SECTION( "correct binary already available" ) {
        f.options.environment.insert( "PATH", QFileInfo( f.options.executable ).absolutePath() );
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Available );
        CHECK( result.changedPaths.isEmpty() );
        CHECK_FALSE( QFileInfo::exists( f.shim() ) );
        CHECK_FALSE( QFileInfo::exists( f.config() ) );
    }
    SECTION( "earlier unrelated binary shadows correct executable" ) {
        const auto earlier = f.options.home + "/earlier";
        put( earlier + "/kloggctl", "#!/bin/sh\n# unrelated\n", true );
        f.options.environment.insert( "PATH", earlier + ':' + QFileInfo( f.options.executable ).absolutePath() );
        CHECK( f.run().status == InstallStatus::Collision );
        CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    }
    SECTION( "user bin already in PATH needs no shell edit" ) {
        f.options.environment.insert( "PATH", QFileInfo( f.shim() ).absolutePath() );
        const auto result = f.run();
        CHECK( result.status == InstallStatus::Installed );
        CHECK_FALSE( result.restartRequired );
        CHECK_FALSE( QFileInfo::exists( f.config() ) );
    }
}

TEST_CASE( "Control installation removes owned bash blocks after login preference changes", "[controlinstall]" )
{
    Fixture f;
    f.options.shell = InstallShell::Bash;
    put( f.options.home + "/.profile", "# existing profile\n" );
    REQUIRE( f.run().status == InstallStatus::Installed );
    put( f.options.home + "/.bash_profile", "# newer login file\n" );
    CHECK( f.run( InstallAction::Remove ).status == InstallStatus::Removed );
    CHECK( get( f.options.home + "/.profile" ) == "# existing profile\n" );
    CHECK( get( f.options.home + "/.bash_profile" ) == "# newer login file\n" );
}

TEST_CASE( "Control installation detects concurrent file replacement and content edits", "[controlinstall]" )
{
    Fixture f;
    put( f.config(), "# original\n" );
    installpal::FileState before;
    REQUIRE( installpal::readFile( f.config(), before ) == InstallStatus::Available );
    SECTION( "content edit" ) { put( f.config(), "# concurrent edit\n" ); }
    SECTION( "replacement with identical content" ) {
        REQUIRE( QFile::remove( f.config() ) );
        put( f.config(), before.bytes );
    }
    SECTION( "symlink substituted" ) {
        REQUIRE( QFile::remove( f.config() ) );
        REQUIRE( QFile::link( f.options.executable, f.config() ) );
    }
    const auto bytes = get( f.config() );
    const auto result = installpal::replaceFile( f.config(), before, QByteArray( "unwanted" ), 0600 );
    CHECK( ( result.status == InstallStatus::ConcurrentChange || result.status == InstallStatus::UnsafePath ) );
    CHECK( result.backup.isEmpty() );
    CHECK( get( f.config() ) == bytes );
}

TEST_CASE( "Control installation refuses group writable configuration content", "[controlinstall]" )
{
    Fixture f;
    put( f.config(), "# shared\n" );
    REQUIRE( QFile::setPermissions( f.config(), QFile::ReadOwner | QFile::WriteOwner | QFile::WriteGroup ) );
    CHECK( f.run().status == InstallStatus::UnsafePath );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
}

TEST_CASE( "Control installation reports partial cross-file failure without rollback claims", "[controlinstall]" )
{
    Fixture f;
    const auto blocked = f.options.home + "/readonly";
    REQUIRE( QDir().mkpath( blocked ) );
    REQUIRE( QFile::setPermissions( blocked, QFile::ReadOwner | QFile::ExeOwner ) );
    f.options.configFiles = QStringList{ blocked + "/config" };
    if ( QFileInfo( blocked ).isWritable() ) {
        WARN( "Permission-denial acceptance requires a non-privileged test process" );
    }
    else {
        const auto result = f.run();
        CHECK( result.status == InstallStatus::PartialFailure );
        CHECK( result.changedPaths == QStringList{ f.shim() } );
        CHECK( QFileInfo::exists( f.shim() ) );
        CHECK_FALSE( QFileInfo::exists( blocked + "/config" ) );
    }
    REQUIRE( QFile::setPermissions( blocked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );
}

TEST_CASE( "Control installation rejects writable ancestors above a private home", "[controlinstall]" )
{
    Fixture f;
    const auto ancestor = f.options.home + "/untrusted";
    f.options.home = ancestor + "/owned/home";
    REQUIRE( QDir().mkpath( f.options.home ) );
    REQUIRE( QFile::setPermissions( f.options.home, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );
    QFile::Permissions permissions = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    SECTION( "group writable non-sticky ancestor" ) { permissions |= QFile::WriteGroup; }
    SECTION( "world writable non-sticky ancestor" ) { permissions |= QFile::WriteOther; }
    REQUIRE( QFile::setPermissions( ancestor, permissions ) );
    CHECK( f.run().status == InstallStatus::UnsafePath );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    CHECK_FALSE( QFileInfo::exists( f.options.home + "/.local" ) );
    CHECK_FALSE( QFileInfo::exists( f.config() ) );
}

TEST_CASE( "Control installation rejects Darwin ACL write grants without changing them", "[controlinstall]" )
{
    if ( QSysInfo::kernelType() != "darwin" ) return;
    Fixture f;
    const auto execute = []( const QString& program, const QStringList& args ) {
        QProcess process;
        process.start( program, args );
        REQUIRE( process.waitForStarted( 5000 ) );
        REQUIRE( process.waitForFinished( 5000 ) );
        REQUIRE( process.exitStatus() == QProcess::NormalExit );
        REQUIRE( process.exitCode() == 0 );
        return process.readAllStandardOutput();
    };
    QString path;
    QString grant;
    SECTION( "home directory" ) {
        path = f.options.home;
        grant = "everyone allow add_file,add_subdirectory,delete_child";
    }
    SECTION( "ancestor above private home" ) {
        path = f.options.home + "/ancestor";
        f.options.home = path + "/private/home";
        REQUIRE( QDir().mkpath( f.options.home ) );
        grant = "everyone allow add_file,add_subdirectory,delete_child";
    }
    SECTION( "configuration file" ) {
        path = f.config();
        put( path, "# original user configuration\n" );
        grant = "everyone allow write,append";
    }
    const auto permissions = QFileInfo( path ).isDir()
        ? QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner : QFile::ReadOwner | QFile::WriteOwner;
    REQUIRE( QFile::setPermissions( path, permissions ) );
    // These native operations touch only freshly created temporary fixtures.
    execute( "/bin/chmod", { "+a", grant, path } );
    CHECK_FALSE( QFileInfo( path ).permissions().testFlag( QFile::WriteGroup ) );
    CHECK_FALSE( QFileInfo( path ).permissions().testFlag( QFile::WriteOther ) );
    const auto before = execute( "/bin/ls", { "-lde", path } );
    const auto result = f.run();
    CHECK( result.status == InstallStatus::UnsafePath );
    CHECK( result.changedPaths.isEmpty() );
    CHECK( result.backupPaths.isEmpty() );
    CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    const auto after = execute( "/bin/ls", { "-lde", path } );
    CHECK( before.mid( before.indexOf( '\n' ) + 1 ) == after.mid( after.indexOf( '\n' ) + 1 ) );
    execute( "/bin/chmod", { "-N", path } );
}

TEST_CASE( "Control removal preview is read-only even when the correct command is on PATH", "[controlinstall]" )
{
    Fixture f;
    REQUIRE( f.run().status == InstallStatus::Installed );
    f.options.environment.insert( "PATH", QFileInfo( f.options.executable ).absolutePath() );
    const auto shim = get( f.shim() );
    const auto configuration = get( f.config() );
    const auto preview = f.run( InstallAction::CheckRemoval );
    CHECK( preview.status == InstallStatus::Installed );
    CHECK( preview.plannedPaths == QStringList{ f.shim(), f.config() } );
    CHECK( preview.changedPaths.isEmpty() );
    CHECK( preview.backupPaths.isEmpty() );
    CHECK( get( f.shim() ) == shim );
    CHECK( get( f.config() ) == configuration );
    REQUIRE( f.run( InstallAction::Remove ).status == InstallStatus::Removed );
    const auto empty = f.run( InstallAction::CheckRemoval );
    CHECK( empty.status == InstallStatus::NotInstalled );
    CHECK( empty.plannedPaths.isEmpty() );
    CHECK( empty.changedPaths.isEmpty() );
}

TEST_CASE( "Control installation applies only the approved mutation target list", "[controlinstall]" )
{
    Fixture f;
    SECTION( "new bash login preference requires a new preview" ) {
        f.options.shell = InstallShell::Bash;
        const auto preview = f.run( InstallAction::Check );
        REQUIRE( preview.plannedPaths.contains( f.options.home + "/.bash_profile" ) );
        put( f.options.home + "/.bash_login", "# new login preference\n" );
        const auto result = configureControlInstallation( f.options, InstallAction::Install, nullptr, &preview.plannedPaths );
        CHECK( result.status == InstallStatus::ConcurrentChange );
        CHECK( result.changedPaths.isEmpty() );
        CHECK( result.backupPaths.isEmpty() );
        CHECK_FALSE( QFileInfo::exists( f.shim() ) );
        CHECK_FALSE( QFileInfo::exists( f.options.home + "/.bashrc" ) );
        CHECK( get( f.options.home + "/.bash_login" ) == "# new login preference\n" );
    }
    SECTION( "removed owned block invalidates removal preview before deleting launcher" ) {
        REQUIRE( f.run().status == InstallStatus::Installed );
        const auto preview = f.run( InstallAction::CheckRemoval );
        put( f.config(), "# replaced by user\n" );
        const auto result = configureControlInstallation( f.options, InstallAction::Remove, nullptr, &preview.plannedPaths );
        CHECK( result.status == InstallStatus::ConcurrentChange );
        CHECK( result.changedPaths.isEmpty() );
        CHECK( result.backupPaths.isEmpty() );
        CHECK( QFileInfo::exists( f.shim() ) );
    }
    SECTION( "unchanged approved paths install normally" ) {
        const auto preview = f.run( InstallAction::Check );
        const auto result = configureControlInstallation( f.options, InstallAction::Install, nullptr, &preview.plannedPaths );
        CHECK( result.status == InstallStatus::Installed );
        CHECK( result.changedPaths == preview.plannedPaths );
    }
    SECTION( "newly available executable still invalidates a nonempty approved plan" ) {
        const auto preview = f.run( InstallAction::Check );
        f.options.environment.insert( "PATH", QFileInfo( f.options.executable ).absolutePath() );
        const auto result = configureControlInstallation( f.options, InstallAction::Install, nullptr, &preview.plannedPaths );
        CHECK( result.status == InstallStatus::ConcurrentChange );
        CHECK( result.changedPaths.isEmpty() );
        CHECK_FALSE( QFileInfo::exists( f.shim() ) );
    }
}

TEST_CASE( "Control installation Windows detects earlier shell command shadows", "[controlinstall]" )
{
    Fixture f;
    f.options.shell = InstallShell::Windows;
    f.options.executable += ".exe";
    put( f.options.executable, "fixture", true );
    const auto earlier = f.options.home + "/earlier";
    put( earlier + "/kloggctl.cmd", "@rem unrelated\n", true );
    f.options.environment.insert( "PATH", earlier + ';' + QFileInfo( f.options.executable ).absolutePath() );
    UserPathAccess access{ [] { return std::optional<UserPathState>( UserPathState{} ); },
        []( const UserPathState&, const UserPathState& ) { FAIL( "Shadowed command must not modify PATH" ); return UserPathMutation{ InstallStatus::IoError, {} }; } };
    CHECK( configureControlInstallation( f.options, InstallAction::Install, &access ).status == InstallStatus::Collision );
}

TEST_CASE( "Control installation Windows PATH logic preserves raw value type and ownership", "[controlinstall]" )
{
    Fixture f;
    f.options.shell = InstallShell::Windows;
    f.options.executable += ".exe";
    put( f.options.executable, "fixture executable, never launched", true );
    UserPathState store{ true, true, "%USERPROFILE%\\tools;C:\\unrelated", {} };
    int writes = 0;
    UserPathAccess access{
        [&store] { return std::optional<UserPathState>( store ); },
        [&]( const UserPathState& expected, const UserPathState& desired ) {
            CHECK( expected.value == store.value );
            CHECK( expected.expandable == store.expandable );
            store = desired;
            ++writes;
            return UserPathMutation{ InstallStatus::Installed, "HKCU\\test-only-backup" };
        }
    };
    const auto run = [&]( InstallAction action ) {
        return configureControlInstallation( f.options, action, &access );
    };
    const auto original = store.value;
    SECTION( "install repeat and remove exact addition" ) {
        const auto installed = run( InstallAction::Install );
        CHECK( installed.status == InstallStatus::Installed );
        CHECK( installed.restartRequired );
        CHECK( installed.backupPaths == QStringList{ "HKCU\\test-only-backup" } );
        CHECK( store.expandable );
        CHECK( store.value.startsWith( original + ';' ) );
        CHECK_FALSE( store.ownedDirectory.isEmpty() );
        CHECK( run( InstallAction::Install ).status == InstallStatus::Installed );
        CHECK( writes == 1 );
        CHECK( run( InstallAction::Remove ).status == InstallStatus::Removed );
        CHECK( store.value == original );
        CHECK( store.expandable );
        CHECK( store.ownedDirectory.isEmpty() );
    }
    SECTION( "preexisting user entry is never claimed or removed" ) {
        store.value = QFileInfo( f.options.executable ).absolutePath();
        const auto prior = store.value;
        CHECK( run( InstallAction::Install ).status == InstallStatus::Available );
        CHECK( store.ownedDirectory.isEmpty() );
        CHECK( run( InstallAction::Remove ).status == InstallStatus::Removed );
        CHECK( store.value == prior );
        CHECK( writes == 0 );
    }
    SECTION( "duplicate owned entries after user edit are ambiguous" ) {
        REQUIRE( run( InstallAction::Install ).status == InstallStatus::Installed );
        store.value += ';' + store.ownedDirectory;
        const auto edited = store.value;
        CHECK( run( InstallAction::Remove ).status == InstallStatus::Collision );
        CHECK( store.value == edited );
        CHECK( writes == 1 );
    }
    SECTION( "unrelated subsequent PATH changes are preserved" ) {
        REQUIRE( run( InstallAction::Install ).status == InstallStatus::Installed );
        store.value = "C:\\extra;" + store.value;
        CHECK( run( InstallAction::Remove ).status == InstallStatus::Removed );
        CHECK( store.value == "C:\\extra;" + original );
    }
    SECTION( "REG_SZ placeholders are not treated as expanded directories" ) {
        f.options.environment.insert( "APPROOT", QFileInfo( f.options.executable ).absolutePath() );
        store.value = "%APPROOT%";
        store.expandable = false;
        CHECK( run( InstallAction::Install ).status == InstallStatus::Installed );
        CHECK( store.value.startsWith( "%APPROOT%;" ) );
        CHECK_FALSE( store.expandable );
        CHECK( writes == 1 );
    }
    SECTION( "REG_EXPAND_SZ matching placeholder remains user owned" ) {
        f.options.environment.insert( "APPROOT", QFileInfo( f.options.executable ).absolutePath() );
        store.value = "%APPROOT%";
        CHECK( run( InstallAction::Install ).status == InstallStatus::Available );
        CHECK( writes == 0 );
        CHECK( store.ownedDirectory.isEmpty() );
    }
    SECTION( "removal preview bypasses already-correct inherited PATH" ) {
        REQUIRE( run( InstallAction::Install ).status == InstallStatus::Installed );
        f.options.environment.insert( "PATH", QFileInfo( f.options.executable ).absolutePath() );
        const auto ownedValue = store.value;
        const auto preview = run( InstallAction::CheckRemoval );
        CHECK( preview.status == InstallStatus::Installed );
        CHECK( preview.plannedPaths.size() == 2 );
        CHECK( preview.changedPaths.isEmpty() );
        CHECK( preview.backupPaths.isEmpty() );
        CHECK( store.value == ownedValue );
        CHECK( writes == 1 );
        REQUIRE( run( InstallAction::Remove ).status == InstallStatus::Removed );
        const auto empty = run( InstallAction::CheckRemoval );
        CHECK( empty.status == InstallStatus::NotInstalled );
        CHECK( empty.plannedPaths.isEmpty() );
        CHECK( writes == 2 );
    }
    SECTION( "Windows guidance does not promise a fresh shell refreshes Explorer PATH" ) {
        const auto result = run( InstallAction::Install );
        CHECK( result.message.contains( "sign out", Qt::CaseInsensitive ) );
        CHECK( result.message.contains( "new shell", Qt::CaseInsensitive ) );
        CHECK( result.message.contains( "old PATH", Qt::CaseInsensitive ) );
    }
    SECTION( "changed registry ownership rejects the approved install plan" ) {
        const auto preview = run( InstallAction::Check );
        REQUIRE( preview.plannedPaths.size() == 2 );
        store.value += ';' + QFileInfo( f.options.executable ).absolutePath();
        const auto result = configureControlInstallation( f.options, InstallAction::Install, &access, &preview.plannedPaths );
        CHECK( result.status == InstallStatus::ConcurrentChange );
        CHECK( result.changedPaths.isEmpty() );
        CHECK( writes == 0 );
    }
    SECTION( "empty approved registry target list cannot authorize new changes" ) {
        const QStringList approved;
        const auto result = configureControlInstallation( f.options, InstallAction::Install, &access, &approved );
        CHECK( result.status == InstallStatus::ConcurrentChange );
        CHECK( result.changedPaths.isEmpty() );
        CHECK( writes == 0 );
    }
    SECTION( "native failure is reported without retry or simulated success" ) {
        access.replace = [&]( const UserPathState&, const UserPathState& ) {
            ++writes;
            return UserPathMutation{ InstallStatus::PartialFailure, {} };
        };
        CHECK( run( InstallAction::Install ).status == InstallStatus::PartialFailure );
        CHECK( writes == 1 );
    }
}
