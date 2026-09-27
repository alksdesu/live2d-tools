#include "cuveditdraft.h"
#include "opengl/cuvgeometry.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool finite(QPointF point) { return std::isfinite(point.x()) && std::isfinite(point.y()); }
bool materialEqual(const CModelDrawableMaterial& a, const CModelDrawableMaterial& b)
{
    return a == b;
}
QPointF pixels(const CModelDrawableMaterial& material, int vertex, QSize size)
{
    return {double(material.uvs[vertex * 2]) * size.width(), double(material.uvs[vertex * 2 + 1]) * size.height()};
}
bool storePoint(CModelDrawableMaterial& material, int vertex, QPointF point, QSize size, QString* error)
{
    const double u = point.x() / size.width(), v = point.y() / size.height();
    if (!finite(point) || !std::isfinite(u) || !std::isfinite(v) || std::abs(u) > std::numeric_limits<float>::max() ||
        std::abs(v) > std::numeric_limits<float>::max()) return fail(error, QStringLiteral("UV 变换超出可保存的数值范围"));
    material.uvs[vertex * 2] = float(u); material.uvs[vertex * 2 + 1] = float(v);
    return true;
}
}

bool CUvEditDraft::Initialize(const Snapshot& snapshot, QString* error)
{
    QSet<QString> ids; qsizetype values = 0;
    if (snapshot.meshes.size() > 100000) return fail(error, QStringLiteral("UV 对象数量超出范围"));
    for (auto atlas = snapshot.atlasSizes.cbegin(); atlas != snapshot.atlasSizes.cend(); ++atlas)
        if (atlas.key() < 0 || atlas->isEmpty()) return fail(error, QStringLiteral("图集槽或尺寸无效"));
    for (const auto& mesh : snapshot.meshes) {
        const auto& material = mesh.material;
        if (material.id.isEmpty() || material.id.size() > 1024 || material.id.contains(QChar::Null) || ids.contains(material.id))
            return fail(error, QStringLiteral("UV 对象身份缺失或重复"));
        ids.insert(material.id);
        if (!snapshot.atlasSizes.contains(material.textureIndex) || material.uvs.size() % 2 ||
            material.uvs.size() / 2 > MaximumVertices || mesh.triangles.size() % 3 || mesh.triangles.size() > MaximumVertices * 3 ||
            material.uvs.size() > CModelMaterialEdit::MaximumValues - values)
            return fail(error, QStringLiteral("UV 对象的图集、顶点或三角索引无效：%1").arg(material.id));
        values += material.uvs.size();
        for (float value : material.uvs)
            if (!std::isfinite(value)) return fail(error, QStringLiteral("UV 包含非有限坐标：%1").arg(material.id));
        for (auto index : mesh.triangles)
            if (index >= material.uvs.size() / 2) return fail(error, QStringLiteral("UV 三角索引越界：%1").arg(material.id));
    }
    m_snapshot = snapshot;
    if (error) error->clear();
    return true;
}

CModelMaterialState CUvEditDraft::InitialState() const
{
    CModelMaterialState result; result.reserve(m_snapshot.meshes.size());
    for (const auto& mesh : m_snapshot.meshes) result.append(mesh.material);
    return result;
}

bool CUvEditDraft::ValidateState(const CModelMaterialState& state, QString* error) const
{
    if (state.size() != m_snapshot.meshes.size()) return fail(error, QStringLiteral("UV 对象数量已经改变"));
    for (int index = 0; index < state.size(); ++index) {
        const auto& material = state[index]; const auto& source = m_snapshot.meshes[index];
        if (material.id != source.material.id || material.uvs.size() != source.material.uvs.size() ||
            !m_snapshot.atlasSizes.contains(material.textureIndex)) return fail(error, QStringLiteral("UV 对象、顶点数量或图集槽已经改变"));
        if (!source.editable && !materialEqual(material, source.material))
            return fail(error, source.reason.isEmpty() ? QStringLiteral("当前对象不可编辑：%1").arg(material.id) : source.reason);
        for (float value : material.uvs)
            if (!std::isfinite(value)) return fail(error, QStringLiteral("UV 包含非有限坐标：%1").arg(material.id));
    }
    if (error) error->clear();
    return true;
}

