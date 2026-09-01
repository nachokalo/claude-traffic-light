@echo off
rem Build with MSVC. Run from a "Developer Command Prompt for VS".
cl /O2 /DUNICODE /D_UNICODE src\traffic-light.c /Fe:claude-traffic-light.exe ^
   /link /SUBSYSTEM:WINDOWS ws2_32.lib shell32.lib gdi32.lib user32.lib advapi32.lib
