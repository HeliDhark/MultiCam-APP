# Testes da lógica do painel para o Viewer (configurações, presets, processos).
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File tests/panel.ps1   (depois de ./build.ps1)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
[Reflection.Assembly]::LoadFrom((Join-Path $root 'dist/MultiCam.exe')) | Out-Null
$viewerExe = Join-Path $root 'dist/x64/MultiCam.Viewer.exe'
$integrationExe = Join-Path $root 'build/x64/Release/MultiCam.Integration.exe'

$failures = 0
function Pass($name) { Write-Host "PASS $name" }
function Fail($name, $why) { $script:failures++; Write-Host "FAIL $name : $why" }
function Run($name, [scriptblock]$body) {
    try { & $body } catch { Fail $name $_.Exception.Message }
}
function New-Mode($w, $h) { $m = New-Object MultiCam.Mode; $m.Width = $w; $m.Height = $h; $m.Interval = 166667; $m.Format = 301; return $m }
function Sizes($list) { return (@($list | ForEach-Object { "$($_.Width)x$($_.Height)" }) -join ',') }

Run 'old_settings_load' {
    $id = [guid]::NewGuid().ToString()
    $json = '{"AutoStart":false,"Cameras":[{"Identity":"x","Id":"' + $id + '","Alias":"A","Enabled":true}]}'
    $config = [MultiCam.Storage]::Json.Deserialize($json, [MultiCam.Config])
    [MultiCam.ViewerSettings]::Normalize($config)
    $p = $config.Cameras[0].Pip; $w = $config.Wallpaper
    if ($p -and -not $p.Enabled -and $w -and -not $w.Enabled -and $w.Fps -eq 30 -and $w.Fit -eq 'fill') { Pass 'old_settings_load' }
    else { Fail 'old_settings_load' "Pip=$p Wallpaper=$w" }
}

Run 'normalize_values' {
    $id1 = [guid]::NewGuid().ToString(); $id2 = [guid]::NewGuid().ToString()
    $json = '{"AutoStart":false,"Wallpaper":{"Enabled":true,"Fps":45,"Fit":"x"},"Cameras":[' +
        '{"Identity":"a","Id":"' + $id1 + '","Alias":"A","Enabled":true,"Pip":{"Enabled":true,"Width":0,"Height":-5,"X":9999999,"Y":10,"HasPosition":true}},' +
        '{"Identity":"b","Id":"' + $id2 + '","Alias":"B","Enabled":true,"Pip":null}]}'
    $config = [MultiCam.Storage]::Json.Deserialize($json, [MultiCam.Config])
    [MultiCam.ViewerSettings]::Normalize($config)
    $p = $config.Cameras[0].Pip
    $ok = $p.Width -eq 480 -and $p.Height -eq 270 -and -not $p.HasPosition -and $config.Wallpaper.Fps -eq 30 -and
          $config.Wallpaper.Fit -eq 'fill' -and $config.Cameras[1].Pip -ne $null
    if ($ok) { Pass 'normalize_values' } else { Fail 'normalize_values' "pip $($p.Width)x$($p.Height) pos=$($p.HasPosition) fps=$($config.Wallpaper.Fps) fit=$($config.Wallpaper.Fit)" }
}

Run 'normalize_offscreen' {
    $config = New-Object MultiCam.Config
    $c = New-Object MultiCam.Camera; $c.Id = [guid]::NewGuid().ToString(); $c.Alias = 'A'
    $c.Pip.X = -3000; $c.Pip.Y = 50; $c.Pip.Width = 640; $c.Pip.Height = 360; $c.Pip.HasPosition = $true
    $config.Cameras.Add($c)
    [MultiCam.ViewerSettings]::Normalize($config)
    if ($c.Pip.X -eq -3000 -and $c.Pip.Y -eq 50 -and $c.Pip.Width -eq 640 -and $c.Pip.HasPosition) { Pass 'normalize_offscreen' }
    else { Fail 'normalize_offscreen' "X=$($c.Pip.X) Y=$($c.Pip.Y) W=$($c.Pip.Width) pos=$($c.Pip.HasPosition)" }
}

Run 'presets' {
    $a = Sizes ([MultiCam.ViewerSettings]::Presets((New-Mode 1920 1080)))
    $b = Sizes ([MultiCam.ViewerSettings]::Presets((New-Mode 640 480)))
    $c = Sizes ([MultiCam.ViewerSettings]::Presets($null))
    $e169 = '320x180,480x270,640x360,960x540'
    if ($a -eq $e169) { Pass 'presets_16_9' } else { Fail 'presets_16_9' $a }
    if ($b -eq '320x240,480x360,640x480,960x720') { Pass 'presets_4_3' } else { Fail 'presets_4_3' $b }
    if ($c -eq $e169) { Pass 'presets_no_mode' } else { Fail 'presets_no_mode' $c }
}

