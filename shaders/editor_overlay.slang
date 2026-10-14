#include "../render/editor/EditorOverlayShaderInterop.h"

struct EditorOverlayLineVertexInput
{
    [[vk::location(0)]] float4 startWidth : POSITION0;
    [[vk::location(1)]] float4 endAlpha : POSITION1;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct EditorOverlayGizmoVertexInput
{
    [[vk::location(0)]] float3 position : POSITION0;
    [[vk::location(1)]] float3 normal : NORMAL0;
};

struct EditorOverlayVertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    nointerpolation uint4 flags : TEXCOORD0;
};

[[vk::binding(kEditorOverlayFrameBinding, kEditorOverlayArgumentSlot)]]
ConstantBuffer<EditorOverlayFrameUniforms> editorOverlayFrame
    : register(b0, space3);

[[vk::binding(kEditorOverlaySceneDepthBinding, kEditorOverlayArgumentSlot)]]
Sampler2D<float> editorOverlaySceneDepth : register(t1, space3);

[[vk::push_constant]]
ConstantBuffer<EditorOverlayDrawConstants> editorOverlayDraw;

float4 multiplyRows(float4 rows[4], float4 value)
{
    return float4(dot(rows[0], value), dot(rows[1], value),
                  dot(rows[2], value), dot(rows[3], value));
}

float2 safeNormalized(float2 value)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared)
                                  : float2(1.0, 0.0);
}

bool clipLinePlane(inout float4 firstClip, inout float4 secondClip,
                   float firstDistance, float secondDistance)
{
    const bool firstInside = firstDistance >= 0.0;
    const bool secondInside = secondDistance >= 0.0;
    if (firstInside && secondInside)
    {
        return true;
    }
    if (!firstInside && !secondInside)
    {
        return false;
    }
    const float denominator = firstDistance - secondDistance;
    if (abs(denominator) <= 1.0e-12)
    {
        return false;
    }
    const float4 clipped = lerp(firstClip, secondClip,
                                saturate(firstDistance / denominator));
    if (!firstInside)
    {
        firstClip = clipped;
    }
    else
    {
        secondClip = clipped;
    }
    return true;
}

bool clipLineToViewVolume(inout float4 firstClip, inout float4 secondClip)
{
    const float minimumW = 1.0e-5;
    if (!clipLinePlane(firstClip, secondClip, firstClip.w - minimumW,
                       secondClip.w - minimumW))
    {
        return false;
    }
    const bool zeroToOne =
        editorOverlayFrame.flags.w == kEditorOverlayClipDepthZeroToOne;
    float firstDistance = zeroToOne ? firstClip.z : firstClip.z + firstClip.w;
    float secondDistance = zeroToOne ? secondClip.z
                                     : secondClip.z + secondClip.w;
    if (!clipLinePlane(firstClip, secondClip, firstDistance, secondDistance))
    {
        return false;
    }
    if (!clipLinePlane(firstClip, secondClip, firstClip.w - firstClip.z,
                       secondClip.w - secondClip.z))
    {
        return false;
    }
    return firstClip.w >= minimumW && secondClip.w >= minimumW;
}

