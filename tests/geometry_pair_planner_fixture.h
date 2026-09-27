#ifndef GEOMETRY_PAIR_PLANNER_FIXTURE_H
#define GEOMETRY_PAIR_PLANNER_FIXTURE_H

#include "deformer_replay_fixture.h"
#include "live2dstruct/cgeometrypairplanner.h"
#include <numeric>

namespace GeometryPairData {
using namespace live2dstruct;
using Pairs = CGeometryObjectPairs;
using Geometry = CModelGeometryKeyform;
using Builder = CMocDeformerStructureBuilder;

inline QVector<int> identity(int count)
{
    QVector<int> values(count); std::iota(values.begin(), values.end(), 0); return values;
}

struct Fixture {
    QByteArray bytes;
    Pairs::Pair pair;
    QString firstWitness, secondWitness;
};

inline QByteArray meshBase(Geometry::Type type, bool reversed, bool culled, bool differentParents,
                           Fixture& fixture, QString* error)
{
    const auto source = DeformerReplayFixture::read("Mark");
    auto reader = CReader::FromBytes(source); CSectionTable table(reader);
    if (!table.IsValid() || table.art_meshes.size() < 2) return {};
    fixture.firstWitness = table.art_meshes[0]->ids; fixture.secondWitness = table.art_meshes[1]->ids;
    DeformerReplayFixture::RawColumns raw(source);
    for (const char* column : {"vertex_counts", "uv_sources_begin_indices", "position_index_sources_begin_indices", "position_index_sources_counts"}) {
        const auto name = QByteArray("art_meshes.") + column; raw.set(name, 1, raw.integer(name, 0));
    }
    const int count = raw.integer("art_meshes.vertex_counts", 0);
    int emptyBinding = -1;
    for (int index = 0; index < table.keyform_bindings.size(); ++index)
        if (table.keyform_bindings[index]->parameter_binding_index_sources_counts == 0) { emptyBinding = index; break; }
    if (emptyBinding < 0) {
        emptyBinding = table.keyform_bindings.size();
        for (const auto* name : {"keyform_bindings.parameter_binding_index_sources_begin_indices", "keyform_bindings.parameter_binding_index_sources_counts"}) {
            const auto span = raw.spans.value(name); auto values = raw.column(name); values.append(QByteArray(4, '\0'));
            raw.state.columns[span.slot] = values; raw.state.counts[span.countIndex] = span.count + 1;
        }
    }
    auto flags = raw.column("art_meshes.drawable_flags");
    for (int mesh = 0; mesh < 2; ++mesh) {
        flags[mesh] = char(culled ? quint8(flags[mesh]) & ~4 : quint8(flags[mesh]) | 4);
        raw.set("art_meshes.parent_deformer_indices", mesh, -1);
        raw.set("art_meshes.parent_part_indices", mesh, -1);
        raw.set("art_meshes.keyform_binding_sources_indices", mesh, emptyBinding);
        raw.set("art_meshes.keyform_sources_counts", mesh, 1);
    }
    raw.state.columns[raw.spans.value("art_meshes.drawable_flags").slot] = flags;
    const auto copyPool = [&](const QByteArray& pool, const QByteArray& offsetColumn, int length) {
        const auto span = raw.spans.value(pool); auto bytes = raw.column(pool);
        bytes.append(bytes.mid(raw.integer(offsetColumn, 0) * span.width, length * span.width));
        raw.set(offsetColumn, 1, span.count); raw.state.columns[span.slot] = bytes; raw.state.counts[span.countIndex] = span.count + length;
    };
    copyPool("uvs.uvs", "art_meshes.uv_sources_begin_indices", count * 2);
    copyPool("position_indices.indices", "art_meshes.position_index_sources_begin_indices", raw.integer("art_meshes.position_index_sources_counts", 0));
    const int form = raw.integer("art_meshes.keyform_sources_begin_indices", 0);
    const int newForm = raw.count("art_mesh_keyforms.opacities");
    for (auto span = raw.spans.cbegin(); span != raw.spans.cend(); ++span) {
        if (!span.key().startsWith("art_mesh_keyforms.") || !span->offset) continue;
        auto bytes = raw.column(span.key()); bytes.append(bytes.mid(form * span->width, span->width));
        raw.state.columns[span->slot] = bytes; raw.state.counts[span->countIndex] = span->count + 1;
    }
    raw.set("art_meshes.keyform_sources_begin_indices", 1, newForm);
    const auto span = raw.spans.value("keyform_positions.xys"); auto positions = raw.column("keyform_positions.xys");
    const int firstPosition = raw.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", form);
    const int secondPosition = positions.size() / 4;
    positions.append(positions.mid(firstPosition * 4, count * 8));
    raw.set("art_mesh_keyforms.keyform_position_sources_begin_indices", newForm, secondPosition);
    const auto write = [&](int index, float value) {
        quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, positions.data() + index * 4);
    };
    if (type != Geometry::ArtMesh) {
        const QVector<QPointF> witness{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {.2, .3}, {.8, .7}};
        for (int vertex = 0; vertex < count; ++vertex) {
            const QPointF value = witness[vertex % witness.size()];
            write(firstPosition + vertex * 2, float(value.x())); write(firstPosition + vertex * 2 + 1, float(value.y()));
            write(secondPosition + vertex * 2, float(type == Geometry::Warp && reversed ? 1 - value.x() : value.x()));
            write(secondPosition + vertex * 2 + 1, float(value.y()));
        }
    } else if (reversed && !differentParents) {
        for (int vertex = 0; vertex < count; ++vertex) {
            const quint32 bits = qFromLittleEndian<quint32>(positions.constData() + (firstPosition + vertex * 2) * 4);
            float value; std::memcpy(&value, &bits, 4); write(secondPosition + vertex * 2, -value + .01f);
        }
    }
    raw.state.columns[span.slot] = positions; raw.state.counts[span.countIndex] = positions.size() / 4;
    if (type == Geometry::ArtMesh && reversed) {
        const auto indicesSpan = raw.spans.value("position_indices.indices"); auto indices = raw.column("position_indices.indices");
        const int begin = raw.integer("art_meshes.position_index_sources_begin_indices", 1);
        const int length = raw.integer("art_meshes.position_index_sources_counts", 1);
        for (int triangle = 0; triangle < length; triangle += 3) {
            auto* bytes = indices.data() + (begin + triangle) * 2;
            const auto a = qFromLittleEndian<quint16>(bytes), b = qFromLittleEndian<quint16>(bytes + 2);
            qToLittleEndian(b, bytes); qToLittleEndian(a, bytes + 2);
        }
        raw.state.columns[indicesSpan.slot] = indices;
    }
    return raw.encode(error);
}

