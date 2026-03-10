# Build and test 68k-emu (no make required). Requires gcc in PATH (e.g. MSYS2, MinGW-w64).
$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
Set-Location $ProjectRoot

# Same as Makefile
$CFLAGS = "-Wall -Wextra -g -std=c11 -Isrc -Isrc/core -Isrc/isa"
$SRCS = @(
    "src/main.c", "src/core/cpu.c", "src/core/memory.c", "src/core/ea.c",
    "src/isa/move.c", "src/isa/alu.c", "src/isa/branch.c", "src/isa/control.c",
    "src/isa/immediate.c", "src/isa/logic.c", "src/isa/shift.c", "src/tests.c", "src/timing.c"
)
$TARGET = "68k-emu"

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host "gcc not found in PATH. Install a C toolchain (e.g. MSYS2 or MinGW-w64) and add it to PATH." -ForegroundColor Red
    exit 1
}

Write-Host "Building $TARGET ..." -ForegroundColor Cyan
& gcc $CFLAGS.Split() -o $TARGET $SRCS
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Build OK." -ForegroundColor Green

Write-Host "Running all tests ..." -ForegroundColor Cyan
& "./$TARGET" --run-all-tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "All tests passed." -ForegroundColor Green
