@echo off
rem Launch Eden with the ported Inotia 4 NRO. Double-click this if the
rem emulator window is not on screen (agent-launched windows can be reaped
rem when the helper shell exits).
setlocal
set "EDEN=C:\1GAME\BaiduDownload\eden\eden\eden.exe"
set "NRO=%APPDATA%\eden\sdmc\switch\inotia4\inotia4_nx.nro"
if not exist "%EDEN%" (
  echo Eden not found: %EDEN%
  pause
  exit /b 1
)
if not exist "%NRO%" (
  echo NRO not found: %NRO%
  pause
  exit /b 1
)
start "" "%EDEN%" -g "%NRO%"
endlocal
