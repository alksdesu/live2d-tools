#include "ctriangl.h"
#include "live2dstruct/cmeshtopology.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kMaxVertices = 1000000;
constexpr int kMaxIndices = 3000000;
constexpr int kMaxIndex = std::numeric_limits<short>::max();
constexpr double kWeightTolerance = 32.0 * std::numeric_limits<double>::epsilon();

bool finitePoint(const QVector2D& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool finiteFloat(double value)
{
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}
}

CPoint::CPoint(const QVector2D& point) : m_IsSelect(false)
{
    SetPoint(point);
}

void CPoint::SetPoint(const QVector2D& point)
{
    if (finitePoint(point)) this->point = point;
}

const QVector2D& CPoint::GetPoint() const
{
    return point;
}

const bool CPoint::GetIsSelect() const
{
    return m_IsSelect;
}

void CPoint::SetIsSelect(bool select)
{
    m_IsSelect = select;
}

CEdge::CEdge(int start, int end) : start(start), end(end), IsEAdd(false) {}

CTriangl::CTriangl(int a, int b, int c) : pointA(a), pointB(b), pointC(c) {}

CTrianglMesh::CTrianglMesh(int ebosize, const short* indices, int uvsize, const float* uvs, const float* positions)
{
    if (ebosize < 0 || ebosize > kMaxIndices || ebosize % 3 != 0 ||
        uvsize < 0 || uvsize > kMaxVertices * 2 || uvsize % 2 != 0 ||
        (ebosize > 0 && !indices) || (uvsize > 0 && (!uvs || !positions))) return;
    const int count = uvsize / 2;
    for (int i = 0; i < ebosize; ++i) {
        if (indices[i] < 0 || indices[i] >= count) return;
    }
    for (int i = 0; i < uvsize; ++i) {
        if (!std::isfinite(uvs[i]) || !std::isfinite(positions[i])) return;
    }
    vetPosPoint.reserve(count);
    vetTexPoint.reserve(count);
    vetTriangl.reserve(ebosize / 3);
    for (int i = 0; i < uvsize; i += 2) {
        vetPosPoint.push_back(new CPoint(QVector2D(positions[i], positions[i + 1])));
        vetTexPoint.push_back(new QVector2D(uvs[i], uvs[i + 1]));
    }
    for (int i = 0; i < ebosize; i += 3) {
        vetTriangl.push_back(new CTriangl(indices[i], indices[i + 1], indices[i + 2]));
    }
    RebuildEdges();
    normalizing();
}

CTrianglMesh::~CTrianglMesh()
{
    for (CEdge* edge : vetEdge) delete edge;
    for (CPoint* point : vetPosPoint) delete point;
    for (QVector2D* point : vetTexPoint) delete point;
    for (CTriangl* triangle : vetTriangl) delete triangle;
}

void CTrianglMesh::RebuildEdges()
{
    for (CEdge* edge : vetEdge) delete edge;
    vetEdge.clear();
    vetEdge.reserve(vetTriangl.size() * 3);
    for (const CTriangl* triangle : vetTriangl) {
        vetEdge.push_back(new CEdge(triangle->pointA, triangle->pointB));
        vetEdge.push_back(new CEdge(triangle->pointB, triangle->pointC));
        vetEdge.push_back(new CEdge(triangle->pointC, triangle->pointA));
    }
}

