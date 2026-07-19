$ErrorActionPreference = 'Stop'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installationPath = $null
if (Test-Path -LiteralPath $vswhere) {
    $installationPath = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
}

if ([string]::IsNullOrWhiteSpace($installationPath)) {
    $installationPath = 'C:\Program Files\Microsoft Visual Studio\18\Community'
}

$vcvars = Join-Path $installationPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "找不到 Visual Studio C++ 工具链：$vcvars"
}

$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null

$source = Join-Path $PSScriptRoot 'src\main.cpp'
$resource = Join-Path $PSScriptRoot 'src\resource.rc'
$resourceObject = Join-Path $buildDirectory 'resource.res'
$output = Join-Path $buildDirectory 'Notease.exe'
$objectFile = Join-Path $buildDirectory 'main.obj'

$command = "`"$vcvars`" x64 && rc /nologo /fo `"$resourceObject`" `"$resource`" && cl /nologo /utf-8 /std:c++17 /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /Fo:`"$objectFile`" /Fe:`"$output`" `"$source`" `"$resourceObject`" /link /SUBSYSTEM:WINDOWS advapi32.lib gdi32.lib shell32.lib user32.lib gdiplus.lib"
cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "编译失败，退出码：$LASTEXITCODE"
}

Write-Host "已生成：$output"
