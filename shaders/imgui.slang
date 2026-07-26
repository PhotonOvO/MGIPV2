struct VertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct ProjectionConstants
{
    float2 scale;
    float2 translate;
};

[[vk::push_constant]]
ConstantBuffer<ProjectionConstants> projection;

[[vk::binding(0, 1)]]
Sampler2D uiTexture;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = float4(
        input.position * projection.scale + projection.translate,
        0.0,
        1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("pixel")]
float4 fragmentMain(VertexOutput input) : SV_TARGET
{
    return input.color * uiTexture.Sample(input.uv);
}
