//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "..\\PCH.h"

#include "..\\InterfacePointers.h"
#include "..\\Assert.h"
#include "..\\MurmurHash.h"

namespace SampleFramework12
{

class CompileOptions
{
public:

    // constants
    static const uint32 MaxDefines = 16;
    static const uint32 BufferSize = 1024;

    CompileOptions();

    void Add(const std::string& name, uint32 value);
    void Reset();

    void MakeDefines(D3D_SHADER_MACRO defines[MaxDefines + 1]) const;

private:

    uint32 nameOffsets[MaxDefines];
    uint32 defineOffsets[MaxDefines];
    char buffer[BufferSize];
    uint32 numDefines;
    uint32 bufferIdx;
};

enum class ShaderType
{
    Vertex = 0,
    Hull,
    Domain,
    Geometry,
    Pixel,
    Compute,

    NumTypes
};

enum class ShaderProfile
{
    SM50 = 0,
    SM51,

    NumProfiles
};

class CompiledShader
{

public:

    std::wstring FilePath;
    std::string FunctionName;
    CompileOptions CompileOpts;
    bool ForceOptimization;
    ID3DBlobPtr ByteCode;
    ShaderType Type;
    Hash ByteCodeHash;

    CompiledShader(const wchar* filePath, const char* functionName,
                   const CompileOptions& compileOptions,
                   bool forceOptimization, ShaderType type) : FilePath(filePath),
                                                              FunctionName(functionName),
                                                              CompileOpts(compileOptions),
                                                              ForceOptimization(forceOptimization),
                                                              Type(type)
    {
    }
};

class CompiledShaderPtr
{
public:

    CompiledShaderPtr() : ptr(nullptr)
    {
    }

    CompiledShaderPtr(const CompiledShader* ptr_) : ptr(ptr_)
    {
    }

    const CompiledShader* operator->() const
    {
        Assert_(ptr != nullptr);
        return ptr;
    }

    const CompiledShader& operator*() const
    {
        Assert_(ptr != nullptr);
        return *ptr;
    }

    bool Valid() const
    {
        return ptr != nullptr;
    }

    D3D12_SHADER_BYTECODE ByteCode() const
    {
        Assert_(ptr != nullptr);
        D3D12_SHADER_BYTECODE byteCode;
        byteCode.pShaderBytecode = ptr->ByteCode->GetBufferPointer();
        byteCode.BytecodeLength = ptr->ByteCode->GetBufferSize();
        return byteCode;
    }

    // Compatability hack, remove this
    operator ID3D11VertexShader*() const { return nullptr; }
    operator ID3D11HullShader*() const { return nullptr; }
    operator ID3D11DomainShader*() const { return nullptr; }
    operator ID3D11GeometryShader*() const { return nullptr; }
    operator ID3D11PixelShader*() const { return nullptr; }
    operator ID3D11ComputeShader*() const { return nullptr; }

private:

    const CompiledShader* ptr;
};

typedef CompiledShaderPtr VertexShaderPtr;
typedef CompiledShaderPtr HullShaderPtr;
typedef CompiledShaderPtr DomainShaderPtr;
typedef CompiledShaderPtr GeometryShaderPtr;
typedef CompiledShaderPtr PixelShaderPtr;
typedef CompiledShaderPtr ComputeShaderPtr;
typedef ComputeShaderPtr ShaderPtr;

// Compiles a shader from file and creates the appropriate shader instance
CompiledShaderPtr CompileFromFile(const wchar* path,
                                  const char* functionName,
                                  ShaderType type,
                                  const CompileOptions& compileOpts = CompileOptions(),
                                  bool forceOptimization = false);

VertexShaderPtr CompileVSFromFile(const wchar* path,
                                  const char* functionName = "VS",
                                  const CompileOptions& compileOpts = CompileOptions(),
                                  bool forceOptimization = false);

PixelShaderPtr CompilePSFromFile(const wchar* path,
                                 const char* functionName = "PS",
                                 const CompileOptions& compileOpts = CompileOptions(),
                                 bool forceOptimization = false);

GeometryShaderPtr CompileGSFromFile(const wchar* path,
                                    const char* functionName = "GS",
                                    const CompileOptions& compileOpts = CompileOptions(),
                                    bool forceOptimization = false);

HullShaderPtr CompileHSFromFile(const wchar* path,
                                const char* functionName = "HS",
                                const CompileOptions& compileOpts = CompileOptions(),
                                bool forceOptimization = false);

DomainShaderPtr CompileDSFromFile(const wchar* path,
                                  const char* functionName = "DS",
                                  const CompileOptions& compileOpts = CompileOptions(),
                                  bool forceOptimization = false);

ComputeShaderPtr CompileCSFromFile(const wchar* path,
                                   const char* functionName = "CS",
                                   const CompileOptions& compileOpts = CompileOptions(),
                                   bool forceOptimization = false);

bool UpdateShaders();
void ShutdownShaders();

}
