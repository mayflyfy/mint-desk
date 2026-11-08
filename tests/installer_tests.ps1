param([string]$Compiler='', [string]$Iscc='')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -Namespace MintDesk.InstallerTests -Name NativePath -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern uint GetLongPathName(string path, System.Text.StringBuilder result, uint length);
'@
function NormalizeExistingPath([string]$Path) {
    $buffer=[Text.StringBuilder]::new(32768)
    $length=[MintDesk.InstallerTests.NativePath]::GetLongPathName($Path,$buffer,[uint32]$buffer.Capacity)
    if($length -gt 0 -and $length -lt $buffer.Capacity){return $buffer.ToString()}
    return $Path
}
$projectDirectory=Split-Path -Parent $PSScriptRoot
if(-not $Compiler){
    $compilerCommand=Get-Command g++.exe -ErrorAction SilentlyContinue
    $Compiler=@($(if($compilerCommand){$compilerCommand.Source}),'C:\TDM-GCC-64\bin\g++.exe',
        'C:\msys64\ucrt64\bin\g++.exe','C:\msys64\mingw64\bin\g++.exe')|
        Where-Object{$_ -and (Test-Path -LiteralPath $_)}|Select-Object -First 1
}
if(-not $Compiler){throw 'Pass -Compiler C:\path\to\g++.exe.'}
$output=Join-Path $projectDirectory 'build\installer-tests'
New-Item -ItemType Directory -Path $output -Force|Out-Null
$resource=Join-Path $output 'fixture-res.o'
$windres=Join-Path (Split-Path -Parent $Compiler) 'windres.exe'
Push-Location (Join-Path $projectDirectory 'src')
try {& $windres '--codepage=65001' '-I' (Join-Path $projectDirectory 'assets') '-i' 'app.rc' '-o' $resource
    if($LASTEXITCODE -ne 0){throw 'Fixture resource build failed.'}}
finally{Pop-Location}
$payload=Join-Path $output 'fixture.exe'
$sources=@('tests\installer_fixture.cpp','src\startup_integration.cpp','src\desktop_menu.cpp','src\state.cpp')|
    ForEach-Object{Join-Path $projectDirectory $_}
& $Compiler '-std=c++17' '-Os' '-s' '-static' '-static-libgcc' '-static-libstdc++' '-municode' '-mwindows' `
    '-DUNICODE' '-D_UNICODE' '-DNOMINMAX' '-DWINVER=0x0A00' '-D_WIN32_WINNT=0x0A00' '-Wall' '-Wextra' `
    '-o' $payload @sources $resource '-lshell32' '-lole32' '-luuid' '-ladvapi32' '-luser32' '-lgdi32' '-lshlwapi'
