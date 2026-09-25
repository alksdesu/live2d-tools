#include "cdrawwarp.h"

#include <QScopeGuard>

CDrawWarp::CDrawWarp()
{
    InitOpenGl();
}

CDrawWarp::~CDrawWarp()
{
    delete shader;
}

bool CDrawWarp::EnsureOpenGl()
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return false;
    setOwningContext(context);
    if (!initializeOpenGLFunctions()) return false;
    if (m_context != context) {
        m_vertexArray.destroy();
        m_vertexBuffer.destroy();
        m_indexBuffer.destroy();
        delete shader;
        shader = nullptr;
        m_context = context;
        m_buffersDirty = true;
    }
    if (!shader) {
        shader = new QOpenGLShaderProgram();
        if (!shader->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_warp.vert") ||
            !shader->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_warp.frag") ||
            !shader->link()) {
            qWarning() << "Warp shader failed:" << shader->log();
            return false;
        }
    }
    if (!shader->isLinked()) return false;
    if (!m_vertexArray.isCreated()) {
        if (!m_vertexArray.create()) return false;
        m_buffersDirty = true;
    }
    if (!m_vertexBuffer.isCreated()) {
        if (!m_vertexBuffer.create()) return false;
        m_buffersDirty = true;
    }
    if (!m_indexBuffer.isCreated()) {
        if (!m_indexBuffer.create()) return false;
        m_buffersDirty = true;
    }
    m_vertexBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_indexBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    return true;
}

void CDrawWarp::Init(CWarpNode* node)
{
    m_buffersDirty = true;
    if (!node) m_grid.Clear();
    else m_grid.SetGrid(node->GetColumns(), node->GetRows(), node->GetCanvasPositions());
}

void CDrawWarp::UpdatePositions(CWarpNode* node)
{
    Init(node);
}

void CDrawWarp::InitTest(int i, CWarpNode* node)
{
    m_buffersDirty = true;
    if (!node || i < 0 || static_cast<std::size_t>(i) >= node->GetKeyFormS().size()) {
        m_grid.Clear();
        return;
    }
    std::vector<float> positions;
    if (!node->MapToCanvas(node->GetKeyFormS()[i].GetPosition(), positions)) {
        m_grid.Clear();
        return;
    }
    m_grid.SetGrid(node->GetColumns(), node->GetRows(), positions);
}

void CDrawWarp::Draw(const QMatrix4x4& mv)
{
    if (!m_IsDraw || m_grid.Indices().empty() || !EnsureOpenGl()) return;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousProgram = 0;
    GLfloat previousLineWidth = 1.0f;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);
    const auto restore = qScopeGuard([&] {
        glBindVertexArray(static_cast<GLuint>(previousVertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
        glUseProgram(static_cast<GLuint>(previousProgram));
        glLineWidth(previousLineWidth);
    });
    m_vertexArray.bind();
    if (!m_vertexBuffer.bind() || !m_indexBuffer.bind() || !shader->bind()) return;
    if (m_buffersDirty) {
        const int vertexBytes = static_cast<int>(m_grid.Positions().size() * sizeof(float));
        const int indexBytes = static_cast<int>(m_grid.Indices().size() * sizeof(quint32));
        m_vertexBuffer.allocate(m_grid.Positions().data(), vertexBytes);
        m_indexBuffer.allocate(m_grid.Indices().data(), indexBytes);
        if (m_vertexBuffer.size() != vertexBytes || m_indexBuffer.size() != indexBytes) return;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        m_buffersDirty = false;
    }
    shader->setUniformValue("projection", mv);
    shader->setUniformValue("spriteColor", color);
    glLineWidth(1.0f);
    glDrawElements(GL_LINES, static_cast<GLsizei>(m_grid.Indices().size()), GL_UNSIGNED_INT, nullptr);
}

void CDrawWarp::InitOpenGl()
{
    EnsureOpenGl();
}

void CDrawWarp::SetDraw(bool flag)
{
    m_IsDraw = flag;
}
