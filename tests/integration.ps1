param([int]$Width=1920,[int]$Height=1080,[long]$Interval=166667)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
[Reflection.Assembly]::LoadFrom((Join-Path $taskRoot 'dist/MultiCam.exe')) | Out-Null
$camera=New-Object MultiCam.Camera
$camera.Id=[Guid]::NewGuid().ToString()
$camera.Alias='MultiCam teste temporario'
$camera.Mode=New-Object MultiCam.Mode
$camera.Mode.Width=$Width
$camera.Mode.Height=$Height
$camera.Mode.Interval=$Interval
$camera.Mode.Format=301
$testId=([Guid]$camera.Id).ToString('B').ToUpperInvariant()
$children=@()
$outDir=Join-Path $taskRoot "artifacts/test-$Width-$Height-$Interval"
New-Item -ItemType Directory -Force $outDir | Out-Null
try {
    [MultiCam.Registration]::Publish($camera)
    $listed=& (Join-Path $taskRoot 'dist/x64/MultiCam.Worker.exe') --list-all | ConvertFrom-Json
    if(-not ($listed | Where-Object Name -eq ($camera.Alias+' (Multi)'))) {throw 'Câmera virtual não enumerada.'}
    $producer=Start-Process -FilePath (Join-Path $taskRoot 'build/x64/Release/MultiCam.Integration.exe') -ArgumentList @('--produce',$testId,$Width,$Height,$Interval,9) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$outDir/producer.json" -RedirectStandardError "$outDir/producer.err"
    $children+=$producer
    $null=$producer.Handle
    Start-Sleep -Milliseconds 400
    $a=Start-Process -FilePath (Join-Path $taskRoot 'build/x64/Release/MultiCam.Integration.exe') -ArgumentList @('--consume',$testId,$Interval,5) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$outDir/x64.json" -RedirectStandardError "$outDir/x64.err"
    $b=Start-Process -FilePath (Join-Path $taskRoot 'build/Win32/Release/MultiCam.Integration.exe') -ArgumentList @('--consume',$testId,333333,5) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$outDir/x86.json" -RedirectStandardError "$outDir/x86.err"
    $children+=@($a,$b)
    $null=$a.Handle
    $null=$b.Handle
    foreach($p in @($a,$b,$producer)) {
        if(-not $p.WaitForExit(15000)) {throw 'Timeout no teste de captura/parada.'}
        if($p.ExitCode -ne 0) {throw "Teste retornou $($p.ExitCode). Veja $outDir"}
    }
    Get-Content "$outDir/producer.json","$outDir/x64.json","$outDir/x86.json"
} finally {
    foreach($p in $children) {if(-not $p.HasExited){Stop-Process -Id $p.Id -Force}}
    [MultiCam.Registration]::Remove($camera,$true)
}
$listed=& (Join-Path $taskRoot 'dist/x64/MultiCam.Worker.exe') --list-all | ConvertFrom-Json
if($listed | Where-Object Name -eq ($camera.Alias+' (Multi)')){throw 'Câmera de teste não removida.'}
Write-Host 'PASS: enumeração, clientes x64 e x86 simultâneos, 60/30 FPS, integridade, timestamps e remoção.'
