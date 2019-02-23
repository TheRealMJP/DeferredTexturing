#include <PCH.h>
#include "AppSettings.h"

using namespace SampleFramework12;

static const char* MSAAModesLabels[] =
{
    "None",
    "2x",
    "4x",
};

static const char* ScenesLabels[] =
{
    "Sponza",
};

static const char* ClusterRasterizationModesLabels[] =
{
    "Normal",
    "MSAA4x",
    "MSAA8x",
    "Conservative",
};

namespace AppSettings
{
    static SettingsContainer Settings;

    BoolSetting EnableSun;
    BoolSetting SunAreaLightApproximation;
    FloatSetting SunSize;
    DirectionSetting SunDirection;
    FloatSetting Turbidity;
    ColorSetting GroundAlbedo;
    MSAAModesSetting MSAAMode;
    ScenesSetting CurrentScene;
    BoolSetting RenderLights;
    BoolSetting RenderDecals;
    Button ClearDecals;
    BoolSetting EnableDecalPicker;
    BoolSetting DepthPrepass;
    BoolSetting SortByDepth;
    IntSetting MaxLightClamp;
    ClusterRasterizationModesSetting ClusterRasterizationMode;
    BoolSetting UseZGradientsForMSAAMask;
    BoolSetting ComputeUVGradients;
    BoolSetting MultiQueueSubmit;
    FloatSetting Exposure;
    FloatSetting BloomExposure;
    FloatSetting BloomMagnitude;
    FloatSetting BloomBlurSigma;
    BoolSetting EnableVSync;
    BoolSetting EnableAlbedoMaps;
    BoolSetting EnableNormalMaps;
    BoolSetting EnableSpecular;
    BoolSetting ShowLightCounts;
    BoolSetting ShowDecalCounts;
    BoolSetting ShowClusterVisualizer;
    BoolSetting ShowMSAAMask;
    BoolSetting ShowUVGradients;
    BoolSetting AnimateLightIntensity;

    ConstantBuffer CBuffer;
    const uint32 CBufferRegister = 12;

