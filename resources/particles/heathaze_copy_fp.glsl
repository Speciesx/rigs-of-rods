#version 150
// Fullscreen copy for the heat haze compositor, GL3+

in vec2 vUV;

uniform sampler2D srcMap;

out vec4 fragColour;

void main()
{
    fragColour = texture(srcMap, vUV);
}