inline bool make(Geometry::Type type, Fixture& fixture, QString* error, bool differentParents = true,
                 bool matrix = false, bool reversed = false, bool culled = false, bool quad = false)
{
    QByteArray bytes = meshBase(type, reversed, culled, differentParents, fixture, error);
    if (bytes.isEmpty()) return false;
    QString first = fixture.firstWitness, second = fixture.secondWitness;
    const auto insert = [&](const QString& id, const Builder::Owner& child, const std::variant<Builder::Warp, Builder::Rotation>& shape) {
        Builder::InsertRequest request; request.id = id; request.children = {child}; request.shape = shape;
        Builder::Result result;
        if (!Builder::Insert(bytes, request, result, error)) return false;
        bytes = std::move(result.bytes); return true;
    };
    if (type != Geometry::ArtMesh) {
        const Builder::Warp warp{1, 1, quad, {0, 0, 1, 0, 0, 1, 2, 2}};
        const Builder::Rotation rotation{.2f, .3f, 31, 17, .5f, false, false};
        const std::variant<Builder::Warp, Builder::Rotation> shape = type == Geometry::Warp ?
            std::variant<Builder::Warp, Builder::Rotation>(warp) : std::variant<Builder::Warp, Builder::Rotation>(rotation);
        first = "PairFirst"; second = "PairSecond";
        if (!insert(first, {Builder::Owner::ArtMesh, fixture.firstWitness}, shape) ||
            !insert(second, {Builder::Owner::ArtMesh, fixture.secondWitness}, shape)) return false;
    }
    if (differentParents) {
        if (!insert("PairParentA", {Builder::Owner::Type(int(type)), first}, Builder::Rotation{.1f, -.2f, 23, 11, .8f, false, false}) ||
            !insert("PairParentB", {Builder::Owner::Type(int(type)), second}, Builder::Rotation{-.1f, .3f, -29, -9, -.7f, true, false})) return false;
    }
    if (matrix) {
        using Parameters = CMocParameterStructureBuilder;
        Parameters::Result result;
        Parameters::CreateRequest primary; primary.parameter.id = "PairPrimary";
        primary.bindings = {{{Parameters::Owner::Type(int(type)), first}, {-1, 1}}};
        if (!Parameters::Create(bytes, primary, result, error)) return false; bytes = std::move(result.bytes);
        Parameters::CreateRequest shared; shared.parameter = {"PairShared", 0, 1, 0, 2, false, 0};
        shared.bindings = {{{Parameters::Owner::Type(int(type)), first}, {0, .5f, 1}},
                           {{Parameters::Owner::Type(int(type)), second}, {0, .5f, 1}}};
        if (!Parameters::Create(bytes, shared, result, error)) return false; bytes = std::move(result.bytes);
        if (!Parameters::BindExistingParameter(bytes, "PairPrimary", {{{Parameters::Owner::Type(int(type)), second}, {-1, 1}}}, result, error)) return false;
        bytes = std::move(result.bytes);
    }
    if (!CMocDeformerOrder::Normalize(bytes, fixture.bytes, error)) return false;
    CModelDocument document; if (!document.LoadBytes(fixture.bytes, "pair-fixture.moc3", error)) return false;
    Pairs::Pair pair; pair.id = "pair";
    if (!Pairs::DescribeEndpoint(document, {type, first}, pair.first, error) ||
        !Pairs::DescribeEndpoint(document, {type, second}, pair.second, error)) return false;
    pair.firstVertexToSecondVertex = identity(pair.first.vertexCount); pair.reversedWinding = reversed;
    if (type == Geometry::Warp && reversed) pair.firstVertexToSecondVertex = {1, 0, 3, 2};
    if (matrix) pair.axes = {{0, 1, {1, 0}}, {1, 0, {0, 1, 2}}};
    Pairs::State normalized;
    if (!Pairs::Normalize({document.SourceDigest(), {pair}}, normalized, error) || !Pairs::Validate(normalized, document, error)) return false;
    fixture.pair = normalized.pairs.front(); return true;
}