int CUvEditDraft::MeshIndex(const QString& id) const
{
    for (int index = 0; index < m_snapshot.meshes.size(); ++index)
        if (m_snapshot.meshes[index].material.id == id) return index;
    return -1;
}

bool CUvEditDraft::CheckSelection(const CModelMaterialState& state, const Selection& selection, int& index, QString* error) const
{
    if (!ValidateState(state, error)) return false;
    index = MeshIndex(selection.meshId);
    if (index < 0 || selection.vertices.isEmpty()) return fail(error, QStringLiteral("请先选择 UV 顶点"));
    const auto& mesh = m_snapshot.meshes[index];
    if (!mesh.editable) return fail(error, mesh.reason.isEmpty() ? QStringLiteral("当前对象不可编辑") : mesh.reason);
    for (int vertex : selection.vertices)
        if (vertex < 0 || vertex >= state[index].uvs.size() / 2) return fail(error, QStringLiteral("UV 选点已经失效"));
    return true;
}

bool CUvEditDraft::SelectionBounds(const CModelMaterialState& state, const Selection& selection, QRectF& result, QString* error) const
{
    int index;
    if (!CheckSelection(state, selection, index, error)) return false;
    const auto& material = state[index]; const auto size = m_snapshot.atlasSizes.value(material.textureIndex);
    double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
    for (int vertex : selection.vertices) {
        const auto point = pixels(material, vertex, size);
        left = std::min(left, point.x()); right = std::max(right, point.x());
        top = std::min(top, point.y()); bottom = std::max(bottom, point.y());
    }
    result = QRectF(QPointF(left, top), QPointF(right, bottom));
    if (error) error->clear();
    return true;
}

