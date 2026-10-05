/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license:
 * https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

// Copyright(c) Live2D Inc. All rights reserved.
// Use is governed by the Live2D Open Software license:
// https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html
#include "ccubismshaderprograms.h"
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>

namespace {
QOpenGLFunctions_3_3_Core* functions(const QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context || context->isOpenGLES())
        return nullptr;
    const auto format = context->format();
    if (format.profile() != QSurfaceFormat::CoreProfile || format.majorVersion() < 3 ||
        (format.majorVersion() == 3 && format.minorVersion() < 3)) return nullptr;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(const_cast<QOpenGLContext*>(context));
    return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
}

QByteArray vertexSource(bool setupMask, bool masked)
{
    QByteArray source = "#version 330 core\n"
        "layout(location=0) in vec4 a_position;\n"
        "layout(location=1) in vec2 a_texCoord;\n"
        "out vec2 v_texCoord;\n";
    if (setupMask) source += "uniform mat4 u_clipMatrix; out vec4 v_myPos;\n";
    else {
        source += "uniform mat4 u_matrix;\n";
        if (masked) source += "uniform mat4 u_clipMatrix; out vec4 v_clipPos;\n";
    }
    source += "void main(){\n";
    if (setupMask) source += "gl_Position=u_clipMatrix*a_position; v_myPos=gl_Position;\n";
    else {
        source += "gl_Position=u_matrix*a_position;\n";
        if (masked) source += "v_clipPos=u_clipMatrix*a_position;\n";
    }
    source += "v_texCoord=a_texCoord; v_texCoord.y=1.0-v_texCoord.y;\n}\n";
    return source;
}

QByteArray fragmentSource(bool setupMask, bool masked, bool inverted, bool premultiplied)
{
    QByteArray source = "#version 330 core\n"
        "in vec2 v_texCoord; uniform sampler2D s_texture0; uniform vec4 u_baseColor;\n"
        "layout(location=0) out vec4 fragColor;\n";
    if (setupMask) {
        source += "in vec4 v_myPos; uniform vec4 u_channelFlag;\n"
            "void main(){\n"
            "float isInside=step(u_baseColor.x,v_myPos.x/v_myPos.w)"
            "*step(u_baseColor.y,v_myPos.y/v_myPos.w)"
            "*step(v_myPos.x/v_myPos.w,u_baseColor.z)"
            "*step(v_myPos.y/v_myPos.w,u_baseColor.w);\n"
            "fragColor=u_channelFlag*texture(s_texture0,v_texCoord).a*isInside;\n}\n";
        return source;
    }
    source += "uniform vec4 u_multiplyColor; uniform vec4 u_screenColor;\n";
    if (masked) source += "in vec4 v_clipPos; uniform sampler2D s_texture1; uniform vec4 u_channelFlag;\n";
    source += "void main(){\nvec4 texColor=texture(s_texture0,v_texCoord);\n"
        "texColor.rgb=texColor.rgb*u_multiplyColor.rgb;\n";
    if (premultiplied)
        source += "texColor.rgb=(texColor.rgb+u_screenColor.rgb*texColor.a)-(texColor.rgb*u_screenColor.rgb);\n";
    else source += "texColor.rgb=texColor.rgb+u_screenColor.rgb-(texColor.rgb*u_screenColor.rgb);\n";
    source += "vec4 color=texColor*u_baseColor;\n";
    if (!premultiplied) source += "color.rgb=color.rgb*color.a;\n";
    if (masked) {
        source += "vec4 clipMask=(1.0-texture(s_texture1,v_clipPos.xy/v_clipPos.w))*u_channelFlag;\n"
            "float maskVal=clipMask.r+clipMask.g+clipMask.b+clipMask.a;\n";
        source += inverted ? "color=color*(1.0-maskVal);\n" : "color=color*maskVal;\n";
    }
    source += "fragColor=color;\n}\n";
    return source;
}

QByteArray offscreenMaskFragmentSource()
{
    return R"(#version 330 core
in vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform vec4 u_channelFlag;
layout(location=0) out vec4 fragColor;
void main(){ fragColor=u_channelFlag*texture(s_texture0,v_texCoord).a; }
)";
}

