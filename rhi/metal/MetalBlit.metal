#include <metal_stdlib>
using namespace metal;

struct MGIPBlitParameters
{
    int2 sourceOffset0;
    int2 sourceOffset1;
    int2 destinationOffset0;
    int2 destinationOffset1;
    uint sourceMipLevel;
    uint destinationMipLevel;
};

[[kernel]] void mgipMetal4Blit2D(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant MGIPBlitParameters& parameters [[buffer(0)]],
    uint2 threadPosition [[thread_position_in_grid]])
{
    const int2 destinationDelta =
        parameters.destinationOffset1 - parameters.destinationOffset0;
    const uint2 destinationExtent = uint2(abs(destinationDelta));
    if(any(threadPosition >= destinationExtent))
        return;

    const int2 destinationDirection = select(int2(-1), int2(1),
                                              destinationDelta > int2(0));
    const int2 destinationCoordinate = parameters.destinationOffset0 +
        destinationDirection * int2(threadPosition) -
        select(int2(1), int2(0), destinationDelta > int2(0));

    const int2 sourceDelta = parameters.sourceOffset1 - parameters.sourceOffset0;
    const float2 sourcePosition = float2(parameters.sourceOffset0) +
        (float2(threadPosition) + 0.5f) * float2(sourceDelta) /
        float2(destinationExtent) - 0.5f;
    const int2 sourceLowerBound = min(parameters.sourceOffset0,
                                      parameters.sourceOffset1);
    const int2 sourceUpperBound = max(parameters.sourceOffset0,
                                      parameters.sourceOffset1) - int2(1);
    const int2 sourceBase = int2(floor(sourcePosition));
    const float2 sourceFraction = fract(sourcePosition);
    const uint2 p00 = uint2(clamp(sourceBase, sourceLowerBound, sourceUpperBound));
    const uint2 p10 = uint2(clamp(sourceBase + int2(1, 0),
                                  sourceLowerBound, sourceUpperBound));
    const uint2 p01 = uint2(clamp(sourceBase + int2(0, 1),
                                  sourceLowerBound, sourceUpperBound));
    const uint2 p11 = uint2(clamp(sourceBase + int2(1),
                                  sourceLowerBound, sourceUpperBound));

    const float4 row0 = mix(source.read(p00, parameters.sourceMipLevel),
                            source.read(p10, parameters.sourceMipLevel),
                            sourceFraction.x);
    const float4 row1 = mix(source.read(p01, parameters.sourceMipLevel),
                            source.read(p11, parameters.sourceMipLevel),
                            sourceFraction.x);
    destination.write(mix(row0, row1, sourceFraction.y),
                      uint2(destinationCoordinate),
                      parameters.destinationMipLevel);
}
