//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "ShaderCompilation.h"

#include "..\\Utility.h"
#include "..\\Exceptions.h"
#include "..\\InterfacePointers.h"
#include "..\\FileIO.h"
#include "..\\MurmurHash.h"
#include "..\\Containers.h"

using std::vector;
using std::wstring;
using std::string;
using std::map;

namespace SampleFramework12
{

static const char* TypeStrings[] = { "vertex", "hull", "domain", "geometry", "pixel", "compute" };
StaticAssert_(ArraySize_(TypeStrings) == uint64(ShaderType::NumTypes));

static const uint64 TotalNumProfiles = uint64(ShaderType::NumTypes) * uint64(ShaderProfile::NumProfiles);
static const char* ProfileStrings[] =
{
    "vs_5_0", "hs_5_0", "ds_5_0", "gs_5_0", "ps_5_0", "cs_5_0",
    "vs_5_1", "hs_5_1", "ds_5_1", "gs_5_1", "ps_5_1", "cs_5_1",
};

StaticAssert_(ArraySize_(ProfileStrings) == TotalNumProfiles);

static string GetExpandedShaderCode(const wchar* path, GrowableList<wstring>& filePaths)
{
    for(uint64 i = 0; i < filePaths.Count(); ++i)
        if(filePaths[i] == path)
            return string();

    filePaths.Add(path);

    string fileContents = ReadFileAsString(path);

    // Look for includes
    size_t lineStart = 0;
    while(true)
    {
        size_t lineEnd = fileContents.find('\n', lineStart);
        size_t lineLength = 0;
        if(lineEnd == string::npos)
            lineLength = string::npos;
        else
            lineLength = lineEnd - lineStart;

        string line = fileContents.substr(lineStart, lineLength);
        if(line.find("#include") == 0)
        {
            wstring fullIncludePath;
            size_t startQuote = line.find('\"');
            if(startQuote != -1)
            {
                size_t endQuote = line.find('\"', startQuote + 1);
                string includePath = line.substr(startQuote + 1, endQuote - startQuote - 1);
                fullIncludePath = AnsiToWString(includePath.c_str());
            }
            else
            {
                startQuote = line.find('<');
                if(startQuote == -1)
                    throw Exception(L"Malformed include statement: \"" + AnsiToWString(line.c_str()) + L"\" in file " + path);
                size_t endQuote = line.find('>', startQuote + 1);
                string includePath = line.substr(startQuote + 1, endQuote - startQuote - 1);
                fullIncludePath = SampleFrameworkDir() + L"Shaders\\" + AnsiToWString(includePath.c_str());
            }

            if(FileExists(fullIncludePath.c_str()) == false)
                throw Exception(L"Couldn't find #included file \"" + fullIncludePath + L"\" in file " + path);

            string includeCode = GetExpandedShaderCode(fullIncludePath.c_str(), filePaths);
            fileContents.insert(lineEnd + 1, includeCode);
            lineEnd += includeCode.length();
        }

        if(lineEnd == string::npos)
            break;

        lineStart = lineEnd + 1;
    }

    return fileContents;
}

static const wstring baseCacheDir = L"ShaderCache\\";

#if _DEBUG
    static const wstring cacheSubDir = L"Debug\\";
#else
    static const std::wstring cacheSubDir = L"Release\\";
#endif

static const wstring cacheDir = baseCacheDir + cacheSubDir;

static string MakeDefinesString(const D3D_SHADER_MACRO* defines)
{
    string definesString;
    while(defines && defines->Name != nullptr && defines != nullptr)
    {
        if(definesString.length() > 0)
            definesString += "|";
        definesString += defines->Name;
        definesString += "=";
        definesString += defines->Definition;
        ++defines;
    }

    return definesString;
}

static wstring MakeShaderCacheName(const std::string& shaderCode, const char* functionName,
                                   const char* profile, const D3D_SHADER_MACRO* defines)
{
    string hashString = shaderCode;
    hashString += "\n";
    hashString += functionName;
    hashString += "\n";
    hashString += profile;
    hashString += "\n";

    hashString += MakeDefinesString(defines);

    Hash codeHash = GenerateHash(hashString.data(), int(hashString.length()), 0);

    return cacheDir + codeHash.ToString() + L".cache";
}

class FrameworkInclude : public ID3DInclude
{
    HRESULT Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
    {
        std::wstring filePath;
        if(IncludeType == D3D_INCLUDE_LOCAL)
            filePath = AnsiToWString(pFileName);
        else if(IncludeType == D3D_INCLUDE_SYSTEM)
            filePath = SampleFrameworkDir() + L"Shaders\\" + AnsiToWString(pFileName);
        else
            return E_FAIL;

        if(FileExists(filePath.c_str()) == false)
            return E_FAIL;
        File file(filePath.c_str(), FileOpenMode::Read);
        *pBytes = UINT(file.Size());
        uint8* data = reinterpret_cast<uint8*>(std::malloc(*pBytes));
        file.Read(*pBytes, data);
        *ppData = data;
        return S_OK;
    }

