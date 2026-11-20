#version 150
// Heat haze particle fragment shader, GL3+ (see heathaze.material)

in vec2 vUV;
in vec4 vClip;
in vec4 vColour;

uniform sampler2D sceneMap;
uniform sampler2D noiseMap;
uniform float time;
uniform float strength;

out vec4 fragColour;

void main()
{
    vec2 c = vUV * 2.0 - 1.0;
    float fall = clamp(1.0 - dot(c, c), 0.0, 1.0);
    fall *= fall;

    vec2 n = texture(noiseMap, vUV * 0.7 + vec2(0.0, time * 0.9)).rg
           + texture(noiseMap, vUV * 1.3 + vec2(time * 0.37, time * 0.6)).rg - 1.0;

    vec2 screen = vClip.xy / vClip.w * 0.5 + 0.5;

    float amount = fall * vColour.a;
    vec3 scene = texture(sceneMap, screen + n * strength * amount).rgb;
    fragColour = vec4(scene, clamp(amount * 3.0, 0.0, 1.0));
}
