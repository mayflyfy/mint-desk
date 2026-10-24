param([string]$Compiler='', [string]$OutputDirectory='')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root 'docs\screenshots'}
if(-not $Compiler){
    $command=Get-Command g++.exe -ErrorAction SilentlyContinue
    $Compiler=@($(if($command){$command.Source}),'C:\TDM-GCC-64\bin\g++.exe','C:\msys64\ucrt64\bin\g++.exe','C:\msys64\mingw64\bin\g++.exe')|Where-Object{$_ -and (Test-Path -LiteralPath $_)}|Select-Object -First 1
}
if(-not $Compiler){throw 'A Windows x64 MinGW-w64 compiler is required.'}
$build=Join-Path $root 'build\tools';New-Item -ItemType Directory -Path $build,$OutputDirectory -Force|Out-Null
$exe=Join-Path $build 'render-screenshots.exe'
$sources=@(Join-Path $root 'tools\render-screenshots.cpp')+@(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.cpp' -File|Where-Object Name -ne 'main.cpp'|ForEach-Object FullName)
$arguments=@('-std=c++17','-O2','-static','-static-libgcc','-static-libstdc++','-municode','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Wpedantic','-o',$exe)+$sources+@('-lcomctl32','-lshell32','-lole32','-luuid','-lshlwapi','-ladvapi32','-ldwmapi','-lgdi32','-luser32','-lcomdlg32','-lgdiplus','-lmsimg32')
& $Compiler @arguments
if($LASTEXITCODE -ne 0){throw 'Screenshot renderer build failed.'}
& $exe ([IO.Path]::GetFullPath($OutputDirectory))
if($LASTEXITCODE -ne 0){throw 'Screenshot renderer failed.'}
