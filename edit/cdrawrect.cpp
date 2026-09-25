#include "cdrawrect.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

CDrawRect::CDrawRect(const QVector3D& color)
    : m_color(color)
{}

CDrawRect::~CDrawRect() = default;

void CDrawRect::InitOpenGl()
{}

bool CDrawRect::EnsureOpenGl()
{
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return false;
    if (!m_context) {
        shader.reset();
        m_context = context;
    }
    if (!m_geometry.SetContext(context)) return false;
    if (!shader) {
        auto candidate = std::make_unique<QOpenGLShaderProgram>();
        if (!candidate->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/drawshape.vert") ||
            !candidate->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/drawshape.frag") ||
            !candidate->link()) return false;
        shader = std::move(candidate);
    }
    return shader->isLinked();
}

void CDrawRect::SetDraw(bool flag)
{
    m_IsDraw = flag;
}

void CDrawRect::Draw(const QMatrix4x4& projection)
{
    if (!m_IsDraw || m_rect.isEmpty() ||
        !std::isfinite(m_color.x()) || !std::isfinite(m_color.y()) || !std::isfinite(m_color.z()) ||
        !std::all_of(projection.constData(), projection.constData() + 16,
                     [](float value) { return std::isfinite(value); })) return;
    const std::array<double, 8> corners{m_rect.left(), m_rect.top(), m_rect.left(), m_rect.bottom(),
                                       m_rect.right(), m_rect.bottom(), m_rect.right(), m_rect.top()};
    std::array<float, 8> positions;
    for (size_t index = 0; index < corners.size(); ++index) {
        if (!std::isfinite(corners[index]) || std::abs(corners[index]) > std::numeric_limits<float>::max()) return;
        positions[index] = static_cast<float>(corners[index]);
    }
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
    if (!gl || !gl->initializeOpenGLFunctions()) return;
    GLint program = 0;
    GLfloat lineWidth = 0;
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl->glGetFloatv(GL_LINE_WIDTH, &lineWidth);
    const auto restore = qScopeGuard([&] {
        gl->glUseProgram(program);
        gl->glLineWidth(lineWidth);
    });
    if (!EnsureOpenGl() || !m_geometry.Upload(positions.data(), 4, nullptr, nullptr, 0) || !shader->bind()) return;
    shader->setUniformValue("projection", projection);
    shader->setUniformValue("spriteColor", m_color);
    gl->glLineWidth(1);
    m_geometry.Draw(GL_LINE_LOOP);
}

void CDrawRect::SetStartAndEndPos(const QVector2D &startPos, const QVector2D &endPos)
{
    if(startPos.x() == 0 && startPos.y() == 0 && endPos.x() == 0 && endPos.y() == 0){
        m_IsDraw = false;
    }else{
        m_IsDraw = true;
    }

    this->startPos = startPos;
    this->endPos = endPos;
    SetQRectF();
}

void CDrawRect::SetSelectRect(const QRectF &rect)
{
    if(rect.width() == 0 && rect.height() == 0 ){
        m_IsDraw = false;
    }else{
        m_IsDraw = true;
    }

    m_rect = rect;
}

void CDrawRect::SetQRectF()
{
    float x = startPos.x() < endPos.x() ? startPos.x() : endPos.x();
    float y = startPos.y() < endPos.y() ? startPos.y() : endPos.y();
    float w = abs(startPos.x() - endPos.x());
    float h = abs(startPos.y() - endPos.y());
    m_rect = QRectF(x,y,w,h);
}

bool CDrawRect::IsContains(const QPointF &point)
{
    if(m_rect.contains(point)){
        return true;
    }
    return false;

};

//传入的是世界坐标系上的二点
void CDrawRect::Move(const QVector2D &vet)
{
    QMatrix4x4 move;
    move.setToIdentity();
    move.translate(QVector3D(vet,0));//世界坐标系的位置
    QVector4D start2 =move * QVector4D(this->startPos,0,1.0f);
    QVector4D end2 = move * QVector4D(this->endPos,0,1.0f);
    this->startPos = QVector2D(start2);
    this->endPos = QVector2D(end2);
    SetQRectF();
}

const QRectF &CDrawRect::GetQQRectF()
{
    return m_rect;
}
