@echo off

xcopy %~dp0..\..\..\..\DirectXShaderCompiler\Include\dxc\dxcapi.h %~dp0..\..\..\Externals\DXCompiler\Include\ /y
xcopy %~dp0..\..\..\..\DirectXShaderCompiler\build\Release\lib\dxcompiler.lib %~dp0..\..\..\Externals\DXCompiler\Lib\ /y
xcopy %~dp0..\..\..\..\DirectXShaderCompiler\build\Release\bin\dxcompiler.* %~dp0..\..\..\Externals\DXCompiler\Bin\ /y
