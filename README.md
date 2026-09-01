# Claude Traffic Light

A tiny traffic light that tells you what Claude is doing, so you can go do
something else while it works.

It slides in at the edge of your screen the moment you switch away from Claude,
shows the current state, and fades out on its own. It comes back whenever the
state changes.

<p align="center">
  <img src="docs/images/states.png" width="620" alt="The three states: red, yellow, green">
</p>

| Light | Meaning |
| --- | --- |
| 🔴 **Red** | Claude is waiting for you to confirm something |
| 🟡 **Yellow** | Claude is working |
| 🟢 **Green** | Done — ready for a new task |

Works with **Claude Code** (via hooks) and with **claude.ai in the browser**
(via a userscript). You can run both at once.

---

## Why

If you leave a long task running and switch to another window, you have no idea
whether Claude finished, is still thinking, or has been sitting there for ten
minutes waiting for you to approve a file write. This is a glanceable answer
that lives outside the terminal.

The idea comes from people wiring a physical traffic light to their desk. This
is the same thing without the hardware.

## Design goals

- **Native.** Plain Win32 in C. No Electron, no Python, no .NET, no runtime to
  install. One `.exe`, about 40 KB, ~3 MB of RAM.
- **Idle means idle.** No polling loop. It sleeps on a system event
  (`EVENT_SYSTEM_FOREGROUND`) and only draws while the light is on screen.
  0% CPU the rest of the time.
- **Never in your way.** The window is click-through (`WS_EX_TRANSPARENT`) and
  never takes focus, so your mouse and keyboard behave as if it weren't there.
- **Local only.** The HTTP listener binds to `127.0.0.1` and nothing is ever
  sent anywhere. The only thing it reads from your system is the title of the
  active window, to know whether you are looking at Claude.

---

## Install

**Requirements:** Windows 10 or 11, 64-bit.

1. Download `claude-traffic-light.exe` from
   [Releases](../../releases) (or from the `dist/` folder of this repo) and put
   it in a folder you'll keep, e.g. `C:\Tools\ClaudeTrafficLight`.
2. Drop `traffic-light.ini` next to it if you want to change the defaults.
   It's optional — without it the built-in defaults apply.
3. Double-click the `.exe`.

The light flashes once so you know it started, then hides. A coloured dot
appears in the notification area next to the clock. Right-click it for:

- **Show now**
- **Test red / yellow / green**
- **Start with Windows**
- **Exit**

