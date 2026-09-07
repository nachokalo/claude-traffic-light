<#
  Claude Traffic Light - self-check
  ---------------------------------
  Runs the whole tool against itself and prints a PASS/FAIL table, so
  "it works" is something you can see rather than something you are told.

  It checks the parts that only a real Windows machine can answer: the tray
  icon, the overlay appearing when you leave the Claude window, DPI scaling
  and a second monitor. Those need your eyes, so the script asks you a few
  yes/no questions at the end. Everything else is automatic.

  Usage:
    powershell -ExecutionPolicy Bypass -File verify.ps1
    powershell -ExecutionPolicy Bypass -File verify.ps1 -Quick     # skip the questions
    powershell -ExecutionPolicy Bypass -File verify.ps1 -Exe "C:\Tools\ClaudeTrafficLight\claude-traffic-light.exe"

  It never touches your real %USERPROFILE%\.claude\settings.json: the hook
  installer is exercised inside a throwaway folder under %TEMP%.
#>

param(
    [switch]$Quick,
    [string]$Exe,
    [int]$Port = 8787
)

# Deliberately NOT 'Stop': this is a diagnostic. If one check blows up in a way
# that was not anticipated, the remaining checks still have to run - a report
# that stops at the first surprise is the least useful kind.
$ErrorActionPreference = 'Continue'
$script:pass = 0
$script:fail = 0
$script:skip = 0
$script:failed = @()

function Section($t) {
    Write-Host ""
    Write-Host "-- $t " -ForegroundColor Cyan -NoNewline
    Write-Host ("-" * [Math]::Max(0, 62 - $t.Length)) -ForegroundColor DarkGray
}

function Ok($name, $detail) {
    $script:pass++
    Write-Host "  PASS  " -ForegroundColor Green -NoNewline
    Write-Host $name -NoNewline
    if ($detail) { Write-Host "   $detail" -ForegroundColor DarkGray } else { Write-Host "" }
}

function No($name, $detail) {
    $script:fail++
    $script:failed += $name
    Write-Host "  FAIL  " -ForegroundColor Red -NoNewline
    Write-Host $name -NoNewline
    if ($detail) { Write-Host "   $detail" -ForegroundColor Yellow } else { Write-Host "" }
}

function Skipped($name, $why) {
    $script:skip++
    Write-Host "  SKIP  " -ForegroundColor DarkYellow -NoNewline
    Write-Host $name -NoNewline
    Write-Host "   $why" -ForegroundColor DarkGray
}

function Check($name, $condition, $detail) {
    if ($condition) { Ok $name $detail } else { No $name $detail }
}

# ---------------------------------------------------------------- exe
Section "The executable"

if (-not $Exe) {
    $root = $PSScriptRoot
    foreach ($c in @(
        (Join-Path $root 'claude-traffic-light.exe'),
        (Join-Path $root 'dist\claude-traffic-light.exe'))) {
        if (Test-Path -LiteralPath $c -PathType Leaf) { $Exe = (Resolve-Path -LiteralPath $c).Path; break }
    }
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Host "  Could not find claude-traffic-light.exe. Pass -Exe <full path>." -ForegroundColor Red
    exit 2
}
Ok "found the executable" $Exe
$len = (Get-Item -LiteralPath $Exe).Length
Check "it is a plausible size (20 KB - 2 MB)" ($len -gt 20000 -and $len -lt 2000000) "$len bytes"

$startedByUs = $false
$already = Get-Process -Name 'claude-traffic-light' -ErrorAction SilentlyContinue
if ($already) {
    Ok "an instance is already running" "pid $($already[0].Id) - it will be left running"
} else {
    Start-Process -FilePath $Exe | Out-Null
    Start-Sleep -Seconds 2
    $startedByUs = $true
    $p = Get-Process -Name 'claude-traffic-light' -ErrorAction SilentlyContinue
    Check "it starts" ($null -ne $p) $(if ($p) { "pid $($p[0].Id)" } else { "no process appeared" })
}

$proc = Get-Process -Name 'claude-traffic-light' -ErrorAction SilentlyContinue
if ($proc) {
    $mb = [Math]::Round($proc[0].WorkingSet64 / 1MB, 1)
    Check "memory stays small (under 25 MB)" ($proc[0].WorkingSet64 -lt 25MB) "$mb MB"
}

