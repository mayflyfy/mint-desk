param(
    [string]$Compiler = '',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    $projectDirectory = Split-Path -Parent $PSScriptRoot
    if (-not $Compiler) {
        $compilerCommand = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
        $compilerCandidates = @(
            $(if ($compilerCommand) { $compilerCommand.Source }),
            'C:\TDM-GCC-64\bin\g++.exe',
            'C:\msys64\ucrt64\bin\g++.exe',
            'C:\msys64\mingw64\bin\g++.exe'
        )
        $Compiler = $compilerCandidates | Where-Object {
            $_ -and (Test-Path -LiteralPath $_ -PathType Leaf)
        } | Select-Object -First 1
    }
    if (-not $Compiler -or -not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw 'Pass -Compiler C:\path\to\g++.exe for a Windows x64 MinGW-w64 compiler.'
    }
    $Compiler = (Resolve-Path -LiteralPath $Compiler).Path
    $target = (& $Compiler '-dumpmachine' 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $target -notmatch '^x86_64-.*(mingw|windows)') {
        throw "The compiler must target Windows x64; reported target: $target"
    }
    $outputDirectory = Join-Path $projectDirectory 'build\tests'
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $commonArguments = @(
        '-std=c++17', '-O2', '-static', '-static-libgcc', '-static-libstdc++',
        '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
        '-Wall', '-Wextra', '-Wpedantic', '-Wl,--dynamicbase', '-Wl,--nxcompat'
    )
    $libraries = @(
        '-lcomctl32', '-lshell32', '-lole32', '-luuid', '-lshlwapi', '-ladvapi32',
        '-ldwmapi', '-lgdi32', '-luser32', '-lcomdlg32', '-lgdiplus', '-lmsimg32'
    )
    $cases = @(
        @{ Name = 'state_tests'; Sources = @('tests\state_tests.cpp', 'src\state.cpp'); Extra = @() },
        @{ Name = 'shell_tests'; Sources = @('tests\shell_tests.cpp', 'src\shell_items.cpp'); Extra = @() },
        @{ Name = 'snap_tests'; Sources = @('tests\snap_tests.cpp', 'src\snap.cpp'); Extra = @() },
        @{ Name = 'desktop_menu_tests'; Sources = @('tests\desktop_menu_tests.cpp', 'src\desktop_menu.cpp', 'src\state.cpp'); Extra = @() },
        @{ Name = 'startup_integration_tests'; Sources = @('tests\startup_integration_tests.cpp', 'src\startup_integration.cpp', 'src\desktop_menu.cpp', 'src\state.cpp'); Extra = @() },
        @{ Name = 'alpha_surface_tests'; Sources = @('tests\alpha_surface_tests.cpp', 'src\alpha_surface.cpp'); Extra = @() },
        @{ Name = 'desktop_items_tests'; Sources = @('tests\desktop_items_tests.cpp', 'src\desktop_items.cpp', 'src\hidden_items.cpp', 'src\state.cpp', 'src\file_helper.cpp'); Extra = @() },
        @{ Name = 'hidden_items_tests'; Sources = @('tests\hidden_items_tests.cpp', 'src\hidden_items.cpp', 'src\state.cpp', 'src\file_helper.cpp'); Extra = @() },
        @{ Name = 'file_helper_tests'; Sources = @('tests\file_helper_tests.cpp', 'src\file_helper.cpp', 'src\state.cpp'); Extra = @() },
        @{ Name = 'dragdrop_tests'; Sources = @('tests\dragdrop_tests.cpp', 'src\dragdrop.cpp', 'src\shell_items.cpp'); Extra = @() },
        @{ Name = 'ui_tests'; Sources = @('tests\ui_tests.cpp', 'src\state.cpp', 'src\shell_items.cpp', 'src\snap.cpp', 'src\desktop_menu.cpp', 'src\startup_integration.cpp', 'src\dragdrop.cpp', 'src\alpha_surface.cpp', 'src\desktop_items.cpp', 'src\hidden_items.cpp', 'src\glass_backdrop.cpp', 'src\file_helper.cpp'); Extra = @('-municode') }
    )
    foreach ($case in $cases) {
        $executable = Join-Path $outputDirectory ($case.Name + '.exe')
        $sources = @($case.Sources | ForEach-Object { Join-Path $projectDirectory $_ })
        Write-Host ('Building ' + $case.Name + '...')
        $arguments = $commonArguments + @($case.Extra) + @('-o', $executable) + $sources + $libraries
        & $Compiler @arguments
        if ($LASTEXITCODE -ne 0) { throw ($case.Name + ' compilation failed.') }
    }
    if (-not $BuildOnly) {
        foreach ($case in $cases) {
            $executable = Join-Path $outputDirectory ($case.Name + '.exe')
            Write-Host ('Running ' + $case.Name + '...')
            & $executable
            if ($LASTEXITCODE -ne 0) { throw ($case.Name + ' failed.') }
        }
        Write-Host 'All native tests passed.'
    }
}
catch {
    [Console]::Error.WriteLine('Tests failed: ' + $_.Exception.Message)
    exit 1
}
