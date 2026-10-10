# Testes do MultiCam.Viewer.exe (PiP e papel de parede). Precisa de uma sessão de área de trabalho interativa.
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native.ps1')
$viewerExe = Build-NativeTarget MultiCam.Viewer
$integrationExe = Build-NativeTarget MultiCam.Integration

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
public class VProc {
    public Process P;
    public List<string> All = new List<string>();
    public System.Text.StringBuilder Err = new System.Text.StringBuilder();
    public static VProc Start(string exe, string args) {
        var v = new VProc();
        var psi = new ProcessStartInfo(exe, args);
        psi.UseShellExecute = false; psi.CreateNoWindow = true;
        psi.RedirectStandardInput = true; psi.RedirectStandardOutput = true; psi.RedirectStandardError = true;
        v.P = new Process(); v.P.StartInfo = psi;
        v.P.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (v.All) v.All.Add(e.Data); };
        v.P.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (v.Err) v.Err.AppendLine(e.Data); };
        v.P.Start(); v.P.BeginOutputReadLine(); v.P.BeginErrorReadLine();
        v.P.StandardInput.AutoFlush = true;
        return v;
    }
    public string[] Lines() { lock (All) return All.ToArray(); }
}
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
}
'@

$failures = 0
function Pass($name) { Write-Host "PASS $name" }
function Fail($name, $why) { $script:failures++; Write-Host "FAIL $name : $why" }
function New-Id { return [guid]::NewGuid().ToString('B').ToUpperInvariant() }
function Get-Events($v) {
    foreach ($line in $v.Lines()) { try { $line | ConvertFrom-Json } catch { } }
}
function Wait-Event($v, [string]$name, [int]$seconds, [scriptblock]$where = { $true }) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        $hit = @(Get-Events $v | Where-Object { $_.Event -eq $name } | Where-Object $where) | Select-Object -First 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}
function Stop-Viewer($v) { if ($v -and -not $v.P.HasExited) { try { $v.P.Kill() } catch { } } }
function Start-Producer($id, $w, $h, $seconds, $y = 82, $u = 90, $vv = 240) {
    return Start-Process -FilePath $integrationExe -PassThru -WindowStyle Hidden `
        -ArgumentList '--produce-solid', $id, $w, $h, 166667, $seconds, $y, $u, $vv
}
function Center-Lparam($hwnd) {
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($hwnd, [ref]$r)
    $cx = [int](($r.Left + $r.Right) / 2); $cy = [int](($r.Top + $r.Bottom) / 2)
    return [IntPtr](((($cy -band 0xFFFF) -shl 16) -bor ($cx -band 0xFFFF)))
}

# ---------------------------------------------------------------- PiP
function Test-PipSnapshotColor {
    $name = 'pip_snapshot_color'; $id = New-Id; $bmp = Join-Path $env:TEMP "mc-pip-$([guid]::NewGuid()).bmp"; $v = $null; $p = $null
    try {
        $v = [VProc]::Start($viewerExe, "--pip $id 100 100 320 180 0 0 $PID --snapshot `"$bmp`"")
        if (-not (Wait-Event $v 'ready' 5)) { return Fail $name 'sem evento ready' }
        $p = Start-Producer $id 640 360 6
        if (-not $v.P.WaitForExit(10000)) { return Fail $name 'viewer não saiu em 10 s' }
        if ($v.P.ExitCode -ne 0) { return Fail $name "código de saída $($v.P.ExitCode)" }
        $b = [IO.File]::ReadAllBytes($bmp)
        $w = [BitConverter]::ToInt32($b, 18); $h = [BitConverter]::ToInt32($b, 22); $off = [BitConverter]::ToInt32($b, 10)
        if ($w -ne 320 -or [Math]::Abs($h) -ne 180) { return Fail $name "BMP ${w}x${h}" }
        $row = (($w * 3 + 3) -band -4)
        $i = $off + (179 - 90) * $row + 160 * 3
        $pxB = $b[$i]; $pxG = $b[$i + 1]; $pxR = $b[$i + 2]
        if ($pxR -ge 247 -and $pxG -le 8 -and $pxB -le 8) { Pass $name } else { Fail $name "pixel central R=$pxR G=$pxG B=$pxB" }
    } finally { Stop-Viewer $v; if ($p -and -not $p.HasExited) { $p.Kill() }; Remove-Item $bmp -ErrorAction SilentlyContinue }
}
function Test-PipLockHittest {
    $name = 'pip_lock_hittest'; $id = New-Id; $v = $null
    try {
        $v = [VProc]::Start($viewerExe, "--pip $id 100 100 320 180 0 0 $PID")
        $ready = Wait-Event $v 'ready' 5
        if (-not $ready) { return Fail $name 'sem evento ready' }
        $hwnd = [IntPtr][int64]$ready.Hwnd; $lp = Center-Lparam $hwnd
        $a = [int64][Win]::SendMessage($hwnd, 0x84, [IntPtr]::Zero, $lp)
        $v.P.StandardInput.WriteLine('lock 1'); Start-Sleep -Milliseconds 300
        $b = [int64][Win]::SendMessage($hwnd, 0x84, [IntPtr]::Zero, $lp)
        $v.P.StandardInput.WriteLine('lock 0'); Start-Sleep -Milliseconds 300
        $c = [int64][Win]::SendMessage($hwnd, 0x84, [IntPtr]::Zero, $lp)
        if ($a -eq 2 -and $b -eq 1 -and $c -eq 2) { Pass $name } else { Fail $name "hit=$a/$b/$c (esperado 2/1/2)" }
    } finally { Stop-Viewer $v }
}
function Test-PipModeChange {
    $name = 'pip_mode_change'; $id = New-Id; $v = $null; $p1 = $null; $p2 = $null
    try {
        $v = [VProc]::Start($viewerExe, "--pip $id 100 100 320 180 0 0 $PID")
        if (-not (Wait-Event $v 'ready' 5)) { return Fail $name 'sem evento ready' }
        $p1 = Start-Producer $id 640 360 3
        if (-not (Wait-Event $v 'source' 5 { $_.Width -eq 640 -and $_.Height -eq 360 })) { return Fail $name 'sem source 640x360' }
        $p1.WaitForExit(5000) | Out-Null
        $p2 = Start-Producer $id 640 480 3
        if (-not (Wait-Event $v 'source' 5 { $_.Width -eq 640 -and $_.Height -eq 480 })) { return Fail $name 'sem source 640x480' }
        if (Wait-Event $v 'geometry' 5 { $_.Width -eq 320 -and $_.Height -eq 240 }) { Pass $name } else { Fail $name 'sem geometry 320x240' }
    } finally { Stop-Viewer $v; foreach ($p in @($p1, $p2)) { if ($p -and -not $p.HasExited) { $p.Kill() } } }
}
function Test-ParentExit {
    $name = 'parent_exit'; $v = $null
    $parent = Start-Process -FilePath cmd.exe -ArgumentList '/c', 'ping -n 4 127.0.0.1 >nul' -PassThru -WindowStyle Hidden
    try {
        $v = [VProc]::Start($viewerExe, "--pip $(New-Id) 100 100 320 180 0 0 $($parent.Id)")
        if (-not (Wait-Event $v 'ready' 5)) { return Fail $name 'sem evento ready' }
        $parent.WaitForExit()
        if (-not $v.P.WaitForExit(3000)) { return Fail $name 'viewer não saiu em 3 s' }
        if ($v.P.ExitCode -eq 0) { Pass $name } else { Fail $name "código $($v.P.ExitCode)" }
    } finally { Stop-Viewer $v }
}
function Test-GarbageStdin {
    $name = 'garbage_stdin'; $v = $null
    try {
        $v = [VProc]::Start($viewerExe, "--pip $(New-Id) 100 100 320 180 0 0 $PID")
        if (-not (Wait-Event $v 'ready' 5)) { return Fail $name 'sem evento ready' }
        foreach ($line in '%%%', 'resize abc', 'lock 9', 'quit') { $v.P.StandardInput.WriteLine($line) }
        if (-not $v.P.WaitForExit(3000)) { return Fail $name 'viewer não saiu em 3 s' }
        $bad = @($v.Lines() | Where-Object { $_ -match 'error|erro' })
        if ($v.P.ExitCode -eq 0 -and -not $bad) { Pass $name } else { Fail $name "código $($v.P.ExitCode), erros: $bad" }
    } finally { Stop-Viewer $v }
}
function Test-BadArgs {
    $name = 'bad_args'
    $p = Start-Process -FilePath $viewerExe -ArgumentList '--pip' -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(5000)) { $p.Kill(); return Fail $name 'não saiu' }
    if ($p.ExitCode -eq 2) { Pass $name } else { Fail $name "código $($p.ExitCode)" }
}

Test-PipSnapshotColor
Test-PipLockHittest
Test-PipModeChange
Test-ParentExit
Test-GarbageStdin
Test-BadArgs

if ($failures) { Write-Host "FAIL ($failures)"; exit 1 }
Write-Host 'PASS'
exit 0