bool CUvEditDraft::TransformSelection(const CModelMaterialState& state, const Selection& selection,
                                     const Transform& transform, CModelMaterialState& result, QString* error) const
{
    int index;
    if (!CheckSelection(state, selection, index, error)) return false;
    if (!finite(transform.translation) || !finite(transform.pivot) || !finite(transform.scale) || !std::isfinite(transform.angleDegrees))
        return fail(error, QStringLiteral("UV 变换必须使用有限数值"));
    if (transform.translation.isNull() && transform.scale == QPointF(1, 1) &&
        std::remainder(transform.angleDegrees, 360.0) == 0 && !transform.snapToPixels) {
        result = state; if (error) error->clear(); return true;
    }
    auto candidate = state; auto& material = candidate[index];
    const auto size = m_snapshot.atlasSizes.value(material.textureIndex);
    const double radians = std::remainder(transform.angleDegrees, 360.0) * std::acos(-1.0) / 180.0;
    const double c = std::cos(radians), s = std::sin(radians);
    for (int vertex : selection.vertices) {
        auto point = pixels(state[index], vertex, size) - transform.pivot;
        point = {point.x() * transform.scale.x(), point.y() * transform.scale.y()};
        point = QPointF(c * point.x() - s * point.y(), s * point.x() + c * point.y()) + transform.pivot + transform.translation;
        if (transform.snapToPixels) point = {std::round(point.x()), std::round(point.y())};
        if (!storePoint(material, vertex, point, size, error)) return false;
    }
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

bool CUvEditDraft::SetPointPixels(const CModelMaterialState& state, const Selection& selection, QPointF point,
                                 CModelMaterialState& result, QString* error) const
{
    int index;
    if (!CheckSelection(state, selection, index, error)) return false;
    if (selection.vertices.size() != 1) return fail(error, QStringLiteral("绝对像素坐标需要只选中一个顶点"));
    auto candidate = state; auto& material = candidate[index];
    if (!storePoint(material, *selection.vertices.cbegin(), point, m_snapshot.atlasSizes.value(material.textureIndex), error)) return false;
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

bool CUvEditDraft::AssignTexture(const CModelMaterialState& state, const QString& meshId, int slot,
                                SlotCoordinates coordinates, CModelMaterialState& result, QString* error) const
{
    if (!ValidateState(state, error)) return false;
    const int index = MeshIndex(meshId);
    if (index < 0 || !m_snapshot.meshes[index].editable) return fail(error, QStringLiteral("当前对象不可分配图集"));
    if (!m_snapshot.atlasSizes.contains(slot) || (coordinates != SlotCoordinates::Normalized && coordinates != SlotCoordinates::Pixels))
        return fail(error, QStringLiteral("目标图集或坐标选项无效"));
    auto candidate = state; auto& material = candidate[index];
    const auto oldSize = m_snapshot.atlasSizes.value(material.textureIndex), nextSize = m_snapshot.atlasSizes.value(slot);
    material.textureIndex = slot;
    if (coordinates == SlotCoordinates::Pixels)
        for (int vertex = 0; vertex < material.uvs.size() / 2; ++vertex)
            if (!storePoint(material, vertex, pixels(state[index], vertex, oldSize), nextSize, error)) return false;
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

bool CUvEditDraft::Reset(const CModelMaterialState& state, const Selection& selection, bool allMeshes,
                        CModelMaterialState& result, QString* error) const
{
    if (!ValidateState(state, error)) return false;
    auto candidate = state;
    if (allMeshes) {
        for (int index = 0; index < candidate.size(); ++index)
            if (m_snapshot.meshes[index].editable) candidate[index] = m_snapshot.meshes[index].material;
    } else {
        int index;
        if (!CheckSelection(state, selection, index, error)) return false;
        const auto& initial = m_snapshot.meshes[index].material;
        for (int vertex : selection.vertices) {
            candidate[index].uvs[vertex * 2] = initial.uvs[vertex * 2];
            candidate[index].uvs[vertex * 2 + 1] = initial.uvs[vertex * 2 + 1];
        }
    }
    result.swap(candidate);
    if (error) error->clear();
    return true;
}

QSet<int> CUvEditDraft::ConnectedIsland(const QString& meshId, int vertex) const
{
    const int index = MeshIndex(meshId);
    if (index < 0 || vertex < 0 || vertex >= m_snapshot.meshes[index].material.uvs.size() / 2) return {};
    const auto& mesh = m_snapshot.meshes[index];
    QVector<QVector<int>> neighbors(mesh.material.uvs.size() / 2);
    for (int triangle = 0; triangle < mesh.triangles.size(); triangle += 3)
        for (int edge = 0; edge < 3; ++edge) {
            const int a = mesh.triangles[triangle + edge], b = mesh.triangles[triangle + (edge + 1) % 3];
            neighbors[a].append(b); neighbors[b].append(a);
        }
    QSet<int> result{vertex}; QVector<int> pending{vertex};
    for (int at = 0; at < pending.size(); ++at)
        for (int other : neighbors[pending[at]]) if (!result.contains(other)) { result.insert(other); pending.append(other); }
    return result;
}

QStringList CUvEditDraft::Diagnostics(const CModelMaterialState& state, const QString& meshId) const
{
    QString error;
    if (!ValidateState(state, &error)) return {error};
    const int index = MeshIndex(meshId); if (index < 0) return {};
    const auto& material = state[index]; const auto& mesh = m_snapshot.meshes[index];
    const auto size = m_snapshot.atlasSizes.value(material.textureIndex);
    int outside = 0, degenerate = 0;
    for (int vertex = 0; vertex < material.uvs.size() / 2; ++vertex) {
        const double u = material.uvs[vertex * 2], v = material.uvs[vertex * 2 + 1];
        outside += u < 0 || u > 1 || v < 0 || v > 1;
    }
    for (int triangle = 0; triangle < mesh.triangles.size(); triangle += 3)
        degenerate += CUvGeometry::Orientation(pixels(material, mesh.triangles[triangle], size),
            pixels(material, mesh.triangles[triangle + 1], size), pixels(material, mesh.triangles[triangle + 2], size)) == 0;
    QStringList result;
    if (!mesh.editable) result.append(mesh.reason.isEmpty() ? QStringLiteral("此对象不可编辑") : mesh.reason);
    if (outside) result.append(QStringLiteral("%1 个顶点在图集外；越界采样可能影响预览和导出").arg(outside));
    if (degenerate) result.append(QStringLiteral("%1 个 UV 三角形面积为零").arg(degenerate));
    return result;
}
}
