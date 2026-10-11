# Ritmo da câmera virtual com uma fonte parecida com uma placa real: relógio 0,1% mais lento
# (59,94 em vez de 60) e cada quadro chegando de 0 a 4 ms atrasado.
# Os apps devem receber cada quadro novo uma vez (60 FPS) ou um sim, um não (30 FPS):
# sem repetir quadro e sem pular quadro da fonte.
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File tests/pacing.ps1   (depois de ./build.ps1)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
[Reflection.Assembly]::LoadFrom((Join-Path $root 'dist/MultiCam.exe')) | Out-Null
$camera = New-Object MultiCam.Camera
$camera.Id = [Guid]::NewGuid().ToString(); $camera.Alias = 'MultiCam teste de ritmo'
$camera.Mode = New-Object MultiCam.Mode
$camera.Mode.Width = 1280; $camera.Mode.Height = 720; $camera.Mode.Interval = 166667; $camera.Mode.Format = 301
$id = ([Guid]$camera.Id).ToString('B').ToUpperInvariant()
$x64 = Join-Path $root 'build/x64/Release/MultiCam.Integration.exe'
$x86 = Join-Path $root 'build/Win32/Release/MultiCam.Integration.exe'
$out = Join-Path $env:TEMP "mc-pacing-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force $out | Out-Null
$children = @(); $failures = 0
try {
    [MultiCam.Registration]::Publish($camera)
    $producer = Start-Process $x64 -ArgumentList '--produce-real', $id, 1280, 720, 166667, 14 -PassThru -WindowStyle Hidden
    $children += $producer
    Start-Sleep -Milliseconds 500
    $cases = @(
        @{ Name = 'x64_60fps'; Exe = $x64; Interval = 166667 },
        @{ Name = 'x64_30fps'; Exe = $x64; Interval = 333333 },
        @{ Name = 'x86_60fps'; Exe = $x86; Interval = 166667 }
    )
    foreach ($c in $cases) {
        $c.Process = Start-Process $c.Exe -ArgumentList '--consume', $id, $c.Interval, 10 -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $out "$($c.Name).json") -RedirectStandardError (Join-Path $out "$($c.Name).err")
        $null = $c.Process.Handle
        $children += $c.Process
    }
    foreach ($c in $cases) {
        if (-not $c.Process.WaitForExit(20000)) { throw "$($c.Name): consumidor não terminou" }
        $r = Get-Content (Join-Path $out "$($c.Name).json") -Raw | ConvertFrom-Json
        $bad = $r.Repeats + $r.Irregular
        $limit = [Math]::Max(2, [int]($r.Samples * 0.01))
        $line = "$($c.Name): $($r.Samples) quadros, $($r.Repeats) repetidos, $($r.Irregular) fora de ordem/pulos"
        if ($r.Samples -gt 0 -and $bad -le $limit) { Write-Host "PASS $line" } else { $failures++; Write-Host "FAIL $line (limite $limit)" }
    }
    # Fonte parada (worker reiniciando): o app continua recebendo na taxa pedida, com o
    # último quadro repetido e, depois de 500 ms, preto.
    [void]$producer.WaitForExit(10000)
    $producer = Start-Process $x64 -ArgumentList '--produce-real', $id, 1280, 720, 166667, 2 -PassThru -WindowStyle Hidden
    $children += $producer
    Start-Sleep -Milliseconds 300
    $stall = Start-Process $x64 -ArgumentList '--consume', $id, 166667, 5 -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $out 'stall.json') -RedirectStandardError (Join-Path $out 'stall.err')
    $null = $stall.Handle; $children += $stall
    if (-not $stall.WaitForExit(15000)) { throw 'stall: consumidor não terminou' }
    $r = Get-Content (Join-Path $out 'stall.json') -Raw | ConvertFrom-Json
    $total = $r.Samples + $r.Black  # o consumidor conta os pretos à parte
    $line = "source_stops: $total quadros em 5 s, $($r.Black) pretos"
    if ($total -ge 280 -and $r.Black -gt 0) { Write-Host "PASS $line" } else { $failures++; Write-Host "FAIL $line (esperado >= 280 e pretos > 0)" }
} finally {
    foreach ($p in $children) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
    [MultiCam.Registration]::Remove($camera, $true)
    Remove-Item $out -Recurse -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "FAIL ($failures)"; exit 1 }
Write-Host 'PASS'
exit 0
