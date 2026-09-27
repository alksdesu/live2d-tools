#include "cmodeldocument.h"
#include "cmodelsession.h"
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace live2dstruct {
struct CModelMeshEditVerification::Data
{
    CModelEdit expected;
    CModelDocument candidate;
    QByteArray sourceBytes, workingBytes, sourceDigest;
    QString sourcePath;
    CTextureAssets::Catalog sources;
};

namespace {
bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}
class SnapshotFreezer
{
public:
    QString owned(const QString& value)
    {
        const auto found = strings.constFind(value);
        if (found != strings.cend())
            return *found;
        retain(qint64(value.size()) * 2 + 64);
        const QString result(value.constData(), value.size());
        strings.insert(result);
        return result;
    }
    QByteArray owned(const QByteArray& value)
    {
        if (value.isEmpty())
            return { };
        const auto hint = blobHints.constFind({ quintptr(value.constData()), value.size() });
        if (hint != blobHints.cend()) {
            const auto found = blobs.constFind(*hint);
            if (found != blobs.cend() && *found == value)
                return *found;
        }
        const auto digest = QCryptographicHash::hash(value, QCryptographicHash::Sha256);
        const auto found = blobs.constFind(digest);
        if (found != blobs.cend()) {
            if (*found != value)
                throw std::invalid_argument("Conflicting frozen bytes");
            blobHints.insert({ quintptr(value.constData()), value.size() }, digest);
            return *found;
        }
        retain(value.size());
        const QByteArray result(value.constData(), value.size());
        blobs.insert(digest, result);
        blobHints.insert({ quintptr(value.constData()), value.size() }, digest);
        return result;
    }
    template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0> T owned(T value) { return value; }

    template <class T> QVector<T> owned(const QVector<T>& values)
    {
        if constexpr (std::is_arithmetic_v<T>) {
            const auto key = qMakePair(quintptr(values.constData()), values.size() * qsizetype(sizeof(T)));
            const auto found = vectors.constFind(key);
            if (found != vectors.cend() && found->metaType() == QMetaType::fromType<QVector<T>>()) {
                const auto frozen = found->template value<QVector<T>>();
                if (frozen.size() == values.size()
                    && (values.isEmpty()
                        || std::memcmp(frozen.constData(), values.constData(), size_t(key.second)) == 0))
                    return frozen;
            }
            retain(key.second + sizeof(QVariant) + sizeof(QVector<T>) + 64);
            QVector<T> result(values.cbegin(), values.cend());
            vectors.insert(key, QVariant::fromValue(result));
            return result;
        } else {
            retain(qint64(values.size()) * sizeof(T));
            QVector<T> result;
            result.reserve(values.size());
            for (const auto& value : values)
                result.append(owned(value));
            return result;
        }
    }
    template <class K, class V> QMap<K, V> owned(const QMap<K, V>& values)
    {
        retain(qint64(values.size()) * (sizeof(K) + sizeof(V) + 32));
        QMap<K, V> result;
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            result.insert(owned(it.key()), owned(it.value()));
        return result;
    }
    template <class V> QHash<QString, V> owned(const QHash<QString, V>& values)
    {
        retain(qint64(values.size()) * (sizeof(QString) + sizeof(V) + 32));
        QHash<QString, V> result;
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            result.insert(owned(it.key()), owned(it.value()));
        return result;
    }
    QSet<QString> owned(const QSet<QString>& values)
    {
        QSet<QString> result;
        for (const auto& value : values)
            result.insert(owned(value));
        return result;
    }
    CModelParameterMetadata owned(const CModelParameterMetadata& value)
    {
        auto result = value;
        result.displayName = owned(value.displayName);
        return result;
    }
    CModelRotationKeyform owned(const CModelRotationKeyform& value) { return value; }
    CModelWarpKeyform owned(const CModelWarpKeyform& value)
    {
        auto result = value;
        result.positions = owned(value.positions);
        return result;
    }
    CModelDrawableMaterial owned(const CModelDrawableMaterial& value)
    {
        return { owned(value.id), value.textureIndex, owned(value.uvs) };
    }
    CTextureAssets::Source owned(const CTextureAssets::Source& value)
    {
        return { owned(value.path), owned(value.png), owned(value.digest), value.size };
    }
    CParameterOrganization::State owned(const CParameterOrganization::State& value)
    {
        CParameterOrganization::State result;
        result.configured = value.configured;
        for (const auto& node : value.nodes)
            result.nodes.append(
                { node.kind, owned(node.id), owned(node.parentFolder), owned(node.name), owned(node.color) });
        for (const auto& pair : value.pairs)
            result.pairs.append({ owned(pair.first), owned(pair.second), pair.reversed, pair.enabled });
        return result;
    }
    CGeometryObjectPairs::State owned(const CGeometryObjectPairs::State& value)
    {
        CGeometryObjectPairs::State result;
        result.sourceDigest = owned(value.sourceDigest);
        for (const auto& original : value.pairs) {
            auto pair = original;
            pair.id = owned(original.id);
            pair.firstVertexToSecondVertex = owned(original.firstVertexToSecondVertex);
            pair.axes.clear();
            for (const auto& axis : original.axes)
                pair.axes.append({ axis.firstAxis, axis.secondAxis, owned(axis.firstKeyToSecondKey) });
            const auto endpoint = [&](const CGeometryObjectPairs::Endpoint& source) {
                auto next = source;
                next.object.id = owned(source.object.id);
                next.triangles = owned(source.triangles);
                next.axes.clear();
                for (const auto& axis : source.axes)
                    next.axes.append({ axis.index, owned(axis.parameterId), owned(axis.keys), axis.parameterType });
                return next;
            };
            pair.first = endpoint(original.first);
            pair.second = endpoint(original.second);
            result.pairs.append(std::move(pair));
        }
        return result;
    }
    CTextureWorkspace::State owned(const CTextureWorkspace::State& value)
    {
        retain(qint64(value.atlases.size()) * 256);
        CTextureWorkspace::State result;
        result.sourceFingerprint = owned(value.sourceFingerprint);
        for (auto it = value.atlases.cbegin(); it != value.atlases.cend(); ++it)
            result.atlases.insert(
                it.key(), { owned(it->id), owned(it->name), it->sourceSlot, it->size, owned(it->png) });
        return result;
    }
    QVariant ownedValue(const QVariant& value)
    {
        const auto type = value.metaType();
        if (type == QMetaType::fromType<bool>() || type == QMetaType::fromType<int>())
            return value;
#define OWNED_VALUE(T)                                                                                                 \
    if (type == QMetaType::fromType<T>())                                                                              \
        return QVariant::fromValue(owned(value.value<T>()));
        OWNED_VALUE(QString)
        OWNED_VALUE(QByteArray)
        OWNED_VALUE(QSet<QString>)
        using Columns = QMap<int, QByteArray>;
        using Counts = QMap<int, int>;
        using Visibility = QMap<QString, bool>;
        using Scalars = QHash<QString, float>;
        using Integers = QHash<QString, int>;
        using Flags = QHash<QString, bool>;
        using Metadata = QHash<QString, CModelParameterMetadata>;
        using Positions = QHash<QString, QVector<QVector<float>>>;
        using Indices = QHash<QString, QVector<quint16>>;
        using Masks = QHash<QString, QVector<int>>;
        using Rotations = QHash<QString, QVector<CModelRotationKeyform>>;
        using Warps = QHash<QString, QVector<CModelWarpKeyform>>;
        OWNED_VALUE(Columns)
        OWNED_VALUE(Counts)
        OWNED_VALUE(Visibility)
        OWNED_VALUE(Scalars)
        OWNED_VALUE(Integers)
        OWNED_VALUE(Flags)
        OWNED_VALUE(Metadata)
        OWNED_VALUE(CDeformerDisplayNames::State)
        OWNED_VALUE(Positions)
        OWNED_VALUE(Indices)
        OWNED_VALUE(Masks)
        OWNED_VALUE(Rotations)
        OWNED_VALUE(Warps)
        OWNED_VALUE(CModelMaterialOverrides)
        OWNED_VALUE(CParameterOrganization::State)
        OWNED_VALUE(CGeometryObjectPairs::State)
        OWNED_VALUE(CTextureWorkspace::State)
#undef OWNED_VALUE
        if (type == QMetaType::fromType<CModelArtwork::State>()) {
            CModelArtwork::State result;
            if (!CModelArtwork::Freeze(value.value<CModelArtwork::State>(), result))
                throw std::invalid_argument("Invalid artwork snapshot");
            return QVariant::fromValue(result);
        }
        throw std::invalid_argument("Unsupported structure field");
    }
    QVariantMap ownedState(const QVariantMap& state)
    {
        QVariantMap result;
        for (auto it = state.cbegin(); it != state.cend(); ++it)
            result.insert(owned(it.key()), ownedValue(it.value()));
        return result;
    }

private:
    void retain(qint64 count)
    {
        if (count < 0 || count > CTextureAssets::MaximumBytes - retainedBytes)
            throw std::length_error("Frozen mesh snapshot exceeds the storage budget");
        retainedBytes += count;
    }
    qint64 retainedBytes = 0;
    QSet<QString> strings;
    QMap<QByteArray, QByteArray> blobs;
    QHash<QPair<quintptr, qsizetype>, QByteArray> blobHints;
    QHash<QPair<quintptr, qsizetype>, QVariant> vectors;
};

template <class T> void bits(QByteArray& output, const T& value)
{
    static_assert(std::is_floating_point_v<T>);
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void bits(QByteArray& output, const QVector<float>& values)
{
    output.append(reinterpret_cast<const char*>(values.constData()), values.size() * sizeof(float));
}
template <class T, class Visit> void each(const QHash<QString, T>& map, Visit visit)
{
    auto keys = map.keys();
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys)
        visit(map.value(key));
}
QByteArray numericBits(const QVariantMap& state)
{
    QByteArray output;
    for (const char* name : { "parameters", "opacity" })
        each(state.value(name).value<QHash<QString, float>>(), [&](float value) { bits(output, value); });
    each(state.value("parameterMetadata").value<QHash<QString, CModelParameterMetadata>>(), [&](const auto& value) {
        bits(output, value.minimum);
        bits(output, value.maximum);
        bits(output, value.defaultValue);
    });
    each(state.value("positions").value<QHash<QString, QVector<QVector<float>>>>(), [&](const auto& forms) {
        for (const auto& form : forms)
            bits(output, form);
    });
    each(state.value("rotation").value<QHash<QString, QVector<CModelRotationKeyform>>>(), [&](const auto& forms) {
        for (const auto& form : forms) {
            bits(output, form.opacity);
            bits(output, form.angle);
            bits(output, form.originX);
            bits(output, form.originY);
            bits(output, form.scale);
        }
    });
    each(state.value("warp").value<QHash<QString, QVector<CModelWarpKeyform>>>(), [&](const auto& forms) {
        for (const auto& form : forms) {
            bits(output, form.positions);
            bits(output, form.opacity);
        }
    });
    for (const auto& pair : state.value("geometryObjectPairs").value<CGeometryObjectPairs::State>().pairs)
        for (const auto& point : { pair.linePoint, pair.lineDirection, pair.rotationLocalX, pair.rotationLocalY }) {
            bits(output, point.x());
            bits(output, point.y());
        }
    return output;
}
bool exactState(const QVariantMap& a, const QVariantMap& b)
{
    if (a.keys() != b.keys())
        return false;
    for (auto it = a.cbegin(); it != a.cend(); ++it)
        if (it->metaType() != b.value(it.key()).metaType() || it.value() != b.value(it.key()))
            return false;
    return numericBits(a) == numericBits(b);
}
bool sameRequest(const CMocMeshStructureBuilder::Request& a, const CMocMeshStructureBuilder::Request& b)
{
    if (a.meshes.size() != b.meshes.size() || a.gluePolicy != b.gluePolicy
        || a.glueRetargets.size() != b.glueRetargets.size())
        return false;
    for (int i = 0; i < a.meshes.size(); ++i) {
        const auto& x = a.meshes[i];
        const auto& y = b.meshes[i];
        if (x.artMeshId != y.artMeshId || x.oldToNew != y.oldToNew || x.indices != y.indices
            || x.vertices.size() != y.vertices.size())
            return false;
        for (int j = 0; j < x.vertices.size(); ++j) {
            const auto& u = x.vertices[j];
            const auto& v = y.vertices[j];
            if (u.indices != v.indices || u.weights.size() != v.weights.size()
                || (!u.weights.isEmpty()
                    && std::memcmp(u.weights.constData(), v.weights.constData(), u.weights.size() * sizeof(double))))
                return false;
        }
    }
    for (int i = 0; i < a.glueRetargets.size(); ++i) {
        const auto& x = a.glueRetargets[i];
        const auto& y = b.glueRetargets[i];
        if (x.glueIndex != y.glueIndex || x.pairIndex != y.pairIndex || x.side != y.side || x.newVertex != y.newVertex)
            return false;
    }
    return true;
}
bool sameRemaps(
    const QVector<CMocMeshStructureBuilder::MeshRemap>& a, const QVector<CMocMeshStructureBuilder::MeshRemap>& b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].artMeshId != b[i].artMeshId || a[i].oldVertexCount != b[i].oldVertexCount
            || a[i].newVertexCount != b[i].newVertexCount || a[i].oldToNew != b[i].oldToNew
            || a[i].newToOld != b[i].newToOld)
            return false;
    return true;
}
}

