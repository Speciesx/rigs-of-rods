OGRE_NATIVE_GLSL_VERSION_DIRECTIVE

precision highp int;
precision highp float;

uniform sampler2D sampleTexture;

in vec4 outUV0;
in vec4 outColor;

out vec4 fragColor;

void main()
{
	fragColor = outColor * textureLod(sampleTexture, outUV0.xy, 0.0); // RoR: GUI textures have no mipmaps
}
