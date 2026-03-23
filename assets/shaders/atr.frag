#version 330 core
out vec4 FragColor;
in vec2 TexCord;
uniform sampler2D textureMaskFrame;
void main(){
    vec4 texColor =texture2D(textureMaskFrame,TexCord);
    texColor.a = 0.0f;
    FragColor = texColor;
}
