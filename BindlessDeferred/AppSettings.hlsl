struct AppSettings_Layout
{
    bool EnableSun;
    bool SunAreaLightApproximation;
    float SunSize;
    float3 SunDirection;
    int MSAAMode;
    bool RenderLights;
    bool RenderDecals;
    int RenderMode;
    float Exposure;
    float BloomExposure;
    float BloomMagnitude;
    float BloomBlurSigma;
    bool EnableAlbedoMaps;
    bool EnableNormalMaps;
    bool EnableSpecular;
    bool ShowLightCounts;
    bool ShowDecalCounts;
    bool ShowMSAAMask;
    bool ShowUVGradients;
};

ConstantBuffer<AppSettings_Layout> AppSettings : register(b12);

static const int MSAAModes_MSAANone = 0;
static const int MSAAModes_MSAA2x = 1;
static const int MSAAModes_MSAA4x = 2;

static const int Scenes_Sponza = 0;

static const int RenderModes_ClusteredForward = 0;
static const int RenderModes_DeferredTexturing = 1;

static const int ClusterRasterizationModes_Normal = 0;
static const int ClusterRasterizationModes_MSAA4x = 1;
static const int ClusterRasterizationModes_MSAA8x = 2;
static const int ClusterRasterizationModes_Conservative = 3;

static const uint ClusterTileSize = 16;
static const uint NumZTiles = 16;
static const uint NumDecalTypes = 8;
static const uint NumTexturesPerDecal = 2;
static const uint NumDecalTextures = 16;
static const uint MaxDecals = 64;
static const uint DecalElementsPerCluster = 2;
static const uint MaxSpotLights = 32;
static const uint SpotLightElementsPerCluster = 1;
static const float SpotLightRange = 7.5000f;
static const uint DeferredTileSize = 8;
static const uint DeferredTileMaskSize = 2;
static const float DeferredUVScale = 2.0000f;