    HRESULT Close(LPCVOID pData) override
    {
        std::free(const_cast<void*>(pData));
        return S_OK;
    }
};

static ID3DBlob* CompileShader(const wchar* path, const char* functionName, ShaderType type, ShaderProfile profile,
                              const D3D_SHADER_MACRO* defines, bool forceOptimization, GrowableList<wstring>& filePaths)
{
    if(FileExists(path) == false)
    {
        Assert_(false);
        throw Exception(L"Shader file " + std::wstring(path) + L" does not exist");
    }

    uint64 profileIdx = uint64(profile) * uint64(ShaderType::NumTypes) + uint64(type);
    Assert_(profileIdx < TotalNumProfiles);
    const char* profileString = ProfileStrings[profileIdx];

    // Make a hash off the expanded shader code
    string shaderCode = GetExpandedShaderCode(path, filePaths);
    wstring cacheName = MakeShaderCacheName(shaderCode, functionName, profileString, defines);

    if(FileExists(cacheName.c_str()))
    {
        File cacheFile(cacheName.c_str(), FileOpenMode::Read);

        const uint64 shaderSize = cacheFile.Size();
        Array<uint8> compressedShader;
        compressedShader.Init(shaderSize);
        cacheFile.Read(shaderSize, compressedShader.Data());

        ID3DBlob* decompressedShader[1] = { nullptr };
        uint32 indices[1] = { 0 };
        DXCall(D3DDecompressShaders(compressedShader.Data(), shaderSize, 1, 0,
                                    indices, 0, decompressedShader, nullptr));

        return decompressedShader[0];
    }

    WriteLog("Compiling %s shader %s_%s %s\n", TypeStrings[uint64(type)],
                WStringToAnsi(GetFileName(path).c_str()).c_str(),
                functionName, MakeDefinesString(defines).c_str());

    // Loop until we succeed, or an exception is thrown
    while(true)
    {
        UINT flags = D3DCOMPILE_WARNINGS_ARE_ERRORS;
        #ifdef _DEBUG
            flags |= D3DCOMPILE_DEBUG;
            // This is causing some shader bugs
            /*if(forceOptimization == false)
                flags |= D3DCOMPILE_SKIP_OPTIMIZATION;*/
        #endif

        ID3DBlob* compiledShader;
        ID3DBlobPtr errorMessages;
        FrameworkInclude include;
        HRESULT hr = D3DCompileFromFile(path, defines, &include, functionName,
                                        profileString, flags, 0, &compiledShader, &errorMessages);

        if(FAILED(hr))
        {
            if(errorMessages)
            {
                wchar message[1024] = { 0 };
                char* blobdata = reinterpret_cast<char*>(errorMessages->GetBufferPointer());

                MultiByteToWideChar(CP_ACP, 0, blobdata, static_cast<int>(errorMessages->GetBufferSize()), message, 1024);
                std::wstring fullMessage = L"Error compiling shader file \"";
                fullMessage += path;
                fullMessage += L"\" - ";
                fullMessage += message;

                // Pop up a message box allowing user to retry compilation
                int retVal = MessageBoxW(nullptr, fullMessage.c_str(), L"Shader Compilation Error", MB_RETRYCANCEL);
                if(retVal != IDRETRY)
                    throw DXException(hr, fullMessage.c_str());
            }
            else
            {
                Assert_(false);
                throw DXException(hr);
            }
        }
        else
        {
            // Compress the shader
            D3D_SHADER_DATA shaderData;
            shaderData.pBytecode = compiledShader->GetBufferPointer();
            shaderData.BytecodeLength = compiledShader->GetBufferSize();
            ID3DBlobPtr compressedShader;
            DXCall(D3DCompressShaders(1, &shaderData, D3D_COMPRESS_SHADER_KEEP_ALL_PARTS, &compressedShader));

            // Create the cache directory if it doesn't exist
            if(DirectoryExists(baseCacheDir.c_str()) == false)
                Win32Call(CreateDirectory(baseCacheDir.c_str(), nullptr));

            if(DirectoryExists(cacheDir.c_str()) == false)
                Win32Call(CreateDirectory(cacheDir.c_str(), nullptr));

            File cacheFile(cacheName.c_str(), FileOpenMode::Write);

            // Write the compiled shader to disk
            uint64 shaderSize = compressedShader->GetBufferSize();
            cacheFile.Write(shaderSize, compressedShader->GetBufferPointer());

            return compiledShader;
        }
    }
}

struct ShaderFile
{
    wstring FilePath;
    uint64 TimeStamp;
    GrowableList<CompiledShader*> Shaders;

