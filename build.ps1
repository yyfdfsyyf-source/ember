$ErrorActionPreference = "Stop"

# Fallback build script (pure g++, no CMake required).
# Builds: TUI library + agent executable + examples + tests into ./out/.
# Incremental: object files are only rebuilt when the source or any header
# under tui/include, agent/include or third_party changed.

$gpp = if ($env:CXX) { $env:CXX } else { "g++" }

# The compiler prints warnings to stderr, and under $ErrorActionPreference = "Stop"
# PowerShell turns native stderr into a terminating error: one warning in one file
# (e.g. uia.cpp) would abort a full rebuild. Warnings must stay warnings, so the
# compiler runs with the preference downgraded for its own duration; real failures
# are still caught by the exit code below.
function Invoke-Gpp([string[]]$cmdArgs) {
  $ErrorActionPreference = "Continue"
  & $gpp @cmdArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$out = Join-Path (Get-Location) "out"
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Path $obj -Force | Out-Null

# VERSION at the repo root is the single source of truth for the build version.
# It is handed to the compiler as a raw, unquoted token (no shell quoting to
# survive) and stringized in agent/version.hpp.
$versionFile = Join-Path (Get-Location) "VERSION"
if (!(Test-Path -LiteralPath $versionFile)) { throw "missing $versionFile" }
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()

$flags = @(
  "-std=c++20",
  "-O2",
  "-Wall",
  "-Wextra",
  "-Wpedantic",
  "-Itui/include",
  "-Iagent/include",
  "-Ithird_party",
  "-DEMBER_VERSION=$version",
  "-c"
)

$tui_srcs = @(
  "tui/src/platform.cpp",
  "tui/src/screen.cpp",
  "tui/src/renderer.cpp",
  "tui/src/input.cpp",
  "tui/src/widgets.cpp",
  "tui/src/md.cpp",
  "tui/src/form.cpp",
  "tui/src/drawing.cpp"
)

$agent_srcs = @(
  "agent/src/main.cpp",
  "agent/src/api.cpp",
  "agent/src/http.cpp",
  "agent/src/tools.cpp",
  "agent/src/background.cpp",
  "agent/src/patch.cpp",
  "agent/src/search.cpp",
  "agent/src/rag.cpp",
  "agent/src/context.cpp",
  "agent/src/session.cpp",
  "agent/src/plugins.cpp",
  "agent/src/app.cpp",
  "agent/src/settings.cpp",
  "agent/src/trajectory.cpp",
  "agent/src/usage.cpp",
  "agent/src/cache.cpp"
)

$isWin = $IsWindows -or $env:OS -match "Windows"
if ($isWin) {
  # CDP browser client (Winsock) and UI Automation are Windows-only by design;
  # tools.cpp does not register browser_*/desktop_* elsewhere.
  $agent_srcs += @("agent/src/browser.cpp", "agent/src/uia.cpp")
}
$linkLibs = @()
if ($isWin) {
  $linkLibs = @("-lwinhttp", "-lws2_32", "-lole32", "-loleaut32", "-luiautomationcore")
} else {
  $linkLibs = @("-lcurl")
}

$headers = @(Get-ChildItem "tui/include", "agent/include", "third_party" -Recurse -Include *.hpp, *.h -File)

function Get-ObjectName([string]$src) {
  return ($src -replace '[\\/]', '_') -replace '\.cpp$', '.o'
}

function Test-ObjectStale([string]$src, [string]$objPath) {
  if (!(Test-Path -LiteralPath $objPath)) { return $true }
  $objTime = (Get-Item -LiteralPath $objPath).LastWriteTimeUtc
  if ((Get-Item -LiteralPath $src).LastWriteTimeUtc -gt $objTime) { return $true }
  foreach ($h in $headers) {
    if ($h.LastWriteTimeUtc -gt $objTime) { return $true }
  }
  # A version bump must rebuild, or the binary would keep reporting the old one.
  if ((Get-Item -LiteralPath $versionFile).LastWriteTimeUtc -gt $objTime) { return $true }
  return $false
}

function Compile-Objects([string[]]$srcs) {
  $objs = @()
  foreach ($s in $srcs) {
    $o = Join-Path $obj (Get-ObjectName $s)
    if (Test-ObjectStale $s $o) {
      Invoke-Gpp ($flags + @("-o", $o, $s))
    }
    $objs += $o
  }
  return $objs
}

function Link-Exe([string]$name, [string[]]$objs, [string[]]$extraLibs) {
  $exe = Join-Path $out $name
  if ($isWin) { $exe += ".exe" }
  # Incremental link: relink only if the exe is missing or any input object
  # is newer than the exe (mirrors the object-file staleness check above).
  $stale = $true
  if (Test-Path -LiteralPath $exe) {
    $exeTime = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
    $stale = $false
    foreach ($o in $objs) {
      if ((Get-Item -LiteralPath $o).LastWriteTimeUtc -gt $exeTime) { $stale = $true; break }
    }
  }
  if (-not $stale) {
    Write-Host "Up to date: $exe"
    return
  }
  $staticFlags = @()
  if ($isWin) { $staticFlags = @("-static", "-static-libgcc", "-static-libstdc++") }
  $all = @($objs) + @($extraLibs) + @($staticFlags)
  Invoke-Gpp (@("-o", $exe) + $all)
  Write-Host "Built: $exe"
}

$tuiObjs = @(Compile-Objects $tui_srcs)

# agent
$agentObjs = @(Compile-Objects $agent_srcs)
Link-Exe "agent" ($tuiObjs + $agentObjs) $linkLibs

# demo
$demoObj = @(Compile-Objects @("examples/demo.cpp"))
Link-Exe "demo" ($tuiObjs + $demoObj) @()

# showcase
$showcaseObj = @(Compile-Objects @("examples/showcase.cpp"))
Link-Exe "showcase" ($tuiObjs + $showcaseObj) @()

# plugin example (standalone child process, JSON-RPC over stdio)
$echoObj = @(Compile-Objects @("examples/plugin_echo.cpp"))
Link-Exe "plugin_echo" ($echoObj) @()

# MCP server example (Model Context Protocol over stdio; used by test_mcp)
$mcpObj = @(Compile-Objects @("examples/mcp_echo.cpp"))
Link-Exe "mcp_echo" ($mcpObj) @()

# tests
$testSrcs = @(Get-ChildItem "tests" -Filter *.cpp | ForEach-Object { $_.FullName })
foreach ($t in $testSrcs) {
  $name = [System.IO.Path]::GetFileNameWithoutExtension($t)
  if (-not $isWin -and $name -in @("test_browser", "test_desktop", "test_alltools")) {
    continue  # these drive Chrome over CDP / UI Automation, neither is built here
  }
  $libSrcs = @("tui/src/screen.cpp", "tui/src/renderer.cpp", "tui/src/input.cpp", "tui/src/widgets.cpp", "tui/src/md.cpp", "tui/src/form.cpp", "tui/src/drawing.cpp")
  if ($name -in @("test_inputflow")) {
    $libSrcs += @("tui/src/platform.cpp", "agent/src/app.cpp")
  }
  $extraLibs = @()
  if ($name -in @("test_desktop")) {
    $libSrcs += @("agent/src/uia.cpp")
    $extraLibs = @("-lole32", "-loleaut32", "-luiautomationcore")
  }
  if ($name -in @("test_client", "test_tools", "test_context", "test_patch", "test_browser", "test_alltools", "test_search", "test_session", "test_rag", "test_plugins", "test_mcp", "test_settings", "test_trajectory", "test_background", "test_usage", "test_inputflow")) {
    $libSrcs += @("agent/src/api.cpp", "agent/src/http.cpp", "agent/src/tools.cpp", "agent/src/background.cpp",
                  "agent/src/context.cpp", "agent/src/patch.cpp", "agent/src/search.cpp",
                  "agent/src/rag.cpp", "agent/src/session.cpp",
                  "agent/src/plugins.cpp", "agent/src/settings.cpp", "agent/src/trajectory.cpp",
                  "agent/src/usage.cpp", "agent/src/cache.cpp")
    if ($isWin) { $libSrcs += @("agent/src/browser.cpp", "agent/src/uia.cpp") }
    $extraLibs = $linkLibs
    if ($isWin) { $extraLibs += "-lws2_32" }
  }
  $tObjs = @()
  foreach ($libSrc in $libSrcs) {
    $o = Join-Path $obj ("test_" + (Get-ObjectName $libSrc))
    if (Test-ObjectStale $libSrc $o) {
      Invoke-Gpp ($flags + @("-o", $o, $libSrc))
    }
    $tObjs += $o
  }
  $tObj = Join-Path $obj ("test_main_" + $name + ".o")
  if (Test-ObjectStale $t $tObj) {
    Invoke-Gpp ($flags + @("-o", $tObj, $t))
  }
  Link-Exe $name ($tObjs + $tObj) $extraLibs
}
