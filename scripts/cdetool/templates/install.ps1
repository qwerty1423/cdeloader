# @@NAME@@ @@VERSION@@ (@@VARIANT@@) - installer (pure PowerShell, no Python).
#
#   install.cmd                    install or update the patch
#   install.cmd -GameDir "D:\..."  use a specific game folder
#   install.cmd -Compat            require a Community-Patch build (stack on it)
#   install.cmd -Check             report the state, change nothing
#   install.cmd -Uninstall         restore the pre-install CDE.exe
#   install.cmd -Proxy             no-exe-patch mode (vcruntime140 forwarder)
#   install.cmd -RestoreRuntime    undo proxy mode
#
# Finds the game (this folder, -GameDir, or your Steam libraries).  The folder
# contains one delta for the stock exe and one for each known CDE Community
# Patch variant; the installer picks the delta that matches the current
# CDE.exe, so a community-patched game is stacked automatically (cdepatch.dll
# is preserved).  It backs up the pre-install exe and verifies the result.
param(
    [string]$GameDir,
    [switch]$Uninstall,
    [switch]$Check,
    [switch]$Compat,
    [switch]$Proxy,
    [switch]$RestoreRuntime
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$Name = '@@NAME@@'
$Version = '@@VERSION@@'
$Variant = '@@VARIANT@@'
$Game = '@@GAME@@'
$DeltaFile = '@@DELTA@@'
$HelperDll = '@@DLL@@'
$HelperIni = '@@INI@@'
$ExtraFiles = @(@@EXTRA_PS@@)

$BackupMarker = 'cdeloader-backup.txt'
$CompatBackup = 'CDE.exe.before-cdeloader'
$StockBackup = 'CDE.exe.original'

function Get-Sha256([byte[]]$bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
}

function HexToBytes([string]$hex) {
    $n = $hex.Length / 2
    $b = New-Object byte[] $n
    for ($i = 0; $i -lt $n; $i++) {
        $b[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return $b
}

function Find-GameDir {
    if ($GameDir) { return $GameDir }
    if (Test-Path (Join-Path $here 'CDE.exe')) { return $here }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
                ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libs) {
            $d = Join-Path $lib ('steamapps\common\' + $Game)
            if (Test-Path (Join-Path $d 'CDE.exe')) { return $d }
        }
    }
    throw "Game folder not found. Run install.cmd -GameDir ""<path to $Game>""."
}

function Read-Marker([string]$game) {
    $p = Join-Path $game $BackupMarker
    if (Test-Path $p) {
        $t = (Get-Content -Raw $p).Trim()
        if ($t) { return $t }
    }
    return $StockBackup
}

$game = Find-GameDir
$exe = Join-Path $game 'CDE.exe'
if (-not (Test-Path $exe)) { throw "No CDE.exe in $game" }

# Load every delta next to the installer; the stock one is first.
$deltas = @()
$seen = @{}
$paths = @((Join-Path $here $DeltaFile))
$paths += @(Get-ChildItem -Path $here -File | Where-Object { $_.Name -like '*.delta.json' } |
            ForEach-Object { $_.FullName })
foreach ($p in $paths) {
    if (-not (Test-Path $p)) { continue }
    $full = (Resolve-Path $p).Path
    if ($seen.ContainsKey($full)) { continue }
    $seen[$full] = $true
    $d = Get-Content -Raw $full | ConvertFrom-Json
    if ($d.source_sha256 -and $d.target_sha256) {
        $d | Add-Member -NotePropertyName _file -NotePropertyValue (Split-Path -Leaf $full) -Force
        $deltas += $d
    }
}
if ($deltas.Count -eq 0) { throw 'no delta files found next to install.ps1' }
$stockSource = ($deltas | Where-Object { $_._file -eq $DeltaFile } |
                Select-Object -First 1).source_sha256
if (-not $stockSource) { $stockSource = $deltas[0].source_sha256 }

function Pick-Delta([string]$have, [bool]$compatOnly) {
    foreach ($d in $deltas) {
        if ($d.source_sha256 -eq $have -and
            -not ($compatOnly -and $d._file -eq $DeltaFile)) {
            return @{ Delta = $d; Kind = 'source' }
        }
    }
    foreach ($d in $deltas) {
        if ($d.target_sha256 -eq $have -and
            -not ($compatOnly -and $d._file -eq $DeltaFile)) {
            return @{ Delta = $d; Kind = 'target' }
        }
    }
    return $null
}

Write-Host "Game folder: $game"
Write-Host "Patch:       $Name $Version ($Variant)"

if ($Uninstall) {
    $name = Read-Marker $game
    $backup = Join-Path $game $name
    if (-not (Test-Path $backup)) {
        $name = $StockBackup
        $backup = Join-Path $game $name
    }
    if (Test-Path $backup) {
        Copy-Item $backup $exe -Force
        Remove-Item (Join-Path $game $BackupMarker) -ErrorAction SilentlyContinue
        Write-Host "Restored $name; the framework is removed."
    } elseif (-not (Test-Path (Join-Path $game 'vcruntime140_orig.dll'))) {
        Write-Host 'No backup found; nothing to uninstall here.'
    }
    $orig = Join-Path $game 'vcruntime140_orig.dll'
    if (Test-Path $orig) {
        Move-Item $orig (Join-Path $game 'vcruntime140.dll') -Force
        Write-Host 'Restored the original vcruntime140.dll (proxy mode off).'
    }
    exit 0
}

$ProxyName = 'vcruntime140.dll'
$ProxyOriginal = 'vcruntime140_orig.dll'

function Test-Proxy([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('vcruntime140_orig')
}

if ($RestoreRuntime) {
    $orig = Join-Path $game $ProxyOriginal
    if (-not (Test-Path $orig)) {
        Write-Error "no $ProxyOriginal in $game; proxy mode was not installed"
        exit 1
    }
    Move-Item $orig (Join-Path $game $ProxyName) -Force
    Write-Host "Restored the original $ProxyName; proxy mode is off."
    exit 0
}

if ($Proxy) {
    $target = Join-Path $game $ProxyName
    if ($Check) {
        if (Test-Proxy $target) {
            Write-Host 'state: proxy mode (CDE.exe untouched)'
        } elseif (Test-Path (Join-Path $game $ProxyOriginal)) {
            Write-Host 'state: proxy files partly present (runtime renamed)'
        } else {
            Write-Host 'state: no proxy'
        }
        exit 0
    }
    $src = Join-Path $here 'proxy\vcruntime140.dll'
    if (-not (Test-Path $src)) {
        Write-Error 'proxy\vcruntime140.dll is missing from the package'
        exit 1
    }
    if ((Test-Path $target) -and -not (Test-Proxy $target)) {
        $orig = Join-Path $game $ProxyOriginal
        if (Test-Path $orig) {
            Write-Error "both $ProxyName and $ProxyOriginal exist and $ProxyName is not our proxy"
            exit 1
        }
        Move-Item $target $orig -Force
        Write-Host "Renamed $ProxyName -> $ProxyOriginal"
    }
    Copy-Item $src $target -Force
    Write-Host 'Installed the vcruntime140 forwarder (CDE.exe untouched).'
    if ($HelperDll) {
        $s = Join-Path $here $HelperDll
        if (Test-Path $s) { Copy-Item $s (Join-Path $game $HelperDll) -Force }
    }
    if ($HelperIni) {
        $s = Join-Path $here $HelperIni
        $d = Join-Path $game $HelperIni
        if ((Test-Path $s) -and -not (Test-Path $d)) { Copy-Item $s $d -Force }
    }
    foreach ($rel in $ExtraFiles) {
        $d = Join-Path $game $rel
        if (($rel -like 'mods/config/*') -and (Test-Path $d)) { continue }
        $s = Join-Path $here $rel
        if (Test-Path $s) {
            $dir = Split-Path -Parent $d
            if ($dir -and -not (Test-Path $dir)) {
                New-Item -ItemType Directory -Force -Path $dir | Out-Null
            }
            Copy-Item $s $d -Force
        }
    }
    Write-Host 'Done. Start the game as usual. Undo with -RestoreRuntime.'
    exit 0
}

$have = Get-Sha256 ([IO.File]::ReadAllBytes($exe))

if ($Check) {
    $pick = Pick-Delta $have ([bool]$Compat)
    if ($pick -and $pick.Kind -eq 'target') {
        $tag = if ($pick.Delta.source_sha256 -ne $stockSource) { ' + community patch' } else { '' }
        Write-Host "state: patched$tag ($($pick.Delta._file))"
    } elseif ($pick -and $pick.Kind -eq 'source') {
        if ($pick.Delta.source_sha256 -eq $stockSource) {
            Write-Host 'state: original'
        } else {
            Write-Host "state: community-patched ($($pick.Delta._file)); run the installer to add the framework"
        }
    } else {
        Write-Host "state: unknown ($have)"
    }
    exit 0
}

$pick = Pick-Delta $have ([bool]$Compat)
if (-not $pick) {
    $srcs = ($deltas | ForEach-Object { $_.source_sha256.Substring(0, 12) } |
             Sort-Object -Unique) -join ', '
    throw "CDE.exe is not a supported build (SHA256 $have). Supported sources: $srcs"
}
$delta = $pick.Delta
$kind = $pick.Kind
$isCompat = $delta.source_sha256 -ne $stockSource
$backupName = if ($isCompat) { $CompatBackup } else { $StockBackup }
$backup = Join-Path $game $backupName

if ($isCompat) {
    Write-Host "Detected a CDE Community Patch build ($($delta._file))."
    Write-Host 'Stacking the loader on top; cdepatch.dll is preserved.'
}

if ($kind -eq 'target') {
    Write-Host 'This version is already installed; refreshing files.'
    $source = [IO.File]::ReadAllBytes($exe)
} else {
    $source = [IO.File]::ReadAllBytes($exe)
    if (-not (Test-Path $backup)) {
        Copy-Item $exe $backup -Force
        Write-Host "Backed up the pre-install exe to $backupName"
    }
}

if ($have -ne $delta.target_sha256) {
    $patched = New-Object byte[] ([int]$delta.target_size)
    [Array]::Copy($source, $patched, [int]$delta.source_size)
    foreach ($c in $delta.changes) {
        $b = HexToBytes $c.bytes
        [Array]::Copy($b, 0, $patched, [int]$c.offset, $b.Length)
    }
    if ($delta.append) {
        $tail = HexToBytes $delta.append
        [Array]::Copy($tail, 0, $patched, [int]$delta.source_size, $tail.Length)
    }
    if ((Get-Sha256 $patched) -ne $delta.target_sha256) {
        throw 'Patched result has the wrong checksum; nothing was changed.'
    }
    $tmp = $exe + '.new'
    [IO.File]::WriteAllBytes($tmp, $patched)
    Move-Item $tmp $exe -Force
    Write-Host 'Patched CDE.exe (verified).'
}

Set-Content -Path (Join-Path $game $BackupMarker) -Value $backupName -Encoding ASCII

if ($HelperDll) {
    $s = Join-Path $here $HelperDll
    if (Test-Path $s) { Copy-Item $s (Join-Path $game $HelperDll) -Force }
}
if ($HelperIni) {
    $s = Join-Path $here $HelperIni
    $d = Join-Path $game $HelperIni
    if ((Test-Path $s) -and -not (Test-Path $d)) { Copy-Item $s $d -Force }
}
foreach ($rel in $ExtraFiles) {
    if ($isCompat -and $rel -like 'mods/community/*') {
        Write-Host "Skipping $rel (its crash fixes are already in the community patch)."
        continue
    }
    $d = Join-Path $game $rel
    if (($rel -like 'mods/config/*') -and (Test-Path $d)) {
        continue  # never overwrite a user's per-mod settings
    }
    $s = Join-Path $here $rel
    if (Test-Path $s) {
        $dir = Split-Path -Parent $d
        if ($dir -and -not (Test-Path $dir)) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
        }
        Copy-Item $s $d -Force
    }
}

Write-Host 'Done. Start the game as usual.'