    ShaderFile(const wstring& filePath) : TimeStamp(0), FilePath(filePath)
    {
    }
};

static GrowableList<ShaderFile*> ShaderFiles;
static GrowableList<CompiledShader*> CompiledShaders;
static SRWLOCK ShaderFilesLock = SRWLOCK_INIT;
static SRWLOCK CompiledShadersLock = SRWLOCK_INIT;

static void CompileShader(CompiledShader* shader)
{
    Assert_(shader != nullptr);

    GrowableList<wstring> filePaths;
    D3D_SHADER_MACRO defines[CompileOptions::MaxDefines + 1];
    shader->CompileOpts.MakeDefines(defines);
    shader->ByteCode = CompileShader(shader->FilePath.c_str(), shader->FunctionName.c_str(),
                                     shader->Type, shader->Profile, defines,
                                     shader->ForceOptimization, filePaths);
    shader->ByteCodeHash = GenerateHash(shader->ByteCode->GetBufferPointer(), int(shader->ByteCode->GetBufferSize()));

    for(uint64 fileIdx = 0; fileIdx < filePaths.Count(); ++ fileIdx)
    {
        const wstring& filePath = filePaths[fileIdx];
        ShaderFile* shaderFile = nullptr;
        const uint64 numShaderFiles = ShaderFiles.Count();
        for(uint64 shaderFileIdx = 0; shaderFileIdx < numShaderFiles; ++shaderFileIdx)
        {
            if(ShaderFiles[shaderFileIdx]->FilePath == filePath)
            {
                shaderFile = ShaderFiles[shaderFileIdx];
                break;
            }
        }
        if(shaderFile == nullptr)
        {
            shaderFile = new ShaderFile(filePath);

            AcquireSRWLockExclusive(&ShaderFilesLock);

            ShaderFiles.Add(shaderFile);

            ReleaseSRWLockExclusive(&ShaderFilesLock);
        }

        bool containsShader = false;
        for(uint64 shaderIdx = 0; shaderIdx < shaderFile->Shaders.Count(); ++shaderIdx)
        {
            if(shaderFile->Shaders[shaderIdx] == shader)
            {
                containsShader = true;
                break;
            }
        }

        if(containsShader == false)
            shaderFile->Shaders.Add(shader);
    }
}

CompiledShaderPtr CompileFromFile(const wchar* path,
                                  const char* functionName,
                                  ShaderType type,
                                  ShaderProfile profile,
                                  const CompileOptions& compileOpts,
                                  bool forceOptimization)
{
    CompiledShader* compiledShader = new CompiledShader(path, functionName, profile, compileOpts, forceOptimization, type);
    CompileShader(compiledShader);

    AcquireSRWLockExclusive(&CompiledShadersLock);

    CompiledShaders.Add(compiledShader);

    ReleaseSRWLockExclusive(&CompiledShadersLock);

    return compiledShader;
}

VertexShaderPtr CompileVSFromFile(const wchar* path,
                                  const char* functionName,
                                  ShaderProfile profile,
                                  const CompileOptions& compileOptions,
                                  bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Vertex, profile, compileOptions, forceOptimization);
}