> Windows SmartScreen will warn you about an unknown publisher — the binary
> isn't code-signed. Click *More info → Run anyway*, or
> [build it yourself](#building-from-source) if you'd rather not trust a
> stranger's binary.

At this point the light already reacts when you leave the Claude window. To
make it reflect Claude's actual state, connect one or both of the following.

---

## Connect it to Claude Code

Run the installer from the `claude-code` folder:

```powershell
powershell -ExecutionPolicy Bypass -File claude-code\install-hooks.ps1
```

or just double-click `claude-code\install-hooks.cmd`.

It edits `%USERPROFILE%\.claude\settings.json`, keeps a backup next to it, and
leaves any hooks you already had from other tools alone. **Open a new Claude
Code session afterwards** — hooks are read at startup.

The mapping it installs:

| Claude Code hook | State |
| --- | --- |
| `SessionStart`, `Stop` | 🟢 green |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | 🟡 yellow |
| `Notification` | 🔴 red |

To remove them: `install-hooks.ps1 -Remove` (or `uninstall-hooks.cmd`).
`claude-code/hooks-example.json` has the same block if you'd rather paste it by
hand.

## Connect it to claude.ai in the browser

**Option A — userscript** (runs by itself, recommended)

1. Install [Tampermonkey](https://www.tampermonkey.net/).
   On Edge you must also enable **Allow user scripts** in
   `edge://extensions/` → Tampermonkey → *Details*.
2. Tampermonkey → *Create a new script* → replace everything with the contents
   of `browser/claude-traffic-light.user.js` → save.
3. Reload claude.ai.

**Option B — bookmarklet** (nothing to install, useful on locked-down machines)

See [`browser/bookmarklet.md`](browser/bookmarklet.md). You click it once per
tab after the page loads.

Browser detection reads the page, so it's less exact than the Claude Code
hooks: yellow and green are reliable, red only shows when a permission dialog
actually appears.

---

## Configuration

`traffic-light.ini`, next to the executable. Restart the app after editing
(tray → Exit, then reopen).

```ini
[traffic-light]
position          = right   ; right, left, top-right, bottom-right, top-left, bottom-left
margin            = 26      ; distance from the screen edge, in pixels
size_pct          = 100     ; 70 = smaller, 140 = bigger
duration_ms       = 4000    ; how long it stays before fading out
fade_in_ms        = 200
fade_out_ms       = 450
opacity           = 240     ; 0-255
show_when_focused = 0       ; 1 = show even while you're looking at Claude
show_on_blur      = 1       ; 1 = show as soon as you leave the Claude window
title_match       = claude  ; lowercase substring that identifies "the Claude window"
port              = 8787    ; local port, 127.0.0.1 only
```

`title_match` is how it recognises the Claude window: it lowercases the active
window's title and looks for this substring. That covers both a terminal
running Claude Code and a `claude.ai` browser tab. If your terminal doesn't put
"claude" in its title, point this at something that is in the title, such as
your project name.

The size scales with your display's DPI on top of `size_pct`, so it stays the
same physical size across monitors.

---

## Driving it from anything else

It isn't Claude-specific. Anything that can run a command or make an HTTP
request can drive it — a build script, a test suite, a long deployment.

```bat
claude-traffic-light.exe --state waiting    :: red
claude-traffic-light.exe --state running    :: yellow
claude-traffic-light.exe --state done       :: green
claude-traffic-light.exe --show             :: show without changing state
claude-traffic-light.exe --quit             :: close it
```

```
GET http://127.0.0.1:8787/state?s=running
```

Accepted values: `waiting` / `red`, `running` / `yellow` / `busy`,
`done` / `green` / `idle`. Launching a second instance doesn't start a second
copy — it just shows the running one.

---

## Building from source

One file, no dependencies. With MinGW-w64 (Linux or Windows):

```sh
x86_64-w64-mingw32-gcc -O2 -municode -mwindows -o claude-traffic-light.exe \
    src/traffic-light.c -lws2_32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lm -s
```

With MSVC, from a Developer Command Prompt:

```bat
cl /O2 /DUNICODE /D_UNICODE src\traffic-light.c /Fe:claude-traffic-light.exe ^
   /link /SUBSYSTEM:WINDOWS ws2_32.lib shell32.lib gdi32.lib user32.lib advapi32.lib
```

The traffic light is drawn pixel by pixel into a 32-bit premultiplied BGRA
buffer and blitted with `UpdateLayeredWindow`, so it's a real shaped, alpha
blended window — no image assets, and it stays sharp at any DPI or scale.

## How it decides what to show

- A `SetWinEventHook` on `EVENT_SYSTEM_FOREGROUND` tells the app whenever the
  active window changes. Leaving a window whose title matches `title_match`
  triggers the light.
- State arrives either through `WM_COPYDATA` (the `--state` command line, used
  by the Claude Code hooks) or through the loopback HTTP listener (used by the
  browser userscript).
- A state change shows the light too, unless you're currently looking at
  Claude — configurable with `show_when_focused`.

## Known limitations

- Windows only. The drawing and the window handling are pure Win32.
- Browser state is inferred from the DOM, so a claude.ai redesign can break it.
  The selectors live at the top of the userscript and are easy to patch.
- The binary is unsigned, so expect a SmartScreen prompt on first run.

## License

MIT — see [LICENSE](LICENSE).
