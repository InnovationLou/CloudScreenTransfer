param(
    [string]$VcpkgRoot = 'C:\vcpkg',
    [string]$BuildDir = 'build',
    [string]$OutDir = 'dist',
    [string]$Generator = '',
    [ValidateRange(1, 64)][int]$Jobs = 1
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$projectRoot = [IO.Path]::GetFullPath($PSScriptRoot)

function Resolve-ProjectChild([string]$value) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $projectRoot $value))
    $prefix = $projectRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build/output directories must be children of this project.'
    }
    return $resolved
}
function Check-Exit([string]$action) {
    if ($LASTEXITCODE -ne 0) { throw "$action failed ($LASTEXITCODE)" }
}
function Find-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'dumpbin or Visual Studio vswhere is required.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Check-Exit 'Visual Studio discovery'
    if (-not $installation) { throw 'MSVC x64 tools were not found.' }
    $versions = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName 'bin\Hostx64\x64\dumpbin.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'MSVC dumpbin was not found.'
}

$buildRoot = Resolve-ProjectChild $BuildDir
$outputRoot = Resolve-ProjectChild $OutDir
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
$opencvNotice = Join-Path $VcpkgRoot 'installed\x64-windows-static\share\opencv4\copyright'
if (-not (Test-Path -LiteralPath $toolchain)) { throw 'vcpkg toolchain was not found.' }
if (-not (Test-Path -LiteralPath $opencvNotice)) { throw 'Installed static OpenCV copyright was not found.' }
$configure = @('-S', $projectRoot, '-B', $buildRoot, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain", '-DVCPKG_TARGET_TRIPLET=x64-windows-static', '-DCMAKE_SUPPRESS_REGENERATION=ON')
if ($Generator) { $configure += @('-G', $Generator) }
& cmake @configure
Check-Exit 'CMake configure'
& cmake --build $buildRoot --config Release --parallel $Jobs
Check-Exit 'Release build'
& ctest --test-dir $buildRoot -C Release --output-on-failure
Check-Exit 'CTest'

$executable = Join-Path $buildRoot 'Release\CloudScreenTransfer.exe'
$dumpbin = Find-Dumpbin
$imports = (& $dumpbin /IMPORTS $executable | Out-String)
Check-Exit 'PE import inspection'
$forbidden = 'CreateFile2|GetSystemTimePreciseAsFileTime|GetTempPath2[AW]?|WaitOnAddress|WakeByAddress|api-ms-win-core-path|VCRUNTIME.*\.dll|MSVCP.*\.dll|ucrtbase\.dll|opencv.*\.dll'
if ($imports -match $forbidden) { throw "Unexpected Win8/runtime DLL import: $($Matches[0])" }
$dependencies = (& $dumpbin /DEPENDENTS $executable | Out-String)
Check-Exit 'PE dependency inspection'

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$runRoot = Join-Path $outputRoot $stamp
$name = 'CloudScreenTransfer-0.1.0-win7-x64'
$package = Join-Path $runRoot $name
if (Test-Path -LiteralPath $runRoot) { throw 'Package output already exists; refusing to replace it.' }
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $package 'CloudScreenTransfer.exe')
foreach ($filename in @('README.md', 'LICENSE', 'THIRD_PARTY.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $filename) -Destination $package
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $package -Recurse
# Windows PowerShell 5 writes a UTF-8 BOM, allowing Win7 Notepad to read Chinese.
Get-Content -LiteralPath (Join-Path $projectRoot 'USAGE_ZH.txt') -Encoding UTF8 |
    Set-Content -LiteralPath (Join-Path $package 'USAGE_ZH.txt') -Encoding UTF8
New-Item -ItemType Directory -Path (Join-Path $package 'licenses') | Out-Null
Copy-Item -LiteralPath $opencvNotice -Destination (Join-Path $package 'licenses\OpenCV-installed.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\libcorrect\LICENSE') -Destination (Join-Path $package 'licenses\libcorrect.txt')
# Preserve the notice filenames referenced by the repository documentation.
Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses\OpenCV-4.12.0.txt') -Destination (Join-Path $package 'licenses')
New-Item -ItemType Directory -Path (Join-Path $package 'third_party\libcorrect') | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\libcorrect\LICENSE') -Destination (Join-Path $package 'third_party\libcorrect')
@(
    'Source: https://github.com/InnovationLou/CloudScreenTransfer',
    'Version: 0.1.0',
    'Run: double-click CloudScreenTransfer.exe; choose sender or receiver in the GUI.',
    'The ZIP is portable; no OpenCV DLLs or Python runtime are required.'
) | Set-Content -LiteralPath (Join-Path $package 'SOURCE.txt') -Encoding UTF8
$dllNames = [regex]::Matches($dependencies, '(?im)^\s+([a-z0-9_.-]+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value }
@('Verified system DLL dependencies (OpenCV and C++ runtime are linked statically):') + $dllNames |
    Set-Content -LiteralPath (Join-Path $package 'PE_DEPENDENCIES.txt') -Encoding UTF8

$prefix = $package.TrimEnd('\') + '\'
$manifest = Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($prefix.Length).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
$manifest | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding ASCII
$zip = Join-Path $runRoot ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$zipHash  $name.zip" | Set-Content -LiteralPath ($zip + '.sha256') -Encoding ASCII
Write-Output "PACKAGE=$zip"
Write-Output "SHA256=$zipHash"
