echo %KLOGG_QT%
echo %KLOGG_QT_DIR%

if exist "%KLOGG_WORKSPACE%\release" rmdir /s /q "%KLOGG_WORKSPACE%\release"
md "%KLOGG_WORKSPACE%\release"

echo "Copying klogg binaries..."
xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\klogg_portable.exe %KLOGG_WORKSPACE%\release\ /y
xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\klogg.exe %KLOGG_WORKSPACE%\release\ /y

if not exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\helpers\adb.exe (
    echo ERROR: source-built ADB helper missing from output\helpers\adb.exe
    exit /b 1
)
md %KLOGG_WORKSPACE%\release\helpers 2>nul
xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\helpers\* %KLOGG_WORKSPACE%\release\helpers\ /s /e /y
if not exist %KLOGG_WORKSPACE%\release\helpers\adb.exe (
    echo ERROR: failed to stage source-built ADB helper at release\helpers\adb.exe
    exit /b 1
)
if not exist %KLOGG_WORKSPACE%\release\helpers\AdbWinApi.dll (
    echo ERROR: private ADB runtime missing from release\helpers\AdbWinApi.dll
    exit /b 1
)
if not exist %KLOGG_WORKSPACE%\release\helpers\AdbWinUsbApi.dll (
    echo ERROR: private ADB runtime missing from release\helpers\AdbWinUsbApi.dll
    exit /b 1
)
if not exist %KLOGG_WORKSPACE%\release\helpers\libusb-1.0.dll (
    echo ERROR: private ADB runtime missing from release\helpers\libusb-1.0.dll
    exit /b 1
)
if exist %KLOGG_WORKSPACE%\release\adb-helper-assets rmdir /s /q %KLOGG_WORKSPACE%\release\adb-helper-assets
md %KLOGG_WORKSPACE%\release\adb-helper-assets 2>nul
for %%A in (
    adb-helper-licenses.tar.gz
    adb-helper-notices.tar.gz
    adb-helper-sbom.spdx.json
    ADB-HELPER-SOURCE-OFFER.txt
    adb-helper-source-manifest.json
    adb-helper-source-set-receipt.json
) do (
    if not exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\adb-helper-package-assets\%%A (
        echo ERROR: required ADB package support asset missing: %%A
        exit /b 1
    )
    if not exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\adb-helper-package-assets\%%A.sha256 (
        echo ERROR: required ADB package support sidecar missing: %%A.sha256
        exit /b 1
    )
    xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\adb-helper-package-assets\%%A %KLOGG_WORKSPACE%\release\adb-helper-assets\ /y
    xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\adb-helper-package-assets\%%A.sha256 %KLOGG_WORKSPACE%\release\adb-helper-assets\ /y
)

xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\klogg_crashpad_handler.exe %KLOGG_WORKSPACE%\release\ /y

echo "Copying TBB libraries..."
rem Try to copy from output directory first (most likely location)
if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbb12.dll (
    xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbb12.dll %KLOGG_WORKSPACE%\release\ /y
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbbmalloc.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbbmalloc.dll %KLOGG_WORKSPACE%\release\ /y
    )
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbbmalloc_proxy.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\output\tbbmalloc_proxy.dll %KLOGG_WORKSPACE%\release\ /y
    )
) else (
    echo "Trying alternative TBB paths..."
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.41_cxx17_64_md_relwithdebinfo\tbb12.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.41_cxx17_64_md_relwithdebinfo\tbb12.dll %KLOGG_WORKSPACE%\release\ /y
    )
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.41_cxx17_32_md_relwithdebinfo\tbb12.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.41_cxx17_32_md_relwithdebinfo\tbb12.dll %KLOGG_WORKSPACE%\release\ /y
    )
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.42_cxx17_64_md_relwithdebinfo\tbb12.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.42_cxx17_64_md_relwithdebinfo\tbb12.dll %KLOGG_WORKSPACE%\release\ /y
    )
    if exist %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.42_cxx17_32_md_relwithdebinfo\tbb12.dll (
        xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\msvc_19.42_cxx17_32_md_relwithdebinfo\tbb12.dll %KLOGG_WORKSPACE%\release\ /y
    )
)

xcopy %KLOGG_WORKSPACE%\%KLOGG_BUILD_ROOT%\generated\documentation.html %KLOGG_WORKSPACE%\release\ /y
xcopy %KLOGG_WORKSPACE%\COPYING %KLOGG_WORKSPACE%\release\ /y
xcopy %KLOGG_WORKSPACE%\NOTICE %KLOGG_WORKSPACE%\release\ /y
xcopy %KLOGG_WORKSPACE%\README.md %KLOGG_WORKSPACE%\release\ /y
xcopy %KLOGG_WORKSPACE%\DOCUMENTATION.md %KLOGG_WORKSPACE%\release\ /y

