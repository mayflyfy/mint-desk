param(
    [string]$Compiler = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    if (-not $Compiler) {
        $compilerCommand = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
        $compilerCandidates = @(
            $(if ($compilerCommand) { $compilerCommand.Source }),
            'C:\TDM-GCC-64\bin\g++.exe',
            'C:\msys64\ucrt64\bin\g++.exe',
            'C:\msys64\mingw64\bin\g++.exe'
        )
        $Compiler = $compilerCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    }
    if (-not $Compiler -or -not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw 'A Windows x64 MinGW-w64 compiler is required. Pass -Compiler C:\path\to\g++.exe.'
    }
    $Compiler = (Resolve-Path -LiteralPath $Compiler).Path
    $compilerDirectory = Split-Path -Parent $Compiler
    $resourceCompiler = Join-Path $compilerDirectory 'windres.exe'
    if (-not (Test-Path -LiteralPath $resourceCompiler -PathType Leaf)) {
        throw "windres.exe was not found beside $Compiler."
    }
    $target = (& $Compiler '-dumpmachine' 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $target -notmatch '^x86_64-.*(mingw|windows)') {
        throw "The compiler must target Windows x64; reported target: $target"
    }

    $sourceFiles = @('src/main.cpp', 'src/state.cpp', 'src/shell_items.cpp', 'src/snap.cpp', 'src/desktop_menu.cpp', 'src/startup_integration.cpp', 'src/dragdrop.cpp', 'src/alpha_surface.cpp', 'src/desktop_items.cpp', 'src/hidden_items.cpp', 'src/glass_backdrop.cpp', 'src/file_helper.cpp') | ForEach-Object { Join-Path $PSScriptRoot $_ }
    foreach ($sourceFile in $sourceFiles) {
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            throw "Missing source file: $sourceFile"
        }
    }
    $buildDirectory = Join-Path $PSScriptRoot 'build'
    New-Item -ItemType Directory -Path $buildDirectory, $OutputDirectory -Force | Out-Null
    $resourceObject = Join-Path $buildDirectory 'app-res.o'
    $executable = Join-Path $OutputDirectory 'MintDesk.exe'

    Write-Host 'Building MintDesk (Windows x64, release)...'
    Push-Location (Join-Path $PSScriptRoot 'src')
    try {
        & $resourceCompiler '--codepage=65001' '-I' (Join-Path $PSScriptRoot 'assets') '-i' 'app.rc' '-o' $resourceObject
        if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    }
    finally { Pop-Location }

    $compilerArguments = @(
        '-std=c++17', '-Os', '-s', '-static', '-static-libgcc', '-static-libstdc++',
        '-municode', '-mwindows', '-fno-ident', '-ffunction-sections', '-fdata-sections',
        '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
        '-Wall', '-Wextra', '-Wpedantic', '-Wl,--gc-sections', '-Wl,--dynamicbase', '-Wl,--nxcompat',
        '-o', $executable
    ) + $sourceFiles + @($resourceObject) + @(
        '-lcomctl32', '-lshell32', '-lole32', '-luuid', '-lshlwapi', '-ladvapi32',
        '-ldwmapi', '-lgdi32', '-luser32', '-lcomdlg32', '-lmsimg32'
    )
    & $Compiler @compilerArguments
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation or linking failed.' }
    $resultFile = Get-Item -LiteralPath $executable
    Write-Host ('Built: {0} ({1:N0} bytes)' -f $resultFile.FullName, $resultFile.Length)
}
catch {
    [Console]::Error.WriteLine('Build failed: ' + $_.Exception.Message)
    exit 1
}
