@echo off
setlocal
set PATH=D:\Tools\msys64\ucrt64\bin;%PATH%
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe
cd /d D:\APP\MemTune

echo === [0/5] stop running instances ===
taskkill /f /im MemTune.exe >nul 2>&1
taskkill /f /im FanBridge.exe >nul 2>&1
timeout /t 2 /nobreak >nul

echo === [1/5] resources (manifest + icon) ===
if not exist .tmpobj mkdir .tmpobj
copy /y app.manifest .tmpobj\app.manifest >nul
copy /y memtune.ico .tmpobj\memtune.ico >nul
echo 101 ICON "memtune.ico" > .tmpobj\icon.rc
echo 1 24 "app.manifest" > .tmpobj\app.rc
pushd .tmpobj
windres icon.rc -O coff -o icon_res.o
if errorlevel 1 ( popd & goto fail )
windres app.rc -O coff -o app_res.o
if errorlevel 1 ( popd & goto fail )
> stub.c echo /* stub */
gcc -c stub.c -o stub.o
popd
copy /y .tmpobj\stub.o default-manifest.o >nul

echo === [2/5] FanBridge.dll ===
"%CSC%" /nologo /target:library /out:FanBridge.dll /r:LibreHardwareMonitorLib.dll FanBridge.cs
if errorlevel 1 goto fail

echo === [3/5] FanBridge.exe ===
"%CSC%" /nologo /target:exe /out:FanBridge.exe FanHost.cs
if errorlevel 1 goto fail

echo === [4/5] MemTune.exe ===
gcc MemTune.c default-manifest.o .tmpobj\icon_res.o .tmpobj\app_res.o -o MemTune.exe -O2 -mwindows -static -static-libgcc -lpsapi -lshlwapi -lpdh -lws2_32 -liphlpapi
if errorlevel 1 goto fail

echo === [5/5] verify ===
strings -a MemTune.exe | findstr /c:"PerMonitorV2" >nul && echo DPI manifest OK || echo WARN: DPI manifest missing

echo.
echo BUILD OK
goto end

:fail
echo.
echo BUILD FAILED
:end
endlocal
