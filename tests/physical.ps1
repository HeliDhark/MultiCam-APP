param([string]$DeviceName='USB3 Video')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
[Reflection.Assembly]::LoadFrom((Join-Path $taskRoot 'dist/MultiCam.exe')) | Out-Null
$worker=Join-Path $taskRoot 'dist/x64/MultiCam.Worker.exe'
$devices=& $worker --list | ConvertFrom-Json
$device=$devices | Where-Object Name -eq $DeviceName | Select-Object -First 1
if(-not $device) {throw "Dispositivo ausente: $DeviceName"}
$available=& $worker --modes $device.Path | ConvertFrom-Json
$mode=$available | Where-Object { $_.Width -eq 1920 -and $_.Height -eq 1080 } | Sort-Object Interval,@{Expression={if($_.Format -eq 301){0}else{1}}} | Select-Object -First 1
if(-not $mode){throw 'Full HD ausente'}
$camera=New-Object MultiCam.Camera
$camera.Id=[Guid]::NewGuid().ToString()
$camera.Alias='MultiCam teste fisico temporario'
$camera.Mode=New-Object MultiCam.Mode
$camera.Mode.Width=$mode.Width;$camera.Mode.Height=$mode.Height;$camera.Mode.Interval=$mode.Interval;$camera.Mode.Format=$mode.Format
$testId=([Guid]$camera.Id).ToString('B').ToUpperInvariant()
$outDir=Join-Path $taskRoot ('artifacts/physical-'+$DeviceName.Replace(' ','-'))
New-Item -ItemType Directory -Force $outDir | Out-Null
$children=@();$capture=$null
try {
    $arguments=@('--capture',$device.Path,$testId,$mode.Width,$mode.Height,$mode.Interval,$mode.Format,$PID,$device.Name) | ForEach-Object { [MultiCam.Native]::Quote([string]$_) }
    $capture=Start-Process -FilePath $worker -ArgumentList ($arguments -join ' ') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$outDir/capture.jsonl" -RedirectStandardError "$outDir/capture.err"
    $null=$capture.Handle;$children+=$capture
    Start-Sleep -Seconds 2
    if($capture.HasExited){throw 'Falha na captura real. Veja capture.err.'}
    [MultiCam.Registration]::Publish($camera)
    foreach($arch in @('x64','Win32')) {
        $p=Start-Process -FilePath (Join-Path $taskRoot "build/$arch/Release/MultiCam.Integration.exe") -ArgumentList @('--observe',$testId,$mode.Interval,5) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$outDir/$arch.json" -RedirectStandardError "$outDir/$arch.err"
        $null=$p.Handle;$children+=$p
    }
    foreach($p in $children | Select-Object -Skip 1) {if(-not $p.WaitForExit(12000) -or $p.ExitCode -ne 0){throw 'Falha no consumidor. Veja os artefatos.'}}
    Get-Content "$outDir/capture.jsonl","$outDir/x64.json","$outDir/Win32.json"
} finally {
    try {$stop=[Threading.EventWaitHandle]::OpenExisting("Local\MultiCam-$testId-stop");$stop.Set()|Out-Null;$stop.Dispose()}catch{}
    foreach($p in $children) {if(-not $p.HasExited -and -not $p.WaitForExit(3000)){Stop-Process -Id $p.Id -Force}}
    [MultiCam.Registration]::Remove($camera,$true)
}
Write-Host "PASS: $DeviceName -> dois consumidores simultâneos. A taxa real da captura está em capture.jsonl."