bool CModelDocument::FreezeMeshRequest(
    const CMocMeshStructureBuilder::Request& request, CMocMeshStructureBuilder::Request& result, QString* error) const
try {
    if (request.meshes.isEmpty() || request.meshes.size() > m_drawables.size() || request.glueRetargets.size() > 1000000
        || (request.gluePolicy != CMocMeshStructureBuilder::GlueEndpointPolicy::Reject
            && request.gluePolicy != CMocMeshStructureBuilder::GlueEndpointPolicy::DropPair
            && request.gluePolicy != CMocMeshStructureBuilder::GlueEndpointPolicy::Retarget)
        || (request.gluePolicy != CMocMeshStructureBuilder::GlueEndpointPolicy::Retarget
            && !request.glueRetargets.isEmpty()))
        return fail(error, QStringLiteral("网格请求数量无效"));
    qint64 references = 0;
    QHash<QString, int> targets;
    for (const auto& mesh : m_drawables) {
        if (targets.contains(mesh.id))
            return fail(error, QStringLiteral("模型对象标识重复"));
        targets.insert(mesh.id, mesh.vertexCount);
    }
    QSet<QString> selected;
    for (const auto& mesh : request.meshes) {
        const auto target = targets.constFind(mesh.artMeshId);
        if (target == targets.cend() || selected.contains(mesh.artMeshId) || *target < 0
            || *target > CMocMeshStructureBuilder::MaximumVertices || mesh.oldToNew.size() != *target)
            return fail(error, QStringLiteral("网格请求目标或原始顶点数量不匹配"));
        selected.insert(mesh.artMeshId);
        if (mesh.artMeshId.isEmpty() || mesh.artMeshId.size() > 1024 || mesh.vertices.isEmpty()
            || mesh.vertices.size() > CMocMeshStructureBuilder::MaximumVertices
            || mesh.oldToNew.size() > CMocMeshStructureBuilder::MaximumVertices || mesh.indices.size() > 3000000)
            return fail(error, QStringLiteral("网格请求的标识、顶点或索引数量无效"));
        for (const auto& vertex : mesh.vertices) {
            if (vertex.indices.isEmpty() || vertex.indices.size() != vertex.weights.size()
                || vertex.indices.size() > 1000000 - references)
                return fail(error, QStringLiteral("网格请求的顶点来源数量无效"));
            references += vertex.indices.size();
            double total = 0;
            for (int i = 0; i < vertex.indices.size(); ++i) {
                if (vertex.indices[i] < 0 || vertex.indices[i] >= *target || !std::isfinite(vertex.weights[i])
                    || vertex.weights[i] < 0)
                    return fail(error, QStringLiteral("网格请求的来源索引或权重无效"));
                total += vertex.weights[i];
            }
            if (!std::isfinite(total) || std::abs(total - 1) > CMocMeshStructureBuilder::WeightTolerance)
                return fail(error, QStringLiteral("网格请求的来源权重之和无效"));
        }
        QVector<bool> retained(mesh.vertices.size());
        for (int destination : mesh.oldToNew) {
            if (destination < -1 || destination >= mesh.vertices.size() || (destination >= 0 && retained[destination]))
                return fail(error, QStringLiteral("网格请求的旧点映射无效"));
            if (destination >= 0)
                retained[destination] = true;
        }
    }
    SnapshotFreezer freezer;
    CMocMeshStructureBuilder::Request frozen;
    frozen.gluePolicy = request.gluePolicy;
    for (const auto& mesh : request.meshes) {
        CMocMeshStructureBuilder::MeshEdit next;
        next.artMeshId = freezer.owned(mesh.artMeshId);
        next.oldToNew = freezer.owned(mesh.oldToNew);
        next.indices = freezer.owned(mesh.indices);
        for (const auto& vertex : mesh.vertices)
            next.vertices.append({ freezer.owned(vertex.indices), freezer.owned(vertex.weights) });
        frozen.meshes.append(std::move(next));
    }
    for (const auto& redirect : request.glueRetargets)
        frozen.glueRetargets.append(redirect);
    result = std::move(frozen);
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法冻结网格请求"));
}

