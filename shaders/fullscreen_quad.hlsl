cbuffer ScalingBuffer : register(b0) {
    float2 uv_scale;
    float2 uv_offset;
    float2 uv_max;
    float  blend_alpha;
    float  has_start_frame;
};

struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint vertexID : SV_VertexID) {
    VS_OUT o;
    float2 raw_uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(raw_uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.uv = (raw_uv * uv_scale + uv_offset) * uv_max;
    return o;
}
