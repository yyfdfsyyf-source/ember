@echo off
rem Launch Ember's TUI in a real console window (it needs one: enterRaw() bails
rem without a console). cd first so it reads ./settings.json.
cd /d "%~dp0"
out\agent.exe %*
if errorlevel 1 (
  echo.
  echo [Ember exited with code %errorlevel%. If this window opened and closed instantly,
  echo  it could not get a console - run out\agent.exe from a normal terminal instead.]
  pause
)
