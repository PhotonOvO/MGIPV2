// Sky pass: draw the HDR equirectangular environment into background pixels.

#include "shader_io.h"

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindLighting, LSetScene)]]
ConstantBuffer<LightingUniforms> lighting;

STATIC_CONST int kGBufferDepthIndex = 3;
STATIC_CONST int kIBLEnvironmentIndex = 10;
STATIC_CONST float PI = 3.14159265359;

float2 directionToEquirectUv(vec3 direction)
{
    vec3 d = normalize(direction);
    float phi = atan2(d.z, d.x);
    float theta = asin(clamp(d.y, -1.0, 1.0));
    return float2(0.5 + phi / (2.0 * PI), 0.5 - theta / PI);
}

vec3 toneMapACES(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    color = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    return pow(color, vec3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}

vec3 applyDisplayTransform(vec3 sceneLinearColor)
{
    return toneMapACES(sceneLinearColor);
}

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    VSOutput output;
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float depthSample = inTexture[kGBufferDepthIndex].Sample(input.uv).r;
    if(depthSample > 0.0001)
    {
        discard;
    }

    float2 ndc = input.uv * 2.0 - 1.0;
    float4 clipPos = float4(ndc.x, ndc.y, 0.0, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    vec3 direction = normalize(worldPos.xyz / worldPos.w - camera.cameraPosition);

    vec3 hdrColor = inTexture[kIBLEnvironmentIndex].SampleLevel(directionToEquirectUv(direction), 0.0).rgb
        * lighting.light.iblParams.y;
    return float4(applyDisplayTransform(hdrColor), 1.0);
}
