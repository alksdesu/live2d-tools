#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCord;
out vec2 TexCord;
void main(){
    gl_Position = vec4(aPos.x, aPos.y, 0, 1.0f);
    TexCord = aTexCord;    //通过流水线的方式进行输出
}
