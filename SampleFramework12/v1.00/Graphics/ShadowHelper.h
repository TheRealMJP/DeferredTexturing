//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include <PCH.h>
#include "..\\SF12_Math.h"

namespace SampleFramework12
{

class Camera;
class OrthographicCamera;
class PerspectiveCamera;
struct ModelSpotLight;

const uint64 NumCascades = 4;

struct SunShadowConstants
{
    Float4x4 ShadowMatrix;
    float CascadeSplits[NumCascades];
    Float4 CascadeOffsets[NumCascades];
    Float4 CascadeScales[NumCascades];
};

extern Float4x4 ShadowScaleOffsetMatrix;
void PrepareShadowCascades(const Float3& lightDir, uint64 shadowMapSize, bool stabilize, const Camera& camera,
                           SunShadowConstants& constants, OrthographicCamera* cascadeCameras);

}
