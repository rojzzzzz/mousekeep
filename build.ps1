param([string]$Compiler = 'zig')
$ErrorActionPreference = 'Stop'
$zig = (Get-Command $Compiler -CommandType Application -ErrorAction Stop).Source
$headers = Join-Path (Split-Path $zig -Parent) 'lib\libc\include\any-windows-any'
if (-not (Test-Path -LiteralPath (Join-Path $headers 'windows.h'))) {
    throw 'Use the portable Zig distribution, with its lib folder beside zig.exe.'
}
Push-Location $PSScriptRoot
try {
    & $zig cc -target x86_64-windows-gnu -isystem $headers -std=c11 -Oz -Wall -Wextra -Werror -fno-builtin -fno-stack-protector -nostdlib '-Wl,--entry=mainCRTStartup,--subsystem,console,--gc-sections,--dynamicbase,--nxcompat,--high-entropy-va' -s -o mousekeep.new.exe mousekeep.c -lkernel32 -luser32 -ladvapi32
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    Move-Item -LiteralPath mousekeep.new.exe -Destination mousekeep.exe -Force
    Get-Item mousekeep.exe | Select-Object Name, Length
} finally {
    Pop-Location
}
