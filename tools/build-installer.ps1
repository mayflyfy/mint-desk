param(
    [string]$Iscc = '',
    [string]$Executable = '',
    [string]$OutputDirectory = '',
    [switch]$TestMode
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
if(-not $Executable){$Executable=Join-Path $root 'dist\MintDesk.exe'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root 'release'}
if(-not $Iscc){
    $command=Get-Command ISCC.exe -ErrorAction SilentlyContinue
    $candidates=@($(if($command){$command.Source}),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'CodexTools\InnoSetup-7.1.1\compiler\ISCC.exe'))
    $Iscc=$candidates|Where-Object{$_ -and (Test-Path -LiteralPath $_ -PathType Leaf)}|Select-Object -First 1
}
if(-not $Iscc){throw 'Inno Setup 7.1+ is required for building the installer only. Pass -Iscc C:\path\to\ISCC.exe.'}
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$versionMatch=[regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),'project\(MintDesk VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if(-not $versionMatch.Success){throw 'Cannot read release version.'}
$version=$versionMatch.Groups[1].Value
if((Get-Item -LiteralPath $Executable).VersionInfo.FileVersion -ne $version){throw 'Installer payload version mismatch.'}
New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
$arguments=@('/Qp',('/DAppVersion='+$version),('/DAppExecutable='+$Executable),('/DReleaseDirectory='+[IO.Path]::GetFullPath($OutputDirectory)))
if($TestMode){$arguments+='/DTestMode=1'}
& $Iscc @arguments (Join-Path $root 'installer\MintDesk.iss')
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed.'}
Get-Item -LiteralPath (Join-Path $OutputDirectory "MintDesk-v$version-setup-x64.exe")|Select-Object Name,Length
