<#
Build ql_sndtap.dll + ql_sndtap_loader.exe (32-bit, i686 MinGW).

    powershell -ExecutionPolicy Bypass -File native\build.ps1
#>
param(
    # Directory holding an i686 MinGW gcc.exe; empty means gcc.exe on PATH.
    [string]$Toolchain = ""
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here '..\bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if ($Toolchain) {
    $gcc = Join-Path $Toolchain 'gcc.exe'
    if (-not (Test-Path $gcc)) { throw "gcc not found: $gcc" }
} else {
    $found = Get-Command gcc.exe -ErrorAction SilentlyContinue
    if (-not $found) { throw "gcc.exe not on PATH; pass -Toolchain <dir of an i686 MinGW gcc>" }
    $gcc = $found.Source
}

$cflags = @(
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-fno-strict-aliasing',   # we deliberately punt structs through char*
    '-m32'
)

$dll = Join-Path $out 'ql_sndtap.dll'
$exe = Join-Path $out 'ql_sndtap_loader.exe'
$ui = Join-Path $out 'ql_sndtap_ui.exe'
$test = Join-Path $out 'test_stub.exe'

Write-Host "building $dll"
& $gcc @cflags '-shared' `
    (Join-Path $here 'ql_sndtap.c') `
    '-o' $dll `
    '-Wl,--image-base,0x72000000' `
    '-static-libgcc' `
    '-lkernel32'
if ($LASTEXITCODE -ne 0) { throw "dll build failed" }

Write-Host "building $exe"
& $gcc @cflags `
    (Join-Path $here 'ql_sndtap_loader.c') (Join-Path $here 'ql_inject.c') `
    '-o' $exe `
    '-static-libgcc'
if ($LASTEXITCODE -ne 0) { throw "loader build failed" }

Write-Host "building $ui"
# UNICODE only for the UI: it is pure wide-char Win32, and without it macros
# like IDC_ARROW resolve to the ANSI variant and fail the W-suffixed calls.
# -municode selects the wWinMain entry point; without it MinGW links the
# ANSI crt stub and reports an undefined WinMain@16.
& $gcc @cflags '-DUNICODE' '-D_UNICODE' '-municode' '-mwindows' `
    (Join-Path $here 'ql_sndtap_ui.c') (Join-Path $here 'ql_inject.c') `
    '-o' $ui `
    '-static-libgcc' `
    '-lkernel32' '-luser32' '-lgdi32' '-lshell32'
if ($LASTEXITCODE -ne 0) { throw "ui build failed" }

# The stub emitter is the one part that fails as a crash inside the game
# rather than a clear error, so its self-test runs on every build.
Write-Host "building $test"
& $gcc '-std=c11' '-O1' '-m32' '-fno-strict-aliasing' '-I' $here `
    (Join-Path $here 'test_stub.c') '-o' $test '-static-libgcc'
if ($LASTEXITCODE -ne 0) { throw "test build failed" }

Write-Host "running stub self-test"
& $test
if ($LASTEXITCODE -ne 0) { throw "STUB SELF-TEST FAILED - do not inject this build" }

Get-ChildItem $dll, $exe, $ui | ForEach-Object {
    "{0,10}  {1}  {2}" -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16), $_.Name
}