echo "Copying vc runtime..."
set "KLOGG_VC_RUNTIME_DIR=%VCToolsRedistDir%%KLOGG_ARCH%\Microsoft.VC143.CRT"
for %%R in (
    msvcp140.dll
    msvcp140_1.dll
    msvcp140_2.dll
    vcruntime140.dll
    vcruntime140_1.dll
) do (
    if not exist "%KLOGG_VC_RUNTIME_DIR%\%%R" (
        echo ERROR: required VC runtime missing: %KLOGG_VC_RUNTIME_DIR%\%%R
        exit /b 1
    )
    xcopy "%KLOGG_VC_RUNTIME_DIR%\%%R" "%KLOGG_WORKSPACE%\release\" /y
    if errorlevel 1 (
        echo ERROR: failed to stage required VC runtime: %%R
        exit /b 1
    )
    if not exist "%KLOGG_WORKSPACE%\release\%%R" (
        echo ERROR: staged VC runtime missing: %KLOGG_WORKSPACE%\release\%%R
        exit /b 1
    )
)

echo "Copying ssl..."
if "%KLOGG_QT%"=="Qt5" (
    xcopy %SSL_DIR%\libcrypto-1_1%SSL_ARCH%.dll %KLOGG_WORKSPACE%\release\ /y
    xcopy %SSL_DIR%\libssl-1_1%SSL_ARCH%.dll %KLOGG_WORKSPACE%\release\ /y
) else (
    echo "Qt6: Skipping OpenSSL DLLs (Schannel TLS backend will be used instead)"
)

echo "Copying Qt..."
set "QTDIR=%KLOGG_QT_DIR:/=\%"
echo %QTDIR%
xcopy %QTDIR%\bin\%KLOGG_QT%Core.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Gui.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Network.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Widgets.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Concurrent.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Xml.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Core5Compat.dll %KLOGG_WORKSPACE%\release\ /y
xcopy %QTDIR%\bin\%KLOGG_QT%Svg.dll %KLOGG_WORKSPACE%\release\ /y

md %KLOGG_WORKSPACE%\release\platforms
xcopy %QTDIR%\plugins\platforms\qwindows.dll %KLOGG_WORKSPACE%\release\platforms\ /y

md %KLOGG_WORKSPACE%\release\styles
xcopy %QTDIR%\plugins\styles\qwindowsvistastyle.dll %KLOGG_WORKSPACE%\release\styles /y
xcopy %QTDIR%\plugins\styles\qmodernwindowsstyle.dll %KLOGG_WORKSPACE%\release\styles /y

md %KLOGG_WORKSPACE%\release\imageformats
xcopy %QTDIR%\plugins\imageformats\qsvg.dll %KLOGG_WORKSPACE%\release\imageformats\ /y

rem Qt6 uses Schannel (built into Windows) instead of OpenSSL for TLS
if "%KLOGG_QT%"=="Qt6" (
    md %KLOGG_WORKSPACE%\release\tls 2>nul
    xcopy %QTDIR%\plugins\tls\qschannelbackend.dll %KLOGG_WORKSPACE%\release\tls\ /y
    echo "Schannel TLS backend deployed"
)

echo "Copying packaging files..."
xcopy %KLOGG_WORKSPACE%\packaging\windows\klogg.nsi  /y
xcopy %KLOGG_WORKSPACE%\packaging\windows\FileAssociation.nsh  /y

echo "Making portable archive..."
set "KLOGG_PORTABLE_ZIP=%KLOGG_WORKSPACE%\klogg-%KLOGG_VERSION%-%KLOGG_ARCH%-%KLOGG_QT%-portable.zip"
if exist "%KLOGG_PORTABLE_ZIP%" del /q "%KLOGG_PORTABLE_ZIP%"
rem Archive the complete staged runtime tree, excluding the installer-only exe.
pushd "%KLOGG_WORKSPACE%\release"
7z a -r "%KLOGG_PORTABLE_ZIP%" .\* -x!klogg.exe
set "KLOGG_7Z_RESULT=%ERRORLEVEL%"
popd
if not "%KLOGG_7Z_RESULT%"=="0" (
    echo "Error creating portable archive (exit code %KLOGG_7Z_RESULT%)"
    exit /b %KLOGG_7Z_RESULT%
)
echo "Portable archive created"

echo "Done!"
exit /b 0
