#version 150
// Heat haze particle vertex shader, GL3+ (see heathaze.material)

in vec4 vertex;
in vec4 colour;
in vec4 uv0;

uniform mat4 worldViewProj;

out vec2 vUV;
out vec4 vClip;
out vec4 vColour;

void main()
{
    gl_Position = worldViewProj * vertex;
    vClip = gl_Position;
    vUV = uv0.xy;
    vColour = colour;
}
