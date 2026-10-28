$ErrorActionPreference = 'Stop';

if (-not [Environment]::Is64BitOperatingSystem) {
  throw 'Windows x86 (32-bit) support has ended. klogg requires a 64-bit operating system.'
}

$packageArgs = @{
  packageName    = 'klogg'
  unzipLocation  = "$(Split-Path -parent $MyInvocation.MyCommand.Definition)"
  fileType       = 'exe'
  url64bit       = 'https://github.com/ZEACENT/klogg/releases/download/v22.06/klogg-22.06.0.1289-Win-x64-Qt5-setup.exe'
  checksum64     = '2a287c26fdbda57afb1d30c2d8fdd7db23960fe15957ef1ebb113b339a6c7d62'
  checksumType64 = 'sha256'
  softwareName   = 'klogg'
  silentArgs     = '/S'
  validExitCodes = @(0)
}

Install-ChocolateyPackage @packageArgs