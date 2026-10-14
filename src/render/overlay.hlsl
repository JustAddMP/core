// Overlay shader: textured, vertex-coloured quads in pixel coordinates.
// Compiled at build time by build.cmd with fxc (vs_4_0 / ps_4_0) into out\obj\gen\overlay_{vs,ps}.h.

cbuffer Screen : register(b0) {
    float2 invScreen;  // 1 / backbuffer size in pixels
    float2 pad;
};

struct VSIn {
    float2 pos : POSITION;  // pixels, origin top-left
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.x * invScreen.x * 2.0 - 1.0, 1.0 - i.pos.y * invScreen.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

Texture2D fontTex : register(t0);
SamplerState fontSampler : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    float coverage = fontTex.Sample(fontSampler, i.uv).r;
    return float4(i.col.rgb, i.col.a * coverage);
}
