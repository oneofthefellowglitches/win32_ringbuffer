:: build_test_spsc_ring_win.bat
@echo off
call loadenv.bat

call %msvcl_build_path%\vcvarsall.bat x64
if not exist build\msvcl_x64 mkdir build\msvcl_x64 
pushd build\msvcl_x64

cl /nologo /O2 /W4 /GS- /Zl /TC ..\..\test_spsc_ring_win.c ^
  /link /nologo /NODEFAULTLIB /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup ^
  test_spsc_ring_win.obj kernel32.lib

popd
pause
