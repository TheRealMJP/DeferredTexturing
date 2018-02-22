@echo off

IF EXIST %1\..\Externals\DXCompiler\Bin\dxcompiler.dll (
    copy %1\..\Externals\DXCompiler\Bin\dxcompiler.dll %2\dxcompiler.dll
) ELSE (
    copy %3\dxcompiler.dll %2\dxcompiler.dll
)

copy %3\dxil.dll %2\dxil.dll