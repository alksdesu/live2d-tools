#include "cdrawmesh.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <limits>

CDrawMesh::CDrawMesh(int ebosize, const short* vetebo, int uvsize, const float* vetuv, const float* vetpos)
    : mesh(new CTrianglMesh(ebosize, vetebo, uvsize, vetuv, vetpos))
{}

CDrawMesh::~CDrawMesh()
{
    delete mesh;
}

bool CDrawMesh::EnsureOpenGl()
{
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return false;
    if (!m_context) {
        shader.reset();
        m_context = context;
    }
    if (!m_regularPoints.SetContext(context) || !m_selectedPoints.SetContext(context)) return false;
    if (!shader) {
        auto candidate = std::make_unique<QOpenGLShaderProgram>();
        if (!candidate->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_meshPoint_circle.vert") ||
            !candidate->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_meshPoint_circle.frag") ||
            !candidate->link()) return false;
        shader = std::move(candidate);
    }
    return shader->isLinked();
}

void CDrawMesh::Draw(const QMatrix4x4& projection)
{
    if ((!IsDraw && !IsOver) || !mesh ||
        !std::all_of(projection.constData(), projection.constData() + 16,
                     [](float value) { return std::isfinite(value); })) return;
    const auto& points = mesh->GetWolrd();
    const auto& edges = mesh->GetEdge();
    if (points.empty() || points.size() > static_cast<size_t>(CGlMeshBuffer::MaxVertices)) return;
    std::vector<float> positions, regular, selected;
    positions.reserve(points.size() * 2);
    for (const auto* point : points) {
        if (!point || !std::isfinite(point->GetPoint().x()) || !std::isfinite(point->GetPoint().y())) return;
        const auto& value = point->GetPoint();
        positions.insert(positions.end(), {value.x(), value.y()});
        auto& group = point->GetIsSelect() ? selected : regular;
        group.insert(group.end(), {value.x(), value.y()});
    }
    std::vector<quint16> indices;
    indices.reserve(edges.size() * 2);
    for (const auto* edge : edges) {
        if (!edge || edge->start < 0 || edge->end < 0 ||
            static_cast<size_t>(edge->start) >= points.size() || static_cast<size_t>(edge->end) >= points.size() ||
            edge->start > std::numeric_limits<quint16>::max() || edge->end > std::numeric_limits<quint16>::max()) return;
        indices.push_back(static_cast<quint16>(edge->start));
        indices.push_back(static_cast<quint16>(edge->end));
    }
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->isValid() || (m_context && m_context != context)) return;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
    if (!gl || !gl->initializeOpenGLFunctions()) return;
    GLint program = 0;
    GLfloat pointSize = 0, lineWidth = 0;
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl->glGetFloatv(GL_POINT_SIZE, &pointSize);
    gl->glGetFloatv(GL_LINE_WIDTH, &lineWidth);
    const GLboolean programPointSize = gl->glIsEnabled(GL_PROGRAM_POINT_SIZE);
    const auto restore = qScopeGuard([&] {
        gl->glUseProgram(program);
        gl->glPointSize(pointSize);
        gl->glLineWidth(lineWidth);
        if (programPointSize) gl->glEnable(GL_PROGRAM_POINT_SIZE);
        else gl->glDisable(GL_PROGRAM_POINT_SIZE);
    });
    if (!EnsureOpenGl()) return;
    if ((!regular.empty() && !m_regularPoints.Upload(regular.data(), static_cast<int>(regular.size() / 2), nullptr, nullptr, 0)) ||
        (!selected.empty() && !m_selectedPoints.Upload(selected.data(), static_cast<int>(selected.size() / 2), nullptr, nullptr, 0))) return;
    constexpr size_t batchSize = CGlMeshBuffer::MaxIndices - CGlMeshBuffer::MaxIndices % 2;
    const size_t batches = (indices.size() + batchSize - 1) / batchSize;
    while (m_lines.size() < batches) m_lines.push_back(std::make_unique<CGlMeshBuffer>());
    for (size_t batch = 0; batch < batches; ++batch) {
        const size_t offset = batch * batchSize;
        if (!m_lines[batch]->SetContext(context) ||
            !m_lines[batch]->Upload(positions.data(), static_cast<int>(points.size()), nullptr,
                                   indices.data() + offset, static_cast<int>(std::min(batchSize, indices.size() - offset)))) return;
    }
    if (!shader->bind()) return;
    shader->setUniformValue("projection", projection);
    const QVector4D color = IsOver && !IsDraw ? QVector4D(0, 1, 0, 1) : QVector4D(0.3f, 0.7f, 1, 0.6f);
    gl->glDisable(GL_PROGRAM_POINT_SIZE);
    gl->glPointSize(4);
    gl->glLineWidth(1);
    shader->setUniformValue("outerColor", color);
    if (!regular.empty() && !m_regularPoints.Draw(GL_POINTS)) return;
    shader->setUniformValue("outerColor", QVector4D(1, 0, 0, 1));
    if (!selected.empty() && !m_selectedPoints.Draw(GL_POINTS)) return;
    shader->setUniformValue("outerColor", color);
    for (size_t batch = 0; batch < batches; ++batch)
        if (!m_lines[batch]->Draw(GL_LINES)) return;
}

