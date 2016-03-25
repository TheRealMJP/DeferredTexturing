#pragma once

#include <PCH.h>
#include <Settings.h>
#include <Graphics\GraphicsTypes.h>

using namespace SampleFramework12;

enum class MSAAModes
{
    MSAANone = 0,
    MSAA2x = 1,
    MSAA4x = 2,

    NumValues
};

typedef EnumSettingT<MSAAModes> MSAAModesSetting;

enum class Scenes
{
    Sponza = 0,

    NumValues
};

typedef EnumSettingT<Scenes> ScenesSetting;

enum class RenderModes
{
    ClusteredForward = 0,
    DeferredTexturing = 1,

    NumValues
};

typedef EnumSettingT<RenderModes> RenderModesSetting;

enum class ClusterRasterizationModes
{
    Normal = 0,
    MSAA4x = 1,
    MSAA8x = 2,
    Conservative = 3,

    NumValues
};

typedef EnumSettingT<ClusterRasterizationModes> ClusterRasterizationModesSetting;

namespace AppSettings
{
    static const uint64 ClusterTileSize = 16;
    static const uint64 NumZTiles = 16;
    static const uint64 NumDecalTypes = 8;
    static const uint64 NumTexturesPerDecal = 2;
    static const uint64 NumDecalTextures = 16;
    static const uint64 MaxDecals = 64;
    static const uint64 DecalElementsPerCluster = 2;
    static const uint64 MaxSpotLights = 32;
    static const uint64 SpotLightElementsPerCluster = 1;
    static const float SpotLightRange = 7.5000f;
    static const uint64 DeferredTileSize = 8;
    static const uint64 DeferredTileMaskSize = 2;
    static const float DeferredUVScale = 2.0000f;

    extern BoolSetting EnableSun;
    extern BoolSetting SunAreaLightApproximation;
    extern FloatSetting SunSize;
    extern DirectionSetting SunDirection;
    extern FloatSetting Turbidity;
    extern ColorSetting GroundAlbedo;
    extern MSAAModesSetting MSAAMode;
    extern ScenesSetting CurrentScene;
    extern BoolSetting RenderLights;
    extern BoolSetting RenderDecals;
    extern Button ClearDecals;
    extern BoolSetting EnableDecalPicker;
    extern RenderModesSetting RenderMode;
    extern BoolSetting DepthPrepass;
    extern BoolSetting SortByDepth;
    extern IntSetting MaxLightClamp;
    extern ClusterRasterizationModesSetting ClusterRasterizationMode;
    extern BoolSetting UseZGradientsForMSAAMask;
    extern BoolSetting ComputeUVGradients;
    extern FloatSetting Exposure;
    extern FloatSetting BloomExposure;
    extern FloatSetting BloomMagnitude;
    extern FloatSetting BloomBlurSigma;
    extern BoolSetting EnableVSync;
    extern BoolSetting EnableAlbedoMaps;
    extern BoolSetting EnableNormalMaps;
    extern BoolSetting EnableSpecular;
    extern BoolSetting ShowLightCounts;
    extern BoolSetting ShowDecalCounts;
    extern BoolSetting ShowClusterVisualizer;
    extern BoolSetting ShowMSAAMask;
    extern BoolSetting ShowUVGradients;

    struct AppSettingsCBuffer
    {
        bool32 EnableSun;
        bool32 SunAreaLightApproximation;
        float SunSize;
        Float4Align Float3 SunDirection;
        int32 MSAAMode;
        bool32 RenderLights;
        bool32 RenderDecals;
        int32 RenderMode;
        float Exposure;
        float BloomExposure;
        float BloomMagnitude;
        float BloomBlurSigma;
        bool32 EnableAlbedoMaps;
        bool32 EnableNormalMaps;
        bool32 EnableSpecular;
        bool32 ShowLightCounts;
        bool32 ShowDecalCounts;
        bool32 ShowMSAAMask;
        bool32 ShowUVGradients;
    };

    extern ConstantBuffer<AppSettingsCBuffer> CBuffer;
    const extern uint32 CBufferRegister;

    void Initialize();
    void Shutdown();
    void Update(uint32 displayWidth, uint32 displayHeight, const Float4x4& viewMatrix);
    void UpdateCBuffer();
    void BindCBufferGfx(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter);
    void BindCBufferCompute(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter);
};

// ================================================================================================

const uint64 NumMSAAModes = uint64(MSAAModes::NumValues);

namespace AppSettings
{
    extern uint64 NumXTiles;
    extern uint64 NumYTiles;

    inline uint32 NumMSAASamples(MSAAModes mode)
    {
        static const uint32 NumSamples[] = { 1, 2, 4 };
        StaticAssert_(ArraySize_(NumSamples) >= uint64(MSAAModes::NumValues));
        return NumSamples[uint32(mode)];
    }

    inline uint32 NumMSAASamples()
    {
        return NumMSAASamples(MSAAMode);
    }

    void UpdateUI();
}