QByteArray compositeMaskedFragmentSource()
{
    return R"(#version 330 core
in vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform sampler2D s_texture1;
uniform vec4 u_baseColor;
uniform vec4 u_multiplyColor;
uniform vec4 u_screenColor;
uniform vec2 u_resolution;
uniform float u_maskInvert;
uniform vec4 u_channelFlag;
layout(location=0) out vec4 fragColor;
void main(){
  vec4 t=texture(s_texture0,v_texCoord);
  vec3 c=t.rgb*u_multiplyColor.rgb;
  c=c+u_screenColor.rgb*t.a-c*u_screenColor.rgb;
  float m=dot(texture(s_texture1,gl_FragCoord.xy/u_resolution),u_channelFlag);
  if(u_maskInvert>0.5) m=1.0-m;
  fragColor=vec4(c,t.a)*u_baseColor.a*m;
}
 )";
}

QByteArray backdropCompositeFragmentSource()
{
    return R"(#version 330 core
in vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform sampler2D s_texture1;
uniform sampler2D s_texture2;
uniform vec4 u_baseColor;
uniform vec4 u_multiplyColor;
uniform vec4 u_screenColor;
uniform vec2 u_resolution;
uniform int u_colorMode;
uniform int u_alphaMode;
uniform float u_useMask;
uniform float u_maskInvert;
uniform vec4 u_channelFlag;
layout(location=0) out vec4 fragColor;
float lum(vec3 c){return .30*c.r+.59*c.g+.11*c.b;}
vec3 clipc(vec3 c){float l=lum(c),mn=min(c.r,min(c.g,c.b)),mx=max(c.r,max(c.g,c.b));if(mn<0.)c=l+(c-l)*l/(l-mn);if(mx>1.)c=l+(c-l)*(1.-l)/(mx-l);return c;}
vec3 setl(vec3 c,float l){return clipc(c+(l-lum(c)));}
vec3 sets(vec3 c,float s){float mx=max(c.r,max(c.g,c.b)),mn=min(c.r,min(c.g,c.b)),md=c.r+c.g+c.b-mx-mn;float a=mn<mx?s:0.,b=mn<mx?(md-mn)*s/(mx-mn):0.;if(c.r==mx)return c.b<c.g?vec3(a,b,0):vec3(a,0,b);if(c.g==mx)return c.r<c.b?vec3(0,a,b):vec3(b,a,0);return c.g<c.r?vec3(b,0,a):vec3(0,b,a);}
float burn(float s,float d){return d>=.999999?1.:s<1e-6?0.:1.-min(1.,(1.-d)/s);}
float dodge(float s,float d){return d<=0.?0.:s>=1.?1.:min(1.,d/(1.-s));}
float ov(float s,float d){return d<.5?2.*s*d:1.-2.*(1.-s)*(1.-d);}
float sl(float s,float d){float a=d-(1.-2.*s)*d*(1.-d),b=d+(2.*s-1.)*d*((16.*d-12.)*d+3.),c=d+(2.*s-1.)*(sqrt(d)-d);return s<=.5?a:d<=.25?b:c;}
vec3 blend(vec3 s,vec3 d){
 if(u_colorMode==1||u_colorMode==3)return min(s+d,1.);if(u_colorMode==4)return s+d;if(u_colorMode==5)return min(s,d);if(u_colorMode==2||u_colorMode==6)return s*d;
 if(u_colorMode==7)return vec3(burn(s.r,d.r),burn(s.g,d.g),burn(s.b,d.b));if(u_colorMode==8)return max(vec3(0),s+d-1.);if(u_colorMode==9)return max(s,d);if(u_colorMode==10)return s+d-s*d;
 if(u_colorMode==11)return vec3(dodge(s.r,d.r),dodge(s.g,d.g),dodge(s.b,d.b));if(u_colorMode==12)return vec3(ov(s.r,d.r),ov(s.g,d.g),ov(s.b,d.b));if(u_colorMode==13)return vec3(sl(s.r,d.r),sl(s.g,d.g),sl(s.b,d.b));
 if(u_colorMode==14)return vec3(ov(s.r,d.r),ov(s.g,d.g),ov(s.b,d.b));if(u_colorMode==15)return 2.*s+d-1.;if(u_colorMode==16)return setl(sets(s,max(d.r,max(d.g,d.b))-min(d.r,min(d.g,d.b))),lum(d));if(u_colorMode==17)return setl(s,lum(d));return s;}
vec3 pd(float sa,float da){if(u_alphaMode==1)return vec3(sa*da,0.,da*(1.-sa));if(u_alphaMode==2)return vec3(0.,0.,da*(1.-sa));if(u_alphaMode==3)return vec3(min(sa,da),max(sa-da,0.),max(da-sa,0.));if(u_alphaMode==4)return vec3(max(sa+da-1.,0.),min(sa,1.-da),min(da,1.-sa));return vec3(sa*da,sa*(1.-da),da*(1.-sa));}
vec4 straight(vec4 c){return c.a<1e-5?vec4(0):vec4(c.rgb/c.a,c.a);}
void main(){
 vec4 t=texture(s_texture0,v_texCoord);t.rgb=t.rgb*u_multiplyColor.rgb;t.rgb=t.rgb+u_screenColor.rgb*t.a-t.rgb*u_screenColor.rgb;
 if(u_useMask>.5){float m=dot(texture(s_texture2,gl_FragCoord.xy/u_resolution),u_channelFlag);if(u_maskInvert>.5)m=1.-m;t*=m;}
 vec4 s=straight(t*u_baseColor),d=straight(texture(s_texture1,gl_FragCoord.xy/u_resolution));vec3 p=pd(s.a,d.a),c=blend(s.rgb,d.rgb);fragColor=vec4(c*p.x+s.rgb*p.y+d.rgb*p.z,p.x+p.y+p.z);
}
)";
}

