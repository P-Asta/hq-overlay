param(
    [ValidateSet('All', 'Debug', 'Release')]
    [string]$Configuration = 'All',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$stageRoot = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installation) {
    # Some VS Community layouts contain a working MSBuild but do not expose
    # the component registration queried above. Resolve the latest instance
    # and validate the executable path below before failing.
    $installation = & $vswhere -latest -products '*' -property installationPath
}
if (-not $installation) {
    throw 'Visual Studio 2022 with MSBuild was not found.'
}
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild was not found at $msbuild"
}

$configurations = if ($Configuration -eq 'All') { @('Release', 'Debug') } else { @($Configuration) }
foreach ($item in $configurations) {
    & $msbuild (Join-Path $stageRoot 'hq-overlay.sln') /m /nr:false /nologo /verbosity:minimal "/p:Configuration=$item" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "$item x64 build failed with exit code $LASTEXITCODE"
    }
    if (-not $SkipTests) {
        $testExecutable = Join-Path $stageRoot "bin\x64\$item\config_parser_tests.exe"
        & $testExecutable
        if ($LASTEXITCODE -ne 0) {
            throw "$item config parser tests failed with exit code $LASTEXITCODE"
        }
    }
    if ($item -eq 'Release') {
        $distribution = Join-Path $stageRoot 'dist\x64\Release'
        New-Item -ItemType Directory -Path $distribution -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $stageRoot 'bin\x64\Release\hq_overlay.dll') -Destination $distribution -Force
        Copy-Item -LiteralPath (Join-Path $stageRoot 'bin\x64\Release\hq_injector.exe') -Destination $distribution -Force
        Copy-Item -LiteralPath (Join-Path $stageRoot 'THIRD_PARTY_NOTICES.md') -Destination $distribution -Force
        $licenseDistribution = Join-Path $distribution 'licenses'
        New-Item -ItemType Directory -Path $licenseDistribution -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $stageRoot 'third_party\imgui\LICENSE.txt') -Destination (Join-Path $licenseDistribution 'imgui-LICENSE.txt') -Force
        Copy-Item -LiteralPath (Join-Path $stageRoot 'third_party\minhook\LICENSE.txt') -Destination (Join-Path $licenseDistribution 'minhook-LICENSE.txt') -Force
        Copy-Item -LiteralPath (Join-Path $stageRoot 'third_party\webview2\LICENSE.txt') -Destination (Join-Path $licenseDistribution 'webview2-LICENSE.txt') -Force
        Copy-Item -LiteralPath (Join-Path $stageRoot 'third_party\webview2\NOTICE.txt') -Destination (Join-Path $licenseDistribution 'webview2-NOTICE.txt') -Force
    }
}