# ------------------------------------------------------------ raw HTTP
# A raw socket, so headers can be set exactly - Invoke-WebRequest will not
# let you send a bogus Origin or an incomplete request.
function Raw([string]$requestText, [int]$timeoutMs = 4000) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect('127.0.0.1', $Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($timeoutMs)) { return $null }
        $c.EndConnect($iar)
        $s = $c.GetStream()
        $s.ReadTimeout = $timeoutMs
        $b = [System.Text.Encoding]::ASCII.GetBytes($requestText)
        $s.Write($b, 0, $b.Length); $s.Flush()
        $buf = New-Object byte[] 4096
        $n = 0
        try { $n = $s.Read($buf, 0, $buf.Length) } catch { return '' }
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    } catch { return $null } finally { $c.Close() }
}

function Req([string]$path, [string[]]$headers) {
    $t = "GET $path HTTP/1.1`r`nHost: 127.0.0.1`r`n"
    foreach ($h in $headers) { $t += "$h`r`n" }
    return (Raw ($t + "`r`n"))
}

Section "The local server (127.0.0.1:$Port)"

$r = Req '/state?s=done' @()
Check "it answers" ($null -ne $r -and $r.Length -gt 0) $(if ($r) { ($r -split "`r`n")[0] } else { "no answer - is the port taken?" })

if ($null -ne $r -and $r.Length -gt 0) {
    Check "sets waiting (red)"   ((Req '/state?s=waiting' @()) -match 'ok') ""
    Check "sets running (yellow)" ((Req '/state?s=running&w=3000' @()) -match 'ok') ""
    Check "sets done (green)"     ((Req '/state?s=done' @()) -match 'ok') ""
    Check "accepts the Spanish aliases" ((Req '/state?s=amarillo' @()) -match 'ok') "s=amarillo"

    $r2 = Req '/state?s=running' @('Origin: https://claude.ai')
    Check "accepts Origin: https://claude.ai" ($r2 -match 'ok') ""

    # The server answers ok to everything on purpose - a probing page learns
    # nothing from the reply - so a rejected request cannot be told apart here
    # by its answer. Whether it was really ignored is the eye check at the end.
    $r3 = Req '/state?s=running' @('Origin: https://claude.ai.evil.com')
    Check "a look-alike domain does not error out" ($null -ne $r3) "claude.ai.evil.com (ignored, see the eye check below)"

    $bad = "GET /state?s=running&pad=" + ("0" * 4200) + " HTTP/1.1`r`nHost: 127.0.0.1`r`n"
    $r4 = Raw $bad
    Check "an over-long request is rejected with 400" ($r4 -match '400') $(if ($r4) { ($r4 -split "`r`n")[0] } else { "no answer" })

    $r5 = Raw "OPTIONS /state?s=running HTTP/1.1`r`nHost: 127.0.0.1`r`n`r`n"
    Check "an OPTIONS preflight is answered" ($null -ne $r5 -and $r5.Length -gt 0) ""

    $r6 = Req '/state?w=5000' @()
    Check "w on its own refreshes the watchdog" ($r6 -match 'ok') ""
} else {
    Skipped "the rest of the server checks" "nothing is listening on $Port"
    Write-Host "        If the app is running, something else has taken the port." -ForegroundColor DarkGray
    Write-Host "        Change 'port' in traffic-light.ini and PORT in the userscript." -ForegroundColor DarkGray
}

# ------------------------------------------------------------- the CLI
Section "The command line"

function RunExe([string[]]$a) {
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $a -Wait -PassThru
        return $p.ExitCode
    } catch {
        return -999   # could not even launch it; the Check below will say so
    }
}

Check "--state done exits 0"        ((RunExe @('--state','done')) -eq 0) ""
Check "-s running exits 0"          ((RunExe @('-s','running')) -eq 0) ""
Check "--state with no value exits 1" ((RunExe @('--state')) -eq 1) "and must NOT open a second window"
Check "--state banana exits 1"      ((RunExe @('--state','banana')) -eq 1) ""
Check "an unknown flag exits 1"     ((RunExe @('--not-a-flag')) -eq 1) ""
Check "--show exits 0"              ((RunExe @('--show')) -eq 0) ""

$n = @(Get-Process -Name 'claude-traffic-light' -ErrorAction SilentlyContinue).Count
Check "still exactly one instance" ($n -eq 1) "$n running"

