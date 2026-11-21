@echo off
rem Black & Bruised launcher: animated front end (Play, Options, Controls). Keyboard, mouse or controller.
rem Falls back to the simple settings window (tools\launcher.pyw) if the front end is not built yet.
if exist "%~dp0build\program\bin\BlackAndBruisedLauncher.exe" (
    start "" "%~dp0build\program\bin\BlackAndBruisedLauncher.exe"
) else (
    start "" "%~dp0python\pythonw.exe" -I "%~dp0tools\launcher.pyw"
)
