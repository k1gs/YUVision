[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo "build-msi" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "out\msi" }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

$cmakeFile = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cmakeFile -notmatch 'project\(YUVision VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read the YUVision version from CMakeLists.txt."
}
$version = $Matches[1]
$packageName = "YUVision-v$version-windows-x64"
$bundleDirectory = Join-Path $OutputDirectory "YUVision-v$version-installer"
$toolDirectory = Join-Path $repo "out\tools\wix-4.0.4"
$extensionDirectory = Join-Path $repo "out\tools\wix-extensions"
$wix = Join-Path $toolDirectory "wix.exe"

New-Item -ItemType Directory -Force $toolDirectory, $extensionDirectory, $BuildDirectory,
    $OutputDirectory | Out-Null

if (-not (Test-Path $wix)) {
    Write-Host "Restoring WiX 4.0.4 into $toolDirectory"
    & dotnet tool install --tool-path $toolDirectory wix --version 4.0.4
    if ($LASTEXITCODE -ne 0) { throw "WiX tool restore failed ($LASTEXITCODE)." }
}

$env:WIX = $toolDirectory
$env:WIX_EXTENSIONS = $extensionDirectory
$installedExtensions = & $wix extension list --global 2>&1 | Out-String
if ($installedExtensions -notmatch 'WixToolset\.UI\.wixext\s+4\.0\.4') {
    Write-Host "Restoring the WiX UI extension"
    & $wix extension add --global WixToolset.UI.wixext/4.0.4
    if ($LASTEXITCODE -ne 0) { throw "WiX UI extension restore failed ($LASTEXITCODE)." }
}

& cmake -S $repo -B $BuildDirectory -G $Generator -A x64 -DYUVISION_ENABLE_MSI=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }
& cmake --build $BuildDirectory --config Release --target yuvision
if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)." }

$releaseExe = Join-Path $BuildDirectory "Release\YUVision.exe"
if (-not (Test-Path $releaseExe)) { throw "Expected release executable was not created: $releaseExe" }

# Guard the portable-release contract. YUVision deliberately uses the static MSVC runtime;
# accidentally returning to /MD would make the app silently fail on clean Windows installs.
$visualStudioRoot = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022"
$dumpbin = Get-ChildItem $visualStudioRoot -Filter dumpbin.exe -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64\\dumpbin\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $dumpbin) { throw "Could not locate the Visual Studio x64 dumpbin.exe dependency checker." }
$dependencies = & $dumpbin.FullName /dependents $releaseExe | Out-String
if ($LASTEXITCODE -ne 0) { throw "Dependency inspection failed ($LASTEXITCODE)." }
if ($dependencies -match '(?im)^\s*(MSVCP\d+|VCRUNTIME\d+(?:_1)?)\.dll\s*$') {
    throw "Release EXE depends on the dynamic MSVC runtime ($($Matches[0].Trim())). Refusing to package it."
}

& cpack --config (Join-Path $BuildDirectory "CPackConfig.cmake") -C Release -G WIX -B $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw "MSI packaging failed ($LASTEXITCODE)." }

$msi = Join-Path $OutputDirectory "$packageName.msi"
if (-not (Test-Path $msi)) { throw "Expected MSI was not created: $msi" }

if (Test-Path $bundleDirectory) { Remove-Item -LiteralPath $bundleDirectory -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $bundleDirectory "logs") | Out-Null
Copy-Item $msi $bundleDirectory
Copy-Item (Join-Path $repo "packaging\Install-YUVision.cmd") $bundleDirectory
Copy-Item (Join-Path $repo "packaging\Uninstall-YUVision.cmd") $bundleDirectory
Copy-Item (Join-Path $repo "packaging\README-INSTALLER.txt") $bundleDirectory
Copy-Item (Join-Path $repo "packaging\logs\README.txt") (Join-Path $bundleDirectory "logs")
Copy-Item (Join-Path $repo "README.md") $bundleDirectory
Copy-Item (Join-Path $repo "LICENSE") $bundleDirectory

$zip = "$bundleDirectory.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $bundleDirectory "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host "MSI bundle: $bundleDirectory"
Write-Host "Shareable ZIP: $zip"
