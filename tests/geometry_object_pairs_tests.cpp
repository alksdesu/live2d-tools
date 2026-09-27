#include "live2dstruct/cgeometryobjectpairs.h"
#include "live2dstruct/cmodeldocument.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Pairs = CGeometryObjectPairs;
namespace {
QVector<int> identity(int count)
{
    QVector<int> result; for (int index = 0; index < count; ++index) result.append(index); return result;
}
Pairs::State sample()
{
    Pairs::Pair pair; pair.id = "eyes";
    pair.first = {{Pairs::Type::ArtMesh, "left"}, 4, 0, 0, {0, 1, 2, 2, 1, 3},
                  {{0, "first", {-1, 0, 1}}, {1, "shared", {-2, 2}}}, 6};
    pair.second = {{Pairs::Type::ArtMesh, "right"}, 4, 0, 0, {0, 1, 2, 2, 1, 3},
                   {{0, "shared", {-2, 2}}, {1, "second", {-1, 0, 1}}}, 6};
    pair.firstVertexToSecondVertex = {0, 2, 1, 3}; pair.reversedWinding = true;
    pair.axes = {{0, 1, {2, 1, 0}}, {1, 0, {0, 1}}};
    return {QByteArray(32, 'a'), {pair}};
}
QByteArray pairedMeshes()
{
    auto source = DeformerReplayFixture::read("Mark");
    DeformerReplayFixture::RawColumns raw(source);
    for (const char* column : {"keyform_binding_sources_indices", "keyform_sources_begin_indices", "keyform_sources_counts",
                              "vertex_counts", "uv_sources_begin_indices", "position_index_sources_begin_indices", "position_index_sources_counts"}) {
        const auto name = QByteArray("art_meshes.") + column;
        raw.set(name, 1, raw.integer(name, 0));
    }
    const auto copyPool = [&](const QByteArray& pool, const QByteArray& offsetColumn, int count) {
        const auto span = raw.spans.value(pool); auto bytes = raw.column(pool);
        const auto previous = raw.integer(offsetColumn, 0);
        bytes.append(bytes.mid(previous * span.width, count * span.width));
        raw.set(offsetColumn, 1, span.count); raw.state.columns[span.slot] = bytes;
        raw.state.counts[span.countIndex] = span.count + count;
    };
    copyPool("uvs.uvs", "art_meshes.uv_sources_begin_indices", raw.integer("art_meshes.vertex_counts", 0) * 2);
    copyPool("position_indices.indices", "art_meshes.position_index_sources_begin_indices", raw.integer("art_meshes.position_index_sources_counts", 0));
    const int formBegin = raw.integer("art_meshes.keyform_sources_begin_indices", 0);
    const int formCount = raw.integer("art_meshes.keyform_sources_counts", 0);
    const int newFormBegin = raw.count("art_mesh_keyforms.opacities");
    for (auto span = raw.spans.cbegin(); span != raw.spans.cend(); ++span) {
        if (!span.key().startsWith("art_mesh_keyforms.") || !span->offset) continue;
        auto bytes = raw.column(span.key()); bytes.append(bytes.mid(formBegin * span->width, formCount * span->width));
        raw.state.columns[span->slot] = bytes; raw.state.counts[span->countIndex] = span->count + formCount;
    }
    raw.set("art_meshes.keyform_sources_begin_indices", 1, newFormBegin);
    const auto positionSpan = raw.spans.value("keyform_positions.xys"); auto positions = raw.column("keyform_positions.xys");
    const int positionCount = raw.integer("art_meshes.vertex_counts", 0) * 2;
    for (int form = 0; form < formCount; ++form) {
        const int previous = raw.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", formBegin + form);
        raw.set("art_mesh_keyforms.keyform_position_sources_begin_indices", newFormBegin + form, positions.size() / 4);
        positions.append(positions.mid(previous * 4, positionCount * 4));
    }
    raw.state.columns[positionSpan.slot] = positions; raw.state.counts[positionSpan.countIndex] = positions.size() / 4;
    return raw.encode();
}
bool stateForMeshes(const CModelDocument& document, Pairs::State& state, QString* error)
{
    Pairs::Pair pair; pair.id = "meshes";
    if (!Pairs::DescribeEndpoint(document, {Pairs::Type::ArtMesh, document.Drawables()[0].id}, pair.first, error) ||
        !Pairs::DescribeEndpoint(document, {Pairs::Type::ArtMesh, document.Drawables()[1].id}, pair.second, error)) return false;
    pair.firstVertexToSecondVertex = identity(pair.first.vertexCount);
    for (const auto& axis : pair.first.axes) pair.axes.append({axis.index, axis.index, identity(axis.keys.size())});
    return Pairs::Normalize({document.SourceDigest(), {pair}}, state, error) && Pairs::Validate(state, document, error);
}
bool structuralMesh(const CModelDocument& before, const QVector<QString>& ids, bool add, CModelDocument& after,
                    QVector<Pairs::VertexRemap>& remaps, QString* error)
{
    CMocMeshStructureBuilder::Request request;
    for (const auto& id : ids) {
        const auto found = std::find_if(before.Drawables().cbegin(), before.Drawables().cend(), [&](const auto& mesh) { return mesh.id == id; });
        if (found == before.Drawables().cend()) return false;
        CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = id;
        mesh.oldToNew = identity(found->vertexCount);
        if (!add) std::swap(mesh.oldToNew[0], mesh.oldToNew[1]);
        QVector<int> inverse(found->vertexCount);
        for (int old = 0; old < mesh.oldToNew.size(); ++old) inverse[mesh.oldToNew[old]] = old;
        for (const auto old : inverse) mesh.vertices.append({{old}, {1}});
        mesh.indices = before.DrawableIndices(id);
        for (auto& index : mesh.indices) index = quint16(mesh.oldToNew[index]);
        if (add) mesh.vertices.append({{0, 1}, {.5, .5}});
        remaps.append({{Pairs::Type::ArtMesh, id}, mesh.oldToNew}); request.meshes.append(mesh);
    }
    CModelEdit command;
    if (!before.MakeMeshStructureEdit(request, command, error)) return false;
    after = before; return after.ExecuteEdit(command);
}
bool insertRotation(CModelDocument& document, const QString& id, QString* error)
{
    QByteArray preview; if (!document.BuildPreviewMoc(preview, error)) return false;
    auto request = DeformerReplayFixture::rootMesh(preview, false); request.id = id;
    CModelEdit command;
    if (!document.MakeDeformerStructureEdit({request, document.SourceDigest(), QCryptographicHash::hash(preview, QCryptographicHash::Sha256)}, command, error)) return false;
    return document.ExecuteEdit(command);
}
}

