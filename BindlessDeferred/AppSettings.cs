enum Scenes
{
    Sponza = 0,
}

enum MSAAModes
{
    [EnumLabel("None")]
    MSAANone = 0,

    [EnumLabel("2x")]
    MSAA2x,

    [EnumLabel("4x")]
    MSAA4x,
}

enum RenderModes
{
    [EnumLabel("Clustered Forward")]
    ClusteredForward = 0,

    [EnumLabel("Deferred Texturing")]
    DeferredTexturing,
}

enum ClusterRasterizationModes
{
    Normal,
    MSAA4x,
    MSAA8x,
    Conservative
}

public class Settings
{
    [ExpandGroup(true)]
    public class SunAndSky
    {
        [HelpText("Enables the sun light")]
        bool EnableSun = true;

        [HelpText("Controls whether the sun is treated as a disc area light in the real-time shader")]
        bool SunAreaLightApproximation = true;

        [HelpText("Angular radius of the sun in degrees")]
        [MinValue(0.01f)]
        [StepSize(0.01f)]
        float SunSize = 1.0f;

        [HelpText("Direction of the sun")]
        [DisplayInViewSpaceAttribute(true)]
        Direction SunDirection = new Direction(0.26f, 0.987f, -0.16f);

        [MinValue(1.0f)]
        [MaxValue(10.0f)]
        [UseAsShaderConstant(false)]
        [HelpText("Atmospheric turbidity (thickness) uses for procedural sun and sky model")]
        float Turbidity = 2.0f;

        [HDR(false)]
        [UseAsShaderConstant(false)]
        [HelpText("Ground albedo color used for procedural sun and sky model")]
        Color GroundAlbedo = new Color(0.25f, 0.25f, 0.25f);
    }

    [ExpandGroup(true)]
    public class AntiAliasing
    {
        [HelpText("MSAA mode to use for rendering")]
        [DisplayName("MSAA Mode")]
        MSAAModes MSAAMode = MSAAModes.MSAANone;
    }

    [ExpandGroup(true)]
    public class Scene
    {
        [UseAsShaderConstant(false)]
        Scenes CurrentScene = Scenes.Sponza;

        [HelpText("Enable or disable deferred light rendering")]
        bool RenderLights = true;

        [HelpText("Enable or disable applying decals in the main pass")]
        bool RenderDecals = true;

        Button ClearDecals;

        [HelpText("Enables or disables placing new decals with the mouse")]
        [UseAsShaderConstant(false)]
        bool EnableDecalPicker = true;
    }

    const uint ClusterTileSize = 16;
    const uint NumZTiles = 16;

    const uint NumDecalTypes = 8;
    const uint NumTexturesPerDecal = 2;
    const uint NumDecalTextures = NumDecalTypes * NumTexturesPerDecal;
    const uint MaxDecals = 64;
    const uint DecalElementsPerCluster = MaxDecals / 32;

    const uint MaxSpotLights = 32;
    const uint SpotLightElementsPerCluster = MaxSpotLights / 32;
    const float SpotLightRange = 7.5f;

    const uint DeferredTileSize = 8;
    const uint DeferredTileMaskSize = (DeferredTileSize * DeferredTileSize) / 32;

    const float DeferredUVScale = 2.0f;

    [ExpandGroup(true)]
    public class Rendering
    {
        [HelpText("The rendering technique to use")]
        RenderModes RenderMode = RenderModes.DeferredTexturing;

        [UseAsShaderConstant(false)]
        [HelpText("Renders a depth prepass before the main pass or G-Buffer pass")]
        bool DepthPrepass = false;

        [UseAsShaderConstant(false)]
        [HelpText("Enables sorting meshes by their depth in front-to-back order")]
        bool SortByDepth = true;

        [UseAsShaderConstant(false)]
        [MinValue(0)]
        [MaxValue((int)MaxSpotLights)]
        [DisplayName("Max Lights")]
        [HelpText("Limits the number of lights in the scene")]
        int MaxLightClamp = (int)MaxSpotLights;

        [UseAsShaderConstant(false)]
        [HelpText("Conservative rasterization mode to use for light binning")]
        ClusterRasterizationModes ClusterRasterizationMode = ClusterRasterizationModes.Conservative;

        [DisplayName("Use Z DX/DY For MSAA Mask")]
        [UseAsShaderConstant(false)]
        [HelpText("Use Z gradients to detect edges during MSAA mask generation")]
        bool UseZGradientsForMSAAMask = false;

        [DisplayName("Compute UV Gradients")]
        [UseAsShaderConstant(false)]
        [HelpText("Choose whether to compute UV gradients for deferred rendering, or explicitly store them in the G-Buffer")]
        bool ComputeUVGradients = false;
    }

    [ExpandGroup(false)]
    public class PostProcessing
    {
        [MinValue(-24.0f)]
        [MaxValue(24.0f)]
        [StepSize(0.1f)]
        [HelpText("Simple exposure value applied to the scene before tone mapping (uses log2 scale)")]
        float Exposure = -14.0f;

        [DisplayName("Bloom Exposure Offset")]
        [MinValue(-10.0f)]
        [MaxValue(0.0f)]
        [StepSize(0.01f)]
        [HelpText("Exposure offset applied to generate the input of the bloom pass")]
        float BloomExposure = -4.0f;

        [DisplayName("Bloom Magnitude")]
        [MinValue(0.0f)]
        [MaxValue(2.0f)]
        [StepSize(0.01f)]
        [HelpText("Scale factor applied to the bloom results when combined with tone-mapped result")]
        float BloomMagnitude = 1.0f;

        [DisplayName("Bloom Blur Sigma")]
        [MinValue(0.5f)]
        [MaxValue(2.5f)]
        [StepSize(0.01f)]
        [HelpText("Sigma parameter of the Gaussian filter used in the bloom pass")]
        float BloomBlurSigma = 2.5f;
    }

    [ExpandGroup(true)]
    public class Debug
    {
        [UseAsShaderConstant(false)]
        [DisplayName("Enable VSync")]
        [HelpText("Enables or disables vertical sync during Present")]
        bool EnableVSync = true;

        [DisplayName("Enable Albedo Maps")]
        [HelpText("Enables albedo maps")]
        bool EnableAlbedoMaps = true;

        [DisplayName("Enable Normal Maps")]
        [HelpText("Enables normal maps")]
        bool EnableNormalMaps = true;

        [HelpText("Enables specular reflections")]
        bool EnableSpecular = true;

        [HelpText("Visualizes the light count for a pixel")]
        bool ShowLightCounts = false;

        [HelpText("Visualizes the decal count for a pixel")]
        bool ShowDecalCounts = false;

        [HelpText("Shows an overhead perspective of the view frustum with a visualization of the light/decal counts")]
        [UseAsShaderConstant(false)]
        bool ShowClusterVisualizer = false;

        [HelpText("Visualizes the pixels where per-sample shading is applied")]
        [DisplayName("Show MSAA Mask")]
        bool ShowMSAAMask = false;

        [HelpText("Visualize the UV gradients used for mip selection")]
        [DisplayName("Show UV Gradients")]
        bool ShowUVGradients = false;
    }
}