bool CModelDocument::SealMeshStructureEdit(CModelEdit& edit, const CModelDocument& prepared, QString* error) const
try {
    SnapshotFreezer freezer;
    CModelEdit frozen = edit;
    frozen.name = freezer.owned(edit.name);
    frozen.type = freezer.owned(edit.type);
    frozen.before = freezer.ownedState(edit.before.toMap());
    frozen.after = freezer.ownedState(edit.after.toMap());
    frozen.runtime.reset();
    frozen.storage.reset();
    frozen.meshVerification = { };
    if (!exactState(AuthoringStructureState(), frozen.before.toMap())
        || !exactState(prepared.AuthoringStructureState(), frozen.after.toMap()))
        return fail(error, QStringLiteral("网格准备期间作者状态已改变"));
    CMocMeshStructureBuilder::Request request;
    if (!edit.meshRequest || !FreezeMeshRequest(*edit.meshRequest, request, error))
        return false;
    frozen.meshRequest = std::make_shared<const CMocMeshStructureBuilder::Request>(std::move(request));
    frozen.meshRemaps.clear();
    for (const auto& remap : edit.meshRemaps)
        frozen.meshRemaps.append({ freezer.owned(remap.artMeshId), remap.oldVertexCount, remap.newVertexCount,
            freezer.owned(remap.oldToNew), freezer.owned(remap.newToOld) });
    CModelSession session;
    if (!DecodeStructureState(frozen.after.toMap(), frozen, session, error))
        return false;
    CModelDocument candidate;
    if (!session.RestorePreparedDocument(*this, prepared, candidate, error)
        || !exactState(candidate.AuthoringStructureState(), frozen.after.toMap()))
        return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("网格候选没有通过完整作者状态验收"));
    const auto state = frozen.after.toMap();
    candidate.m_structure = session.structure;
    candidate.m_drawableMaterialOverrides = session.drawableMaterials;
    candidate.m_parameterValues = state.value("parameters").value<QHash<QString, float>>();
    candidate.m_parameterMetadataOverrides = session.parameterMetadata;
    candidate.m_deformerDisplayNames = session.deformerDisplayNames.value_or(CDeformerDisplayNames::State{});
    candidate.m_parameterOrganization = session.parameterOrganization;
    candidate.m_geometryObjectPairs = session.geometryObjectPairs;
    candidate.m_lockedKeys = state.value("locked").value<QSet<QString>>();
    candidate.m_drawableOpacityOverrides = session.drawableOpacity;
    candidate.m_drawableDrawOrderOverrides = session.drawableDrawOrder;
    candidate.m_drawableKeyformPositionOverrides = session.drawableKeyformPositions;
    candidate.m_drawableIndexOverrides = session.drawableIndices;
    candidate.m_drawableMaskIndexOverrides = session.drawableMaskIndices;
    candidate.m_deformerParentOverrides = session.deformerParentIndices;
    candidate.m_deformerRotationKeyformOverrides = session.deformerRotationKeyforms;
    candidate.m_deformerWarpKeyformOverrides = session.deformerWarpKeyforms;
    candidate.m_drawableMaskInvertedOverrides = session.drawableMaskInverted;
    candidate.m_textureWorkspace = state.value("textureWorkspace").value<CTextureWorkspace::State>();
    candidate.m_artwork = session.artwork;
    candidate.m_sourceDeformerIds = freezer.owned(candidate.m_sourceDeformerIds);
    candidate.m_sourceDeformerParents = freezer.owned(candidate.m_sourceDeformerParents);

    candidate.m_editHistory.clear();
    candidate.m_editIndex = 0;
    auto verified = std::make_shared<CModelMeshEditVerification::Data>();
    verified->sourceBytes = m_sourceBytes;
    verified->workingBytes = m_workingBaseBytes;
    verified->sourceDigest = freezer.owned(m_sourceDigest);
    verified->sourcePath = freezer.owned(m_sourcePath);
    verified->sources = freezer.owned(m_textureSources);
    candidate.m_sourceBytes = verified->sourceBytes;
    candidate.m_sourceDigest = verified->sourceDigest;
    candidate.m_sourcePath = verified->sourcePath;
    candidate.m_textureSources = verified->sources;
    verified->expected = frozen;
    verified->candidate = std::move(candidate);
    frozen.meshVerification.m_data = std::move(verified);
    edit = std::move(frozen);
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法封存网格编辑候选"));
}