void CDrawMesh::MoveSelect(const QVector2D &vet)
{
    MoveSelected(vet);
}

bool CDrawMesh::SetPositions(const QVector<float>& positions)
{
    return mesh && mesh->SetPositions(positions);
}

bool CDrawMesh::SetIndices(const QVector<quint16>& indices)
{
    return mesh && mesh->SetIndices(indices);
}

bool CDrawMesh::SelectNearestPoint(const QVector2D& position, float radius)
{
    const bool selected = mesh && mesh->SelectNearestPoint(position, radius);
    RefreshSelectedPoints();
    return selected;
}

bool CDrawMesh::CanCopyEditorStateFrom(const CDrawMesh& source) const
{
    if (!mesh || !source.mesh) return false;
    const auto& points = mesh->GetWolrd();
    const auto& sourcePoints = source.mesh->GetWolrd();
    if (points.size() != sourcePoints.size()) return false;
    for (size_t index = 0; index < points.size(); ++index)
        if (!points[index] || !sourcePoints[index]) return false;
    return true;
}

bool CDrawMesh::CopyEditorStateFrom(const CDrawMesh& source)
{
    if (!CanCopyEditorStateFrom(source)) return false;
    const auto& points = mesh->GetWolrd();
    const auto& sourcePoints = source.mesh->GetWolrd();
    std::vector<CPoint*> selected;
    selected.reserve(points.size());
    for (size_t index = 0; index < points.size(); ++index)
        if (sourcePoints[index]->GetIsSelect()) selected.push_back(points[index]);
    for (size_t index = 0; index < points.size(); ++index)
        points[index]->SetIsSelect(sourcePoints[index]->GetIsSelect());
    IsDraw = source.IsDraw;
    IsOver = source.IsOver;
    m_vetSelectPoint.swap(selected);
    return true;
}

void CDrawMesh::MoveSelected(const QVector2D& delta)
{
    if (mesh) mesh->MoveSelected(delta);
}

QVector<float> CDrawMesh::Positions() const
{
    return mesh ? mesh->Positions() : QVector<float>();
}

//世界坐标系
void CDrawMesh::SetSelect(const QRectF &rect)
{
    if (!mesh) return;
    for (auto* point : mesh->GetWolrd())
        if (point) point->SetIsSelect(rect.contains(point->GetPoint().toPointF()));
    RefreshSelectedPoints();
}

void CDrawMesh::CancelSelect()
{
    if (mesh) for (auto* point : mesh->GetWolrd())
        if (point) point->SetIsSelect(false);
    m_vetSelectPoint.clear();
}

void CDrawMesh::RefreshSelectedPoints() const
{
    m_vetSelectPoint.clear();
    if (mesh) for (auto* point : mesh->GetWolrd())
        if (point && point->GetIsSelect()) m_vetSelectPoint.push_back(point);
}

const std::vector<CPoint*> &CDrawMesh::GetSelectPoint() const
{
    RefreshSelectedPoints();
    return m_vetSelectPoint;//应该是选择的点进行移动
}

void CDrawMesh::AddPoint(const QVector2D &vet)
{
    mesh->AddPoint(vet);
}

bool CDrawMesh::ClickMesh(const QPointF &point)
{
    //当前点选中了mesh吗？
    return mesh->GetContentPoint(point);//->GetRectF().contains(point);
}
