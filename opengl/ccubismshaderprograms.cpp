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

bool hasRequiredLocations(std::size_t variant, QOpenGLShaderProgram& shader,
                          const CCubismShaderPrograms::Program& program)
{
    if (shader.attributeLocation("a_position") != 0 || shader.attributeLocation("a_texCoord") != 1 ||
        program.sampler0 < 0 || program.baseColor < 0) return false;
    if (variant == 0) return program.clipMatrix >= 0 && program.channelFlag >= 0;
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
        {vertexSource(false, true), fragmentSource(false, true, true, true)}
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
        program.baseColor = shader->uniformLocation("u_baseColor");
        program.multiplyColor = shader->uniformLocation("u_multiplyColor");
        program.screenColor = shader->uniformLocation("u_screenColor");
        program.channelFlag = shader->uniformLocation("u_channelFlag");
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
