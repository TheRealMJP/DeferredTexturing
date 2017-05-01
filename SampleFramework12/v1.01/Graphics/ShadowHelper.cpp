//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "PCH.h"
#include "ShadowHelper.h"
#include "Camera.h"

namespace SampleFramework12
{

// Transforms from [-1,1] post-projection space to [0,1] UV space
Float4x4 ShadowScaleOffsetMatrix = Float4x4(Float4(0.5f,  0.0f, 0.0f, 0.0f),
                                          Float4(0.0f, -0.5f, 0.0f, 0.0f),
                                          Float4(0.0f,  0.0f, 1.0f, 0.0f),
                                          Float4(0.5f,  0.5f, 0.0f, 1.0f));

void PrepareShadowCascades(const Float3& lightDir, uint64 shadowMapSize, bool stabilize, const Camera& camera,
                           SunShadowConstants& constants, OrthographicCamera* cascadeCameras)
{
    const float MinDistance = 0.0f;
    const float MaxDistance = 1.0f;

    // Compute the split distances based on the partitioning mode
    float cascadeSplits[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    {
        float lambda = 0.5f;

        float nearClip = camera.NearClip();
        float farClip = camera.FarClip();
        float clipRange = farClip - nearClip;

        float minZ = nearClip + MinDistance * clipRange;
        float maxZ = nearClip + MaxDistance * clipRange;

        float range = maxZ - minZ;
        float ratio = maxZ / minZ;

        for(uint32 i = 0; i < NumCascades; ++i)
        {
            float p = (i + 1) / static_cast<float>(NumCascades);
            float log = minZ * std::pow(ratio, p);
            float uniform = minZ + range * p;
            float d = lambda * (log - uniform) + uniform;
            cascadeSplits[i] = (d - nearClip) / clipRange;
        }
    }

    Float3 c0Extents;
    Float4x4 c0Matrix;

    // Prepare the projections ofr each cascade
    for(uint64 cascadeIdx = 0; cascadeIdx < NumCascades; ++cascadeIdx)
    {
        // Get the 8 points of the view frustum in world space
        Float3 frustumCornersWS[8] =
        {
            Float3(-1.0f,  1.0f, 0.0f),
            Float3( 1.0f,  1.0f, 0.0f),
            Float3( 1.0f, -1.0f, 0.0f),
            Float3(-1.0f, -1.0f, 0.0f),
            Float3(-1.0f,  1.0f, 1.0f),
            Float3( 1.0f,  1.0f, 1.0f),
            Float3( 1.0f, -1.0f, 1.0f),
            Float3(-1.0f, -1.0f, 1.0f),
        };

        float prevSplitDist = cascadeIdx == 0 ? MinDistance : cascadeSplits[cascadeIdx - 1];
        float splitDist = cascadeSplits[cascadeIdx];

        Float4x4 invViewProj = Float4x4::Invert(camera.ViewProjectionMatrix());
        for(uint64 i = 0; i < 8; ++i)
            frustumCornersWS[i] = Float3::Transform(frustumCornersWS[i], invViewProj);

        // Get the corners of the current cascade slice of the view frustum
        for(uint64 i = 0; i < 4; ++i)
        {
            Float3 cornerRay = frustumCornersWS[i + 4] - frustumCornersWS[i];
            Float3 nearCornerRay = cornerRay * prevSplitDist;
            Float3 farCornerRay = cornerRay * splitDist;
            frustumCornersWS[i + 4] = frustumCornersWS[i] + farCornerRay;
            frustumCornersWS[i] = frustumCornersWS[i] + nearCornerRay;
        }

        // Calculate the centroid of the view frustum slice
        Float3 frustumCenter = Float3(0.0f);
        for(uint64 i = 0; i < 8; ++i)
            frustumCenter += frustumCornersWS[i];
        frustumCenter *= (1.0f / 8.0f);

        // Pick the up vector to use for the light camera
        Float3 upDir = camera.Right();

        Float3 minExtents;
        Float3 maxExtents;

        if(stabilize)
        {
            // This needs to be constant for it to be stable
            upDir = Float3(0.0f, 1.0f, 0.0f);

            // Calculate the radius of a bounding sphere surrounding the frustum corners
            float sphereRadius = 0.0f;
            for(uint64 i = 0; i < 8; ++i)
            {
                float dist = Float3::Length(Float3(frustumCornersWS[i]) - frustumCenter);
                sphereRadius = Max(sphereRadius, dist);
            }

            sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

            maxExtents = Float3(sphereRadius, sphereRadius, sphereRadius);
            minExtents = -maxExtents;
        }
        else
        {
            // Create a temporary view matrix for the light
            Float3 lightCameraPos = frustumCenter;
            Float3 lookAt = frustumCenter - lightDir;
            DirectX::XMMATRIX lightView = DirectX::XMMatrixLookAtLH(lightCameraPos.ToSIMD(), lookAt.ToSIMD(), upDir.ToSIMD());

            // Calculate an AABB around the frustum corners
            DirectX::XMVECTOR mins = DirectX::XMVectorSet(FloatMax, FloatMax, FloatMax, FloatMax);
            DirectX::XMVECTOR maxes = DirectX::XMVectorSet(-FloatMax, -FloatMax, -FloatMax, -FloatMax);
            for(uint32 i = 0; i < 8; ++i)
            {
                DirectX::XMVECTOR corner = DirectX::XMVector3TransformCoord(frustumCornersWS[i].ToSIMD(), lightView);
                mins = DirectX::XMVectorMin(mins, corner);
                maxes = DirectX::XMVectorMax(maxes, corner);
            }

            minExtents = Float3(mins);
            maxExtents = Float3(maxes);
        }

        // Adjust the min/max to accommodate the filtering size
        float scale = (shadowMapSize + 7.0f) / shadowMapSize;
        minExtents.x *= scale;
        minExtents.y *= scale;
        maxExtents.x *= scale;
        maxExtents.y *= scale;

        Float3 cascadeExtents = maxExtents - minExtents;

        // Get position of the shadow camera
        Float3 shadowCameraPos = frustumCenter + lightDir * -minExtents.z;

        // Come up with a new orthographic camera for the shadow caster
        OrthographicCamera& shadowCamera = cascadeCameras[cascadeIdx];
        shadowCamera.Initialize(minExtents.x, minExtents.y, maxExtents.x, maxExtents.y, 0.0f, cascadeExtents.z);
        shadowCamera.SetLookAt(shadowCameraPos, frustumCenter, upDir);

        if(stabilize)
        {
            // Create the rounding matrix, by projecting the world-space origin and determining
            // the fractional offset in texel space
            DirectX::XMMATRIX shadowMatrix = shadowCamera.ViewProjectionMatrix().ToSIMD();
            DirectX::XMVECTOR shadowOrigin = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            shadowOrigin = DirectX::XMVector4Transform(shadowOrigin, shadowMatrix);
            shadowOrigin = DirectX::XMVectorScale(shadowOrigin, shadowMapSize / 2.0f);

            DirectX::XMVECTOR roundedOrigin = DirectX::XMVectorRound(shadowOrigin);
            DirectX::XMVECTOR roundOffset = DirectX::XMVectorSubtract(roundedOrigin, shadowOrigin);
            roundOffset = DirectX::XMVectorScale(roundOffset, 2.0f / shadowMapSize);
            roundOffset = DirectX::XMVectorSetZ(roundOffset, 0.0f);
            roundOffset = DirectX::XMVectorSetW(roundOffset, 0.0f);

            DirectX::XMMATRIX shadowProj = shadowCamera.ProjectionMatrix().ToSIMD();
            shadowProj.r[3] = DirectX::XMVectorAdd(shadowProj.r[3], roundOffset);
            shadowCamera.SetProjection(Float4x4(shadowProj));
        }

        Float4x4 shadowMatrix = shadowCamera.ViewProjectionMatrix();
        shadowMatrix = shadowMatrix * ShadowScaleOffsetMatrix;

        // Store the split distance in terms of view space depth
        const float clipDist = camera.FarClip() - camera.NearClip();
        constants.CascadeSplits[cascadeIdx] = camera.NearClip() + splitDist * clipDist;

        if(cascadeIdx == 0)
        {
            c0Extents = cascadeExtents;
            c0Matrix = shadowMatrix;
            constants.ShadowMatrix = shadowMatrix;
            constants.CascadeOffsets[0] = Float4(0.0f, 0.0f, 0.0f, 0.0f);
            constants.CascadeScales[0] = Float4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            // Calculate the position of the lower corner of the cascade partition, in the UV space
            // of the first cascade partition
            Float4x4 invCascadeMat = Float4x4::Invert(shadowMatrix);
            Float3 cascadeCorner = Float3::Transform(Float3(0.0f, 0.0f, 0.0f), invCascadeMat);
            cascadeCorner = Float3::Transform(cascadeCorner, c0Matrix);

            // Do the same for the upper corner
            Float3 otherCorner = Float3::Transform(Float3(1.0f, 1.0f, 1.0f), invCascadeMat);
            otherCorner = Float3::Transform(otherCorner, c0Matrix);

            // Calculate the scale and offset
            Float3 cascadeScale = Float3(1.0f, 1.0f, 1.f) / (otherCorner - cascadeCorner);
            constants.CascadeOffsets[cascadeIdx] = Float4(-cascadeCorner, 0.0f);
            constants.CascadeScales[cascadeIdx] = Float4(cascadeScale, 1.0f);
        }
    }
}

}
