@echo off
set "SHADPS4_DMC_COMPAT=1"
set "SHADPS4_DMC_JC_COMPAT=0"
start "" "%~dp0shadPS4.exe" %*
