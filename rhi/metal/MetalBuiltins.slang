struct MGIPBlitParameters
{
    int2 sourceOffset0;
    int2 sourceOffset1;
    int2 destinationOffset0;
    int2 destinationOffset1;
    uint sourceMipLevel;
    uint destinationMipLevel;
};

float4 metalReadMip(Texture2D<float4> texture, uint2 coordinate, uint mipLevel)
{
    __intrinsic_asm "$0.read($1, $2)";
}

void metalWriteMip(
    RWTexture2D<float4> texture,
    float4 value,
    uint2 coordinate,
    uint mipLevel)
{
    __intrinsic_asm "$0.write($1, $2, $3)";
}

[numthreads(8, 8, 1)]
[shader("compute")]
void mgipMetal4Blit2D(
    Texture2D<float4> source,
    RWTexture2D<float4> destination,
    ConstantBuffer<MGIPBlitParameters> parameters,
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 threadPosition = dispatchThreadId.xy;
    const int2 destinationDelta =
        parameters.destinationOffset1 - parameters.destinationOffset0;
    const uint2 destinationExtent = uint2(abs(destinationDelta));
    if(any(threadPosition >= destinationExtent))
        return;

    const int2 destinationDirection = int2(
        destinationDelta.x > 0 ? 1 : -1,
        destinationDelta.y > 0 ? 1 : -1);
    const int2 destinationCorrection = int2(
        destinationDelta.x > 0 ? 0 : 1,
        destinationDelta.y > 0 ? 0 : 1);
    const int2 destinationCoordinate = parameters.destinationOffset0 +
        destinationDirection * int2(threadPosition) - destinationCorrection;

    const int2 sourceDelta =
        parameters.sourceOffset1 - parameters.sourceOffset0;
    const float2 sourcePosition = float2(parameters.sourceOffset0) +
        (float2(threadPosition) + 0.5f) * float2(sourceDelta) /
        float2(destinationExtent) - 0.5f;
    const int2 sourceLowerBound = min(
        parameters.sourceOffset0, parameters.sourceOffset1);
    const int2 sourceUpperBound = max(
        parameters.sourceOffset0, parameters.sourceOffset1) - int2(1);
    const int2 sourceBase = int2(floor(sourcePosition));
    const float2 sourceFraction = frac(sourcePosition);
    const uint2 p00 = uint2(clamp(sourceBase, sourceLowerBound, sourceUpperBound));
    const uint2 p10 = uint2(clamp(
        sourceBase + int2(1, 0), sourceLowerBound, sourceUpperBound));
    const uint2 p01 = uint2(clamp(
        sourceBase + int2(0, 1), sourceLowerBound, sourceUpperBound));
    const uint2 p11 = uint2(clamp(
        sourceBase + int2(1), sourceLowerBound, sourceUpperBound));

    const float4 row0 = lerp(
        metalReadMip(source, p00, parameters.sourceMipLevel),
        metalReadMip(source, p10, parameters.sourceMipLevel),
        sourceFraction.x);
    const float4 row1 = lerp(
        metalReadMip(source, p01, parameters.sourceMipLevel),
        metalReadMip(source, p11, parameters.sourceMipLevel),
        sourceFraction.x);
    metalWriteMip(
        destination,
        lerp(row0, row1, sourceFraction.y),
        uint2(destinationCoordinate),
        parameters.destinationMipLevel);
}

__target_intrinsic(metal, "command_buffer")
struct MetalIndirectCommandBuffer
{
}

__target_intrinsic(metal, "primitive_type")
struct MetalPrimitiveType
{
}

[ForceInline]
MetalPrimitiveType metalPrimitiveType(uint value)
{
    __intrinsic_asm
        "($0 == 0 ? primitive_type::point : $0 == 1 ? primitive_type::line : $0 == 2 ? primitive_type::line_strip : $0 == 4 ? primitive_type::triangle_strip : primitive_type::triangle)";
}

__target_intrinsic(metal, "render_command")
struct MetalIndirectRenderCommand
{
    void drawIndexedUInt32(
        MetalPrimitiveType primitiveType,
        uint indexCount,
        StructuredBuffer<uint> indexBuffer,
        uint firstIndex,
        uint instanceCount,
        int baseVertex,
        uint baseInstance)
    {
        __intrinsic_asm
            "$0.draw_indexed_primitives($1, $2, $3 + $4, $5, $6, $7)";
    }

    void drawIndexedUInt16(
        MetalPrimitiveType primitiveType,
        uint indexCount,
        StructuredBuffer<uint16_t> indexBuffer,
        uint firstIndex,
        uint instanceCount,
        int baseVertex,
        uint baseInstance)
    {
        __intrinsic_asm
            "$0.draw_indexed_primitives($1, $2, $3 + $4, $5, $6, $7)";
    }
}

MetalIndirectRenderCommand metalRenderCommand(
    MetalIndirectCommandBuffer commandBuffer,
    uint commandIndex)
{
    __intrinsic_asm "render_command($0, $1)";
}

