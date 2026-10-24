cbuffer ScalingBuffer : register(b0) {
    float2 uv_scale;
    float2 uv_offset;
    float2 uv_max;
    float  blend_alpha;
    float  has_start_frame;
};

Texture2DArray<float>  texY        : register(t0);
Texture2DArray<float2> texUV       : register(t1);
Texture2DArray<float>  texY_start  : register(t2);
Texture2DArray<float2> texUV_start : register(t3);
SamplerState           samp        : register(s0);

static float3 ConvertNV12ToBT709(float y_raw, float2 uv_val) {
    // Expand limited video range (16..235 -> 0..1) if needed, or standard BT.709 YCbCr -> RGB
    float u = uv_val.x - 0.5f;
    float v = uv_val.y - 0.5f;
    float r = y_raw + 1.5748f * v;
    float g = y_raw - 0.1873f * u - 0.4681f * v;
    float b = y_raw + 1.8556f * u;
    return saturate(float3(r, g, b));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 clamped_uv = clamp(uv, float2(0.0f, 0.0f), uv_max);
    float3 uvw = float3(clamped_uv, 0.0f);

    float y = texY.Sample(samp, uvw);
    float2 uv_val = texUV.Sample(samp, uvw);
    float3 rgb_curr = ConvertNV12ToBT709(y, uv_val);

    if (has_start_frame > 0.5f && blend_alpha > 0.001f) {
        float y_s = texY_start.Sample(samp, uvw);
        float2 uv_val_s = texUV_start.Sample(samp, uvw);
        float3 rgb_start = ConvertNV12ToBT709(y_s, uv_val_s);

        float t = smoothstep(0.0f, 1.0f, saturate(blend_alpha));
        return float4(lerp(rgb_curr, rgb_start, t), 1.0f);
    }

    return float4(rgb_curr, 1.0f);
}
