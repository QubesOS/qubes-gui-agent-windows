@echo off
set csc=%SystemRoot%\Microsoft.NET\Framework\v2.0.50727\csc.exe
%csc% /t:exe /out:test\pattern-generator\pattern-generator.exe test\pattern-generator\pattern-generator.cs