struct MetalIndexedIndirectCommandBufferArguments
{
    MetalIndirectCommandBuffer commandBuffer;
};

struct MetalIndexedIndirectPreparationParameters
{
    uint maxDrawCount;
    uint argumentStride;
    uint primitiveType;
    uint reserved;
};

[ForceInline]
void writeExecutionRange(
    ByteAddressBuffer drawCountBuffer,
    RWStructuredBuffer<uint> executionRange,
    ConstantBuffer<MetalIndexedIndirectPreparationParameters> parameters,
    uint commandIndex,
    out uint drawCount)
{
    drawCount = min(drawCountBuffer.Load(0), parameters.maxDrawCount);
    if(commandIndex == 0u)
    {
        executionRange[0] = 0u;
        executionRange[1] = drawCount;
    }
}

[ForceInline]
void writeClampedIndexedArguments(
    ByteAddressBuffer indirectArguments,
    RWByteAddressBuffer clampedArguments,
    ConstantBuffer<MetalIndexedIndirectPreparationParameters> parameters,
    uint commandIndex,
    uint drawCount)
{
    const uint argumentOffset = commandIndex * parameters.argumentStride;
    [ForceUnroll]
    for(uint wordIndex = 0u; wordIndex < 5u; ++wordIndex)
    {
        const uint wordOffset = argumentOffset + wordIndex * 4u;
        const uint value = commandIndex < drawCount
            ? indirectArguments.Load(wordOffset)
            : 0u;
        clampedArguments.Store(wordOffset, value);
    }
}

[numthreads(64, 1, 1)]
[shader("compute")]
void mgipPrepareIndexedIndirectUInt32(
    ParameterBlock<MetalIndexedIndirectCommandBufferArguments> commandArguments,
    ByteAddressBuffer indirectArguments,
    ByteAddressBuffer drawCountBuffer,
    RWStructuredBuffer<uint> executionRange,
    StructuredBuffer<uint> indexBuffer,
    ConstantBuffer<MetalIndexedIndirectPreparationParameters> parameters,
    RWByteAddressBuffer clampedArguments,
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint commandIndex = dispatchThreadId.x;
    uint drawCount = 0u;
    writeExecutionRange(
        drawCountBuffer, executionRange, parameters, commandIndex, drawCount);
    writeClampedIndexedArguments(
        indirectArguments, clampedArguments, parameters, commandIndex, drawCount);
    if(commandIndex >= drawCount)
        return;

    const uint argumentOffset = commandIndex * parameters.argumentStride;
    const uint indexCount = indirectArguments.Load(argumentOffset + 0u);
    const uint instanceCount = indirectArguments.Load(argumentOffset + 4u);
    const uint firstIndex = indirectArguments.Load(argumentOffset + 8u);
    const int baseVertex = asint(indirectArguments.Load(argumentOffset + 12u));
    const uint baseInstance = indirectArguments.Load(argumentOffset + 16u);
    MetalIndirectRenderCommand command = metalRenderCommand(
        commandArguments.commandBuffer, commandIndex);
    command.drawIndexedUInt32(
        metalPrimitiveType(parameters.primitiveType), indexCount, indexBuffer, firstIndex,
        instanceCount, baseVertex, baseInstance);
}

[numthreads(64, 1, 1)]
[shader("compute")]
void mgipPrepareIndexedIndirectUInt16(
    ParameterBlock<MetalIndexedIndirectCommandBufferArguments> commandArguments,
    ByteAddressBuffer indirectArguments,
    ByteAddressBuffer drawCountBuffer,
    RWStructuredBuffer<uint> executionRange,
    StructuredBuffer<uint16_t> indexBuffer,
    ConstantBuffer<MetalIndexedIndirectPreparationParameters> parameters,
    RWByteAddressBuffer clampedArguments,
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint commandIndex = dispatchThreadId.x;
    uint drawCount = 0u;
    writeExecutionRange(
        drawCountBuffer, executionRange, parameters, commandIndex, drawCount);
    writeClampedIndexedArguments(
        indirectArguments, clampedArguments, parameters, commandIndex, drawCount);
    if(commandIndex >= drawCount)
        return;

    const uint argumentOffset = commandIndex * parameters.argumentStride;
    const uint indexCount = indirectArguments.Load(argumentOffset + 0u);
    const uint instanceCount = indirectArguments.Load(argumentOffset + 4u);
    const uint firstIndex = indirectArguments.Load(argumentOffset + 8u);
    const int baseVertex = asint(indirectArguments.Load(argumentOffset + 12u));
    const uint baseInstance = indirectArguments.Load(argumentOffset + 16u);
    MetalIndirectRenderCommand command = metalRenderCommand(
        commandArguments.commandBuffer, commandIndex);
    command.drawIndexedUInt16(
        metalPrimitiveType(parameters.primitiveType), indexCount, indexBuffer, firstIndex,
        instanceCount, baseVertex, baseInstance);
}