    void Initialize()
    {

        Settings.Initialize(6);

        Settings.AddGroup("Sun And Sky", true);

        Settings.AddGroup("Anti Aliasing", true);

        Settings.AddGroup("Scene", true);

        Settings.AddGroup("Rendering", true);

        Settings.AddGroup("Post Processing", false);

        Settings.AddGroup("Debug", true);

        EnableSun.Initialize("EnableSun", "Sun And Sky", "Enable Sun", "Enables the sun light", true);
        Settings.AddSetting(&EnableSun);

        SunAreaLightApproximation.Initialize("SunAreaLightApproximation", "Sun And Sky", "Sun Area Light Approximation", "Controls whether the sun is treated as a disc area light in the real-time shader", true);
        Settings.AddSetting(&SunAreaLightApproximation);

        SunSize.Initialize("SunSize", "Sun And Sky", "Sun Size", "Angular radius of the sun in degrees", 1.0000f, 0.0100f, 340282300000000000000000000000000000000.0000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&SunSize);

        SunDirection.Initialize("SunDirection", "Sun And Sky", "Sun Direction", "Direction of the sun", Float3(0.2600f, 0.9870f, -0.1600f), true);
        Settings.AddSetting(&SunDirection);

        Turbidity.Initialize("Turbidity", "Sun And Sky", "Turbidity", "Atmospheric turbidity (thickness) uses for procedural sun and sky model", 2.0000f, 1.0000f, 10.0000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&Turbidity);

        GroundAlbedo.Initialize("GroundAlbedo", "Sun And Sky", "Ground Albedo", "Ground albedo color used for procedural sun and sky model", Float3(0.2500f, 0.2500f, 0.2500f), false, -340282300000000000000000000000000000000.0000f, 340282300000000000000000000000000000000.0000f, 0.0100f, ColorUnit::None);
        Settings.AddSetting(&GroundAlbedo);

        MSAAMode.Initialize("MSAAMode", "Anti Aliasing", "MSAA Mode", "MSAA mode to use for rendering", MSAAModes::MSAANone, 3, MSAAModesLabels);
        Settings.AddSetting(&MSAAMode);
        MSAAMode.SetVisible(false);

        CurrentScene.Initialize("CurrentScene", "Scene", "Current Scene", "", Scenes::Sponza, 1, ScenesLabels);
        Settings.AddSetting(&CurrentScene);

        RenderLights.Initialize("RenderLights", "Scene", "Render Lights", "Enable or disable deferred light rendering", true);
        Settings.AddSetting(&RenderLights);

        RenderDecals.Initialize("RenderDecals", "Scene", "Render Decals", "Enable or disable applying decals in the main pass", true);
        Settings.AddSetting(&RenderDecals);

        ClearDecals.Initialize("ClearDecals", "Scene", "Clear Decals", "");
        Settings.AddSetting(&ClearDecals);

        EnableDecalPicker.Initialize("EnableDecalPicker", "Scene", "Enable Decal Picker", "Enables or disables placing new decals with the mouse", true);
        Settings.AddSetting(&EnableDecalPicker);

        DepthPrepass.Initialize("DepthPrepass", "Rendering", "Depth Prepass", "Renders a depth prepass before the main pass or G-Buffer pass", false);
        Settings.AddSetting(&DepthPrepass);

        SortByDepth.Initialize("SortByDepth", "Rendering", "Sort By Depth", "Enables sorting meshes by their depth in front-to-back order", true);
        Settings.AddSetting(&SortByDepth);

        MaxLightClamp.Initialize("MaxLightClamp", "Rendering", "Max Lights", "Limits the number of lights in the scene", 32, 0, 32);
        Settings.AddSetting(&MaxLightClamp);

        ClusterRasterizationMode.Initialize("ClusterRasterizationMode", "Rendering", "Cluster Rasterization Mode", "Conservative rasterization mode to use for light binning", ClusterRasterizationModes::Conservative, 4, ClusterRasterizationModesLabels);
        Settings.AddSetting(&ClusterRasterizationMode);

        UseZGradientsForMSAAMask.Initialize("UseZGradientsForMSAAMask", "Rendering", "Use Z DX/DY For MSAA Mask", "Use Z gradients to detect edges during MSAA mask generation", false);
        Settings.AddSetting(&UseZGradientsForMSAAMask);

        ComputeUVGradients.Initialize("ComputeUVGradients", "Rendering", "Compute UV Gradients", "Choose whether to compute UV gradients for deferred rendering, or explicitly store them in the G-Buffer", false);
        Settings.AddSetting(&ComputeUVGradients);

        MultiQueueSubmit.Initialize("MultiQueueSubmit", "Rendering", "Multi-Queue Submission", "If enabled, submit shadows and SSAO on multiple queues to execute simultaneously", true);
        Settings.AddSetting(&MultiQueueSubmit);

        Exposure.Initialize("Exposure", "Post Processing", "Exposure", "Simple exposure value applied to the scene before tone mapping (uses log2 scale)", -14.0000f, -24.0000f, 24.0000f, 0.1000f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&Exposure);

        BloomExposure.Initialize("BloomExposure", "Post Processing", "Bloom Exposure Offset", "Exposure offset applied to generate the input of the bloom pass", -4.0000f, -10.0000f, 0.0000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&BloomExposure);

        BloomMagnitude.Initialize("BloomMagnitude", "Post Processing", "Bloom Magnitude", "Scale factor applied to the bloom results when combined with tone-mapped result", 1.0000f, 0.0000f, 2.0000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&BloomMagnitude);

        BloomBlurSigma.Initialize("BloomBlurSigma", "Post Processing", "Bloom Blur Sigma", "Sigma parameter of the Gaussian filter used in the bloom pass", 2.5000f, 0.5000f, 2.5000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&BloomBlurSigma);

        EnableVSync.Initialize("EnableVSync", "Debug", "Enable VSync", "Enables or disables vertical sync during Present", true);
        Settings.AddSetting(&EnableVSync);

        EnableAlbedoMaps.Initialize("EnableAlbedoMaps", "Debug", "Enable Albedo Maps", "Enables albedo maps", true);
        Settings.AddSetting(&EnableAlbedoMaps);

        EnableNormalMaps.Initialize("EnableNormalMaps", "Debug", "Enable Normal Maps", "Enables normal maps", true);
        Settings.AddSetting(&EnableNormalMaps);

        EnableSpecular.Initialize("EnableSpecular", "Debug", "Enable Specular", "Enables specular reflections", true);
        Settings.AddSetting(&EnableSpecular);

        ShowLightCounts.Initialize("ShowLightCounts", "Debug", "Show Light Counts", "Visualizes the light count for a pixel", false);
        Settings.AddSetting(&ShowLightCounts);

        ShowDecalCounts.Initialize("ShowDecalCounts", "Debug", "Show Decal Counts", "Visualizes the decal count for a pixel", false);
        Settings.AddSetting(&ShowDecalCounts);

        ShowClusterVisualizer.Initialize("ShowClusterVisualizer", "Debug", "Show Cluster Visualizer", "Shows an overhead perspective of the view frustum with a visualization of the light/decal counts", false);
        Settings.AddSetting(&ShowClusterVisualizer);

        ShowMSAAMask.Initialize("ShowMSAAMask", "Debug", "Show MSAA Mask", "Visualizes the pixels where per-sample shading is applied", false);
        Settings.AddSetting(&ShowMSAAMask);

        ShowUVGradients.Initialize("ShowUVGradients", "Debug", "Show UV Gradients", "Visualize the UV gradients used for mip selection", false);
        Settings.AddSetting(&ShowUVGradients);

        AnimateLightIntensity.Initialize("AnimateLightIntensity", "Debug", "Animate Light Intensity", "Modulates the light intensity to test buffer uploads", false);
        Settings.AddSetting(&AnimateLightIntensity);

        ConstantBufferInit cbInit;
        cbInit.Size = sizeof(AppSettingsCBuffer);
        cbInit.Dynamic = true;
        cbInit.Name = L"AppSettings Constant Buffer";
        CBuffer.Initialize(cbInit);
    }

