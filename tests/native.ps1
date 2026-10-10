# Ajuda para os testes nativos: compila um alvo CMake em x64 e devolve o caminho do executável.
# Uso: . tests/native.ps1; & (Build-NativeTarget MultiCam.ViewerCoreTest)
$script:NativeRoot = Split-Path $PSScriptRoot -Parent
function Build-NativeTarget([string]$Target) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $script:NativeRoot 'build.ps1') -Target $Target | Out-Host
    if ($LASTEXITCODE) { throw "Falha ao compilar $Target (código $LASTEXITCODE)." }
    $found = @("build/ninja-x64/$Target.exe", "build/x64/Release/$Target.exe") |
        ForEach-Object { Join-Path $script:NativeRoot $_ } | Where-Object { Test-Path $_ } |
        Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
    if (-not $found) { throw "Executável de $Target não encontrado depois do build." }
    return $found
}