bool hasRequiredLocations(std::size_t variant, QOpenGLShaderProgram& shader,
                          const CCubismShaderPrograms::Program& program)
{
    const bool offscreenMask = variant == static_cast<std::size_t>(CCubismShaderPrograms::Variant::OffscreenMask);
    if (shader.attributeLocation("a_position") != 0 || shader.attributeLocation("a_texCoord") != 1 ||
        program.sampler0 < 0 || (!offscreenMask && program.baseColor < 0)) return false;
    if (variant == 0) return program.clipMatrix >= 0 && program.channelFlag >= 0;
    if (variant == static_cast<std::size_t>(CCubismShaderPrograms::Variant::OffscreenMask))
        return program.matrix >= 0 && program.channelFlag >= 0;
    if (variant == static_cast<std::size_t>(CCubismShaderPrograms::Variant::CompositeMasked))
        return program.matrix >= 0 && program.sampler1 >= 0 && program.resolution >= 0 &&
            program.maskInvert >= 0 && program.channelFlag >= 0 && program.multiplyColor >= 0 &&
            program.screenColor >= 0;
    if (variant == static_cast<std::size_t>(CCubismShaderPrograms::Variant::BackdropComposite))
        return program.matrix >= 0 && program.sampler1 >= 0 && program.sampler2 >= 0 &&
            program.resolution >= 0 && program.colorMode >= 0 && program.alphaMode >= 0 &&
            program.useMask >= 0 && program.maskInvert >= 0 && program.channelFlag >= 0;
    if (program.matrix < 0 || program.multiplyColor < 0 || program.screenColor < 0) return false;
    const bool masked = variant == 2 || variant == 3 || variant == 5 || variant == 6;
    return !masked || (program.clipMatrix >= 0 && program.channelFlag >= 0 && program.sampler1 >= 0);
}
}

CCubismShaderPrograms::CCubismShaderPrograms() = default;
CCubismShaderPrograms::~CCubismShaderPrograms() = default;

const CCubismShaderPrograms::SourceSet& CCubismShaderPrograms::Sources()
{
    static const SourceSet sources{{
        {vertexSource(true, false), fragmentSource(true, false, false, false)},
        {vertexSource(false, false), fragmentSource(false, false, false, false)},
        {vertexSource(false, true), fragmentSource(false, true, false, false)},
        {vertexSource(false, true), fragmentSource(false, true, true, false)},
        {vertexSource(false, false), fragmentSource(false, false, false, true)},
        {vertexSource(false, true), fragmentSource(false, true, false, true)},
        {vertexSource(false, true), fragmentSource(false, true, true, true)},
        {vertexSource(false, false), offscreenMaskFragmentSource()},
        {vertexSource(false, false), compositeMaskedFragmentSource()},
        {vertexSource(false, false), backdropCompositeFragmentSource()}
    }};
    return sources;
}

bool CCubismShaderPrograms::Initialize(const QOpenGLContext* context)
{
    if (context && m_context == context && Get(Variant::SetupMask)) {
        m_error.clear();
        return true;
    }
    return Initialize(context, Sources());
}

