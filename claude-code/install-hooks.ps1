<#
  Claude Traffic Light - hook installer for Claude Code
  -----------------------------------------------------
  Adds (or removes) the hooks in %USERPROFILE%\.claude\settings.json,
  leaving any hooks you already have from other tools untouched.

  Usage:
    powershell -ExecutionPolicy Bypass -File install-hooks.ps1
    powershell -ExecutionPolicy Bypass -File install-hooks.ps1 -Remove
    powershell -ExecutionPolicy Bypass -File install-hooks.ps1 -Exe "C:\Tools\ClaudeTrafficLight\claude-traffic-light.exe"

  Use -Exe when you keep the executable somewhere other than this repo. The
  hooks store an absolute path, so if you install from a folder you later move
  or delete, they stop working.
#>

param(
    [switch]$Remove,
    [string]$Exe
)

$ErrorActionPreference = 'Stop'

# --- locate the executable ------------------------------------------
if ($Exe) {
    if (-not (Test-Path $Exe)) {
        Write-Host "No file at -Exe path: $Exe" -ForegroundColor Red
        exit 1
    }
    $exePath = (Resolve-Path $Exe).Path
} else {
    $root = Split-Path -Parent $PSScriptRoot
    $exePath = $null
    foreach ($candidate in @(
        (Join-Path $root 'claude-traffic-light.exe'),
        (Join-Path $root 'dist\claude-traffic-light.exe'),
        (Join-Path $PSScriptRoot 'claude-traffic-light.exe'))) {
        if (Test-Path $candidate) { $exePath = (Resolve-Path $candidate).Path; break }
    }
}

if (-not $exePath -and -not $Remove) {
    Write-Host "Could not find claude-traffic-light.exe." -ForegroundColor Red
    Write-Host "Put it next to this folder (or in dist\), or pass -Exe <full path>."
    exit 1
}

if ($exePath -and -not $Remove) {
    Write-Host "Hooks will point at:" -ForegroundColor DarkGray
    Write-Host "  $exePath" -ForegroundColor DarkGray
    Write-Host "  (if you move or delete that file, re-run this with -Exe)" -ForegroundColor DarkGray
    Write-Host ""
}

$dir = Join-Path $env:USERPROFILE '.claude'
$cfg = Join-Path $dir 'settings.json'

if ($Remove -and -not (Test-Path $cfg)) {
    Write-Host "Nothing to do: $cfg does not exist." -ForegroundColor Yellow
    exit 0
}
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

# --- read settings.json ---------------------------------------------
if (Test-Path $cfg) {
    # Keep the FIRST backup forever: re-running this used to overwrite it with
    # the already-modified file, so the pristine original was lost.
    $backup = "$cfg.backup-traffic-light"
    if (-not (Test-Path $backup)) {
        Copy-Item $cfg $backup
        Write-Host "Original backed up to $backup" -ForegroundColor DarkGray
    } else {
        $stamped = "$cfg." + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".bak"
        Copy-Item $cfg $stamped
        Write-Host "Backup of the current file: $stamped" -ForegroundColor DarkGray
        Write-Host "(the untouched original is still at $backup)" -ForegroundColor DarkGray
    }

    $raw = Get-Content $cfg -Raw
    if ([string]::IsNullOrWhiteSpace($raw)) {
        $json = [pscustomobject]@{}
    } else {
        try {
            $json = $raw | ConvertFrom-Json
        } catch {
            Write-Host ""
            Write-Host "$cfg is not valid JSON, so nothing was changed." -ForegroundColor Red
            Write-Host "Open it and fix it (a trailing comma or a comment is the usual cause)."
            Write-Host "Your file is untouched; a copy is in the backup listed above."
            exit 1
        }
    }
} else {
    $json = [pscustomobject]@{}
}

if ($json -isnot [System.Management.Automation.PSCustomObject]) {
    Write-Host "$cfg does not contain a JSON object at the top level." -ForegroundColor Red
    Write-Host "Nothing was changed. Fix it by hand and run this again."
    exit 1
}

if ($null -ne $json.hooks -and
    $json.hooks -isnot [System.Management.Automation.PSCustomObject]) {
    Write-Host "The 'hooks' entry in $cfg is not an object." -ForegroundColor Red
    Write-Host "Nothing was changed, because merging into it would corrupt the file."
    exit 1
}

function ToHash($o) {
    $h = @{}
    if ($null -ne $o) { foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = $p.Value } }
    return $h
}

$hooks = ToHash $json.hooks

# matches this tool's own entries, including the older "semaforo.exe" name
$mine = 'claude-traffic-light\.exe|semaforo\.exe'

$map = [ordered]@{
    'SessionStart'     = 'done'
    'UserPromptSubmit' = 'running'
    'PreToolUse'       = 'running'
    'PostToolUse'      = 'running'
    'Notification'     = 'waiting'
    'Stop'             = 'done'
}
$withMatcher = @('PreToolUse', 'PostToolUse')

foreach ($ev in $map.Keys) {

    # 1) keep whatever is already there, dropping our own previous entries
    $keep = @()
    foreach ($grp in @($hooks[$ev])) {
        if ($null -eq $grp) { continue }
        $cmds = @()
        foreach ($hk in @($grp.hooks)) {
            if ($null -eq $hk) { continue }
            if ("$($hk.command)" -notmatch $mine) { $cmds += $hk }
        }
        if ($cmds.Count -gt 0) {
            $grp.hooks = @($cmds)
            $keep += $grp
        }
    }

    if ($Remove) {
        if ($keep.Count -gt 0) { $hooks[$ev] = @($keep) } else { $hooks.Remove($ev) }
        continue
    }

    # 2) add ours
    $cmd   = '"' + $exePath + '" --state ' + $map[$ev]
    $entry = @{ hooks = @( @{ type = 'command'; command = $cmd } ) }
    if ($withMatcher -contains $ev) { $entry['matcher'] = '*' }

    $hooks[$ev] = @($keep + $entry)
}

# --- write it back ---------------------------------------------------
if ($hooks.Count -gt 0) {
    $json | Add-Member -NotePropertyName 'hooks' -NotePropertyValue $hooks -Force
} elseif ($json.PSObject.Properties.Name -contains 'hooks') {
    $json.PSObject.Properties.Remove('hooks')
}

# UTF-8 WITHOUT a BOM. Set-Content -Encoding UTF8 writes one in Windows
# PowerShell 5.1, and those three invisible bytes are enough to make a JSON
# parser reject the file - which would leave Claude Code unable to read its
# own settings, with nothing visibly wrong.
$text = $json | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText($cfg, $text, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ""
if ($Remove) {
    Write-Host "Done: traffic light hooks removed from $cfg" -ForegroundColor Yellow
} else {
    Write-Host "Done: hooks installed in $cfg" -ForegroundColor Green
    Write-Host "Start a NEW Claude Code session for them to take effect." -ForegroundColor Green
}
