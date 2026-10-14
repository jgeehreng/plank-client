#include <metal_stdlib>
using namespace metal;

struct Vertex
{
    float4 position [[ position ]];
    float2 texCoords;
};

struct CscParams
{
    float3x3 matrix;
    float3 offsets;
    float bitnessScaleFactor;
};

constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);

// Extremely small windows use a separable horizontal pass, leaving the vertical
// reduction to the normal draw. This prevents millions of serial lookups in a
// single fragment when (for example) a 5K frame is reduced to one drawable pixel.
// The intermediate is floating point, including for low-aligned 10-bit planes.
kernel void cs_reduce_horizontal(texture2d<float> plane [[texture(0)]],
                                 texture2d<float, access::write> output [[texture(1)]],
                                 constant float2 &crop [[buffer(0)]],
                                 uint2 pos [[thread_position_in_grid]])
{
    if (pos.x >= output.get_width() || pos.y >= output.get_height()) return;
    const float width = plane.get_width();
    const float span = max((crop.y - crop.x) * width / output.get_width(), 1.0f);
    const float center = (crop.x + (pos.x + 0.5f) * (crop.y - crop.x) / output.get_width()) * width;
    const float begin = center - span * 0.5f;
    const float end = begin + span;
    float4 result = 0.0f;
    for (int x = int(floor(begin)); x < int(ceil(end)); x += 2) {
        const float2 columns = float2(x, x + 1);
        const float2 wx = max(min(columns + 1.0f, end) - max(columns, begin), 0.0f);
        const float weight = wx.x + wx.y;
        const float u = (x + 0.5f + wx.y / weight) / width;
        result += plane.sample(s, float2(u, (pos.y + 0.5f) / plane.get_height()), level(0.0f)) * (weight / span);
    }
    output.write(result, pos);
}

// Integrate the source pixels covered by one output pixel when reducing video.
// A fixed bilinear lookup can skip whole rows/columns during minification.
// Pair adjacent texels into one weighted linear lookup in each dimension: the
// result is an area average, without an intermediate image or CPU readback.
float4 sampleVideoPlane(texture2d<float> plane, float2 uv)
{
    const float2 size(plane.get_width(), plane.get_height());
    const float2 footprint = max((abs(dfdx(uv)) + abs(dfdy(uv))) * size, 1.0f);
    // Derivatives have small roundoff at non-power-of-two dimensions. Preserve
    // the existing 1:1/upscale path (including chroma reconstruction).
    if (all(footprint <= 1.001f)) {
        return plane.sample(s, uv, level(0.0f));
    }

    const float2 begin = uv * size - footprint * 0.5f;
    const float2 end = begin + footprint;
    float4 result = 0.0f;
    for (int y = int(floor(begin.y)); y < int(ceil(end.y)); y += 2) {
        const float2 rows = float2(y, y + 1);
        const float2 wy = max(min(rows + 1.0f, end.y) - max(rows, begin.y), 0.0f);
        const float weightY = wy.x + wy.y;
        const float sampleY = (y + 0.5f + wy.y / weightY) / size.y;
        float4 row = 0.0f;
        for (int x = int(floor(begin.x)); x < int(ceil(end.x)); x += 2) {
            const float2 columns = float2(x, x + 1);
            const float2 wx = max(min(columns + 1.0f, end.x) - max(columns, begin.x), 0.0f);
            const float weightX = wx.x + wx.y;
            const float sampleX = (x + 0.5f + wx.y / weightX) / size.x;
            // Explicit level avoids implicit derivatives inside variable loops.
            row += plane.sample(s, float2(sampleX, sampleY), level(0.0f)) * (weightX / footprint.x);
        }
        result += row * (weightY / footprint.y);
    }
    return result;
}

vertex Vertex vs_draw(constant Vertex *vertices [[ buffer(0) ]], uint id [[ vertex_id ]])
{
    return vertices[id];
}

fragment float4 ps_draw_biplanar(Vertex v [[ stage_in ]],
                                constant CscParams &cscParams [[ buffer(0) ]],
                                texture2d<float> luminancePlane [[ texture(0) ]],
                                texture2d<float> chrominancePlane [[ texture(1) ]])
{
    float3 yuv = float3(sampleVideoPlane(luminancePlane, v.texCoords).r,
                      sampleVideoPlane(chrominancePlane, v.texCoords).rg);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return float4(yuv * cscParams.matrix, 1.0f);
}

fragment float4 ps_draw_triplanar(Vertex v [[ stage_in ]],
                                 constant CscParams &cscParams [[ buffer(0) ]],
                                 texture2d<float> luminancePlane [[ texture(0) ]],
                                 texture2d<float> chrominancePlaneU [[ texture(1) ]],
                                 texture2d<float> chrominancePlaneV [[ texture(2) ]])
{
    float3 yuv = float3(sampleVideoPlane(luminancePlane, v.texCoords).r,
                      sampleVideoPlane(chrominancePlaneU, v.texCoords).r,
                      sampleVideoPlane(chrominancePlaneV, v.texCoords).r);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return float4(yuv * cscParams.matrix, 1.0f);
}

fragment float4 ps_draw_rgb(Vertex v [[ stage_in ]],
                           texture2d<float> rgbTexture [[ texture(0) ]])
{
    return rgbTexture.sample(s, v.texCoords);
}
