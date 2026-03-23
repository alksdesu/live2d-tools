#version 330 core
layout(location = 0) in vec2 positions;


uniform mat4 projection ;

void main()
{

    gl_Position = projection  * vec4(positions.x,positions.y,0,1.0f) ;
}
