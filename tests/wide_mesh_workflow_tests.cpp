#include "wide_mesh_fixture.h"
#include "real_core_model_fixture.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmesheditplanner.h"
#include "live2dstruct/cmodelsession.h"
#include <QtTest>
#include <QJsonArray>
#include <QTemporaryDir>
#include <numeric>

using namespace live2dstruct;
namespace {
CModelSession sessionFor(const CModelDocument& document)
{
    CModelSession result;
    result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.structure = document.ParameterStructure(); result.meshStructure = document.HasMeshStructure();
    result.geometryObjectPairs = document.GeometryObjectPairs();
    result.drawableIndices = document.DrawableIndexOverrides();
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    result.drawableMaterials = document.DrawableMaterialOverrides();
    result.materialStructure = document.HasMaterialStructure();
    result.artwork = document.Artwork();
    return result;
}

int drawable(CoreModel& core, const QString& id)
{
    for (int i = 0; i < csmGetDrawableCount(core.value); ++i)
        if (QString::fromUtf8(csmGetDrawableIds(core.value)[i]) == id) return i;
    return -1;
}

QVector<float> positions(CoreModel& core, int index)
{
    QVector<float> result;
    for (int i = 0; i < csmGetDrawableVertexCounts(core.value)[index]; ++i) {
        const auto value = csmGetDrawableVertexPositions(core.value)[index][i];
        result.append({value.X, value.Y});
    }
    return result;
}

void pose(CoreModel& core, const QMap<QString, float>& values)
{
    QVector<float> parameters;
    for (int i = 0; i < csmGetParameterCount(core.value); ++i)
        parameters.append(values.value(QString::fromUtf8(csmGetParameterIds(core.value)[i]), csmGetParameterDefaultValues(core.value)[i]));
    core.pose(parameters);
}
}

class WideMeshWorkflowTests : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    bool load(const WideMeshData::Fixture& fixture, CModelDocument& result, QString* error)
    {
        const auto path = directory.filePath(QStringLiteral("wide-%1.moc3").arg(fixture.vertexCount));
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(fixture.bytes) == fixture.bytes.size() && result.LoadBytes(fixture.bytes, path, error);
    }