[shader("vertex")]
EditorOverlayVertexOutput editorOverlayLineVertexMain(
    EditorOverlayLineVertexInput input, uint vertexId : SV_VertexID)
{
    float4 firstClip = multiplyRows(
        editorOverlayFrame.viewProjectionRows, float4(input.startWidth.xyz, 1.0));
    float4 secondClip = multiplyRows(
        editorOverlayFrame.viewProjectionRows, float4(input.endAlpha.xyz, 1.0));
    const bool lineVisible = clipLineToViewVolume(firstClip, secondClip);
    if (!lineVisible)
    {
        firstClip = float4(0.0, 0.0, 0.0, 1.0);
        secondClip = firstClip;
    }
    const float2 firstNdc = firstClip.xy / max(firstClip.w, 1.0e-7);
    const float2 secondNdc = secondClip.xy / max(secondClip.w, 1.0e-7);
    const float2 viewportExtent = max(editorOverlayFrame.colorViewportPx.zw,
                                      float2(1.0, 1.0));
    const float2 directionPx = (secondNdc - firstNdc) * viewportExtent;
    const float2 sourceFramebufferScale = max(
        editorOverlayFrame.logicalExtentAndFramebufferScale.zw,
        float2(1.0e-6, 1.0e-6));
    const uint surfaceTransform = editorOverlayFrame.flags.z;
    const bool swapsPhysicalAxes =
        surfaceTransform == kEditorOverlaySurfaceRotate90 ||
        surfaceTransform == kEditorOverlaySurfaceRotate270 ||
        surfaceTransform == kEditorOverlaySurfaceMirrorHorizontalRotate90 ||
        surfaceTransform == kEditorOverlaySurfaceMirrorHorizontalRotate270;
    const float2 framebufferScale = swapsPhysicalAxes
                                        ? sourceFramebufferScale.yx
                                        : sourceFramebufferScale;
    const float2 directionLogical =
        safeNormalized(directionPx / framebufferScale);
    const float2 normalLogical =
        float2(-directionLogical.y, directionLogical.x);
    float2 offsetPhysical = normalLogical * framebufferScale *
                            (input.startWidth.w * 0.5);
    const float offsetLength = length(offsetPhysical);
    if (offsetLength < 0.5)
    {
        offsetPhysical = safeNormalized(offsetPhysical) * 0.5;
    }
    const float2 offsetNdc = offsetPhysical * 2.0 / viewportExtent;

    static const uint endpoint[6] = {0u, 0u, 1u, 0u, 1u, 1u};
    static const float side[6] = {-1.0, 1.0, 1.0, -1.0, 1.0, -1.0};
    float4 clip = endpoint[vertexId % 6u] == 0u ? firstClip : secondClip;
    clip.xy += offsetNdc * side[vertexId % 6u] * clip.w;

    EditorOverlayVertexOutput output;
    output.position = clip;
    output.color = float4(
        input.color.rgb,
        lineVisible ? saturate(input.color.a * input.endAlpha.w) : 0.0);
    output.flags = editorOverlayDraw.flags;
    return output;
}

[shader("vertex")]
EditorOverlayVertexOutput editorOverlayGizmoVertexMain(
    EditorOverlayGizmoVertexInput input)
{
    const float4 worldPosition = multiplyRows(
        editorOverlayDraw.modelRows, float4(input.position, 1.0));
    EditorOverlayVertexOutput output;
    output.position = multiplyRows(editorOverlayFrame.viewProjectionRows,
                                   worldPosition);
    output.color = editorOverlayDraw.color;
    output.flags = editorOverlayDraw.flags;
    return output;
}

bool overlayFragmentOccluded(float4 fragmentPosition)
{
    const float2 colorExtent = max(editorOverlayFrame.colorViewportPx.zw,
                                   float2(1.0, 1.0));
    const float2 local = fragmentPosition.xy - editorOverlayFrame.colorViewportPx.xy;
    const float2 normalized = saturate(local / colorExtent);
    const float2 depthPixel = editorOverlayFrame.depthViewportPx.xy +
                              normalized * editorOverlayFrame.depthViewportPx.zw;
    const float2 depthExtent = max(editorOverlayFrame.attachmentExtentsPx.zw,
                                   float2(1.0, 1.0));
    const float2 uv = depthPixel / depthExtent;
    const float sceneDepth = editorOverlaySceneDepth.SampleLevel(uv, 0.0).r;
    const float epsilon = max(editorOverlayFrame.rasterParams.y, 0.0);
    return editorOverlayFrame.flags.x != 0u
               ? fragmentPosition.z < sceneDepth - epsilon
               : fragmentPosition.z > sceneDepth + epsilon;
}

[shader("fragment")]
float4 editorOverlayFragmentMain(EditorOverlayVertexOutput input) : SV_Target0
{
    const uint drawKind = input.flags.x;
    const uint depthMode = input.flags.y;
    const uint coverageFlags = input.flags.w;
    const bool sceneOccluded =
        depthMode == kEditorOverlayDepthSceneOccluded ||
        depthMode == kEditorOverlayDepthXRayDithered;
    const bool occluded = sceneOccluded && overlayFragmentOccluded(input.position);

    if (depthMode == kEditorOverlayDepthSceneOccluded && occluded)
    {
        discard;
    }
    const uint checker = (uint(input.position.x) ^ uint(input.position.y)) & 1u;
    if ((coverageFlags & kEditorOverlayFlagPlaneChecker) != 0u && checker == 0u)
    {
        discard;
    }
    if ((coverageFlags & kEditorOverlayFlagOcclusionDither) != 0u &&
        occluded && checker == 0u)
    {
        discard;
    }

    float4 color = input.color;
    if (drawKind == kEditorOverlayDrawGizmo &&
        (coverageFlags & kEditorOverlayFlagPlaneChecker) != 0u)
    {
        color.a = 1.0;
    }
    return color;
}
