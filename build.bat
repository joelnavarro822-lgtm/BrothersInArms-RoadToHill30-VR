@echo off
setlocal
call "%~dp0build_env.bat"
cd /d "%~dp0"
if not exist out mkdir out
set MH=third_party\minhook-master
set OVR=third_party\openvr-master
cl /nologo /O2 /MT /EHsc /W3 /LD /D_CRT_SECURE_NO_WARNINGS ^
  /I%MH%\include /I%OVR%\headers ^
  src\bia_vr.cpp %MH%\src\hook.c %MH%\src\buffer.c %MH%\src\trampoline.c %MH%\src\hde\hde32.c ^
  /Foout\ /Feout\d3d9.dll ^
  /link /DEF:src\d3d9.def %OVR%\lib\win32\openvr_api.lib d3d11.lib user32.lib kernel32.lib