void CTrianglMesh::normalizing()
{
    vetebo.clear();
    vetuv.clear();
    vetpos.clear();
    m_meshRect = QRectF();
    m_UvMeshRect = QRectF();
    vetebo.reserve(vetTriangl.size() * 3);
    vetuv.reserve(vetTexPoint.size() * 2);
    vetpos.reserve(vetPosPoint.size() * 2);
    for (const CTriangl* triangle : vetTriangl) {
        vetebo.push_back(static_cast<short>(triangle->pointA));
        vetebo.push_back(static_cast<short>(triangle->pointB));
        vetebo.push_back(static_cast<short>(triangle->pointC));
    }
    if (vetPosPoint.empty()) return;
    double left = std::numeric_limits<double>::max();
    double right = -left;
    double top = left;
    double bottom = right;
    double uvLeft = left;
    double uvRight = right;
    double uvTop = top;
    double uvBottom = bottom;
    for (size_t i = 0; i < vetPosPoint.size(); ++i) {
        const QVector2D& position = vetPosPoint[i]->GetPoint();
        const QVector2D& uv = *vetTexPoint[i];
        vetpos.push_back(position.x());
        vetpos.push_back(position.y());
        vetuv.push_back(uv.x());
        vetuv.push_back(uv.y());
        left = std::min(left, static_cast<double>(position.x()));
        right = std::max(right, static_cast<double>(position.x()));
        top = std::min(top, static_cast<double>(position.y()));
        bottom = std::max(bottom, static_cast<double>(position.y()));
        uvLeft = std::min(uvLeft, static_cast<double>(uv.x()));
        uvRight = std::max(uvRight, static_cast<double>(uv.x()));
        uvTop = std::min(uvTop, 1.0 - uv.y());
        uvBottom = std::max(uvBottom, 1.0 - uv.y());
    }
    m_meshRect = QRectF(left, top, right - left, bottom - top);
    m_UvMeshRect = QRectF(uvLeft, uvTop, uvRight - uvLeft, uvBottom - uvTop);
}

bool CTrianglMesh::TriangleWeights(const QPointF& position, const CTriangl& triangle, double (&weights)[3]) const
{
    if (!std::isfinite(position.x()) || !std::isfinite(position.y())) return false;
    for (int index : {triangle.pointA, triangle.pointB, triangle.pointC}) {
        if (index < 0 || static_cast<size_t>(index) >= vetPosPoint.size()) return false;
    }
    const QPointF a = vetPosPoint[triangle.pointA]->GetPoint().toPointF();
    const QPointF b = vetPosPoint[triangle.pointB]->GetPoint().toPointF();
    const QPointF c = vetPosPoint[triangle.pointC]->GetPoint().toPointF();
    const QPointF ab = b - a;
    const QPointF ac = c - a;
    const QPointF ap = position - a;
    const double positive = ab.x() * ac.y();
    const double negative = ab.y() * ac.x();
    const double determinant = positive - negative;
    if (std::abs(determinant) <= kWeightTolerance * (std::abs(positive) + std::abs(negative))) return false;
    weights[1] = (ap.x() * ac.y() - ap.y() * ac.x()) / determinant;
    weights[2] = (ab.x() * ap.y() - ab.y() * ap.x()) / determinant;
    weights[0] = 1.0 - weights[1] - weights[2];
    for (double weight : weights) {
        if (!std::isfinite(weight) || weight < -kWeightTolerance || weight > 1.0 + kWeightTolerance) return false;
    }
    return true;
}

bool CTrianglMesh::GetContentPoint(const QPointF& point) const
{
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || !m_meshRect.contains(point)) return false;
    for (const CTriangl* triangle : vetTriangl) {
        double weights[3];
        if (TriangleWeights(point, *triangle, weights)) return true;
    }
    return false;
}

bool CTrianglMesh::AddPoint(const QVector2D& position)
{
    if (!finitePoint(position) || vetPosPoint.size() > kMaxIndex || vetTriangl.size() * 3 > kMaxIndices - 6) return false;
    for (size_t i = 0; i < vetTriangl.size(); ++i) {
        const CTriangl triangle = *vetTriangl[i];
        double weights[3];
        if (!TriangleWeights(position.toPointF(), triangle, weights)) continue;
        if (weights[0] <= kWeightTolerance || weights[1] <= kWeightTolerance || weights[2] <= kWeightTolerance) continue;
        const int sourceIndices[3] = {triangle.pointA, triangle.pointB, triangle.pointC};
        double u = 0.0;
        double v = 0.0;
        for (int vertex = 0; vertex < 3; ++vertex) {
            const QVector2D& uv = *vetTexPoint[sourceIndices[vertex]];
            u += weights[vertex] * uv.x();
            v += weights[vertex] * uv.y();
        }
        if (!finiteFloat(u) || !finiteFloat(v)) return false;
        const int index = static_cast<int>(vetPosPoint.size());
        vetPosPoint.push_back(new CPoint(position));
        vetTexPoint.push_back(new QVector2D(static_cast<float>(u), static_cast<float>(v)));
        *vetTriangl[i] = CTriangl(triangle.pointA, triangle.pointB, index);
        vetTriangl.push_back(new CTriangl(triangle.pointB, triangle.pointC, index));
        vetTriangl.push_back(new CTriangl(triangle.pointC, triangle.pointA, index));
        RebuildEdges();
        normalizing();
        return true;
    }
    return false;
}

