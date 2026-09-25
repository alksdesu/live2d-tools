#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCord;

out vec2 TexCoords;
uniform mat4 projection ;


void main()
{
    TexCoords = aTexCord;
    gl_Position = projection * vec4(aPos, 0.0, 1.0);
}