Run 'viewer_process_lifecycle' {
    $c = New-Object MultiCam.Camera; $c.Id = [guid]::NewGuid().ToString(); $c.Alias = 'Teste'; $c.Mode = New-Mode 640 360
    $c.Pip.Enabled = $true; $c.Pip.Width = 320; $c.Pip.Height = 180; $c.Pip.X = 100; $c.Pip.Y = 100; $c.Pip.HasPosition = $true
    $guid = ([guid]$c.Id).ToString('B').ToUpperInvariant()
    $producer = Start-Process -FilePath $integrationExe -PassThru -WindowStyle Hidden -ArgumentList '--produce-solid', $guid, 640, 360, 166667, 8, 82, 90, 240
    $v = $null
    try {
        $v = [MultiCam.ViewerProcess]::StartPip($viewerExe, $c, $PID)
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (-not $v.Ready -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
        if (-not $v.Ready -or $v.Hwnd -eq 0) { return Fail 'viewer_process_lifecycle' "não ficou Ready (erro: $($v.Error))" }
        $v.Send('lock 1')
        $procId = $v.ProcessId
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $v.Dispose(); $v = $null
        $alive = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if (-not $alive -and $sw.ElapsedMilliseconds -le 3000) { Pass 'viewer_process_lifecycle' } else { Fail 'viewer_process_lifecycle' "processo ainda existe ou demorou $($sw.ElapsedMilliseconds) ms" }
    } finally { if ($v) { $v.Dispose() }; if (-not $producer.HasExited) { $producer.Kill() } }
}

Run 'manager_restart_limit' {
    $fake = Join-Path $env:TEMP "mc-fake-viewer-$([guid]::NewGuid()).cmd"
    Set-Content -Path $fake -Value '@exit /b 1' -Encoding ascii
    $manager = New-Object MultiCam.ViewerManager $fake, $PID
    try {
        $config = New-Object MultiCam.Config
        $c = New-Object MultiCam.Camera; $c.Id = [guid]::NewGuid().ToString(); $c.Alias = 'A'; $c.Mode = New-Mode 1920 1080
        $c.Pip.Enabled = $true; $config.Cameras.Add($c)
        [MultiCam.ViewerSettings]::Normalize($config)
        $ready = [Func[MultiCam.Camera, bool]] { param($x) $true }
        $deadline = [DateTime]::UtcNow.AddSeconds(14)
        while ([DateTime]::UtcNow -lt $deadline -and -not $manager.Status($c).StartsWith('Falhou')) {
            [void]$manager.Reconcile($config, $ready); Start-Sleep -Milliseconds 200
        }
        for ($i = 0; $i -lt 15; $i++) { [void]$manager.Reconcile($config, $ready); Start-Sleep -Milliseconds 200 }
        $status = $manager.Status($c)
        if ($status.StartsWith('Falhou:') -and $manager.Started -eq 3) { Pass 'manager_restart_limit' }
        else { Fail 'manager_restart_limit' "Status='$status' Started=$($manager.Started)" }
    } finally { $manager.Dispose(); Remove-Item $fake -ErrorAction SilentlyContinue }
}

function New-FakeViewer([string[]]$lines) {
    $fake = Join-Path $env:TEMP "mc-fake-viewer-$([guid]::NewGuid()).cmd"
    Set-Content -Path $fake -Value $lines -Encoding ascii
    return $fake
}
function New-WallpaperConfig {
    $config = New-Object MultiCam.Config
    $c = New-Object MultiCam.Camera; $c.Id = [guid]::NewGuid().ToString(); $c.Alias = 'A'; $c.Mode = New-Mode 1920 1080
    $config.Cameras.Add($c)
    [MultiCam.ViewerSettings]::Normalize($config)
    $config.Wallpaper.Enabled = $true; $config.Wallpaper.CameraId = $c.Id; $config.Wallpaper.Monitor = '\\.\DISPLAY1'
    return $config
}
function Run-Manager($manager, $config, [int]$seconds) {
    $ready = [Func[MultiCam.Camera, bool]] { param($x) $true }
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) { [void]$manager.Reconcile($config, $ready); Start-Sleep -Milliseconds 200 }
}

# Monitor ausente (4) ou área de trabalho indisponível (6): o painel espera e tenta de novo, sem desistir.
foreach ($case in @(@{ Code = 4; Text = 'Monitor' }, @{ Code = 6; Text = 'rea de trabalho' })) {
    $name = "manager_waits_on_exit_$($case.Code)"
    Run $name {
        $fake = New-FakeViewer "@exit /b $($case.Code)"
        $manager = New-Object MultiCam.ViewerManager $fake, $PID
        try {
            $config = New-WallpaperConfig
            Run-Manager $manager $config 12
            $status = $manager.WallpaperStatus()
            if (-not $status.StartsWith('Falhou') -and $status.Contains($case.Text) -and $manager.Started -ge 2) { Pass $name }
            else { Fail $name "Status='$status' Started=$($manager.Started)" }
        } finally { $manager.Dispose(); Remove-Item $fake -ErrorAction SilentlyContinue }
    }
}

# Falhas espalhadas: depois de um tempo funcionando, o contador volta a zero.
Run 'manager_failures_reset' {
    $fake = New-FakeViewer '@echo {"Event":"ready","Hwnd":1}', '@ping -n 2 127.0.0.1 >nul', '@exit /b 1'
    $manager = New-Object MultiCam.ViewerManager $fake, $PID
    $manager.HealthyAfter = [TimeSpan]::FromMilliseconds(500)
    try {
        $config = New-WallpaperConfig
        Run-Manager $manager $config 16
        $status = $manager.WallpaperStatus()
        if (-not $status.StartsWith('Falhou') -and $manager.Started -ge 4) { Pass 'manager_failures_reset' }
        else { Fail 'manager_failures_reset' "Status='$status' Started=$($manager.Started)" }
    } finally { $manager.Dispose(); Remove-Item $fake -ErrorAction SilentlyContinue }
}

if ($failures) { Write-Host "FAIL ($failures)"; exit 1 }
Write-Host 'PASS'
exit 0
