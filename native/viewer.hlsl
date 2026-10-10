// Two passes: ps_convert turns packed YUY2 (uploaded as RGBA8 of width/2: Y0,U,Y1,V)
// into RGB; ps_blit draws that texture scaled into the target viewport.
Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer Params : register(b0) {
    float srcW;
    float srcH;
    float useBt709;
    float pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 ps_convert(VSOut i) : SV_Target {
    int x = (int)i.pos.x;
    int y = (int)i.pos.y;
    float4 p = tex.Load(int3(x >> 1, y, 0));
    float Y = (x & 1) ? p.b : p.r;
    float yy = (Y * 255.0 - 16.0) / 219.0;
    float u = (p.g * 255.0 - 128.0) / 224.0;
    float v = (p.a * 255.0 - 128.0) / 224.0;
    float3 rgb;
    if (useBt709 > 0.5) {
        rgb = float3(yy + 1.5748 * v, yy - 0.1873 * u - 0.4681 * v, yy + 1.8556 * u);
    } else {
        rgb = float3(yy + 1.402 * v, yy - 0.3441 * u - 0.7141 * v, yy + 1.772 * u);
    }
    return float4(saturate(rgb), 1.0);
}

float4 ps_blit(VSOut i) : SV_Target {
    return float4(tex.Sample(samp, i.uv).rgb, 1.0);
}