private slots:
    void indexOverridesRoundTripThroughSessionAndCore_data()
    {
        QTest::addColumn<int>("count");
        for (int count : {32768, 32769, 65536, 65537}) QTest::newRow(qPrintable(QString::number(count))) << count;
    }
    void indexOverridesRoundTripThroughSessionAndCore()
    {
        QFETCH(int, count);
        WideMeshData::Fixture fixture; QString error;
        QVERIFY2(WideMeshData::make(count, WideMeshData::Layout::Sparse, fixture, &error), qPrintable(error));
        CModelDocument original; QVERIFY2(load(fixture, original, &error), qPrintable(error));
        const auto id = fixture.ids.first(); QCOMPARE(original.DrawableIndices(id), fixture.indices);
        auto document = original; auto changed = fixture.indices; std::rotate(changed.begin(), changed.begin() + 1, changed.end());
        QVERIFY(document.SetDrawableIndices(id, changed));
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(sessionFor(document).ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.drawableIndices.value(id), changed);
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(original, restored, &error), qPrintable(error));
        QCOMPARE(restored.DrawableIndices(id), changed); QCOMPARE(restored.SourceBytes(), fixture.bytes);
        QByteArray runtime; QVERIFY2(restored.BuildRuntimeMoc(runtime, &error), qPrintable(error));
        CoreModel core; QVERIFY(core.load(runtime)); pose(core, {}); const int index = drawable(core, id); QVERIFY(index >= 0);
        QCOMPARE(csmGetDrawableVertexCounts(core.value)[index], count);
        QCOMPARE(csmGetDrawableIndexCounts(core.value)[index], 3);
        const auto* actual = csmGetDrawableIndices(core.value)[index];
        for (int i = 0; i < changed.size(); ++i) QCOMPARE(actual[i], changed[2 - i]);
        QVERIFY(core.intact());
        if (count == 65537) {
            CMocMeshStructureBuilder::Request request;
            CMocMeshStructureBuilder::MeshEdit edit; edit.artMeshId = id; edit.indices = changed;
            for (int vertex = 0; vertex < count; ++vertex) { edit.vertices.append({{vertex}, {1}}); edit.oldToNew.append(vertex); }
            request.meshes.append(edit); CMocMeshStructureBuilder::Impact impact; impact.blockingReasons = {"sentinel"};
            QVERIFY(!restored.AnalyzeMeshStructureEdit(request, impact, &error));
            QCOMPARE(impact.blockingReasons, QStringList{"sentinel"});
        }
    }

    void malformedWideSessionValuesStayAtomic_data()
    {
        QTest::addColumn<double>("value");
        for (double value : {-1., 65536., 65535.5, 1e30}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }
    void malformedWideSessionValuesStayAtomic()
    {
        QFETCH(double, value); CModelSession result; result.sourceMoc = "unchanged";
        const QJsonObject root{{"version", 1}, {"drawableIndices", QJsonObject{{"mesh", QJsonArray{0, 1, value}}}}};
        QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(root).toJson(), &result, &error));
        QCOMPARE(result.sourceMoc, QString("unchanged")); QVERIFY(!error.isEmpty());
    }

    void highPairMappingMovesBothCoreEndpointsAndRestores()
    {
        WideMeshData::Fixture fixture; QString error;
        QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Sparse, fixture, &error, 2), qPrintable(error));
        CModelDocument document; QVERIFY2(load(fixture, document, &error), qPrintable(error));
        CGeometryObjectPairs::Pair pair; pair.id = "wide-pair";
        QVERIFY(CGeometryObjectPairs::DescribeEndpoint(document, {CGeometryObjectPairs::Type::ArtMesh, fixture.ids[0]}, pair.first, &error));
        QVERIFY(CGeometryObjectPairs::DescribeEndpoint(document, {CGeometryObjectPairs::Type::ArtMesh, fixture.ids[1]}, pair.second, &error));
        QCOMPARE(pair.first.keyformCount, 1); QCOMPARE(pair.second.keyformCount, 1);
        pair.firstVertexToSecondVertex.resize(65536);
        std::iota(pair.firstVertexToSecondVertex.begin(), pair.firstVertexToSecondVertex.end(), 0);
        QVERIFY2(document.SetGeometryObjectPairs({document.SourceDigest(), {pair}}, &error), qPrintable(error));
        QJsonObject json; QVERIFY(CGeometryObjectPairs::ToJson(document.GeometryObjectPairs(), json, &error));
        CGeometryObjectPairs::State parsed; QVERIFY(CGeometryObjectPairs::FromJson(json, parsed, &error));
        QCOMPARE(parsed, document.GeometryObjectPairs()); QCOMPARE(parsed.pairs[0].firstVertexToSecondVertex.last(), 65535);
        CKeyformEditController controller; controller.SetDocument(&document); QByteArray published;
        controller.SetPublisher([&](const CModelDocument& next, const auto&, bool, QString* detail) { return next.BuildRuntimeMoc(published, detail); }, [](QString*) { return true; });
        QVERIFY2(controller.Activate(fixture.ids[0], 0, {}, &error), qPrintable(error));
        CoreModel before; QVERIFY(before.load(published)); pose(before, controller.Pose());
        const int first = drawable(before, fixture.ids[0]), second = drawable(before, fixture.ids[1]); QVERIFY(first >= 0 && second >= 0);
        const auto firstBefore = positions(before, first), secondBefore = positions(before, second);
        QVector<double> weights(65536, 0); weights[32768] = .25; weights[65535] = .5;
        const auto initial = document.AuthoringStructureState();
        QVERIFY2(controller.BeginWeightedDrag(weights, firstBefore, &error), qPrintable(error));
        QVERIFY2(controller.DragBy({.02, -.04}, &error), qPrintable(error)); controller.FinishDrag();
        const auto edited = document.AuthoringStructureState();
        CoreModel after; QVERIFY(after.load(published)); pose(after, controller.Pose());
        const auto firstAfter = positions(after, first), secondAfter = positions(after, second);
        for (int vertex : {0, 32767, 32768, 65534, 65535}) {
            const double x = .02 * weights[vertex], y = -.04 * weights[vertex];
            QVERIFY(std::abs(firstAfter[vertex * 2] - firstBefore[vertex * 2] - x) < .00002);
            QVERIFY(std::abs(firstAfter[vertex * 2 + 1] - firstBefore[vertex * 2 + 1] - y) < .00002);
            QVERIFY(std::abs(secondAfter[vertex * 2] - secondBefore[vertex * 2] + x) < .00002);
            QVERIFY(std::abs(secondAfter[vertex * 2 + 1] - secondBefore[vertex * 2 + 1] - y) < .00002);
        }
        CModelEdit edit; QVERIFY(controller.History(true, edit)); QCOMPARE(document.AuthoringStructureState(), initial); QVERIFY(!document.CanUndoEdit());
        QVERIFY(controller.History(false, edit)); QCOMPARE(document.AuthoringStructureState(), edited);
        CModelSession decoded; QVERIFY(CModelSession::FromJson(sessionFor(document).ToJson(), &decoded, &error));
        CModelDocument original, restored; QVERIFY(load(fixture, original, &error));
        QVERIFY2(decoded.RestoreDocument(original, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), edited); QVERIFY(before.intact()); QVERIFY(after.intact());
    }

    void denseTopologyMigratesAllKeyformsAndUvs()
    {
        WideMeshData::Fixture fixture; QString error;
        QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Grid, fixture, &error, 1, 2), qPrintable(error));
        CModelDocument original; QVERIFY2(load(fixture, original, &error), qPrintable(error)); auto document = original;
        const auto id = fixture.ids.first();
        const auto found = std::find_if(original.Drawables().cbegin(), original.Drawables().cend(), [&](const auto& item) { return item.id == id; });
        QVERIFY(found != original.Drawables().cend()); QVERIFY(found->keyforms.size() >= 2);
        CMeshEditPlanner::Input input{id, found->keyforms[0].positions, fixture.indices, {}};
        CMeshEditPlanner::Result deleted; CMeshEditPlanner::Diagnostic diagnostic;
        QVERIFY2(CMeshEditPlanner::Delete(input, {65535}, CMeshEditPlanner::BoundaryPolicy::ReplaceWithChord, deleted, &diagnostic), qPrintable(diagnostic.message));
        CMocMeshStructureBuilder::Request request; request.meshes.append(deleted.edit); CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(command));
        const auto narrowed = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& item) { return item.id == id; });
        QCOMPARE(narrowed->vertexCount, 65535); QCOMPARE(narrowed->uvs, found->uvs.mid(0, 65535 * 2));
        for (int key = 0; key < found->keyforms.size(); ++key)
            QCOMPARE(document.DrawableKeyformPositions(id, key), original.DrawableKeyformPositions(id, key).mid(0, 65535 * 2));
        input.positions = document.DrawableKeyformPositions(id, 0); input.indices = document.DrawableIndices(id);
        const int triangle = input.indices.size() / 3 - 1; QPointF center;
        for (int i = 0; i < 3; ++i) { const int vertex = input.indices[triangle * 3 + i]; center += QPointF(input.positions[vertex * 2], input.positions[vertex * 2 + 1]) / 3; }
        CMeshEditPlanner::Result inserted;
        QVERIFY2(CMeshEditPlanner::Insert(input, center, triangle, inserted, &diagnostic), qPrintable(diagnostic.message));
        QCOMPARE(inserted.edit.vertices.size(), 65536); QVERIFY(inserted.edit.indices.contains(65535));
        const auto beforeInsert = document;
        const auto uvBeforeInsert = narrowed->uvs;
        request.meshes = {inserted.edit}; QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(command));
        const auto checkValues = [&](const QVector<float>& before, const QVector<float>& after) {
            QCOMPARE(after.size(), before.size() + 2); QCOMPARE(after.mid(0, before.size()), before);
            const auto& vertex = inserted.edit.vertices.last();
            for (int axis = 0; axis < 2; ++axis) {
                double value = 0;
                for (int i = 0; i < vertex.indices.size(); ++i) value += double(before[vertex.indices[i] * 2 + axis]) * vertex.weights[i];
                QVERIFY(std::abs(double(after[after.size() - 2 + axis]) - value) < .000002);
            }
        };
        const auto completed = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& item) { return item.id == id; });
        checkValues(uvBeforeInsert, completed->uvs);
        for (int key = 0; key < found->keyforms.size(); ++key)
            checkValues(beforeInsert.DrawableKeyformPositions(id, key), document.DrawableKeyformPositions(id, key));
        const auto expected = document.AuthoringStructureState();
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(sessionFor(document).ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(original, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), expected);
        QByteArray runtime; QVERIFY(restored.BuildRuntimeMoc(runtime, &error)); CoreModel core; QVERIFY(core.load(runtime)); pose(core, {});
        QCOMPARE(csmGetDrawableVertexCounts(core.value)[drawable(core, id)], 65536); QVERIFY(core.intact());
        QVERIFY(document.UndoEdit()); QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original.AuthoringStructureState());
        QVERIFY(document.RedoEdit()); QVERIFY(document.RedoEdit()); QCOMPARE(document.AuthoringStructureState(), expected);
    }
};

QTEST_GUILESS_MAIN(WideMeshWorkflowTests)
#include "wide_mesh_workflow_tests.moc"
