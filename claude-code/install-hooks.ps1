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
    # -LiteralPath en todo: sin eso, una carpeta como "claude-traffic-light[1]"
    # -- el nombre que pone Windows a una descarga repetida -- se interpreta
    # como comodin y el archivo "no existe".
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
        Write-Host "No file at -Exe path (or it is a folder, not the exe): $Exe" -ForegroundColor Red
        exit 1
    }
    $exePath = (Resolve-Path -LiteralPath $Exe).Path
} else {
    $root = Split-Path -Parent $PSScriptRoot
    $exePath = $null
    foreach ($candidate in @(
        (Join-Path $root 'claude-traffic-light.exe'),
        (Join-Path $root 'dist\claude-traffic-light.exe'),
        (Join-Path $PSScriptRoot 'claude-traffic-light.exe'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $exePath = (Resolve-Path -LiteralPath $candidate).Path
            break
        }
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

if ($Remove -and -not (Test-Path -LiteralPath $cfg)) {
    Write-Host "Nothing to do: $cfg does not exist." -ForegroundColor Yellow
    exit 0
}
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

# --- read settings.json ---------------------------------------------
# Nothing is backed up and nothing is written until we know the file actually
# has to change: an earlier version copied a .bak on every run, so running the
# installer five times left five backups in the user's .claude folder.
$original = $null
if (Test-Path -LiteralPath $cfg) {
    $raw = Get-Content -LiteralPath $cfg -Raw
    $original = $raw
    if ([string]::IsNullOrWhiteSpace($raw)) {
        $json = [pscustomobject]@{}
    } else {
        try {
            $json = $raw | ConvertFrom-Json
        } catch {
            Write-Host ""
            Write-Host "$cfg is not valid JSON, so nothing was changed." -ForegroundColor Red
            Write-Host "Open it and fix it (a trailing comma or a comment is the usual cause)."
            Write-Host "Your file is exactly as you left it - this script did not touch it."
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
    # [ordered] para que el archivo escrito no cambie de orden en cada corrida:
    # con una Hashtable comun, cada ejecucion producia un diff distinto.
    $h = [ordered]@{}
    if ($null -ne $o) { foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = $p.Value } }
    return $h
}

$hooks = ToHash $json.hooks
$changed = $false

# Matches this tool's own entries so they can be replaced instead of piled up.
# The two names cover the current executable and the older "semaforo.exe", but
# a renamed copy would not match either - and then every run appended a second
# copy of every hook, forever. So the exact path being installed counts as ours
# too, whatever the file is called.
$mine = 'claude-traffic-light\.exe|semaforo\.exe'
if ($exePath) { $mine += '|' + [regex]::Escape($exePath) }

$map = [ordered]@{
    'SessionStart'     = 'done'
    'UserPromptSubmit' = 'running'
    'PreToolUse'       = 'running'
    'PostToolUse'      = 'running'
    'Notification'     = 'waiting'
    'Stop'             = 'done'
}
$withMatcher = @('PreToolUse', 'PostToolUse')

$looksLikeOurs = 0   # entries that smell like this tool but did not match $mine

foreach ($ev in $map.Keys) {

    # 1) keep whatever is already there, dropping our own previous entries
    $keep = @()
    foreach ($grp in @($hooks[$ev])) {
        if ($null -eq $grp) { continue }
        $cmds = @()
        foreach ($hk in @($grp.hooks)) {
            if ($null -eq $hk) { continue }
            $c = "$($hk.command)"
            if ($c -notmatch $mine) {
                if ($c -match '(--state|\s-s)\s+(done|running|waiting)\b') { $looksLikeOurs++ }
                $cmds += $hk
            }
        }
        if ($cmds.Count -gt 0) {
            $grp.hooks = @($cmds)
            $keep += $grp
        }
    }

    if ($Remove) {
        if ($keep.Count -gt 0) {
            if (@($hooks[$ev]).Count -ne $keep.Count) { $changed = $true }
            $hooks[$ev] = @($keep)
        } elseif ($hooks.Contains($ev)) {
            $hooks.Remove($ev)
            $changed = $true
        }
        continue
    }

    # 2) add ours
    # [ordered] everywhere: a plain @{} Hashtable has no defined key order, so
    # "matcher" landed before or after "hooks" at random and every run produced
    # a different settings.json even when nothing had actually changed.
    $cmd   = '"' + $exePath + '" --state ' + $map[$ev]
    $entry = [ordered]@{}
    if ($withMatcher -contains $ev) { $entry['matcher'] = '*' }
    $entry['hooks'] = @( [ordered]@{ type = 'command'; command = $cmd } )

    $hooks[$ev] = @($keep + $entry)
    $changed = $true
}

if (-not $changed) {
    # Reescribir el archivo sin necesidad no es inocuo: ConvertTo-Json de
    # PowerShell 5.1 escapa < > & ' como \u003c y compania, asi que el usuario
    # abre su settings.json y lo encuentra transformado sin haber cambiado nada.
    Write-Host ""
    Write-Host "Nothing to change: $cfg already matches. File untouched." -ForegroundColor Yellow
    if ($Remove -and $looksLikeOurs -gt 0) {
        Write-Host ""
        Write-Host "Heads up: $looksLikeOurs hook(s) in there look like this tool but point at an" -ForegroundColor Yellow
        Write-Host "executable with a different name, so they were left alone on purpose." -ForegroundColor Yellow
        Write-Host "To remove those too, run this again with the path you installed with:" -ForegroundColor Yellow
        Write-Host '  install-hooks.ps1 -Remove -Exe "C:\path\to\your.exe"' -ForegroundColor Yellow
    }
    exit 0
}

# --- build the new contents ------------------------------------------
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

# The $changed flag above only means "the hook list was rebuilt", which on an
# install is always true, so it can never stop a needless write on its own.
# Compare the finished text against what is on disk instead.
if ($null -ne $original -and $original -eq $text) {
    Write-Host ""
    Write-Host "Nothing to change: $cfg already matches. File untouched." -ForegroundColor Yellow
    exit 0
}

# --- write it back ---------------------------------------------------
if ($null -ne $original) {
    # Keep the FIRST backup forever: re-running this used to overwrite it with
    # the already-modified file, so the pristine original was lost. Backups are
    # only taken when the file is really about to change, so repeated runs no
    # longer leave a pile of .bak files in the user's .claude folder.
    $backup = "$cfg.backup-traffic-light"
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $cfg -Destination $backup
        Write-Host "Original backed up to $backup" -ForegroundColor DarkGray
    } else {
        $stamped = "$cfg." + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".bak"
        Copy-Item -LiteralPath $cfg -Destination $stamped
        Write-Host "Backup of the current file: $stamped" -ForegroundColor DarkGray
        Write-Host "(the untouched original is still at $backup)" -ForegroundColor DarkGray
    }
}

[System.IO.File]::WriteAllText($cfg, $text, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ""
if ($Remove) {
    Write-Host "Done: traffic light hooks removed from $cfg" -ForegroundColor Yellow
} else {
    Write-Host "Done: hooks installed in $cfg" -ForegroundColor Green
    Write-Host "Start a NEW Claude Code session for them to take effect." -ForegroundColor Green
}