if($LASTEXITCODE -ne 0){throw 'Fixture payload build failed.'}
& (Join-Path $projectDirectory 'tools\build-installer.ps1') -Iscc $Iscc -Executable $payload -OutputDirectory $output -TestMode
if($LASTEXITCODE -ne 0){throw 'Fixture installer build failed.'}
$version=[regex]::Match([IO.File]::ReadAllText((Join-Path $projectDirectory 'CMakeLists.txt')),'project\(MintDesk VERSION ([0-9.]+)').Groups[1].Value
$setup=Join-Path $output "MintDesk-v$version-setup-x64.exe"
$fixtureKey='HKCU:\Software\MCC\MintDeskInstallerTests'
if(Test-Path -LiteralPath $fixtureKey){throw 'An earlier isolated installer fixture exists; inspect it before retrying.'}
$fixtureRoot=Join-Path ([IO.Path]::GetTempPath()) ('MintDesk-installer-test-'+[guid]::NewGuid().ToString('N'))
$desktop=Join-Path $fixtureRoot 'Desktop';$appDirectory=Join-Path $fixtureRoot 'Installed'
$legacyData=Join-Path $fixtureRoot 'Data\DeskCards'
New-Item -ItemType Directory -Path $desktop,$legacyData -Force|Out-Null
New-Item -Path $fixtureKey -Force|Out-Null
New-ItemProperty -LiteralPath $fixtureKey -Name FixtureDesktop -Value $desktop -PropertyType String|Out-Null
$keepFile=Join-Path $desktop 'Keep Me.txt';$layout=Join-Path $legacyData 'state.dat';$journal=Join-Path $legacyData 'hidden-items.dat'
foreach($file in @($keepFile,$layout,$journal)){[IO.File]::WriteAllText($file,'Fictional installer fixture '+[guid]::NewGuid().ToString('N'))}
$beforeHashes=@{};foreach($file in @($keepFile,$layout,$journal)){$beforeHashes[$file]=(Get-FileHash -LiteralPath $file).Hash}
$shortcut=Join-Path $desktop '薄荷桌面助手.lnk'
$logNumber=0
function Check([bool]$Value,[string]$Message){if(-not $Value){throw $Message}}
function RunSetup([string]$Tasks='', [bool]$Explicit=$false, [string]$Directory=$appDirectory) {
    $script:logNumber++
    $arguments='/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LANG=english /DIR="'+$Directory+'" /LOG="'+(Join-Path $fixtureRoot ("setup-$script:logNumber.log"))+'"'
    if($Explicit){$arguments+=' /TASKS="'+$Tasks+'"'}
    $process=Start-Process -FilePath $setup -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
    return $process.ExitCode
}
function ReadPreference([string]$Name){return Get-ItemPropertyValue -LiteralPath $fixtureKey -Name $Name}
function CheckChoices([bool]$DesktopIcon,[bool]$Autostart,[bool]$Update,[bool]$Menu) {
    Check ((ReadPreference 'DesktopShortcutEnabled') -eq [int]$DesktopIcon) 'Desktop shortcut preference mismatch.'
    Check ((Test-Path -LiteralPath $shortcut) -eq $DesktopIcon) 'Desktop shortcut existence mismatch.'
    $runValues=Get-ItemProperty -LiteralPath ($fixtureKey+'\Run')
    Check (($null -ne $runValues.PSObject.Properties['MintDesk']) -eq $Autostart) 'Autostart preference mismatch.'
    if($Autostart){Check ($runValues.MintDesk -eq ('"'+(Join-Path $appDirectory 'MintDesk.exe')+'" /background')) 'Autostart path was not refreshed.'}
    Check ((ReadPreference 'AutoCheckUpdates') -eq [int]$Update) 'Reserved update preference mismatch.'
    Check ((Test-Path -LiteralPath ($fixtureKey+'\Shell\MintDesk')) -eq $Menu) 'Desktop menu preference mismatch.'
    if($DesktopIcon){$shell=New-Object -ComObject WScript.Shell;$link=$shell.CreateShortcut($shortcut)
        $actualTarget=$link.TargetPath
        $expectedTarget=Join-Path $appDirectory 'MintDesk.exe'
        # Windows Script Host can expand the runner's RUNNER~1 temporary path
        # to its long form. Both names must still identify the installed file.
        Check ((NormalizeExistingPath $actualTarget) -eq (NormalizeExistingPath $expectedTarget)) `
            ('Shortcut did not point at installed executable. Actual: '+$actualTarget+'; expected: '+$expectedTarget)}
}
function Uninstall {
    $uninstaller=Join-Path $appDirectory 'Uninstall\unins000.exe'
    $process=Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}

Check ((RunSetup) -eq 0) 'Fresh installation failed.'
CheckChoices $true $true $false $true
Check ((ReadPreference 'InstallDirectory') -eq $appDirectory) 'Selected installation directory was not retained.'
Check ((RunSetup 'autoupdate' $true) -eq 0) 'Changing installer choices failed.'
CheckChoices $false $false $true $true
Check ((RunSetup) -eq 0) 'Upgrade with saved preferences failed.'
CheckChoices $false $false $true $true
Check ((RunSetup '' $true) -eq 0) 'Explicitly disabling every option failed.'
CheckChoices $false $false $false $true

$installedHash=(Get-FileHash -LiteralPath (Join-Path $appDirectory 'MintDesk.exe')).Hash
New-ItemProperty -LiteralPath $fixtureKey -Name PrepareResult -PropertyType DWord -Value 3 -Force|Out-Null
Check ((RunSetup 'desktopicon,autostart' $true) -ne 0) 'Setup must reject unfinished desktop restoration.'
CheckChoices $false $false $false $true
Check ((Get-FileHash -LiteralPath (Join-Path $appDirectory 'MintDesk.exe')).Hash -eq $installedHash) 'Rejected upgrade changed executable.'
Check ((Uninstall) -ne 0) 'Uninstall must stop when restoration is blocked.'
Check (Test-Path -LiteralPath (Join-Path $appDirectory 'MintDesk.exe')) 'Blocked uninstall removed the executable.'
Set-ItemProperty -LiteralPath $fixtureKey -Name PrepareResult -Value 0
Check ((RunSetup '' $true (Join-Path $fixtureRoot 'Different')) -ne 0) 'Installed upgrade must not leave an obsolete uninstaller in another directory.'
Check ((Uninstall) -eq 0) 'Uninstall failed.'
Check (-not(Test-Path -LiteralPath (Join-Path $appDirectory 'MintDesk.exe'))) 'Uninstaller left installed program behind.'
Check (-not(Test-Path -LiteralPath ($fixtureKey+'\Shell\MintDesk'))) 'Uninstaller left a menu registration behind.'

# Simulate v0.8.x portable preferences: completed defaults, no new installer
# preference values, disabled startup/menu, and an existing native shortcut.
foreach($name in @('DesktopShortcutEnabled','AutoCheckUpdates')){Remove-ItemProperty -LiteralPath $fixtureKey -Name $name}
$shell=New-Object -ComObject WScript.Shell;$link=$shell.CreateShortcut($shortcut)
$link.TargetPath=$payload;$link.Arguments='/show';$link.Description='薄荷桌面助手 / MintDesk';$link.Save()
Check ((RunSetup) -eq 0) 'Legacy portable-to-installed upgrade failed.'
CheckChoices $true $false $false $false
Check ((Uninstall) -eq 0) 'Legacy upgrade uninstall failed.'
Check (-not(Test-Path -LiteralPath $shortcut)) 'Uninstall did not remove the owned shortcut.'
foreach($file in $beforeHashes.Keys){Check ((Get-FileHash -LiteralPath $file).Hash -eq $beforeHashes[$file]) 'Installation or uninstall touched fictional user data.'}

# Cleanup is restricted to this uniquely named temporary fixture and test key.
$resolvedFixture=[IO.Path]::GetFullPath($fixtureRoot)
$temporaryPrefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\MintDesk-installer-test-'
if(-not $resolvedFixture.StartsWith($temporaryPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid fixture cleanup target.'}
Remove-Item -LiteralPath $fixtureKey -Recurse
Remove-Item -LiteralPath $resolvedFixture -Recurse
$report=[pscustomobject]@{Passed=$true;Version=$version;FreshInstallDefaults=$true;ExplicitOptOuts=$true;SavedPreferencesOnUpgrade=$true;LegacyPortableUpgrade=$true;RestoreFailureBlocksUpgradeAndUninstall=$true;UninstallPreservesUserData=$true;Scope='Isolated registry, desktop links, directories and test-only payload'}
$report|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'verification.json') -Encoding UTF8
$report|ConvertTo-Json
