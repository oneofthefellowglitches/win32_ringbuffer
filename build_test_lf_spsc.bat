:: build_test_lf_spsc.bat
@echo off
call loadenv.bat
set PATH=%llvm_path%;%PATH%

call %msvcl_build_path%\vcvarsall.bat x64
if not exist build\msvcl_x64 mkdir build\msvcl_x64 
pushd build\msvcl_x64
cl.exe ..\..\test_lf_spsc.c /O2 /Oi ^
  /OUT:test_lf_spsc_defaultlib.exe

cl.exe ..\..\test_lf_spsc.c /O2 /Oi /link ^
  /nodefaultlib:libcmt.lib ^
  /OUT:test_lf_spsc_crtfree.exe

popd

pause
::/permissive-  Standard conformance mode (Turn this on instead of /Za). It is much smarter and won't break windows.h.
::/GS-  Disables stack security cookies (Required).
::/Oi Enables intrinsics (Helps the compiler replace some CRT calls with CPU instructions).
::/GR-  Disables RTTI (Run-Time Type Information).
::/EHa- Disables C++ Exception Handling.
::/NODEFAULTLIB The "Nuclear Option" that removes the CRT entirely.
