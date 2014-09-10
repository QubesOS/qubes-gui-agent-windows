@echo off

cd hook-dll
copy /y sources32 sources
cmd /c "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH% %WIN_BUILD_TYPE% x86 WIN7 no_oacr && popd && build -cgZ"
copy /y sources64 sources

cd ..\hook-server-32
cmd /c "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH% %WIN_BUILD_TYPE% x86 WIN7 no_oacr && popd && build -cgZ"
