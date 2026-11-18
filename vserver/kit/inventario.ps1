# Inventario hardware della macchina di prova. Legge soltanto, non modifica nulla e non invia nulla in rete.
# Uso: powershell -ExecutionPolicy Bypass -File inventario.ps1
# Risultato: inventario.txt nella stessa cartella.
$ErrorActionPreference = 'Continue'
$out = Join-Path $PSScriptRoot 'inventario.txt'
$lines = New-Object System.Collections.Generic.List[string]
function Section($title) { $lines.Add(''); $lines.Add("== $title ==") }
function Dump($objects) { foreach ($l in (($objects | Format-List | Out-String).Trim() -split "`r?`n")) { $lines.Add($l) } }

$lines.Add("Inventario del $(Get-Date -Format 'yyyy-MM-dd HH:mm')")

Section 'Macchina'
Dump (Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model, NumberOfProcessors, NumberOfLogicalProcessors, @{n='RAM_GB';e={[math]::Round($_.TotalPhysicalMemory / 1GB, 1)}})
Dump (Get-CimInstance Win32_BIOS | Select-Object SMBIOSBIOSVersion, ReleaseDate)

Section 'Sistema operativo'
Dump (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture, InstallDate)
$lines.Add("PowerShell: $($PSVersionTable.PSVersion)")

Section 'Processori'
Dump (Get-CimInstance Win32_Processor | Select-Object SocketDesignation, Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed)

Section 'Memoria'
Dump (Get-CimInstance Win32_PhysicalMemory | Select-Object DeviceLocator, @{n='GB';e={[math]::Round($_.Capacity / 1GB, 1)}}, Speed, PartNumber)

Section 'Dischi fisici (come li vede Windows; dietro un RAID sono dischi virtuali)'
Dump (Get-CimInstance Win32_DiskDrive | Select-Object Model, InterfaceType, @{n='GB';e={[math]::Round($_.Size / 1GB, 0)}})

Section 'Volumi'
Dump (Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' | Select-Object DeviceID, VolumeName, FileSystem, @{n='GB';e={[math]::Round($_.Size / 1GB, 0)}}, @{n='Liberi_GB';e={[math]::Round($_.FreeSpace / 1GB, 0)}})

Section 'Controller dischi e RAID'
Dump (Get-CimInstance Win32_SCSIController | Select-Object Name, Manufacturer, DriverName)

Section 'Schede video e grafiche'
Dump (Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, @{n='RAM_MB';e={[math]::Round($_.AdapterRAM / 1MB, 0)}})

Section 'Schede di rete'
Dump (Get-CimInstance Win32_NetworkAdapter -Filter 'PhysicalAdapter=True' | Select-Object Name, Speed, NetConnectionStatus)

Section 'Slot di espansione'
Dump (Get-CimInstance Win32_SystemSlot | Select-Object SlotDesignation, CurrentUsage, Description)

Section 'Possibili schede video I/O (AJA, Blackmagic, Bluefish, Deltacast, Matrox, DekTec)'
$io = Get-CimInstance Win32_PnPEntity | Where-Object { "$($_.Name) $($_.Manufacturer)" -match 'AJA|Corvid|Kona|Blackmagic|DeckLink|Bluefish|Deltacast|Matrox|DekTec|Stream Labs|SDI' }
if ($io) { Dump ($io | Select-Object Name, Manufacturer, PNPDeviceID, Status) } else { $lines.Add('Nessuna trovata per nome: vedere l''elenco completo qui sotto.') }

Section 'Dispositivi PCI (elenco completo)'
Dump (Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'PCI\*' } | Sort-Object Name | Select-Object Name, Manufacturer, PNPDeviceID)

Section 'Dispositivi con problemi o senza driver'
$bad = Get-CimInstance Win32_PnPEntity | Where-Object { $_.ConfigManagerErrorCode -ne 0 }
if ($bad) { Dump ($bad | Select-Object Name, PNPDeviceID, ConfigManagerErrorCode) } else { $lines.Add('Nessuno.') }

[System.IO.File]::WriteAllLines($out, $lines)
Write-Host "Scritto: $out"
