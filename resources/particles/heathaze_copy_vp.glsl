#version 150
// Fullscreen copy for the heat haze compositor, GL3+

in vec4 vertex;
in vec4 uv0;

uniform mat4 worldViewProj;

out vec2 vUV;

void main()
{
    gl_Position = worldViewProj * vertex;
    vUV = uv0.xy;
}
