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
$locked = & tasklist /m MultiCam.Camera.dll /fo csv /nh 2>$null | Where-Object { $_ -like '"*' }
if ($locked) { Write-Warning "MultiCam.Camera.dll em uso (ex.: Discord); essa DLL pode não ser trocada:`n$($locked -join "`n")" }

Write-Host '=== build\package (espelho de dist, sem data)'
Robocopy-Checked @($dist, $package, '/MIR', '/XD', 'data')

Write-Host '=== portabilidade'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tests\portable-check.ps1') -Root $package | Out-Host
if ($LASTEXITCODE) { throw 'portable-check falhou; MultiCam-Final não foi alterado.' }

Write-Host '=== MultiCam-Final (mantendo data)'
$before = Data-Hash
Robocopy-Checked @($package, $final, '/E', '/XD', 'data')
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
