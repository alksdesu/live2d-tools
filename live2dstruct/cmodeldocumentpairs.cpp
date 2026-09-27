#include "cmodeldocument.h"
#include <algorithm>
#include <cstring>
#include <exception>

namespace live2dstruct {
namespace {
using Pairs = CGeometryObjectPairs;
using Builder = CMocParameterStructureBuilder;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QVector<int> identity(int count)
{
    QVector<int> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) result.append(index);
    return result;
}

bool sameKeys(const QVector<float>& first, const QVector<float>& second)
{
    return first.size() == second.size() && (first.isEmpty() ||
        std::memcmp(first.constData(), second.constData(), size_t(first.size()) * sizeof(float)) == 0);
}

bool exists(const CModelDocument& document, const Pairs::Object& object)
{
    if (object.type == Pairs::Type::ArtMesh)
        return std::any_of(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& mesh) { return mesh.id == object.id; });
    return std::any_of(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& deformer) {
        return deformer.id == object.id && (deformer.type == CModelDeformer::Warp ? object.type == Pairs::Type::Warp
                                                                              : object.type == Pairs::Type::Rotation);
    });
}

QPair<int, QString> objectKey(const Pairs::Object& object) { return {int(object.type), object.id}; }

Pairs::Object object(const Builder::Owner& owner)
{
    const auto type = owner.type == Builder::Owner::ArtMesh ? Pairs::Type::ArtMesh :
        owner.type == Builder::Owner::Warp ? Pairs::Type::Warp : Pairs::Type::Rotation;
    return {type, owner.id};
}
}

bool CModelDocument::SetGeometryObjectPairs(const CGeometryObjectPairs::State& state, QString* error) try
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型，无法保存几何对象配对"));
    Pairs::State candidate;
    if (!Pairs::Normalize(state, candidate, error) || !Pairs::Validate(candidate, *this, error)) return false;
    if (candidate == m_geometryObjectPairs) return true;
    m_geometryObjectPairs = std::move(candidate);
    ++m_revision;
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法保存几何对象配对，原状态保持不变")); }

bool CModelDocument::MakeGeometryObjectPairsEdit(const CGeometryObjectPairs::State& state, CModelEdit& edit, QString* error) const try
{
    auto candidate = *this;
    if (!candidate.SetGeometryObjectPairs(state, error)) return false;
    CModelEdit command{QStringLiteral("geometryPairs"), QStringLiteral("objects"), CModelEdit::GeometryObjectPairs,
        QVariant::fromValue(m_geometryObjectPairs), QVariant::fromValue(candidate.m_geometryObjectPairs), -1, false};
    edit = std::move(command);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备几何对象配对修改，原结果保持不变")); }

bool CModelDocument::ReconcileGeometryObjectPairs(CModelDocument& candidate, const QVector<Pairs::VertexRemap>& vertices,
                                                 const Builder::Result* parameters,
                                                 const Builder::EditBindingKeysRequest* keys, QString* error) const
{
    QSet<QPair<int, QString>> paired;
    for (const auto& pair : m_geometryObjectPairs.pairs) {
        paired.insert(objectKey(pair.first.object)); paired.insert(objectKey(pair.second.object));
    }
    QVector<Pairs::VertexRemap> relevantVertices;
    for (const auto& remap : vertices) if (paired.contains(objectKey(remap.object))) relevantVertices.append(remap);
    if (!parameters) return Pairs::Reconcile(m_geometryObjectPairs, *this, candidate, relevantVertices, {}, candidate.m_geometryObjectPairs, error);
    QHash<QPair<int, QString>, const Builder::KeyformRemap*> keyforms;
    for (const auto& remap : parameters->keyforms) keyforms.insert(objectKey(object(remap.owner)), &remap);
    QSet<QPair<int, QString>> seen;
    QVector<Pairs::AxisRemap> axes;
    for (const auto& pair : m_geometryObjectPairs.pairs) for (const auto* endpoint : {&pair.first, &pair.second}) {
        if (pair.requiresReview) continue;
        const auto& target = endpoint->object;
        const auto key = objectKey(target);
        if (seen.contains(key) || !exists(candidate, target)) continue;
        seen.insert(key);
        Pairs::Endpoint before, after;
        if (!Pairs::DescribeEndpoint(*this, target, before, error)) return false;
        if (!Pairs::DescribeEndpoint(candidate, target, after)) continue;
        const auto* remap = keyforms.value(key, nullptr);
        int oldStride = 1;
        for (int oldAxis = 0; oldAxis < before.axes.size(); ++oldAxis) {
            const auto& previous = before.axes[oldAxis];
            int newAxis = -1, newStride = 1;
            bool ambiguous = false;
            for (int index = 0; index < after.axes.size(); ++index) {
                if (after.axes[index].parameterId == previous.parameterId) {
                    if (newAxis != -1) ambiguous = true;
                    newAxis = index;
                }
            }
            if (ambiguous) { oldStride *= previous.keys.size(); continue; }
            for (int index = 0; index < newAxis; ++index) newStride *= after.axes[index].keys.size();
            QVector<int> mapping(previous.keys.size(), -1);
            bool explicitIdentity = newAxis == -1;
            if (newAxis >= 0 && keys && object(keys->owner) == target && keys->axisIndex == oldAxis) {
                for (int index = 0; index < keys->keys.size(); ++index) {
                    const int previousIndex = keys->keys[index].oldKeyIndex;
                    if (previousIndex < 0) continue;
                    if (previousIndex >= mapping.size() || mapping[previousIndex] != -1 || index >= after.axes[newAxis].keys.size())
                        return fail(error, QStringLiteral("配对关键值的迁移身份无效"));
                    mapping[previousIndex] = index;
                }
                explicitIdentity = true;
            } else if (newAxis >= 0 && previous.parameterType == after.axes[newAxis].parameterType &&
                       sameKeys(previous.keys, after.axes[newAxis].keys)) {
                mapping = identity(previous.keys.size());
                explicitIdentity = true;
            } else if (newAxis >= 0 && remap && remap->newLocalToOldLocal.size() == after.keyformCount &&
                       remap->oldKeyformCount == before.keyformCount && remap->newKeyformCount == after.keyformCount) {
                bool conflict = false;
                for (int nextLocal = 0; nextLocal < remap->newLocalToOldLocal.size(); ++nextLocal) {
                    const int previousLocal = remap->newLocalToOldLocal[nextLocal];
                    if (previousLocal < 0) continue;
                    if (previousLocal >= before.keyformCount) return fail(error, QStringLiteral("配对关键形迁移身份越界"));
                    const int oldKey = previousLocal / oldStride % previous.keys.size();
                    const int newKey = nextLocal / newStride % after.axes[newAxis].keys.size();
                    if (mapping[oldKey] != -1 && mapping[oldKey] != newKey) { conflict = true; break; }
                    mapping[oldKey] = newKey;
                }
                explicitIdentity = !conflict;
            }
            if (explicitIdentity) axes.append({target, oldAxis, newAxis, std::move(mapping)});
            oldStride *= previous.keys.size();
        }
    }
    return Pairs::Reconcile(m_geometryObjectPairs, *this, candidate, relevantVertices, axes, candidate.m_geometryObjectPairs, error);
}
}
