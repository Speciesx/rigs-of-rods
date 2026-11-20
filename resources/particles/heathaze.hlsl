// Heat haze shaders, D3D9 and D3D11 (see heathaze.material)

void HeatHaze_vp(
    float4 iPos    : POSITION,
    float4 iColour : COLOR,
    float2 iUV     : TEXCOORD0,

    out float4 oPos    : POSITION,
    out float2 oUV     : TEXCOORD0,
    out float4 oClip   : TEXCOORD1,
    out float4 oColour : COLOR,

    uniform float4x4 worldViewProj)
{
    oPos = mul(worldViewProj, iPos);
    oClip = oPos;
    oUV = iUV;
    oColour = iColour;
}

float4 HeatHaze_fp(
    float2 iUV     : TEXCOORD0,
    float4 iClip   : TEXCOORD1,
    float4 iColour : COLOR,

    uniform sampler2D sceneMap : register(s0),
    uniform sampler2D noiseMap : register(s1),
    uniform float time,
    uniform float strength) : COLOR
{
    // soft round particle
    float2 c = iUV * 2.0 - 1.0;
    float fall = saturate(1.0 - dot(c, c));
    fall *= fall;

    // two scrolling noise layers -> offset in -1..1
    float2 n = tex2D(noiseMap, iUV * 0.7 + float2(0.0, time * 0.9)).rg
             + tex2D(noiseMap, iUV * 1.3 + float2(time * 0.37, time * 0.6)).rg - 1.0;

    // screen position of this pixel in the scene texture (D3D: v goes down)
    float2 screen = iClip.xy / iClip.w;
    screen = float2(screen.x * 0.5 + 0.5, 0.5 - screen.y * 0.5);

    float amount = fall * iColour.a;
    float3 scene = tex2D(sceneMap, screen + n * strength * amount).rgb;
    return float4(scene, saturate(amount * 3.0));
}

void HeatHazeCopy_vp(
    float4 iPos : POSITION,
    float2 iUV  : TEXCOORD0,

    out float4 oPos : POSITION,
    out float2 oUV  : TEXCOORD0,

    uniform float4x4 worldViewProj)
{
    oPos = mul(worldViewProj, iPos);
    oUV = iUV;
}

float4 HeatHazeCopy_fp(
    float2 iUV : TEXCOORD0,
    uniform sampler2D srcMap : register(s0)) : COLOR
{
    return tex2D(srcMap, iUV);
}
