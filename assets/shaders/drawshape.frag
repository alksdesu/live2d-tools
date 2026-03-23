#version 330 core

out vec4 color;
uniform vec3 spriteColor;


void main()
{
    color =vec4(spriteColor,1.0f) ;
}
