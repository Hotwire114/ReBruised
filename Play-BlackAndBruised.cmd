@echo off
rem Black & Bruised - native PC recompilation (development build). Plays the program built by tools\build_program.ps1.
rem Uses the disc image chosen in the launcher (GAME DISC); nothing from the game ships with ReBruised.
rem Settings: settings.ini next to this file (BlackAndBruised-Launcher.cmd edits it; environment variables override it).
rem Controller: any XInput pad (Xbox layout: A = Cross, B = Circle, X = Square, Y = Triangle).
rem Keyboard: W/A/S/D move, Down/X/Space = Cross, Right/C/Esc = Circle, Left/Z = Square, Up/V = Triangle,
rem           Q/E = L1/R1, Left/Right Shift = L2/R2, Enter = Start, Tab = Select.
rem F11 or Alt+Enter: fullscreen. F9: widescreen on/off. F1: runtime debug panel. Logs: logs\play.log / logs\play.err.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
"%ROOT%build\program\bin\BlackAndBruised.exe" > "%ROOT%logs\play.log" 2> "%ROOT%logs\play.err"
echo Black ^& Bruised exited with code %errorlevel% (logs\play.err)
