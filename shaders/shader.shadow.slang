// Shadow pass shader: renders a directional shadow map with alpha-cutout support

#include "shader_io.h"

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfNormal)]]   float3 normal   : NORMAL;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
    [[vk::location(LVGltfTangent)]]  float4 tangent  : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniforms> drawData;

float3 safeNormalizeShadowVector(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    if(!(lengthSquared > 1.0e-12))
    {
        return fallback;
    }
    return value * rsqrt(lengthSquared);
}

float3 transformShadowNormal(mat4 modelMatrix, float3 objectNormal)
{
    const float3x3 modelLinear = (float3x3)modelMatrix;
    const float3 fallbackNormal =
        safeNormalizeShadowVector(mul(modelLinear, objectNormal), float3(0.0, 1.0, 0.0));

    // Cofactor(modelLinear) / determinant is inverse-transpose. Compute the
    // product directly because Slang's HLSL-style standard library has no
    // matrix inverse intrinsic for float3x3.
    const float cofactor00 = modelLinear[1][1] * modelLinear[2][2]
        - modelLinear[1][2] * modelLinear[2][1];
    const float cofactor01 = modelLinear[1][2] * modelLinear[2][0]
        - modelLinear[1][0] * modelLinear[2][2];
    const float cofactor02 = modelLinear[1][0] * modelLinear[2][1]
        - modelLinear[1][1] * modelLinear[2][0];
    const float cofactor10 = modelLinear[0][2] * modelLinear[2][1]
        - modelLinear[0][1] * modelLinear[2][2];
    const float cofactor11 = modelLinear[0][0] * modelLinear[2][2]
        - modelLinear[0][2] * modelLinear[2][0];
    const float cofactor12 = modelLinear[0][1] * modelLinear[2][0]
        - modelLinear[0][0] * modelLinear[2][1];
    const float cofactor20 = modelLinear[0][1] * modelLinear[1][2]
        - modelLinear[0][2] * modelLinear[1][1];
    const float cofactor21 = modelLinear[0][2] * modelLinear[1][0]
        - modelLinear[0][0] * modelLinear[1][2];
    const float cofactor22 = modelLinear[0][0] * modelLinear[1][1]
        - modelLinear[0][1] * modelLinear[1][0];
    const float determinantValue = modelLinear[0][0] * cofactor00
        + modelLinear[0][1] * cofactor01
        + modelLinear[0][2] * cofactor02;
    if(!(abs(determinantValue) > 1.0e-8))
    {
        return fallbackNormal;
    }

    const float3 transformedNormal = float3(
        cofactor00 * objectNormal.x + cofactor01 * objectNormal.y + cofactor02 * objectNormal.z,
        cofactor10 * objectNormal.x + cofactor11 * objectNormal.y + cofactor12 * objectNormal.z,
        cofactor20 * objectNormal.x + cofactor21 * objectNormal.y + cofactor22 * objectNormal.z)
        / determinantValue;
    return safeNormalizeShadowVector(transformedNormal, fallbackNormal);
}

// normalBias is a caster-only world-space offset. Receiver depthBias is applied
// once during the reverse-Z comparison in shader.light.slang.
float3 applyShadowCasterNormalOffset(float3 worldPosition, float3 worldNormal)
{
    const float3 dirToLight = camera.shadowDirectionAndSlopeBias.xyz;
    const float normalDotLight = dot(worldNormal, dirToLight);
    const float cosTheta = saturate(abs(normalDotLight));
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    const float normalBiasWorld = max(camera.shadowDirectionAndSlopeBias.w, 0.0);
    const float3 offsetNormal = normalDotLight >= 0.0 ? -worldNormal : worldNormal;
    return worldPosition + offsetNormal * (normalBiasWorld * sinTheta);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    float3 worldNormal = transformShadowNormal(draw.modelMatrix, input.normal);
    worldPos.xyz = applyShadowCasterNormalOffset(worldPos.xyz, worldNormal);
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    return output;
}

struct VertexOutputMdi
{
    float4               position  : SV_Position;
    float2               uv        : TEXCOORD0;
    nointerpolation uint drawIndex : TEXCOORD1;
};

[shader("vertex")]
VertexOutputMdi vertexMdiMain(VertexInput input, uint drawIndex : SV_StartInstanceLocation)
{
    const DrawUniforms mdiDraw = drawData[drawIndex];

    VertexOutputMdi output;
    float4 worldPos = mul(mdiDraw.modelMatrix, float4(input.position, 1.0));
    float3 worldNormal = transformShadowNormal(mdiDraw.modelMatrix, input.normal);
    worldPos.xyz = applyShadowCasterNormalOffset(worldPos.xyz, worldNormal);
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    output.drawIndex = drawIndex;
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    if(draw.alphaMode != LAlphaMask || draw.baseColorTextureIndex < 0)
    {
        return;
    }

    uint baseColorTextureIndex = NonUniformResourceIndex(uint(draw.baseColorTextureIndex));
    float alpha = inTexture[baseColorTextureIndex].Sample(input.uv).a * draw.baseColorFactor.a;
    if(alpha < draw.alphaCutoff)
    {
        discard;
    }
}

[shader("fragment")]
void fragmentMdiMain(VertexOutputMdi input)
{
    const DrawUniforms mdiDraw = drawData[input.drawIndex];
    if(mdiDraw.alphaMode != LAlphaMask || mdiDraw.baseColorTextureIndex < 0)
    {
        return;
    }

    uint baseColorTextureIndex = NonUniformResourceIndex(uint(mdiDraw.baseColorTextureIndex));
    float alpha = inTexture[baseColorTextureIndex].Sample(input.uv).a * mdiDraw.baseColorFactor.a;
    if(alpha < mdiDraw.alphaCutoff)
    {
        discard;
    }
}
