param([switch]$IncludeArtifacts, [string[]]$PrivateTerms=@())
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$extensions=@('.cpp','.h','.rc','.manifest','.ps1','.md','.txt','.yml','.yaml','.ico','.png','.gitattributes','.gitignore')
$files=@()
if(Test-Path -LiteralPath (Join-Path $root '.git')){
    $tracked=@(git -C $root ls-files --cached)
    if($LASTEXITCODE -ne 0){throw 'Cannot enumerate Git index.'}
    foreach($name in $tracked){
        if($name -match '(^|/)(build[^/]*|dist|release|\.private)/' -or $name -match '(^|/)\.env[^/]*(/|$)' -or $name -match '\.(exe|dll|pdb|dat|log|dmp|zip|7z)$'){throw "Private/generated file staged: $name"}
        $path=Join-Path $root $name
        $working=git -C $root hash-object -- $path;$indexed=git -C $root rev-parse (':'+$name)
        if($LASTEXITCODE -ne 0 -or $working -ne $indexed){throw "Stage current content before auditing: $name"}
        $files+=Get-Item -LiteralPath $path
    }
}else{
    foreach($folder in @('src','assets','tests','tools','docs','.github')){$files+=@(Get-ChildItem -LiteralPath (Join-Path $root $folder) -Recurse -File)}
    foreach($name in @('README.md','LICENSE','CHANGELOG.md','CONTRIBUTING.md','CMakeLists.txt','build.ps1','.gitignore','.gitattributes')){$files+=Get-Item -LiteralPath (Join-Path $root $name)}
}
$prefixes=@($root,$env:USERPROFILE)|Where-Object{$_}
$needles=@(foreach($prefix in $prefixes){$prefix;$prefix.Replace('\','/');$prefix.Replace('\','\\')})+$PrivateTerms
$rules=@('-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----','\bgh[pousr]_[A-Za-z0-9]{30,}\b','\bgithub_pat_[A-Za-z0-9_]{30,}\b','\bAKIA[0-9A-Z]{16}\b','\bsk-[A-Za-z0-9_-]{35,}\b')
function CheckText([string]$text,[string]$label){
    foreach($needle in $needles){if($needle -and $text.IndexOf($needle,[StringComparison]::OrdinalIgnoreCase) -ge 0){throw "Private local identifier/path found in: $label"}}
    foreach($rule in $rules){if([regex]::IsMatch($text,$rule)){throw "Credential-like content found in: $label"}}
}
foreach($file in $files){
    $relative=$file.FullName.Substring($root.Length+1).Replace('\','/')
    if($file.Name -notin @('LICENSE','CMakeLists.txt','.gitignore','.gitattributes') -and $file.Extension -notin $extensions){throw "Unreviewed file type: $relative"}
    if($file.Extension -eq '.png'){
        if($relative -notin @('docs/screenshots/overview.png','docs/screenshots/reorder.png')){throw "Unreviewed image: $relative"}
        $bytes=[IO.File]::ReadAllBytes($file.FullName);$offset=8
        while($offset+12 -le $bytes.Length){$length=[int]($bytes[$offset]*16777216+$bytes[$offset+1]*65536+$bytes[$offset+2]*256+$bytes[$offset+3]);$type=[Text.Encoding]::ASCII.GetString($bytes,$offset+4,4)
            if($type -in @('tEXt','zTXt','iTXt','eXIf')){throw "Image metadata requires review: $relative"}
            if($length -lt 0 -or $offset+$length+12 -gt $bytes.Length){throw "Invalid PNG: $relative"};$offset+=$length+12}
    }elseif($file.Extension -ne '.ico'){CheckText ([IO.File]::ReadAllText($file.FullName)) $relative}
}
if($IncludeArtifacts){
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $release=Join-Path $root 'release';$exe=Join-Path $release 'MintDesk.exe'
    $versionMatch=[regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),'project\(MintDesk VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
    if(-not $versionMatch.Success){throw 'Cannot determine the current release version.'}
    $version=$versionMatch.Groups[1].Value
    if((Get-Item -LiteralPath $exe).VersionInfo.FileVersion -ne $version){throw 'Standalone executable version does not match this release.'}
    $bytes=[IO.File]::ReadAllBytes($exe);CheckText ([Text.Encoding]::ASCII.GetString($bytes)) 'release executable';CheckText ([Text.Encoding]::Unicode.GetString($bytes)) 'release executable'
    foreach($zip in @(Get-Item -LiteralPath (Join-Path $release ("MintDesk-v$version-windows-x64.zip")))){
        $archive=[IO.Compression.ZipFile]::OpenRead($zip.FullName)
        try{$entries=@($archive.Entries|Where-Object Name);$expected=@('MintDesk.exe','LICENSE','README.txt')
            if($entries.Count -ne 3){throw 'Portable ZIP must contain exactly three files.'}
            foreach($entry in $entries){if($entry.Name -notin $expected -or $entry.FullName -match '\.\.[/\\]'){throw 'Unexpected ZIP entry.'}
                if($entry.Name -ne 'MintDesk.exe'){$stream=$entry.Open();$reader=[IO.StreamReader]::new($stream);try{CheckText ($reader.ReadToEnd()) ('ZIP '+$entry.Name)}finally{$reader.Dispose();$stream.Dispose()}}
                else{$stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create();try{$actual=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','');if($actual -ne (Get-FileHash -LiteralPath $exe).Hash){throw 'ZIP executable differs from standalone release.'}}finally{$sha.Dispose();$stream.Dispose()}}
            }
        }finally{$archive.Dispose()}
    }
}
[pscustomobject]@{Passed=$true;SourceFilesReviewed=$files.Count;ArtifactsChecked=[bool]$IncludeArtifacts;ImagePolicy='Fictional renderer previews only; no PNG text/EXIF metadata'}|ConvertTo-Json
