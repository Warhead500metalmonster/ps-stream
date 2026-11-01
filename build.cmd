@echo off
set MSYSTEM=MINGW64
set "PS_DIR=%~dp0"
C:\msys64\usr\bin\bash.exe -lc "cd \"$(cygpath -u \"$PS_DIR\")\" && ./build.sh"
echo.
if errorlevel 1 (echo Derleme basarisiz oldu.) else (echo Tamam. dist klasorundeki "PS Stream.exe" dosyasini ac.)
pause