class GeometryObjectPairsTests : public QObject
{
    Q_OBJECT
    CModelDocument m_meshes;
    Pairs::State m_meshState;
    QTemporaryDir m_directory;
private slots:
    void unsignedEndpointIndicesRoundTripWithoutNarrowing()
    {
        auto state = sample(); auto& pair = state.pairs[0];
        pair.reversedWinding = false;
        for (auto* value : {&pair.first, &pair.second}) {
            value->vertexCount = 65536;
            value->triangles = {0, 32768, 65535};
        }
        pair.firstVertexToSecondVertex = identity(65536);
        QString error; Pairs::State expected;
        QVERIFY2(Pairs::Normalize(state, expected, &error), qPrintable(error));
        QJsonObject json; QVERIFY(Pairs::ToJson(expected, json, &error));
        Pairs::State actual; QVERIFY2(Pairs::FromJson(json, actual, &error), qPrintable(error));
        QCOMPARE(actual, expected); QCOMPARE(actual.pairs[0].firstVertexToSecondVertex.last(), 65535);
        QCOMPARE(actual.pairs[0].first.triangles, (QVector<quint16>{0, 32768, 65535}));
        for (double invalid : {-1., 65536., 32768.5}) {
            auto changed = json; auto pairs = changed["pairs"].toArray(); auto item = pairs[0].toObject();
            auto endpoint = item["first"].toObject(); endpoint["triangles"] = QJsonArray{0, 1, invalid};
            item["first"] = endpoint; pairs[0] = item; changed["pairs"] = pairs;
            QVERIFY(!Pairs::FromJson(changed, actual, &error)); QCOMPARE(actual, expected);
        }
        state.pairs[0].first.vertexCount = 65537;
        QVERIFY(!Pairs::Normalize(state, actual, &error)); QCOMPARE(actual, expected);
    }
    void initTestCase()
    {
        QVERIFY(m_directory.isValid()); QFile file(m_directory.filePath("paired.moc3")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = pairedMeshes(); QVERIFY(!bytes.isEmpty()); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QString error; QVERIFY2(m_meshes.Load(file.fileName(), &error), qPrintable(error));
        QVERIFY2(stateForMeshes(m_meshes, m_meshState, &error), qPrintable(error));
    }
    void explicitMappingsRespectAxisOrderAndInverse()
    {
        auto state = sample(); Pairs::State normalized; QString error;
        QVERIFY2(Pairs::Normalize(state, normalized, &error), qPrintable(error));
        for (int source = 0; source < 6; ++source) {
            int target = -1, roundtrip = -1;
            QVERIFY(Pairs::ResolveKeyform(normalized.pairs[0], source, true, target, &error));
            QCOMPARE(target, source / 3 + (2 - source % 3) * 2);
            QVERIFY(Pairs::ResolveKeyform(normalized.pairs[0], target, false, roundtrip, &error)); QCOMPARE(roundtrip, source);
        }
        int held = 77; QVERIFY(!Pairs::ResolveKeyform(state.pairs[0], 6, true, held)); QCOMPARE(held, 77);
        state.pairs[0].requiresReview = true; QVERIFY(!Pairs::ResolveKeyform(state.pairs[0], 0, true, held)); QCOMPARE(held, 77);
        state.pairs[0].requiresReview = false; state.pairs[0].enabled = false;
        QVERIFY(!Pairs::ResolveKeyform(state.pairs[0], 0, true, held)); QCOMPARE(held, 77);
    }
    void topologyCanonicalizationPreservesWinding()
    {
        auto state = sample(); Pairs::State baseline, normalized;
        QVERIFY(Pairs::Normalize(state, baseline));
        state.pairs[0].first.triangles = {1, 3, 2, 1, 2, 0};
        QVERIFY(Pairs::Normalize(state, normalized)); QCOMPARE(normalized, baseline);
        std::swap(state.pairs[0].first.triangles[1], state.pairs[0].first.triangles[2]);
        QVERIFY(!Pairs::Normalize(state, normalized)); QCOMPARE(normalized, baseline);
        state = sample(); state.pairs[0].reversedWinding = false; QVERIFY(!Pairs::Normalize(state, normalized));
        QCOMPARE(normalized, baseline);
    }
    void warpGridMappingRequiresCellsAndOrientation()
    {
        auto state = sample(); auto& pair = state.pairs[0]; pair.axes.clear();
        pair.first = {{Pairs::Type::Warp, "left"}, 6, 1, 2, {}, {}, 1};
        pair.second = {{Pairs::Type::Warp, "right"}, 6, 1, 2, {}, {}, 1};
        pair.firstVertexToSecondVertex = {2, 1, 0, 5, 4, 3};
        Pairs::State normalized; QVERIFY(Pairs::Normalize(state, normalized));
        std::swap(pair.firstVertexToSecondVertex[0], pair.firstVertexToSecondVertex[1]); QVERIFY(!Pairs::Normalize(state, normalized));
        pair.firstVertexToSecondVertex = {0, 2, 4, 1, 3, 5}; pair.second.rows = 2; pair.second.columns = 1;
        QVERIFY(Pairs::Normalize(state, normalized));
    }
    void rotationFrameIsExplicitOrthogonalAndRoundTrips()
    {
        auto state = sample(); auto& pair = state.pairs[0];
        pair.first = {{Pairs::Type::Rotation, "left"}, 0, 0, 0, {}, {}, 1};
        pair.second = {{Pairs::Type::Rotation, "right"}, 0, 0, 0, {}, {}, 1};
        pair.firstVertexToSecondVertex.clear(); pair.axes.clear(); pair.reversedWinding = false;
        const double angle = .73;
        pair.rotationLocalX = QPointF(std::cos(angle), std::sin(angle));
        pair.rotationLocalY = QPointF(std::sin(angle), -std::cos(angle));
        Pairs::State normalized, decoded; QString error;
        QVERIFY2(Pairs::Normalize(state, normalized, &error), qPrintable(error));
        QJsonObject json; QVERIFY(Pairs::ToJson(state, json)); QVERIFY(Pairs::FromJson(json, decoded)); QCOMPARE(decoded, normalized);
        pair.rotationLocalY = pair.rotationLocalX; QVERIFY(!Pairs::Normalize(state, decoded)); QCOMPARE(decoded, normalized);
        pair.rotationLocalY = QPointF(std::sin(angle), -std::cos(angle)); pair.rotationLocalX *= 1.0001;
        QVERIFY(!Pairs::Normalize(state, decoded)); QCOMPARE(decoded, normalized);
        pair.rotationLocalX.setX(std::numeric_limits<double>::infinity()); QVERIFY(!Pairs::Normalize(state, decoded));
        state = sample(); state.pairs[0].rotationLocalX = QPointF(-1, 0); QVERIFY(!Pairs::Normalize(state, decoded));
        auto rows = json["pairs"].toArray(); auto row = rows[0].toObject(); row.remove("rotationLocalY"); rows[0] = row; json["pairs"] = rows;
        QVERIFY(!Pairs::FromJson(json, decoded)); QCOMPARE(decoded, normalized);
    }
    void typedNamesDoNotAliasAndIncompleteRelationsStayInvalid()
    {
        auto state = sample(); auto rotation = state.pairs[0]; rotation.id = "rotation";
        rotation.first = {{Pairs::Type::Rotation, "left"}, 0, 0, 0, {}, {}, 1};
        rotation.second = {{Pairs::Type::Rotation, "right"}, 0, 0, 0, {}, {}, 1};
        rotation.firstVertexToSecondVertex.clear(); rotation.axes.clear(); rotation.reversedWinding = false;
        state.pairs.append(rotation); Pairs::State normalized; QVERIFY(Pairs::Normalize(state, normalized));
        state.pairs[0].requiresReview = true; state.pairs[0].firstVertexToSecondVertex.clear();
        QVERIFY(!Pairs::Normalize(state, normalized));
    }
    void resourceBudgetsRejectOversizedMappings()
    {
        auto state = sample(); state.pairs[0].first.triangles.resize(Pairs::MaximumMappings + 1);
        Pairs::State result; result.sourceDigest = QByteArray(32, 's'); const auto held = result;
        QVERIFY(!Pairs::Normalize(state, result)); QCOMPARE(result, held);
        state = sample(); state.pairs.resize(Pairs::MaximumPairs + 1); QVERIFY(!Pairs::Normalize(state, result)); QCOMPARE(result, held);
    }
    void malformedStatesPreserveOutput_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 24; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void malformedStatesPreserveOutput()
    {
        QFETCH(int, kind); auto state = sample(); auto& pair = state.pairs[0];
        switch (kind) {
        case 0: state.sourceDigest = "short"; break;
        case 1: pair.id.clear(); break;
        case 2: pair.second.object = pair.first.object; break;
        case 3: pair.second.object.type = Pairs::Type::Warp; break;
        case 4: pair.first.object.type = Pairs::Type(99); break;
        case 5: pair.firstVertexToSecondVertex.clear(); break;
        case 6: pair.firstVertexToSecondVertex[0] = 4; break;
        case 7: pair.firstVertexToSecondVertex[0] = -1; break;
        case 8: pair.firstVertexToSecondVertex[0] = pair.firstVertexToSecondVertex[1]; break;
        case 9: pair.first.triangles.removeLast(); break;
        case 10: pair.first.triangles[0] = 4; break;
        case 11: pair.first.axes[0].keys[0] = std::numeric_limits<float>::infinity(); break;
        case 12: pair.first.axes[0].keys[1] = -1; break;
        case 13: pair.first.axes[0].index = 1; break;
        case 14: pair.first.axes[1].parameterId = pair.first.axes[0].parameterId; break;
        case 15: pair.axes[0].firstKeyToSecondKey = {0, 0, 1}; break;
        case 16: pair.axes[0].secondAxis = 2; break;
        case 17: pair.axes[0].firstAxis = 1; break;
        case 18: pair.first.keyformCount = 7; break;
        case 19: pair.lineDirection = {}; break;
        case 20: pair.linePoint.setX(std::numeric_limits<double>::quiet_NaN()); break;
        case 21: pair.lineDirection.setY(std::numeric_limits<double>::infinity()); break;
        case 22: state.pairs.append(pair); break;
        case 23: pair.first.axes[0].parameterType = 1; break;
        }
        Pairs::State output; output.sourceDigest = QByteArray(32, 'z'); const auto held = output; QString error;
        QVERIFY(!Pairs::Normalize(state, output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output, held);
        QJsonObject json{{"keep", 7}}; QVERIFY(!Pairs::ToJson(state, json, &error)); QCOMPARE(json, QJsonObject({{"keep", 7}}));
    }
    void serdePreservesFloatBitsAndReviewState()
    {
        auto state = sample();
        const QVector<float> keys{-0.f, std::numeric_limits<float>::denorm_min(), 1.f};
        state.pairs[0].first.axes[0].keys = keys; state.pairs[0].second.axes[1].keys = keys;
        state.pairs[0].requiresReview = true; state.pairs[0].enabled = false;
        QJsonObject object; QString error; QVERIFY2(Pairs::ToJson(state, object, &error), qPrintable(error));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QVERIFY(bytes.contains("80000000")); QVERIFY(bytes.contains("00000001"));
        Pairs::State decoded, normalized; QVERIFY(Pairs::Normalize(state, normalized));
        QVERIFY2(Pairs::FromJson(QJsonDocument::fromJson(bytes).object(), decoded, &error), qPrintable(error)); QCOMPARE(decoded, normalized);
        auto changed = decoded; changed.pairs[0].first.axes[0].keys[0] = 0.f; QVERIFY(changed != decoded);
        QVERIFY(std::signbit(decoded.pairs[0].first.axes[0].keys[0]));
        QJsonObject second; QVERIFY(Pairs::ToJson(decoded, second)); QCOMPARE(second, object);
    }
    void malformedJsonPreservesOutput_data()
    {
        QTest::addColumn<int>("kind"); for (int kind = 0; kind < 12; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void malformedJsonPreservesOutput()
    {
        QFETCH(int, kind); QJsonObject object; QVERIFY(Pairs::ToJson(sample(), object));
        if (kind == 0) object["version"] = 2;
        if (kind == 1) object["extra"] = true;
        if (kind == 2) object["sourceDigest"] = QString(64, 'g');
        if (kind == 3) object["pairs"] = QJsonObject{};
        auto rows = object["pairs"].toArray();
        if (kind >= 4) {
            auto pair = rows[0].toObject();
            if (kind == 4) pair["enabled"] = 1;
            if (kind == 5) pair["vertices"] = QJsonArray{1, .5, 3, 2};
            if (kind == 6) pair["lineDirection"] = QJsonArray{0, 0};
            if (kind >= 7) {
                auto endpoint = pair["first"].toObject(); auto axes = endpoint["axes"].toArray(); auto axis = axes[0].toObject();
                if (kind == 7) axis["keys"] = QJsonArray{-1, 0, 1};
                if (kind == 8) axis["keys"] = QJsonArray{"bf800000", "00000000", "7f800000"};
                if (kind == 9) axis["keys"] = QJsonArray{"BF800000", "00000000", "3f800000"};
                if (kind == 10) axis["keys"] = QJsonArray{"bf800000", "0000000", "3f800000"};
                if (kind == 11) axis["parameterType"] = 2;
                axes[0] = axis; endpoint["axes"] = axes; pair["first"] = endpoint;
            }
            rows[0] = pair; object["pairs"] = rows;
        }
        auto output = sample(); const auto held = output; QString error;
        QVERIFY(!Pairs::FromJson(object, output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output, held);
    }
    void ordinaryGeometryChangesKeepIdentity()
    {
        auto changed = m_meshes; const auto& endpoint = m_meshState.pairs[0].first;
        auto positions = changed.DrawableKeyformPositions(endpoint.object.id, 0); positions[0] += .05f;
        QVERIFY(changed.SetDrawableKeyformPositions(endpoint.object.id, 0, positions));
        Pairs::State next; QString error; QVERIFY2(Pairs::Reconcile(m_meshState, m_meshes, changed, {}, {}, next, &error), qPrintable(error));
        QCOMPARE(next, m_meshState); QVERIFY(Pairs::Validate(next, changed));
        Pairs::Endpoint described; QVERIFY(Pairs::DescribeEndpoint(changed, endpoint.object, described)); QCOMPARE(described, endpoint);
    }
    void uvChangesAndUnrelatedHintsDoNotInvalidatePairs()
    {
        DeformerReplayFixture::RawColumns raw(m_meshes.SourceBytes());
        const int offset = raw.integer("art_meshes.uv_sources_begin_indices", 0);
        raw.setFloat("uvs.uvs", offset, raw.number("uvs.uvs", offset) + .05f);
        auto changed = m_meshes; QString error; QVERIFY2(changed.SetAuthoringStructure(raw.state, &error), qPrintable(error));
        Pairs::State next;
        QVERIFY2(Pairs::Reconcile(m_meshState, m_meshes, changed, {{{Pairs::Type::ArtMesh, "unpaired"}, {-99}}}, {}, next, &error), qPrintable(error));
        QCOMPARE(next, m_meshState); QVERIFY(Pairs::Validate(next, changed));
        Pairs::State empty;
        QVERIFY(Pairs::Reconcile({}, m_meshes, changed, {{{Pairs::Type::ArtMesh, "unpaired"}, {-99}}}, {}, empty)); QVERIFY(empty.pairs.isEmpty());
    }
    void explicitVertexRemapRetainsExactCorrespondence()
    {
        CModelDocument after; QVector<Pairs::VertexRemap> remaps; QString error;
        const auto first = m_meshState.pairs[0].first.object.id;
        QVERIFY2(structuralMesh(m_meshes, {first}, false, after, remaps, &error), qPrintable(error));
        Pairs::State migrated; QVERIFY2(Pairs::Reconcile(m_meshState, m_meshes, after, remaps, {}, migrated, &error), qPrintable(error));
        QVERIFY(!migrated.pairs[0].requiresReview); QCOMPARE(migrated.pairs[0].firstVertexToSecondVertex[0], 1);
        QCOMPARE(migrated.pairs[0].firstVertexToSecondVertex[1], 0); QVERIFY(Pairs::Validate(migrated, after));
        Pairs::State missing; QVERIFY(Pairs::Reconcile(m_meshState, m_meshes, after, {}, {}, missing)); QVERIFY(missing.pairs[0].requiresReview);
        auto invalid = remaps; invalid[0].oldToNew[0] = invalid[0].oldToNew[1]; const auto held = migrated;
        QVERIFY(!Pairs::Reconcile(m_meshState, m_meshes, after, invalid, {}, migrated)); QCOMPARE(migrated, held);
    }
    void newPointsInvalidateAndMatchingSignatureDoesNotReactivate()
    {
        CModelDocument after; QVector<Pairs::VertexRemap> remaps; QString error;
        const auto& pair = m_meshState.pairs[0];
        QVERIFY2(structuralMesh(m_meshes, {pair.first.object.id, pair.second.object.id}, true, after, remaps, &error), qPrintable(error));
        Pairs::State review; QVERIFY2(Pairs::Reconcile(m_meshState, m_meshes, after, remaps, {}, review, &error), qPrintable(error));
        QVERIFY(review.pairs[0].requiresReview); QCOMPARE(review.pairs[0].first, pair.first); QVERIFY(Pairs::Validate(review, after));
        Pairs::State restored; QVERIFY(Pairs::Reconcile(review, after, m_meshes, {}, {}, restored)); QVERIFY(restored.pairs[0].requiresReview);
        int resolved = 100; QVERIFY(!Pairs::ResolveKeyform(restored.pairs[0], 0, true, resolved)); QCOMPARE(resolved, 100);
    }
    void topologyOnlyChangesNeedExplicitIdentityAndStillValidateParity()
    {
        auto after = m_meshes; const auto& pair = m_meshState.pairs[0];
        auto indices = after.DrawableIndices(pair.first.object.id); std::swap(indices[0], indices[1]);
        QVERIFY(after.SetDrawableIndices(pair.first.object.id, indices));
        Pairs::State review;
        QVERIFY(Pairs::Reconcile(m_meshState, m_meshes, after, {{pair.first.object, identity(pair.first.vertexCount)}}, {}, review));
        QVERIFY(review.pairs[0].requiresReview);
    }
    void bindingKeysUseExplicitOldIdentity()
    {
        CModelDocument before = m_meshes; CModelEdit command; QString error;
        CMocParameterStructureBuilder::CreateRequest create; create.parameter.id = "PairAxis";
        create.bindings = {{{CMocParameterStructureBuilder::Owner::ArtMesh, before.Drawables()[0].id}, {-1, 0, 1}},
                           {{CMocParameterStructureBuilder::Owner::ArtMesh, before.Drawables()[1].id}, {-1, 0, 1}}};
        QVERIFY2(before.MakeCreateParameterEdit(create, {}, command, &error), qPrintable(error)); QVERIFY(before.ExecuteEdit(command));
        Pairs::State state; QVERIFY2(stateForMeshes(before, state, &error), qPrintable(error));
        const auto& first = state.pairs[0].first; const int axisIndex = first.axes.size() - 1;
        CMocParameterStructureBuilder::EditBindingKeysRequest request;
        request.owner = {CMocParameterStructureBuilder::Owner::ArtMesh, first.object.id}; request.axisIndex = axisIndex;
        request.parameterId = "PairAxis"; request.expectedKeys = {-1, 0, 1};
        request.keys = {{-1, 1}, {0, 0}, {1, 2}};
        QVERIFY2(before.MakeBindingKeysEdit(request, command, &error), qPrintable(error)); auto after = before; QVERIFY(after.ExecuteEdit(command));
        Pairs::State migrated;
        QVERIFY2(Pairs::Reconcile(state, before, after, {}, {{first.object, axisIndex, axisIndex, {1, 0, 2}}}, migrated, &error), qPrintable(error));
        QVERIFY(!migrated.pairs[0].requiresReview); QCOMPARE(migrated.pairs[0].axes.last().firstKeyToSecondKey, QVector<int>({1, 0, 2}));
        request.keys = {{-1, 0}, {-.5f, -1}, {0, 1}, {1, 2}};
        QVERIFY2(before.MakeBindingKeysEdit(request, command, &error), qPrintable(error)); after = before; QVERIFY(after.ExecuteEdit(command));
        QVERIFY(Pairs::Reconcile(state, before, after, {}, {{first.object, axisIndex, axisIndex, {0, 2, 3}}}, migrated, &error));
        QVERIFY(migrated.pairs[0].requiresReview);
    }
    void deletionAndRecreationDoNotRevivePair()
    {
        CModelDocument before; QString error; QVERIFY(before.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        QVERIFY2(insertRotation(before, "PairRotationA", &error), qPrintable(error));
        QVERIFY2(insertRotation(before, "PairRotationB", &error), qPrintable(error));
        Pairs::Pair pair; pair.id = "rotations";
        QVERIFY(Pairs::DescribeEndpoint(before, {Pairs::Type::Rotation, "PairRotationA"}, pair.first));
        QVERIFY(Pairs::DescribeEndpoint(before, {Pairs::Type::Rotation, "PairRotationB"}, pair.second));
        Pairs::State state{before.SourceDigest(), {pair}}; QVERIFY(Pairs::Validate(state, before));
        QByteArray preview; QVERIFY(before.BuildPreviewMoc(preview)); CModelEdit command;
        CDeformerPreserveRequest remove{CMocDeformerStructureBuilder::RemoveRequest{"PairRotationA"}, before.SourceDigest(), QCryptographicHash::hash(preview, QCryptographicHash::Sha256)};
        QVERIFY2(before.MakeDeformerStructureEdit(remove, command, &error), qPrintable(error)); auto removed = before; QVERIFY(removed.ExecuteEdit(command));
        Pairs::State pruned; QVERIFY(Pairs::Reconcile(state, before, removed, {}, {}, pruned)); QVERIFY(pruned.pairs.isEmpty());
        auto recreated = removed; QVERIFY2(insertRotation(recreated, "PairRotationA", &error), qPrintable(error));
        Pairs::State result; QVERIFY(Pairs::Reconcile(pruned, removed, recreated, {}, {}, result)); QVERIFY(result.pairs.isEmpty());
        QVERIFY(Pairs::Validate(state, before));
    }
    void sourceChangesRequireExplicitValidatedReanchor()
    {
        auto changed = m_meshes; auto positions = changed.DrawableKeyformPositions(changed.Drawables()[0].id, 0); positions[0] += .1f;
        QVERIFY(changed.SetDrawableKeyformPositions(changed.Drawables()[0].id, 0, positions));
        QByteArray bytes; QVERIFY(changed.BuildRuntimeMoc(bytes)); CModelDocument reopened; QString error; QVERIFY(reopened.LoadBytes(bytes, {}, &error));
        QVERIFY(reopened.SourceDigest() != m_meshes.SourceDigest());
        Pairs::State result = sample(); const auto held = result;
        QVERIFY(!Pairs::Reconcile(m_meshState, changed, reopened, {}, {}, result)); QCOMPARE(result, held);
        QVERIFY2(Pairs::Reanchor(m_meshState, changed, reopened, result, &error), qPrintable(error));
        QCOMPARE(result.sourceDigest, reopened.SourceDigest()); QVERIFY(Pairs::Validate(result, reopened));
        auto indices = reopened.DrawableIndices(result.pairs[0].first.object.id); std::swap(indices[0], indices[1]);
        QVERIFY(reopened.SetDrawableIndices(result.pairs[0].first.object.id, indices)); const auto good = result;
        QVERIFY(!Pairs::Reanchor(m_meshState, changed, reopened, result)); QCOMPARE(result, good);
    }
};

QTEST_APPLESS_MAIN(GeometryObjectPairsTests)
#include "geometry_object_pairs_tests.moc"
