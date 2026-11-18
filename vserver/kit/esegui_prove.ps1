# Prove del software sulla macchina, con scheda video simulata. Non serve nessuna scheda SDI.
# Uso: powershell -ExecutionPolicy Bypass -File esegui_prove.ps1 -Cartella D:\prove [-Secondi 120]
# La cartella deve stare sul disco che ospitera' le registrazioni. Servono circa 6 GB liberi con i valori predefiniti.
# Risultato: rapporto_prove.txt accanto a questo script. Lo script scrive solo dentro -Cartella e non cancella nulla.
param(
    [Parameter(Mandatory = $true)][string]$Cartella,
    [int]$Secondi = 120
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path $PSScriptRoot 'bin'
$report = Join-Path $PSScriptRoot 'rapporto_prove.txt'
if (-not (Test-Path $Cartella)) { New-Item -ItemType Directory -Path $Cartella | Out-Null }
$work = Join-Path $Cartella ("prova-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $work | Out-Null

$log = New-Object System.Collections.Generic.List[string]
function Say($text) { Write-Host $text; $log.Add($text) }
$results = New-Object System.Collections.Generic.List[string]

function RunTest($name, $exe, $arguments) {
    Say ''
    Say "=== $name ==="
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ErrorActionPreference = 'Continue'
    $output = & (Join-Path $bin $exe) @arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    foreach ($l in $output) { Say "  $l" }
    $verdict = if ($code -eq 0) { 'OK' } else { "FALLITO (codice $code)" }
    Say ("  -> {0} in {1:n0} s" -f $verdict, $sw.Elapsed.TotalSeconds)
    $results.Add(("{0,-46} {1}" -f $name, $verdict))
}

$cs = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Say "Prove del $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
Say "Macchina: $($cs.Manufacturer) $($cs.Model)"
Say "CPU: $($cpu.Name.Trim()) x $($cs.NumberOfProcessors), $($cs.NumberOfLogicalProcessors) thread"
Say "Cartella di lavoro: $work"

# 1. Velocita' del disco: scrittura e rilettura di 2 GB senza cache di Windows.
Say ''
Say '=== Disco: scrittura e lettura sequenziale di 2 GB ==='
$diskFile = Join-Path $work 'disco.bin'
$block = New-Object byte[] (4MB)
(New-Object System.Random 1).NextBytes($block)
$writeThrough = [System.IO.FileOptions]::WriteThrough
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$fs = New-Object System.IO.FileStream($diskFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, 4MB, $writeThrough)
for ($i = 0; $i -lt 512; $i++) { $fs.Write($block, 0, $block.Length) }
$fs.Flush($true); $fs.Close()
$writeMbs = 2048 / $sw.Elapsed.TotalSeconds
$noBuffering = [System.IO.FileOptions]0x20000000
$sw.Restart()
$fs = New-Object System.IO.FileStream($diskFile, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None, 4MB, $noBuffering)
while ($fs.Read($block, 0, $block.Length) -gt 0) { }
$fs.Close()
$readMbs = 2048 / $sw.Elapsed.TotalSeconds
Say ("  scrittura {0:n0} MB/s, lettura {1:n0} MB/s (servono circa 25 MB/s in scrittura e 13 in lettura)" -f $writeMbs, $readMbs)
$diskOk = ($writeMbs -ge 75) -and ($readMbs -ge 40)
$results.Add(("{0,-46} {1}" -f 'Disco (almeno 3 volte il necessario)', $(if ($diskOk) { 'OK' } else { 'INSUFFICIENTE' })))

# 2. Prove del software.
RunTest 'Logica di base' 'core_tests.exe' @()
RunTest "Registrazione 4 canali, $Secondi s in tempo reale" 'record_smoke.exe' @($work, "$Secondi", '1')
RunTest 'Registrazione 4 canali a velocita doppia' 'record_smoke.exe' @($work, '30', '2')
RunTest 'Registrazione 4 canali a velocita tripla' 'record_smoke.exe' @($work, '30', '3')
RunTest 'Riproduzione, rallenty e ricerca' 'play_smoke.exe' @($work, '1')
RunTest 'Flusso completo: catalogo e playlist' 'flow_smoke.exe' @($work, '1')

Say ''
Say '=== RIEPILOGO ==='
foreach ($r in $results) { Say $r }
Say ''
Say 'Le prove a velocita doppia e tripla misurano il margine: possono fallire senza che la macchina sia inadatta.'
Say "I file di prova restano in $work e si possono cancellare a mano."
[System.IO.File]::WriteAllLines($report, $log)
Write-Host "Rapporto scritto: $report"
