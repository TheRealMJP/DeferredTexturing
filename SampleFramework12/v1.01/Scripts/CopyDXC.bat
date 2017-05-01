@echo off

xcopy %HLSL_SRC_DIR%\Include\dxc\dxcapi.h %~dp0..\..\..\Externals\DXCompiler\Include\ /y
xcopy %HLSL_BLD_DIR%\Release\lib\dxcompiler.lib %~dp0..\..\..\Externals\DXCompiler\Lib\ /y
xcopy %HLSL_BLD_DIR%\Release\bin\dxcompiler.* %~dp0..\..\..\Externals\DXCompiler\Bin\ /y