    void Update(uint32 displayWidth, uint32 displayHeight, const Float4x4& viewMatrix)
    {
        Settings.Update(displayWidth, displayHeight, viewMatrix);

    }

    void UpdateCBuffer()
    {
        AppSettingsCBuffer cbData;
        cbData.EnableSun = EnableSun;
        cbData.SunAreaLightApproximation = SunAreaLightApproximation;
        cbData.SunSize = SunSize;
        cbData.SunDirection = SunDirection;
        cbData.MSAAMode = MSAAMode;
        cbData.RenderLights = RenderLights;
        cbData.RenderDecals = RenderDecals;
        cbData.Exposure = Exposure;
        cbData.BloomExposure = BloomExposure;
        cbData.BloomMagnitude = BloomMagnitude;
        cbData.BloomBlurSigma = BloomBlurSigma;
        cbData.EnableAlbedoMaps = EnableAlbedoMaps;
        cbData.EnableNormalMaps = EnableNormalMaps;
        cbData.EnableSpecular = EnableSpecular;
        cbData.ShowLightCounts = ShowLightCounts;
        cbData.ShowDecalCounts = ShowDecalCounts;
        cbData.ShowMSAAMask = ShowMSAAMask;
        cbData.ShowUVGradients = ShowUVGradients;
        cbData.AnimateLightIntensity = AnimateLightIntensity;

        CBuffer.MapAndSetData(cbData);
    }
    void BindCBufferGfx(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter)
    {
        CBuffer.SetAsGfxRootParameter(cmdList, rootParameter);
    }
    void BindCBufferCompute(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter)
    {
        CBuffer.SetAsComputeRootParameter(cmdList, rootParameter);
    }
    void Shutdown()
    {
        CBuffer.Shutdown();
    }
}

// ================================================================================================

namespace AppSettings
{
    uint64 NumXTiles = 0;
    uint64 NumYTiles = 0;

    void UpdateUI()
    {
    }
}