Start-Process -FilePath $Exe | Out-Null
Start-Sleep -Seconds 2
$n2 = @(Get-Process -Name 'claude-traffic-light' -ErrorAction SilentlyContinue).Count
Check "launching it twice does not start a second copy" ($n2 -eq 1) "$n2 running"

# ------------------------------------------------------- hook installer
Section "The Claude Code hook installer"

$ps1 = Join-Path $PSScriptRoot 'claude-code\install-hooks.ps1'
if (-not (Test-Path -LiteralPath $ps1 -PathType Leaf)) {
    Skipped "installer checks" "claude-code\install-hooks.ps1 not next to this script"
} else {
    $sandbox = Join-Path $env:TEMP ("ctl-verify-" + [Guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $sandbox | Out-Null

    # NOT $home / $args: both are automatic variables in PowerShell and
    # assigning to them fails at run time even though the file parses fine.
    function Inst([string]$profileDir, [string[]]$extra) {
        $psArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-Command',
                    "`$env:USERPROFILE='$profileDir'; & '$ps1' $($extra -join ' ') -Exe '$Exe'; exit `$LASTEXITCODE")
        $out = & powershell @psArgs 2>&1
        return @{ text = ($out -join "`n"); code = $LASTEXITCODE }
    }

    # 1) fresh install
    $h1 = Join-Path $sandbox 'h1'
    $r = Inst $h1 @()
    $cfg1 = Join-Path $h1 '.claude\settings.json'
    Check "installs into a fresh profile" (Test-Path -LiteralPath $cfg1) ""

    if (Test-Path -LiteralPath $cfg1) {
        $bytes = [System.IO.File]::ReadAllBytes($cfg1)
        Check "writes UTF-8 with no BOM" (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) "a BOM would break Claude Code"

        $j = Get-Content -LiteralPath $cfg1 -Raw | ConvertFrom-Json
        $evs = @('SessionStart','UserPromptSubmit','PreToolUse','PostToolUse','Notification','Stop')
        $missing = @($evs | Where-Object { $null -eq $j.hooks.$_ })
        Check "all six hooks are present" ($missing.Count -eq 0) $(if ($missing) { "missing: $($missing -join ', ')" } else { "" })
        Check "PreToolUse carries matcher *" ($j.hooks.PreToolUse[0].matcher -eq '*') ""

        # 2) idempotent
        $before = Get-Content -LiteralPath $cfg1 -Raw
        $r2 = Inst $h1 @()
        $after = Get-Content -LiteralPath $cfg1 -Raw
        Check "running it again changes nothing" ($before -eq $after) $(if ($before -ne $after) { "the file was rewritten" } else { "" })
        Check "and it says so" ($r2.text -match 'Nothing to change') ""

        for ($i = 0; $i -lt 3; $i++) { Inst $h1 @() | Out-Null }
        $files = @(Get-ChildItem -LiteralPath (Join-Path $h1 '.claude') -File).Count
        Check "repeated runs do not pile up .bak files" ($files -eq 1) "$files file(s) in .claude"

        # Re-read: $j was parsed before those extra runs, so counting hooks in
        # it would have proved nothing at all.
        $jAfter = Get-Content -LiteralPath $cfg1 -Raw | ConvertFrom-Json
        $dup = @($jAfter.hooks.Stop).Count
        Check "five runs still leave one hook per event" ($dup -eq 1) "$dup entries under Stop"
    }

    # 3) foreign hooks survive
    $h2 = Join-Path $sandbox 'h2'
    New-Item -ItemType Directory -Path (Join-Path $h2 '.claude') -Force | Out-Null
    $foreign = '{"model":"opus","hooks":{"Stop":[{"hooks":[{"type":"command","command":"echo other-tool"}]}],"SessionEnd":[{"hooks":[{"type":"command","command":"echo bye"}]}]}}'
    [System.IO.File]::WriteAllText((Join-Path $h2 '.claude\settings.json'), $foreign, (New-Object System.Text.UTF8Encoding($false)))
    Inst $h2 @() | Out-Null
    $j2 = Get-Content -LiteralPath (Join-Path $h2 '.claude\settings.json') -Raw | ConvertFrom-Json
    Check "other settings are preserved" ($j2.model -eq 'opus') ""
    Check "another tool's hooks are preserved" ($null -ne $j2.hooks.SessionEnd) ""
    Check "its Stop hook is kept alongside ours" (@($j2.hooks.Stop).Count -eq 2) "$(@($j2.hooks.Stop).Count) entries"

    # 4) -Remove
    Inst $h2 @('-Remove') | Out-Null
    $j3 = Get-Content -LiteralPath (Join-Path $h2 '.claude\settings.json') -Raw | ConvertFrom-Json
    Check "-Remove takes out only ours" (@($j3.hooks.Stop).Count -eq 1 -and $null -ne $j3.hooks.SessionEnd) ""
    $r4 = Inst $h2 @('-Remove')
    Check "-Remove twice is a no-op" ($r4.text -match 'Nothing to change') ""

    # 5) broken JSON
    $h3 = Join-Path $sandbox 'h3'
    New-Item -ItemType Directory -Path (Join-Path $h3 '.claude') -Force | Out-Null
    $broken = '{ "hooks": {},,, }'
    [System.IO.File]::WriteAllText((Join-Path $h3 '.claude\settings.json'), $broken, (New-Object System.Text.UTF8Encoding($false)))
    $r5 = Inst $h3 @()
    $still = Get-Content -LiteralPath (Join-Path $h3 '.claude\settings.json') -Raw
    Check "invalid JSON is refused, not mangled" ($r5.code -eq 1 -and $still -eq $broken) "exit $($r5.code)"

    # 6) hooks of the wrong shape
    $h4 = Join-Path $sandbox 'h4'
    New-Item -ItemType Directory -Path (Join-Path $h4 '.claude') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $h4 '.claude\settings.json'), '{"hooks":["nope"]}', (New-Object System.Text.UTF8Encoding($false)))
    $r6 = Inst $h4 @()
    Check "a hooks entry of the wrong shape is refused" ($r6.code -eq 1) "exit $($r6.code)"

    # 7) bad -Exe
    $psArgs7 = @('-NoProfile','-ExecutionPolicy','Bypass','-Command',
                 "`$env:USERPROFILE='$sandbox\h9'; & '$ps1' -Exe 'C:\definitely\not\here.exe'; exit `$LASTEXITCODE")
    & powershell @psArgs7 2>&1 | Out-Null
    Check "a wrong -Exe path exits 1" ($LASTEXITCODE -eq 1) "exit $LASTEXITCODE"

    Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
    Ok "the sandbox was cleaned up" "your real settings.json was never touched"
}

