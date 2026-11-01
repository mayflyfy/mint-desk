param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not $OutputDirectory){$OutputDirectory=Join-Path (Split-Path -Parent $PSScriptRoot) 'build\toolchain\InnoSetup-7.1.1'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
$download=Join-Path $OutputDirectory 'innosetup-7.1.1-x64.exe'
if(-not(Test-Path -LiteralPath $download)){
    Invoke-WebRequest -Uri 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-7.1.1-x64.exe' -OutFile $download
}
if((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne '8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0'){throw 'Inno Setup download checksum mismatch.'}
if((Get-AuthenticodeSignature -LiteralPath $download).Status -ne 'Valid'){throw 'Inno Setup signature validation failed.'}
$compilerDirectory=Join-Path $OutputDirectory 'compiler'
if(-not(Test-Path -LiteralPath (Join-Path $compilerDirectory 'ISCC.exe'))){
    # Portable extraction: no compiler uninstall entry, icons or file association.
    $arguments='/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /PORTABLE=1 /NOICONS /DIR="'+$compilerDirectory+'"'
    $process=Start-Process -FilePath $download -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if($process.ExitCode -ne 0){throw 'Inno Setup extraction failed.'}
}
(Get-Item -LiteralPath (Join-Path $compilerDirectory 'ISCC.exe')).FullName
