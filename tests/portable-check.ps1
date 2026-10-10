# Confere que uma pasta do MultiCam roda em qualquer Windows 10/11 sem instalar nada:
# todo .exe/.dll só importa DLLs do próprio Windows (ou que vêm na própria pasta) e não há dados pessoais.
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File tests/portable-check.ps1 [-Root <pasta>]   (padrão: dist)
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist' }
$Root = [IO.Path]::GetFullPath($Root)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
public static class PeImports {
    // Names of the DLLs in the import table (normal imports only; delay-loads are optional by design).
    public static List<string> Read(string path) {
        var result = new List<string>();
        byte[] b = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(b, 0x3C);
        if (BitConverter.ToUInt32(b, pe) != 0x4550) throw new InvalidDataException("não é um PE: " + path);
        int sections = BitConverter.ToUInt16(b, pe + 6);
        int optSize = BitConverter.ToUInt16(b, pe + 20);
        int opt = pe + 24;
        bool pe32plus = BitConverter.ToUInt16(b, opt) == 0x20B;
        int dirs = opt + (pe32plus ? 112 : 96);
        uint importRva = BitConverter.ToUInt32(b, dirs + 8);
        if (importRva == 0) return result;
        int secTable = opt + optSize;
        Func<uint, int> offset = rva => {
            for (int i = 0; i < sections; i++) {
                int s = secTable + i * 40;
                uint va = BitConverter.ToUInt32(b, s + 12), size = Math.Max(BitConverter.ToUInt32(b, s + 8), BitConverter.ToUInt32(b, s + 16));
                uint raw = BitConverter.ToUInt32(b, s + 20);
                if (rva >= va && rva < va + size) return (int)(rva - va + raw);
            }
            throw new InvalidDataException("RVA fora das seções: " + path);
        };
        for (int d = offset(importRva); ; d += 20) {
            uint nameRva = BitConverter.ToUInt32(b, d + 12);
            if (nameRva == 0) break;
            int n = offset(nameRva), end = n;
            while (b[end] != 0) end++;
            result.Add(Encoding.ASCII.GetString(b, n, end - n));
        }
        return result;
    }
}
'@

$problems = New-Object System.Collections.Generic.List[string]
if (-not (Test-Path $Root)) { Write-Host "FAIL: pasta $Root não existe"; exit 1 }
if (Test-Path (Join-Path $Root 'data')) { $problems.Add("$Root\data deve ser removida antes de distribuir (tem perfis e log pessoais)") }
foreach ($required in 'MultiCam.exe', 'x64\MultiCam.Worker.exe', 'x64\MultiCam.Camera.dll', 'x64\MultiCam.Viewer.exe', 'x64\turbojpeg.dll', 'x86\MultiCam.Worker.exe', 'x86\MultiCam.Camera.dll') {
    if (-not (Test-Path (Join-Path $Root $required))) { $problems.Add("falta $required") }
}
$system = @{}
foreach ($dir in "$env:WINDIR\System32", "$env:WINDIR\SysWOW64") {
    if (Test-Path $dir) { Get-ChildItem $dir -Filter *.dll -File -ErrorAction SilentlyContinue | ForEach-Object { $system[$_.Name.ToLowerInvariant()] = $true } }
}
$binaries = Get-ChildItem $Root -Recurse -File -Include *.exe, *.dll
foreach ($file in $binaries) {
    $local = @{}
    Get-ChildItem $file.DirectoryName -File -Filter *.dll | ForEach-Object { $local[$_.Name.ToLowerInvariant()] = $true }
    foreach ($dll in [PeImports]::Read($file.FullName)) {
        $name = $dll.ToLowerInvariant()
        if ($name.StartsWith('api-ms-win-') -or $name.StartsWith('ext-ms-')) {
            if ($name.StartsWith('api-ms-win-crt-')) { $problems.Add("$($file.Name) usa o runtime C universal ($dll): compile com runtime estático") }
            continue
        }
        if ($name -match '^(vcruntime|msvcp|ucrtbase|concrt|vcomp)') { $problems.Add("$($file.Name) depende do Visual C++ Redistributable ($dll)") ; continue }
        if ($system.ContainsKey($name) -or $local.ContainsKey($name)) { continue }
        $problems.Add("$($file.Name) importa $dll, que não é do Windows nem está na pasta")
    }
}
Write-Host "Conferidos $($binaries.Count) binários em $Root"
if ($problems.Count) { foreach ($p in $problems) { Write-Host "FAIL: $p" }; exit 1 }
Write-Host 'PASS: só DLLs do Windows, nenhum runtime extra, sem dados pessoais.'
exit 0
