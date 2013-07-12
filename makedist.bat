@echo off

SETLOCAL ENABLEEXTENSIONS
IF NOT EXIST set_ddk_path.bat ECHO >set_ddk_path.bat SET DDK_PATH=C:\WinDDK\7600.16385.1
rem IF NOT EXIST set_ddk_path_2k.bat ECHO >set_ddk_path_2k.bat SET DDK_PATH_2K=C:\WinDDK\6001.18002

FOR /F %%V IN (version) DO SET VERSION=%%V

SET /A NEW_BUILD_NUMBER=%BUILD_NUMBER%+1
ECHO >build_number.bat SET BUILD_NUMBER=%NEW_BUILD_NUMBER%

ECHO BUILDING %VERSION%

CALL set_ddk_path.bat
rem CALL set_ddk_path_2K.bat

SET SIGNTOOL=%DDK_PATH%\bin\x86\signtool.exe
IF NOT EXIST %SIGNTOOL% SET SIGNTOOL=%DDK_PATH%\bin\selfsign\signtool.exe

SET CERT_FILENAME=
SET CERT_PASSWORD=
SET CERT_CROSS_CERT_FILENAME=
SET CERT_PUBLIC_FILENAME=
IF NOT EXIST SIGN_CONFIG.BAT GOTO DONT_SIGN
CALL SIGN_CONFIG.BAT
SET CERT_CROSS_CERT_FLAG=
SET CERT_PASSWORD_FLAG=
IF DEFINED CERT_CROSS_CERT_FILENAME SET CERT_CROSS_CERT_FLAG=/ac %CERT_CROSS_CERT_FILENAME%
IF DEFINED CERT_PASSWORD SET CERT_PASSWORD_FLAG=-p %CERT_PASSWORD%
IF EXIST %CERT_FILENAME% GOTO :DONT_SIGN
"%DDK_PATH%"\bin\x86\makecert -r -pe -ss PrivateCertStore -n "CN=GPLPV Test Cert" %CERT_PUBLIC_FILENAME%
certutil -exportpfx -user -privatekey %CERT_PASSWORD_FLAG% PrivateCertStore "GPLPV Test Cert" "%CERT_FILENAME%
:DONT_SIGN

powershell -ExecutionPolicy RemoteSigned -File set_version.ps1

rem cmd /C "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WIN7 && popd && build -cZg && sign.bat && call wix.bat"

cmd /C "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk x64 WIN7 && popd && build -cZg && sign.bat && call wix.bat"

rmdir /S /Q gui-agent\bin-debug qvideo\bin-debug misc\qvcontrol\bin-debug
move gui-agent\bin gui-agent\bin-debug
move qvideo\bin qvideo\bin-debug
move misc\qvcontrol\bin misc\qvcontrol\bin-debug

rem cmd /C "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WIN7 && popd && build -cZg && sign.bat && call wix.bat"

cmd /C "pushd . && %DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre x64 WIN7 && popd && build -cZg && sign.bat && call wix.bat"