# ------------------------------------------------------------ the files
Section "The files in this folder"

$ini = Join-Path $PSScriptRoot 'traffic-light.ini'
if (Test-Path -LiteralPath $ini) {
    $keys = @('position','margin','size_pct','vertical_pct','duration_ms','fade_in_ms',
              'fade_out_ms','opacity','show_when_focused','show_on_blur','title_match','port')
    $txt = Get-Content -LiteralPath $ini -Raw
    $absent = @($keys | Where-Object { $txt -notmatch "(?m)^\s*$_\s*=" })
    Check "traffic-light.ini documents every setting" ($absent.Count -eq 0) $(if ($absent) { "missing: $($absent -join ', ')" } else { "$($keys.Count) settings" })
} else { Skipped "traffic-light.ini" "not next to this script" }

$hx = Join-Path $PSScriptRoot 'claude-code\hooks-example.json'
if (Test-Path -LiteralPath $hx) {
    $bad = $false
    try { $null = Get-Content -LiteralPath $hx -Raw | ConvertFrom-Json } catch { $bad = $true }
    Check "hooks-example.json is valid JSON" (-not $bad) ""
} else { Skipped "hooks-example.json" "not next to this script" }

$us = Join-Path $PSScriptRoot 'browser\claude-traffic-light.user.js'
if (Test-Path -LiteralPath $us) {
    $u = Get-Content -LiteralPath $us -Raw
    Check "the userscript targets claude.ai" ($u -match '@match\s+https://claude\.ai/\*') ""
    Check "it is allowed to reach 127.0.0.1" ($u -match '@connect\s+127\.0\.0\.1') ""
    $m = [regex]::Match($u, 'const\s+PORT\s*=\s*(\d+)')
    if ($m.Success) {
        Check "its port matches this check ($Port)" ([int]$m.Groups[1].Value -eq $Port) "userscript uses $($m.Groups[1].Value)"
    }
} else { Skipped "the userscript" "not next to this script" }

