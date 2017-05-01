//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "Utility.h"
#include "Exceptions.h"
#include "App.h"

namespace SampleFramework12
{

void WriteLog(const wchar* format, ...)
{
    wchar buffer[1024] = { 0 };
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, ArraySize_(buffer), format, args);
    if(GlobalApp != nullptr)
        GlobalApp->AddToLog(WStringToAnsi(buffer).c_str());

    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
}

void WriteLog(const char* format, ...)
{
    char buffer[1024] = { 0 };
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, ArraySize_(buffer), format, args);
    if(GlobalApp != nullptr)
        GlobalApp->AddToLog(buffer);

    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

std::wstring MakeString(const wchar* format, ...)
{
    wchar buffer[1024] = { 0 };
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, ArraySize_(buffer), format, args);
    return std::wstring(buffer);
}

std::string MakeString(const char* format, ...)
{
    char buffer[1024] = { 0 };
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, ArraySize_(buffer), format, args);
    return std::string(buffer);
}

std::wstring SampleFrameworkDir()
{
    return std::wstring(SampleFrameworkDir_);
}

}