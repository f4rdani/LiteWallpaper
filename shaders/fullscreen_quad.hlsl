// Fullscreen triangle trick — no vertex buffer needed!
// Draw 3 vertices forming a large triangle that covers the full viewport
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint vertexID : SV_VertexID) {
    VS_OUT o;
    // Generate fullscreen triangle from vertex ID (0, 1, 2)
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
