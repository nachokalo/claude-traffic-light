<#
  Claude Traffic Light - hook installer for Claude Code
  -----------------------------------------------------
  Adds (or removes) the hooks in %USERPROFILE%\.claude\settings.json,
  leaving any hooks you already have from other tools untouched.

  Usage:
    powershell -ExecutionPolicy Bypass -File install-hooks.ps1
    powershell -ExecutionPolicy Bypass -File install-hooks.ps1 -Remove
#>

param([switch]$Remove)

$ErrorActionPreference = 'Stop'

# --- locate the executable ------------------------------------------
$root = Split-Path -Parent $PSScriptRoot
$exe  = $null
foreach ($candidate in @(
    (Join-Path $root 'claude-traffic-light.exe'),
    (Join-Path $root 'dist\claude-traffic-light.exe'),
    (Join-Path $PSScriptRoot 'claude-traffic-light.exe'))) {
    if (Test-Path $candidate) { $exe = $candidate; break }
}

if (-not $exe -and -not $Remove) {
    Write-Host "Could not find claude-traffic-light.exe." -ForegroundColor Red
    Write-Host "Put it next to this folder (or in dist\) and run this again."
    exit 1
}

$dir = Join-Path $env:USERPROFILE '.claude'
$cfg = Join-Path $dir 'settings.json'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

# --- read settings.json ---------------------------------------------
if (Test-Path $cfg) {
    Copy-Item $cfg "$cfg.backup-traffic-light" -Force
    $raw = Get-Content $cfg -Raw
    if ([string]::IsNullOrWhiteSpace($raw)) { $json = [pscustomobject]@{} }
    else { $json = $raw | ConvertFrom-Json }
    Write-Host "Backup written to $cfg.backup-traffic-light" -ForegroundColor DarkGray
} else {
    $json = [pscustomobject]@{}
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
    $cmd   = '"' + $exe + '" --state ' + $map[$ev]
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

($json | ConvertTo-Json -Depth 20) | Set-Content -Path $cfg -Encoding UTF8

Write-Host ""
if ($Remove) {
    Write-Host "Done: traffic light hooks removed from $cfg" -ForegroundColor Yellow
} else {
    Write-Host "Done: hooks installed in $cfg" -ForegroundColor Green
    Write-Host "Start a NEW Claude Code session for them to take effect." -ForegroundColor Green
}
