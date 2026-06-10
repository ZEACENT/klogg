# Verify SSL/TLS packaging correctness
# Qt6: must NOT ship OpenSSL DLLs, must ship Schannel TLS backend
# Qt5: must ship OpenSSL 1.1.x DLLs
param(
    [Parameter(Mandatory=$true)]
    [string]$ReleaseDir,

    [Parameter(Mandatory=$true)]
    [string]$QtVersion  # "Qt5" or "Qt6"
)

$ErrorActionPreference = "Stop"
$exitCode = 0

function Write-CheckResult {
    param([string]$Item, [bool]$Pass)
    if ($Pass) {
        Write-Host "  PASS: $Item" -ForegroundColor Green
    } else {
        Write-Host "  FAIL: $Item" -ForegroundColor Red
        $script:exitCode = 1
    }
}

Write-Host "========================================"
Write-Host "SSL/TLS Packaging Verification"
Write-Host "  Release directory: $ReleaseDir"
Write-Host "  Qt version: $QtVersion"
Write-Host "========================================"

if (-not (Test-Path $ReleaseDir)) {
    Write-Error "Release directory not found: $ReleaseDir"
    exit 1
}

# Check for OpenSSL DLLs
$opensslDlls = Get-ChildItem -Path $ReleaseDir -Filter "libcrypto-*.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'libcrypto-\d.*\.dll$' }
$osslDlls = Get-ChildItem -Path $ReleaseDir -Filter "libssl-*.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'libssl-\d.*\.dll$' }

# Check for TLS plugin directory
$tlsDir = Join-Path $ReleaseDir "tls"
$schannelDll = Join-Path $tlsDir "qschannelbackend.dll"
$opensslBackendDll = Join-Path $tlsDir "qopensslbackend.dll"

if ($QtVersion -eq "Qt6") {
    Write-Host "`nQt6 assertions:"
    Write-Host "  Expected: NO OpenSSL DLLs in release root"
    Write-Host "  Expected: tls/qschannelbackend.dll present"
    Write-Host "  Optional: tls/qopensslbackend.dll may be absent (Schannel preferred)`n"

    # Qt6 must NOT ship OpenSSL 1.1.x DLLs
    $hasOpenSsl = ($opensslDlls.Count -gt 0) -or ($osslDlls.Count -gt 0)
    Write-CheckResult "No libcrypto-1_1-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libcrypto-1_1-x64.dll")))
    Write-CheckResult "No libssl-1_1-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libssl-1_1-x64.dll")))
    Write-CheckResult "No libcrypto-1_1.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libcrypto-1_1.dll")))
    Write-CheckResult "No libssl-1_1.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libssl-1_1.dll")))

    # Qt6 must NOT ship OpenSSL 3.x DLLs either (we use Schannel, not OpenSSL)
    Write-CheckResult "No libcrypto-3-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libcrypto-3-x64.dll")))
    Write-CheckResult "No libssl-3-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libssl-3-x64.dll")))
    Write-CheckResult "No libcrypto-3.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libcrypto-3.dll")))
    Write-CheckResult "No libssl-3.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libssl-3.dll")))

    # Qt6 must ship Schannel TLS backend
    Write-CheckResult "tls/ directory exists" (Test-Path $tlsDir)
    Write-CheckResult "tls/qschannelbackend.dll present" (Test-Path $schannelDll)

    # Verify schannel DLL is a real file (not zero bytes)
    if (Test-Path $schannelDll) {
        $size = (Get-Item $schannelDll).Length
        Write-CheckResult "tls/qschannelbackend.dll is non-empty ($size bytes)" ($size -gt 0)
    }

    # Qt plugin DLLs must live in their subdirectories, not in release root.
    # A root-level copy is a packaging bug (e.g. misconfigured 7z flags).
    $pluginDlls = @{
        "tls/qschannelbackend.dll" = "tls"
        "platforms/qwindows.dll" = "platforms"
        "styles/qmodernwindowsstyle.dll" = "styles"
        "styles/qwindowsvistastyle.dll" = "styles"
        "imageformats/qsvg.dll" = "imageformats"
    }
    foreach ($subPath in $pluginDlls.Keys) {
        $leaf = Split-Path $subPath -Leaf
        $rootCopy = Join-Path $ReleaseDir $leaf
        Write-CheckResult "No $leaf in release root (must be in $($pluginDlls[$subPath])/)" (-not (Test-Path $rootCopy))
    }

} elseif ($QtVersion -eq "Qt5") {
    Write-Host "`nQt5 assertions:"
    Write-Host "  Expected: OpenSSL 1.1.x DLLs present (x64 or x86 depending on arch)"
    Write-Host "  Expected: No Schannel requirement (Qt5 uses OpenSSL exclusively)`n"

    # Qt5 must ship OpenSSL 1.1.x DLLs
    # Check both x64 and x86 naming patterns (one set should exist)
    $hasX64 = (Test-Path (Join-Path $ReleaseDir "libcrypto-1_1-x64.dll")) -and
              (Test-Path (Join-Path $ReleaseDir "libssl-1_1-x64.dll"))
    $hasX86 = (Test-Path (Join-Path $ReleaseDir "libcrypto-1_1.dll")) -and
              (Test-Path (Join-Path $ReleaseDir "libssl-1_1.dll"))
    $hasOpenSsl11 = $hasX64 -or $hasX86

    Write-CheckResult "OpenSSL 1.1.x DLLs present (x64: $hasX64, x86: $hasX86)" $hasOpenSsl11

    # Qt5 should NOT ship OpenSSL 3.x DLLs (incompatible)
    Write-CheckResult "No libcrypto-3-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libcrypto-3-x64.dll")))
    Write-CheckResult "No libssl-3-x64.dll in release root" (-not (Test-Path (Join-Path $ReleaseDir "libssl-3-x64.dll")))

} else {
    Write-Error "Unknown QtVersion: $QtVersion (expected Qt5 or Qt6)"
    exit 1
}

Write-Host ""
if ($exitCode -eq 0) {
    Write-Host "========================================"
    Write-Host "  ALL CHECKS PASSED" -ForegroundColor Green
    Write-Host "========================================"
} else {
    Write-Host "========================================"
    Write-Host "  SOME CHECKS FAILED" -ForegroundColor Red
    Write-Host "========================================"
}
exit $exitCode