void CModelDocument::FreezeVerifiedMeshCommand(CModelEdit& edit) const
{
    const auto verified = edit.meshVerification.m_data;
    const auto runtime = edit.runtime;
    edit = verified->expected;
    edit.runtime = runtime;
}

bool CModelDocument::ApplyVerifiedMeshEdit(const CModelEdit& edit, bool forward, QString* error)
try {
    const auto& verified = edit.meshVerification.m_data;
    if (!forward || !verified)
        return fail(error, QStringLiteral("网格候选只能用于初次提交"));
    const auto& expected = verified->expected;
    if (!m_loaded || m_sourceBytes != verified->sourceBytes || m_workingBaseBytes != verified->workingBytes
        || m_sourceDigest != verified->sourceDigest || m_sourcePath != verified->sourcePath
        || m_textureSources != verified->sources || edit.name != expected.name || edit.type != expected.type
        || edit.field != expected.field || edit.index != expected.index || edit.coalesce != expected.coalesce
        || edit.parameterChange != expected.parameterChange || edit.freezeValue != expected.freezeValue || edit.storage
        || edit.bindingKeysRequest || edit.deformerRequest || !edit.meshRequest
        || !sameRequest(*edit.meshRequest, *expected.meshRequest) || !sameRemaps(edit.meshRemaps, expected.meshRemaps)
        || edit.before.metaType() != QMetaType::fromType<QVariantMap>()
        || edit.after.metaType() != QMetaType::fromType<QVariantMap>()
        || !exactState(edit.before.toMap(), expected.before.toMap())
        || !exactState(edit.after.toMap(), expected.after.toMap())
        || !exactState(AuthoringStructureState(), expected.before.toMap()))
        return fail(error, QStringLiteral("网格候选的来源、请求或作者状态已改变"));
    CModelDocument candidate = verified->candidate;
    candidate.m_editHistory = m_editHistory;
    candidate.m_editIndex = m_editIndex;
    candidate.m_revision = m_revision;
    *this = std::move(candidate);
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法应用已验证的网格候选"));
}
}