inline Geometry changed(Geometry source)
{
    if (auto* values = std::get_if<QVector<float>>(&source.value)) {
        (*values)[0] += .013f; (*values)[1] -= .017f;
    } else {
        auto& value = std::get<CModelRotationTransform>(source.value);
        value.originX += .013f; value.angle += 13; value.scale *= 1.1f;
    }
    return source;
}

inline bool reindexTarget(Fixture& fixture, QString* error)
{
    CModelDocument document;
    if (!document.LoadBytes(fixture.bytes, "reindex.moc3", error)) return false;
    const int count = fixture.pair.second.vertexCount;
    CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = fixture.pair.second.object.id;
    mesh.oldToNew.resize(count);
    for (int index = 0; index < count; ++index) {
        mesh.oldToNew[index] = (index + 1) % count;
        mesh.vertices.append({{(index + count - 1) % count}, {1}});
    }
    mesh.indices = document.DrawableIndices(mesh.artMeshId);
    for (auto& index : mesh.indices) index = quint16(mesh.oldToNew[index]);
    CMocMeshStructureBuilder::Request request; request.meshes = {mesh}; CModelEdit edit;
    if (!document.MakeMeshStructureEdit(request, edit, error) || !document.ExecuteEdit(edit) ||
        !document.BuildRuntimeMoc(fixture.bytes, error) || !document.LoadBytes(fixture.bytes, "reindexed.moc3", error) ||
        !Pairs::DescribeEndpoint(document, fixture.pair.second.object, fixture.pair.second, error)) return false;
    fixture.pair.firstVertexToSecondVertex = mesh.oldToNew;
    Pairs::State normalized;
    if (!Pairs::Normalize({document.SourceDigest(), {fixture.pair}}, normalized, error) || !Pairs::Validate(normalized, document, error)) return false;
    fixture.pair = normalized.pairs.front(); return true;
}
}

#endif