# --------------------------------------------------------- your eyes
if ($Quick) {
    Section "What only you can see"
    Skipped "the visual checks" "-Quick was passed"
} else {
    Section "What only you can see"
    Write-Host "  Six quick questions. Answer y or n." -ForegroundColor DarkGray
    Write-Host ""

    function Ask($name, $instruction) {
        Write-Host "  $instruction" -ForegroundColor White
        $a = Read-Host "    ...did that happen? (y/n/s to skip)"
        if ($a -match '^[sS]') { Skipped $name "you skipped it"; Write-Host ""; return }
        Check $name ($a -match '^[yY]') ""
        Write-Host ""
    }

    Req '/state?s=waiting' @() | Out-Null
    Ask "the light appears, and it is RED" "A red traffic light should have just appeared at the edge of the screen."

    Req '/state?s=running' @() | Out-Null
    Ask "it turns YELLOW" "It should now be yellow."

    Req '/state?s=done' @() | Out-Null
    Ask "it turns GREEN and fades on its own" "It should be green, and disappear by itself after a few seconds."

    Ask "the tray icon is there and matches the colour" "Look next to the clock: there should be a small traffic light icon."

    Write-Host "  Now open Claude (the app, or a claude.ai tab) and click on it." -ForegroundColor White
    Read-Host  "    press Enter when Claude has the focus"
    Req '/state?s=running' @() | Out-Null
    Ask "it stays hidden while you are IN Claude" "Nothing should have appeared."

    Write-Host "  Now click on any other window (this one counts)." -ForegroundColor White
    Read-Host  "    press Enter after switching away from Claude"
    Ask "it appears when you leave Claude, ON TOP of everything" "The light should have appeared without you having to minimise anything."

    # This is the one attack whose effect is only visible on screen: the reply
    # is "ok" either way, so the question is whether the light actually moved.
    Req '/state?s=done' @() | Out-Null
    Start-Sleep -Milliseconds 500
    Req '/state?s=waiting' @('Origin: https://claude.ai.evil.com') | Out-Null
    Start-Sleep -Milliseconds 800
    Write-Host "  A fake site just asked it to turn red." -ForegroundColor White
    Ask "a foreign site cannot change the light" "It should have stayed GREEN (or stayed off) - NOT turned red."

    $mons = @()
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $mons = @([System.Windows.Forms.Screen]::AllScreens)
    } catch { $mons = @() }
    if ($mons.Count -gt 1) {
        Req '/state?s=waiting' @() | Out-Null
        Ask "on $($mons.Count) monitors it lands fully inside one screen" "It should not be cut in half or off the edge."
    } else {
        Skipped "second monitor" "you only have one screen connected"
    }

    $dpi = 96
    try {
        $dpi = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction SilentlyContinue).LogPixels
        if (-not $dpi) { $dpi = 96 }
    } catch {}
    if ($dpi -ne 96) {
        Ask "at $([Math]::Round($dpi/96*100))% scaling it looks the right size" "Not tiny, not blurry, not gigantic."
    } else {
        Skipped "DPI scaling" "your display is at 100%"
    }
}

# ------------------------------------------------------------- summary
Write-Host ""
Write-Host ("=" * 68) -ForegroundColor DarkGray
Write-Host "  PASS $script:pass" -ForegroundColor Green -NoNewline
Write-Host "   FAIL $script:fail" -ForegroundColor $(if ($script:fail) { 'Red' } else { 'DarkGray' }) -NoNewline
Write-Host "   SKIP $script:skip" -ForegroundColor DarkYellow
Write-Host ("=" * 68) -ForegroundColor DarkGray

if ($script:fail -gt 0) {
    Write-Host ""
    Write-Host "  What failed:" -ForegroundColor Red
    foreach ($f in $script:failed) { Write-Host "    - $f" -ForegroundColor Red }
    Write-Host ""
    Write-Host "  Open an issue with this list and it can be looked at:" -ForegroundColor DarkGray
    Write-Host "  https://github.com/nachokalo/claude-traffic-light/issues" -ForegroundColor DarkGray
} else {
    Write-Host ""
    Write-Host "  Everything checked is working." -ForegroundColor Green
}

if ($startedByUs) {
    Write-Host ""
    Write-Host "  (this check started the app; it is still running in the tray)" -ForegroundColor DarkGray
}
Write-Host ""
exit $(if ($script:fail -gt 0) { 1 } else { 0 })