PixelShaderPtr CompilePSFromFile(const wchar* path,
                                 const char* functionName,
                                 ShaderProfile profile,
                                 const CompileOptions& compileOptions,
                                 bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Pixel, profile, compileOptions, forceOptimization);
}

GeometryShaderPtr CompileGSFromFile(const wchar* path,
                                    const char* functionName,
                                    ShaderProfile profile,
                                    const CompileOptions& compileOptions,
                                    bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Geometry, profile, compileOptions, forceOptimization);
}

HullShaderPtr CompileHSFromFile(const wchar* path,
                                const char* functionName,
                                ShaderProfile profile,
                                const CompileOptions& compileOptions,
                                bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Hull, profile, compileOptions, forceOptimization);
}

DomainShaderPtr CompileDSFromFile(const wchar* path,
                                  const char* functionName,
                                  ShaderProfile profile,
                                  const CompileOptions& compileOptions,
                                  bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Domain, profile, compileOptions, forceOptimization);
}

ComputeShaderPtr CompileCSFromFile(const wchar* path,
                                   const char* functionName,
                                   ShaderProfile profile,
                                   const CompileOptions& compileOptions,
                                   bool forceOptimization)
{
    return CompileFromFile(path, functionName, ShaderType::Compute, profile, compileOptions, forceOptimization);
}

bool UpdateShaders()
{
    uint64 numShaderFiles = ShaderFiles.Count();
    if(numShaderFiles == 0)
        return false;

    static uint64 currFile = 0;
    currFile = (currFile + 1) % uint64(numShaderFiles);

    ShaderFile* file = ShaderFiles[currFile];
    const uint64 newTimeStamp = GetFileTimestamp(file->FilePath.c_str());
    if(file->TimeStamp == 0)
    {
        file->TimeStamp = newTimeStamp;
        return false;
    }

    if(file->TimeStamp < newTimeStamp)
    {
        WriteLog("Hot-swapping shaders for %ls\n", file->FilePath.c_str());
        file->TimeStamp = newTimeStamp;
        for(uint64 i = 0; i < file->Shaders.Count(); ++i)
        {
            // Retry a few times to avoid file conflicts with text editors
            const uint64 NumRetries = 10;
            for(uint64 retryCount = 0; retryCount < NumRetries; ++retryCount)
            {
                try
                {
                    CompiledShader* shader = file->Shaders[i];
                    CompileShader(shader);
                    break;
                }
                catch(Win32Exception& exception)
                {
                    if(retryCount == NumRetries - 1)
                        throw exception;
                    Sleep(15);
                }
            }
        }

        return true;
    }

    return false;
}

void ShutdownShaders()
{
    for(uint64 i = 0; i < ShaderFiles.Count(); ++i)
        delete ShaderFiles[i];

    for(uint64 i = 0; i < CompiledShaders.Count(); ++i)
        delete CompiledShaders[i];
}

// == CompileOptions ==============================================================================

CompileOptions::CompileOptions()
{
    Reset();
}

void CompileOptions::Add(const std::string& name, uint32 value)
{
    Assert_(numDefines < MaxDefines);

    nameOffsets[numDefines] = bufferIdx;
    for(uint32 i = 0; i < name.length(); ++i)
        buffer[bufferIdx++] = name[i];
    ++bufferIdx;

    std::string stringVal = ToAnsiString(value);
    defineOffsets[numDefines] = bufferIdx;
    for(uint32 i = 0; i < stringVal.length(); ++i)
        buffer[bufferIdx++] = stringVal[i];
    ++bufferIdx;

    ++numDefines;
}

void CompileOptions::Reset()
{
    numDefines = 0;
    bufferIdx = 0;

    for(uint32 i = 0; i < MaxDefines; ++i)
    {
        nameOffsets[i] = 0xFFFFFFFF;
        defineOffsets[i] = 0xFFFFFFFF;
    }

    ZeroMemory(buffer, BufferSize);
}

void CompileOptions::MakeDefines(D3D_SHADER_MACRO defines[MaxDefines + 1]) const
{
    for(uint32 i = 0; i < numDefines; ++i)
    {
        defines[i].Name = buffer + nameOffsets[i];
        defines[i].Definition = buffer + defineOffsets[i];
    }

    defines[numDefines].Name = nullptr;
    defines[numDefines].Definition = nullptr;
}

}