bool CCubismShaderPrograms::Initialize(const QOpenGLContext* context, const SourceSet& sources)
{
    if (!functions(context) || (m_context && m_context != context)) {
        m_error = QStringLiteral("Shader programs require their owning OpenGL 3.3 Core context.");
        return false;
    }
    std::array<std::unique_ptr<QOpenGLShaderProgram>, ProgramCount> candidates;
    std::array<Program, ProgramCount> locations{};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto shader = std::make_unique<QOpenGLShaderProgram>();
        if (sources[index].vertex.isEmpty() || sources[index].fragment.isEmpty() ||
            !shader->addShaderFromSourceCode(QOpenGLShader::Vertex, sources[index].vertex) ||
            !shader->addShaderFromSourceCode(QOpenGLShader::Fragment, sources[index].fragment) || !shader->link()) {
            m_error = QStringLiteral("Shader %1 failed: %2").arg(qulonglong(index)).arg(shader->log());
            return false;
        }
        Program& program = locations[index];
        program.id = shader->programId();
        program.matrix = shader->uniformLocation("u_matrix");
        program.clipMatrix = shader->uniformLocation("u_clipMatrix");
        program.sampler0 = shader->uniformLocation("s_texture0");
        program.sampler1 = shader->uniformLocation("s_texture1");
        program.sampler2 = shader->uniformLocation("s_texture2");
        program.baseColor = shader->uniformLocation("u_baseColor");
        program.multiplyColor = shader->uniformLocation("u_multiplyColor");
        program.screenColor = shader->uniformLocation("u_screenColor");
        program.channelFlag = shader->uniformLocation("u_channelFlag");
        program.resolution = shader->uniformLocation("u_resolution");
        program.maskInvert = shader->uniformLocation("u_maskInvert");
        program.colorMode = shader->uniformLocation("u_colorMode");
        program.alphaMode = shader->uniformLocation("u_alphaMode");
        program.useMask = shader->uniformLocation("u_useMask");
        if (!program.id || !hasRequiredLocations(index, *shader, program)) {
            m_error = QStringLiteral("Shader %1 does not provide the required inputs.").arg(qulonglong(index));
            return false;
        }
        candidates[index] = std::move(shader);
    }
    m_context = const_cast<QOpenGLContext*>(context);
    m_programs.swap(candidates);
    m_locations = locations;
    m_error.clear();
    return true;
}

const CCubismShaderPrograms::Program* CCubismShaderPrograms::Get(Variant variant) const
{
    const auto index = static_cast<std::size_t>(variant);
    if (!m_context || !m_context->isValid() || QOpenGLContext::currentContext() != m_context ||
        index >= ProgramCount || !m_programs[index] || !m_locations[index].id) return nullptr;
    return &m_locations[index];
}

bool CCubismShaderPrograms::Bind(Variant variant, BlendMode blendMode)
{
    const Program* program = Get(variant);
    auto* gl = functions(m_context);
    if (!program || !gl || gl->glGetError() != GL_NO_ERROR) return false;
    GLenum srcColor, dstColor, srcAlpha, dstAlpha;
    switch (blendMode) {
    case BlendMode::Normal:
        srcColor = GL_ONE; dstColor = GL_ONE_MINUS_SRC_ALPHA;
        srcAlpha = GL_ONE; dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        srcColor = GL_ONE; dstColor = GL_ONE; srcAlpha = GL_ZERO; dstAlpha = GL_ONE;
        break;
    case BlendMode::Multiplicative:
        srcColor = GL_DST_COLOR; dstColor = GL_ONE_MINUS_SRC_ALPHA; srcAlpha = GL_ZERO; dstAlpha = GL_ONE;
        break;
    default: return false;
    }
    if (variant == Variant::SetupMask) {
        srcColor = GL_ZERO; dstColor = GL_ONE_MINUS_SRC_COLOR;
        srcAlpha = GL_ZERO; dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    }
    gl->glUseProgram(program->id);
    gl->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl->glBlendFuncSeparate(srcColor, dstColor, srcAlpha, dstAlpha);
    return gl->glGetError() == GL_NO_ERROR;
}

bool CCubismShaderPrograms::Release()
{
    if (m_context && QOpenGLContext::currentContext() != m_context) {
        m_error = QStringLiteral("Shader programs must be released from their owning context.");
        return false;
    }
    for (auto& program : m_programs) program.reset();
    m_locations = {};
    m_context.clear();
    m_error.clear();
    return true;
}