bool CTrianglMesh::SetPositions(const QVector<float>& positions)
{
    if (positions.size() != static_cast<qsizetype>(vetPosPoint.size() * 2)) return false;
    for (float value : positions) if (!std::isfinite(value)) return false;
    for (size_t i = 0; i < vetPosPoint.size(); ++i) {
        vetPosPoint[i]->SetPoint(QVector2D(positions[i * 2], positions[i * 2 + 1]));
    }
    normalizing();
    return true;
}

bool CTrianglMesh::SetIndices(const QVector<quint16>& indices)
{
    if (!live2dstruct::CMeshTopology::IsValid(indices, static_cast<int>(vetPosPoint.size()))) return false;
    for (CTriangl* triangle : vetTriangl) delete triangle;
    vetTriangl.clear();
    vetTriangl.reserve(indices.size() / 3);
    for (qsizetype i = 0; i < indices.size(); i += 3) {
        vetTriangl.push_back(new CTriangl(indices[i], indices[i + 1], indices[i + 2]));
    }
    RebuildEdges();
    normalizing();
    return true;
}

bool CTrianglMesh::SelectNearestPoint(const QVector2D& position, float radius)
{
    if (!finitePoint(position) || !std::isfinite(radius) || radius < 0.0f) return false;
    int nearest = -1;
    double nearestDistance = static_cast<double>(radius) * radius;
    for (size_t i = 0; i < vetPosPoint.size(); ++i) {
        const QVector2D& point = vetPosPoint[i]->GetPoint();
        const double dx = static_cast<double>(point.x()) - position.x();
        const double dy = static_cast<double>(point.y()) - position.y();
        const double distance = dx * dx + dy * dy;
        if (distance <= nearestDistance) {
            nearest = static_cast<int>(i);
            nearestDistance = distance;
        }
        vetPosPoint[i]->SetIsSelect(false);
    }
    if (nearest < 0) return false;
    vetPosPoint[nearest]->SetIsSelect(true);
    return true;
}

void CTrianglMesh::MoveSelected(const QVector2D& delta)
{
    if (!finitePoint(delta)) return;
    for (const CPoint* point : vetPosPoint) {
        if (!point->GetIsSelect()) continue;
        const double x = static_cast<double>(point->GetPoint().x()) + delta.x();
        const double y = static_cast<double>(point->GetPoint().y()) + delta.y();
        if (!finiteFloat(x) || !finiteFloat(y)) return;
    }
    for (CPoint* point : vetPosPoint) {
        if (point->GetIsSelect()) point->SetPoint(point->GetPoint() + delta);
    }
    normalizing();
}

QVector<float> CTrianglMesh::Positions() const
{
    QVector<float> positions;
    positions.reserve(static_cast<qsizetype>(vetPosPoint.size() * 2));
    for (const CPoint* point : vetPosPoint) {
        positions.append(point->GetPoint().x());
        positions.append(point->GetPoint().y());
    }
    return positions;
}

void CTrianglMesh::DeleteEdge(int, int) {}

const short* CTrianglMesh::GetEBOIndex() const
{
    return vetebo.data();
}

const int CTrianglMesh::GetEBOSize() const
{
    return static_cast<int>(vetebo.size());
}

const float* CTrianglMesh::GetUV() const
{
    return vetuv.data();
}

const float* CTrianglMesh::GetPos() const
{
    return vetpos.data();
}

const std::vector<CPoint*>& CTrianglMesh::GetWolrd() const
{
    return vetPosPoint;
}

const std::vector<CEdge*>& CTrianglMesh::GetEdge() const
{
    return vetEdge;
}

std::vector<CPoint*>& CTrianglMesh::GetCpoint()
{
    return vetPosPoint;
}
