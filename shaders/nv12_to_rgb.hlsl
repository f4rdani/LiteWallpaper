// NV12 (YUV 4:2:0) to RGB conversion pixel shader
// Y plane: texture array 0 (R8_UNORM)
// UV plane: texture array 1 (R8G8_UNORM)

Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Sample slice 0 of the bound Shader Resource View
    float y = texY.Sample(samp, float3(uv, 0.0f));
    float2 uv_val = texUV.Sample(samp, float3(uv, 0.0f));
    
    // BT.709 YUV to RGB conversion matrix
    float u = uv_val.x - 0.5f;
    float v = uv_val.y - 0.5f;
    
    float r = y + 1.5748f * v;
    float g = y - 0.1873f * u - 0.4681f * v;
    float b = y + 1.8556f * u;
    
    return float4(saturate(float3(r, g, b)), 1.0f);
}
