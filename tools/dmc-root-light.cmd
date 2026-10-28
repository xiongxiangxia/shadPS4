@echo off
set "SHADPS4_DMC_NATIVE=1"
set "SHADPS4_DMC_TRACE=light"
start "" "%~dp0shadPS4.exe" %*
