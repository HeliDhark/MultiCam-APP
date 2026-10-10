param([switch]$SkipX86,[string]$Target)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
function Invoke-BuildTool([string]$File, [string[]]$ToolArgs) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $File
    $start.UseShellExecute = $false
    $start.Arguments = ($ToolArgs | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' '
    # Some launchers supply both Path and PATH; old MSBuild treats this as an error.
    $start.EnvironmentVariables.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $start.EnvironmentVariables[$entry.Key.ToUpperInvariant()] = $entry.Value
    }
    $process = [System.Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if ($process.ExitCode) { throw "Falha em $File ($($process.ExitCode))." }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
$portable = Join-Path $taskRoot '.buildtools'
$portableCmake = Get-ChildItem -Path (Join-Path $portable 'cmake-*/bin/cmake.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
$usePortable = (-not $vsPath) -and (Test-Path "$portable/msvc/setup_x64.bat") -and $portableCmake -and (Test-Path "$portable/ninja/ninja.exe")
if (-not $vsPath -and -not $usePortable) { throw 'Instale as Build Tools do Visual Studio com C++ e Windows SDK, ou prepare a toolchain portátil em .buildtools (veja o README).' }
function Import-PortableEnvironment([string]$Arch) {
    $setup = Join-Path $portable "msvc/setup_$Arch.bat"
    foreach ($line in (cmd /c "`"$setup`" >nul && set")) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}
if ($usePortable) { $cmake = $portableCmake.FullName; $env:PATH = "$portable/ninja;$env:PATH" }
else {
    $cmake = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
}
$savedPath = $env:PATH; $savedInclude = $env:INCLUDE; $savedLib = $env:LIB
foreach ($arch in @('x64','Win32')) {
    if ($arch -eq 'Win32' -and ($SkipX86 -or $Target)) { continue }
    $dest = Join-Path $taskRoot ('dist/' + $(if ($arch -eq 'Win32') {'x86'} else {'x64'}))
    if ($usePortable) {
        $env:PATH = $savedPath; $env:INCLUDE = $savedInclude; $env:LIB = $savedLib
        Import-PortableEnvironment $(if ($arch -eq 'Win32') {'x86'} else {'x64'})
        $build = Join-Path $taskRoot "build/ninja-$arch"
        Invoke-BuildTool $cmake @('-S',$taskRoot,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl')
        Invoke-BuildTool $cmake (@('--build',$build,'--parallel') + $(if ($Target) { @('--target',$Target) }))
        if ($Target) { exit 0 }
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -LiteralPath "$build/MultiCam.Camera.dll","$build/MultiCam.Worker.exe" -Destination $dest
        if ($arch -eq 'x64' -and (Test-Path "$build/MultiCam.Viewer.exe")) { Copy-Item -LiteralPath "$build/MultiCam.Viewer.exe" -Destination $dest }
        # tests/integration.ps1 looks for the test tool under build/<arch>/Release.
        $tools = Join-Path $taskRoot "build/$arch/Release"
        New-Item -ItemType Directory -Force -Path $tools | Out-Null
        Copy-Item -LiteralPath "$build/MultiCam.Integration.exe" -Destination $tools
        continue
    }
    $build = Join-Path $taskRoot "build/$arch"
    Invoke-BuildTool $cmake @('-S',$taskRoot,'-B',$build,'-A',$arch)
    Invoke-BuildTool $cmake (@('--build',$build,'--config','Release','--parallel') + $(if ($Target) { @('--target',$Target) }))
    if ($Target) { exit 0 }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -LiteralPath "$build/Release/MultiCam.Camera.dll","$build/Release/MultiCam.Worker.exe" -Destination $dest
    if ($arch -eq 'x64' -and (Test-Path "$build/Release/MultiCam.Viewer.exe")) { Copy-Item -LiteralPath "$build/Release/MultiCam.Viewer.exe" -Destination $dest }
}
$env:PATH = $savedPath; $env:INCLUDE = $savedInclude; $env:LIB = $savedLib
$csc = "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
Copy-Item -LiteralPath "$taskRoot/third_party/turbojpeg/x64/turbojpeg.dll" -Destination "$taskRoot/dist/x64/turbojpeg.dll"
$panelSource = [IO.Path]::GetFullPath((Join-Path $taskRoot 'app/MultiCam.cs'))
$panelTarget = [IO.Path]::GetFullPath((Join-Path $taskRoot 'dist/MultiCam.exe'))
$panelIcon = [IO.Path]::GetFullPath((Join-Path $taskRoot 'app/assets/multicam.ico'))
$panelResponse = Join-Path $taskRoot 'build/panel.rsp'
@('/nologo','/target:winexe','/platform:x64','/optimize+','/utf8output','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll',('/win32icon:"'+$panelIcon+'"'),('/out:"'+$panelTarget+'"'),('"'+$panelSource+'"')) | Set-Content $panelResponse
Invoke-BuildTool $csc @('@'+$panelResponse)
Copy-Item -LiteralPath "$taskRoot/README.md" -Destination "$taskRoot/dist/LEIA-ME.md"
Copy-Item -LiteralPath "$taskRoot/third_party/libdshowcapture/COPYING" -Destination "$taskRoot/dist/LICENSE-libdshowcapture.txt"
Copy-Item -LiteralPath "$taskRoot/third_party/turbojpeg/LICENSE.md" -Destination "$taskRoot/dist/LICENSE-turbojpeg.md"
Copy-Item -LiteralPath "$taskRoot/third_party/turbojpeg/README.ijg" -Destination "$taskRoot/dist/README.ijg"
Copy-Item -LiteralPath "$taskRoot/VALIDATION.md" -Destination "$taskRoot/dist/VALIDACAO.md"
Copy-Item -LiteralPath "$taskRoot/THIRD_PARTY.md","$taskRoot/LICENSE" -Destination "$taskRoot/dist"
Write-Host "Compilado: $taskRoot/dist/MultiCam.exe"
