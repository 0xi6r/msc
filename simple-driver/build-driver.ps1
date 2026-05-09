$ErrorActionPreference = 'Stop'

$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$kitVersion = Get-ChildItem -Path "$kitRoot\Include" -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name

$kmInclude = "$kitRoot\Include\$kitVersion\km"
$sharedInclude = "$kitRoot\Include\$kitVersion\shared"
$crtInclude = "$kitRoot\Include\$kitVersion\ucrt"
$kmLib = "$kitRoot\Lib\$kitVersion\km\x64"
$ucrtLib = "$kitRoot\Lib\$kitVersion\ucrt\x64"

if (!(Test-Path "$kmInclude\ntddk.h")) {
    throw "WDK kernel headers not found at $kmInclude. Install the Windows Driver Kit, then rerun this script."
}

if (!(Test-Path "$kmLib\ntoskrnl.lib")) {
    throw "WDK kernel libraries not found at $kmLib. Install the Windows Driver Kit, then rerun this script."
}

cl /nologo /c /W4 /kernel /GS- /Zl /D_X64_=1 /DAMD64 /DWINNT=1 /D_WIN32_WINNT=0x0A00 `
    /I"$kmInclude" /I"$sharedInclude" /I"$crtInclude" `
    simple_echo_driver.c

link /nologo /driver /subsystem:native /entry:DriverEntry /out:simple_echo.sys `
    simple_echo_driver.obj `
    /libpath:"$kmLib" /libpath:"$ucrtLib" `
    ntoskrnl.lib hal.lib BufferOverflowK.lib
