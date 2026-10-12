# Monta o pacote portátil a partir de dist/ (rode build.ps1 antes).
#   build/package                    espelho de dist sem data/
#   MultiCam-Final                   recebe o pacote; MultiCam-Final/data (perfis reais) nunca é tocado
#   artifacts/MultiCam-Windows10.zip refeito a partir de build/package
# Uso: powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$package = Join-Path $root 'build\package'
$final = Join-Path $root 'MultiCam-Final'
$zip = Join-Path $root 'artifacts\MultiCam-Windows10.zip'

function Robocopy-Checked([string[]]$arguments) {
    & robocopy @arguments /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "robocopy falhou (código $LASTEXITCODE): $arguments" }
}
function Data-Hash {
    $file = Join-Path $final 'data\settings.json'
    if (Test-Path $file) { return (Get-FileHash $file).Hash }
    return ''
}

if (-not (Test-Path (Join-Path $dist 'MultiCam.exe'))) { throw 'dist\MultiCam.exe não existe. Rode build.ps1 antes.' }
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($final + '\', [StringComparison]::OrdinalIgnoreCase) })
if ($running) { throw "Feche o MultiCam pela bandeja (Sair) antes de atualizar: $(($running | ForEach-Object { $_.ProcessName + ' ' + $_.Id }) -join ', ')" }

# Apps que só listam câmeras (Discord, navegadores) carregam MultiCam.Camera.dll e a prendem.
# O Windows deixa mover um arquivo em uso: a DLL antiga vai para .substituidos e a nova entra
# no lugar; quem já a carregou segue com a antiga até reabrir. Sobras antigas são apagadas aqui.
$replaced = Join-Path $final '.substituidos'
if (Test-Path $replaced) {
    Get-ChildItem $replaced -File | ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch { } }
}
function Move-LockedAside([string]$file) {
    if (-not (Test-Path $file)) { return }
    try { [IO.File]::Open($file, 'Open', 'ReadWrite', 'None').Dispose(); return } catch { }
    New-Item -ItemType Directory -Force $replaced | Out-Null
    $aside = Join-Path $replaced ("{0}.{1:yyyyMMdd-HHmmss}" -f (Split-Path $file -Leaf), (Get-Date))
    Move-Item -LiteralPath $file -Destination $aside
    Write-Host "Em uso, movido para .substituidos: $file"
}

Write-Host '=== build\package (espelho de dist, sem data)'
Robocopy-Checked @($dist, $package, '/MIR', '/XD', 'data')

Write-Host '=== portabilidade'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tests\portable-check.ps1') -Root $package | Out-Host
if ($LASTEXITCODE) { throw 'portable-check falhou; MultiCam-Final não foi alterado.' }

Write-Host '=== MultiCam-Final (mantendo data)'
$before = Data-Hash
foreach ($arch in 'x64', 'x86') {
    $dll = Join-Path $final "$arch\MultiCam.Camera.dll"
    $new = Join-Path $package "$arch\MultiCam.Camera.dll"
    if ((Test-Path $dll) -and (Test-Path $new) -and (Get-FileHash $dll).Hash -ne (Get-FileHash $new).Hash) { Move-LockedAside $dll }
}
Robocopy-Checked @($package, $final, '/E', '/XD', 'data', '.substituidos')
if ((Data-Hash) -ne $before) { throw 'MultiCam-Final\data\settings.json mudou! Confira o .bak.' }

Write-Host '=== zip'
New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip -Force

$different = @(Get-ChildItem $package -Recurse -File | Where-Object {
    $copy = Join-Path $final $_.FullName.Substring($package.Length + 1)
    -not (Test-Path $copy) -or (Get-FileHash $copy).Hash -ne (Get-FileHash $_.FullName).Hash
} | ForEach-Object { $_.FullName.Substring($package.Length + 1) })
if ($different) { Write-Warning "Arquivos não atualizados em MultiCam-Final (em uso?): $($different -join ', ')"; exit 1 }
Write-Host "PASS: pacote atualizado, data preservado, zip com $((Get-Item $zip).Length) bytes."
exit 0
