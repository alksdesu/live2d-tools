#ifndef GEOMETRY_OBJECT_PAIRS_DOCUMENT_FIXTURE_H
#define GEOMETRY_OBJECT_PAIRS_DOCUMENT_FIXTURE_H

#include "geometry_batch_fixture.h"
#include <QCryptographicHash>

namespace GeometryPairsData {
using namespace live2dstruct;
using Pairs = CGeometryObjectPairs;

inline bool load(CModelDocument& document, bool rotation = false, QString* error = nullptr)
{
    auto bytes = GeometryBatchData::sharedFixture(rotation);
    if (!rotation) {
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        if (!table.IsValid()) return false;
        const auto& o = *table.sectionOffsetTable;
        const auto append = [&](int slot, const QByteArray& column) {
            bytes.append(QByteArray((64 - bytes.size() % 64) % 64, '\0'));
            const int offset = bytes.size(); bytes.append(column);
            DeformerSnapshotData::put(bytes, slot, offset);
            bytes.append(QByteArray((64 - bytes.size() % 64) % 64, '\0'));
        };
        const auto count = [&](int index, int value) { DeformerSnapshotData::put(bytes, o.count_info->count_info_offset + index * 4, value); };
        const QByteArray positions = bytes.mid(o.keyform_positions->xys, 20 * 4);
        append(0x15c, positions + positions.left(12 * 4)); count(10, 32);
        for (int slot : {0x150, 0x154}) {
            const auto column = bytes.mid(DeformerSnapshotData::integer(bytes, slot), 2 * 4);
            append(slot, column + column);
        }
        QByteArray begins(4 * 4, '\0');
        for (int index = 0; index < 4; ++index) DeformerSnapshotData::put(begins, index * 4, QVector<int>{0, 6, 20, 26}[index]);
        append(0x158, begins); count(9, 4);
        DeformerSnapshotData::put(bytes, o.art_meshes->keyform_sources_begin_indices + 4, 2);
        const auto uvs = bytes.mid(o.uvs->uvs, 6 * 4); append(0x178, uvs + uvs); count(15, 12);
        DeformerSnapshotData::put(bytes, o.art_meshes->uv_sources_begin_indices + 4, 6);
        const auto indices = bytes.mid(o.position_indices->indices, 3 * 2); append(0x17c, indices + indices); count(16, 6);
        DeformerSnapshotData::put(bytes, o.art_meshes->position_index_sources_begin_indices + 4, 3);
    }
    return document.LoadBytes(bytes, QStringLiteral("geometry-pairs-fixture"), error);
}

inline Pairs::State paired(const CModelDocument& document, bool rotation = false)
{
    Pairs::State state;
    state.sourceDigest = document.SourceDigest();
    Pairs::Pair pair;
    pair.id = QStringLiteral("LeftRight");
    const auto type = rotation ? Pairs::Type::Rotation : Pairs::Type::ArtMesh;
    if (!Pairs::DescribeEndpoint(document, {type, rotation ? QStringLiteral("Root") : QStringLiteral("Mesh")}, pair.first) ||
        !Pairs::DescribeEndpoint(document, {type, QStringLiteral("Twin")}, pair.second)) return {};
    for (int index = 0; index < pair.first.vertexCount; ++index) pair.firstVertexToSecondVertex.append(index);
    for (int axis = 0; axis < pair.first.axes.size(); ++axis) {
        Pairs::AxisMapping mapping{axis, axis, {}};
        for (int key = 0; key < pair.first.axes[axis].keys.size(); ++key) mapping.firstKeyToSecondKey.append(key);
        pair.axes.append(std::move(mapping));
    }
    state.pairs.append(std::move(pair));
    return state;
}

inline CMocMeshStructureBuilder::Request reorder(const CModelDocument& document, bool sameTopology = false)
{
    const QVector<int> newToOld = sameTopology ? QVector<int>{1, 2, 0} : QVector<int>{1, 0, 2};
    CMocMeshStructureBuilder::MeshEdit mesh;
    mesh.artMeshId = "Mesh"; mesh.oldToNew = QVector<int>(3, -1);
    for (int index = 0; index < newToOld.size(); ++index) {
        mesh.vertices.append({{newToOld[index]}, {1.0}}); mesh.oldToNew[newToOld[index]] = index;
    }
    for (auto index : document.DrawableIndices(mesh.artMeshId)) mesh.indices.append(quint16(mesh.oldToNew[index]));
    CMocMeshStructureBuilder::Request request; request.meshes.append(std::move(mesh)); return request;
}

inline CModelSession capture(const CModelDocument& document)
{
    auto session = GeometryBatchData::session(document);
    session.parameters.clear();
    for (auto value = document.ParameterValues().cbegin(); value != document.ParameterValues().cend(); ++value)
        session.parameters.insert(value.key(), value.value());
    session.structure = document.ParameterStructure(); session.meshStructure = document.HasMeshStructure();
    session.deformerStructure = document.HasDeformerStructure();
    if (session.deformerStructure) session.sourceDeformerIds = document.SourceDeformerIds();
    session.geometryObjectPairs = document.GeometryObjectPairs(); session.parameterOrganization = document.ParameterOrganization();
    session.parameterMetadata = document.ParameterMetadataOverrides();
    session.drawableIndices = document.DrawableIndexOverrides(); session.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    session.drawableMaskInverted = document.DrawableMaskInvertedOverrides(); session.drawableOpacity = document.DrawableOpacityOverrides();
    session.drawableDrawOrder = document.DrawableDrawOrderOverrides(); session.deformerParentIndices = document.DeformerParentOverrides();
    session.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) session.locked.insert(key, true);
    return session;
}

inline CDeformerPreserveRequest removal(const CModelDocument& document, const QString& id)
{
    QByteArray bytes; document.BuildPreviewMoc(bytes);
    return {CMocDeformerStructureBuilder::RemoveRequest{id}, document.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
}
}

#endif
