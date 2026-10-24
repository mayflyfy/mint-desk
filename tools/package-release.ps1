param([string]$Compiler='', [string]$Version='', [string]$Executable='', [switch]$SkipBuild)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
if(-not $Version){$match=[regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),'project\(MintDesk VERSION ([0-9]+\.[0-9]+\.[0-9]+)');if(-not $match.Success){throw 'Cannot read release version.'};$Version=$match.Groups[1].Value}
if($Version -notmatch '^\d+\.\d+\.\d+$'){throw 'Version must be x.y.z.'}
if(-not $SkipBuild){& (Join-Path $root 'build.ps1') -Compiler $Compiler;if($LASTEXITCODE -ne 0){throw 'Release build failed.'}}
if(-not $Executable){$Executable=Join-Path $root 'dist\MintDesk.exe'}
$exe=Get-Item -LiteralPath $Executable
if($exe.VersionInfo.FileVersion -ne $Version -or $exe.VersionInfo.InternalName -ne 'MintDesk'){throw 'Executable identity/version does not match this release.'}
$release=Join-Path $root 'release';$name="MintDesk-v$Version-windows-x64";$stage=Join-Path $release ('.stage-'+[guid]::NewGuid().ToString('N'));$bundle=Join-Path $stage $name
New-Item -ItemType Directory -Path $bundle -Force|Out-Null
Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $bundle 'MintDesk.exe')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $bundle 'LICENSE')
$readme=@'
薄荷桌面助手 / MintDesk
Copyright (c) 2026 MCC — MIT License

Windows 10 / 11 x64. Portable: extract this folder to a fixed location and run MintDesk.exe.
首次正常运行自动加入开机启动并注册桌面右键，以后登录 Windows 自动整理。
托盘菜单可关闭这两项；手动关闭后会保留选择。移动 EXE 后请重新注册路径。

桌面项保持原路径，仅临时设置 Hidden/System 可见性标记；资源管理器中也会隐藏。
正常退出恢复原属性，文件夹内容不递归修改。重要资料仍应有独立备份。
受保护桌面项目可能请求 Windows 管理员确认；主界面保持普通权限。

退出请使用托盘或桌面右键。先退出正常实例后，可执行：
MintDesk.exe /restore-desktop
不要在运行中删除 %LOCALAPPDATA% 下的恢复记录。

Source, usage and known limitations:
https://github.com/mayflyfy/mint-desk
'@
[IO.File]::WriteAllText((Join-Path $bundle 'README.txt'),$readme,[Text.UTF8Encoding]::new($true))
$zip=Join-Path $release ($name+'.zip');Compress-Archive -LiteralPath $bundle -DestinationPath $zip -Force
Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $release 'MintDesk.exe') -Force
$sums=@(foreach($path in @((Join-Path $release 'MintDesk.exe'),$zip)){$hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();$hash+'  '+[IO.Path]::GetFileName($path)})
[IO.File]::WriteAllLines((Join-Path $release 'SHA256SUMS.txt'),$sums,[Text.UTF8Encoding]::new($false))
# Remove only the three known staged files and then their empty directories.
$absoluteStage=[IO.Path]::GetFullPath($stage)
if(-not $absoluteStage.StartsWith([IO.Path]::GetFullPath($release)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid staging cleanup path.'}
foreach($file in @('MintDesk.exe','LICENSE','README.txt')){Remove-Item -LiteralPath (Join-Path $bundle $file)}
Remove-Item -LiteralPath $bundle;Remove-Item -LiteralPath $stage
Get-Item -LiteralPath $zip,(Join-Path $release 'MintDesk.exe'),(Join-Path $release 'SHA256SUMS.txt')|Select-Object Name,Length
