#include "cdrawrotation.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

CDrawRotation::CDrawRotation() = default;
CDrawRotation::~CDrawRotation() = default;

bool CDrawRotation::EnsureOpenGl()
{
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return false;
    if (!m_context) {
        shader.reset();
        m_rclTex.reset();
        m_dirTex.reset();
        m_context = context;
    }
    if (!m_originGeometry.SetContext(context) || !m_directionGeometry.SetContext(context)) return false;
    if (!shader) {
        auto candidate = std::make_unique<QOpenGLShaderProgram>();
        if (!candidate->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_rotation.vert") ||
            !candidate->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_rotation.frag") ||
            !candidate->link()) return false;
        shader = std::move(candidate);
    }
    if (!m_rclTex || !m_dirTex) {
        const QImage origin(":/awesomeface.png"), direction(":/read.png");
        if (origin.isNull() || direction.isNull()) return false;
        auto originTexture = std::make_unique<QOpenGLTexture>(origin.mirrored());
        auto directionTexture = std::make_unique<QOpenGLTexture>(direction.mirrored());
        if (!originTexture->isCreated() || !directionTexture->isCreated()) return false;
        m_rclTex = std::move(originTexture);
        m_dirTex = std::move(directionTexture);
    }
    return shader->isLinked();
}

bool CDrawRotation::Init(CRotationNode* node)
{
    vetrclPos.clear();
    vetdirPos.clear();
    if (!node || !node->GetCanvasBasis() || node->GetCurKeyForm().GetPosition().size() != 2 ||
        node->GetCanvasPositions().size() != 2) return false;
    auto& form = node->GetCurKeyForm();
    if (!std::isfinite(node->GetBaseAngles()) || !std::isfinite(form.Getangles()) ||
        !std::isfinite(form.Getorigin_x()) || !std::isfinite(form.Getorigin_y())) return false;
    return ClaRect(*node->GetCanvasBasis());
}

bool CDrawRotation::PrepareOpenGl()
{
    if (vetrclPos.size() != 8 || vetdirPos.size() != 8 || vetTex.size() != 8 || vetIndex.size() != 6) return false;
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return false;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
    if (!gl || !gl->initializeOpenGLFunctions()) return false;
    GLint program = 0, activeTexture = 0, texture = 0, unpackBuffer = 0;
    GLint alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    gl->glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    gl->glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    const auto restore = qScopeGuard([&] {
        gl->glUseProgram(program);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glActiveTexture(activeTexture);
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    });
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    return EnsureOpenGl() &&
        m_originGeometry.Upload(vetrclPos.data(), 4, vetTex.data(), vetIndex.data(), 6) &&
        m_directionGeometry.Upload(vetdirPos.data(), 4, vetTex.data(), vetIndex.data(), 6);
}

void CDrawRotation::Draw(const QMatrix4x4& projection)
{
    if (!m_IsDraw || !std::all_of(projection.constData(), projection.constData() + 16,
                                 [](float value) { return std::isfinite(value); }) || !PrepareOpenGl()) return;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(QOpenGLContext::currentContext());
    GLint program = 0, activeTexture = 0, texture = 0;
    const GLboolean culling = gl->glIsEnabled(GL_CULL_FACE);
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    const auto restore = qScopeGuard([&] {
        gl->glUseProgram(program);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glActiveTexture(activeTexture);
        if (culling) gl->glEnable(GL_CULL_FACE);
    });
    gl->glDisable(GL_CULL_FACE);
    if (!shader->bind()) return;
    shader->setUniformValue("projection", projection);
    shader->setUniformValue("image", 0);
    m_rclTex->bind(0);
    if (!m_originGeometry.Draw(GL_TRIANGLES)) return;
    m_dirTex->bind(0);
    m_directionGeometry.Draw(GL_TRIANGLES);
}

void CDrawRotation::SetDraw(bool flag)
{
    m_IsDraw = flag;
}

void CDrawRotation::InitOpenGl()
{}

bool CDrawRotation::ClaRect(const CCoordinateFrame::RotationBasis& basis)
{
    const double x = basis.origin.x(), y = basis.origin.y();
    constexpr double halfSide = 0.025;
    const std::array<double, 8> origin{x - halfSide, y + halfSide, x - halfSide, y - halfSide,
                                      x + halfSide, y - halfSide, x + halfSide, y + halfSide};
    const std::array<QPointF, 4> direction{QPointF(-0.01, 0), QPointF(-0.01, -0.1),
                                          QPointF(0.01, -0.1), QPointF(0.01, 0)};
    std::vector<float> originPositions(8), directionPositions(8);
    const double maximum = std::numeric_limits<float>::max();
    for (size_t index = 0; index < direction.size(); ++index) {
        const QPointF point = basis.origin + basis.xAxis * direction[index].x() + basis.yAxis * direction[index].y();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            std::abs(point.x()) > maximum || std::abs(point.y()) > maximum ||
            !std::isfinite(origin[index * 2]) || !std::isfinite(origin[index * 2 + 1]) ||
            std::abs(origin[index * 2]) > maximum || std::abs(origin[index * 2 + 1]) > maximum) return false;
        directionPositions[index * 2] = static_cast<float>(point.x());
        directionPositions[index * 2 + 1] = -static_cast<float>(point.y());
        originPositions[index * 2] = static_cast<float>(origin[index * 2]);
        originPositions[index * 2 + 1] = -static_cast<float>(origin[index * 2 + 1]);
    }
    vetrclPos = std::move(originPositions);
    vetdirPos = std::move(directionPositions);
    vetIndex = {0, 1, 3, 1, 2, 3};
    vetTex = {0, 0, 1, 0, 1, 1, 0, 1};
    return true;
}
