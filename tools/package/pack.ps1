# pack.ps1 - build Ember and assemble the internal-testing zip payload.
# Usage: .\tools\package\pack.ps1                  (uses version from VERSION file)
#        .\tools\package\pack.ps1 -Version 0.10.0 (override; must still match the
#                                                  version the binary reports)
#
# Produces:  releases/Ember-<version>/  (folder)
#            releases/Ember-<version>-win64.zip
#
# VERSION at the repo root is the single source of truth: the build injects it
# into the binary (see agent/version.hpp) and this script verifies the two agree
# before packaging, so a release can no longer ship under a stale number.
param([string]$Version = "")

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$utf8 = New-Object System.Text.UTF8Encoding($true)  # with BOM so Notepad reads it fine

if ([string]::IsNullOrWhiteSpace($Version)) {
  $verFile = Join-Path $root "VERSION"
  if (!(Test-Path -LiteralPath $verFile)) {
    throw "missing $verFile - it is the single source of truth for the release version"
  }
  $Version = (Get-Content -LiteralPath $verFile -Raw).Trim()
}

Write-Host "==> building (static, single-file exe)"
Push-Location $root
try { & (Join-Path $root "build.ps1") } finally { Pop-Location }
$code = $LASTEXITCODE
if ($code) { exit $code }

$agentExe = Join-Path $root "out\agent.exe"
if (!(Test-Path -LiteralPath $agentExe)) { throw "missing $agentExe" }

# Drift guard: the binary has to report the version this package is named after.
$reported = (& $agentExe --version | Out-String).Trim()
if ($reported -notmatch [regex]::Escape($Version)) {
  throw "version mismatch: $agentExe reports '$reported' but VERSION says $Version"
}
Write-Host "==> version check: $reported"

$relParent = Join-Path $root "releases"
$relDir = Join-Path $relParent "Ember-$Version"
$zipPath = Join-Path $relParent "Ember-$Version-win64.zip"

Write-Host "==> staging $relDir"
if (Test-Path -LiteralPath $relDir) { Remove-Item -LiteralPath $relDir -Recurse -Force }
New-Item -ItemType Directory -Path $relDir -Force | Out-Null
Copy-Item -LiteralPath $agentExe -Destination (Join-Path $relDir "agent.exe")

$tpl = Join-Path $PSScriptRoot "templates\README.txt"
if (Test-Path -LiteralPath $tpl) {
  $text = [System.IO.File]::ReadAllText($tpl, $utf8)
  $text = $text.Replace("@VERSION@", $Version)
  $readme = Join-Path $relDir "README.txt"
  [System.IO.File]::WriteAllText($readme, $text, $utf8)
}

# AGPL-3.0-only requires the license text to travel with the binary.
$lic = Join-Path $root "LICENSE"
if (!(Test-Path -LiteralPath $lic)) { throw "missing $lic (the package must carry the license text)" }
Copy-Item -LiteralPath $lic -Destination (Join-Path $relDir "LICENSE")

Write-Host "==> zipping"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $relDir "*") -DestinationPath $zipPath -CompressionLevel Optimal

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
$size = (Get-Item -LiteralPath $zipPath).Length
Write-Host "zip: $zipPath"
Write-Host ("size: {0:N1} KB" -f ($size / 1KB))
Write-Host "SHA256: $hash"