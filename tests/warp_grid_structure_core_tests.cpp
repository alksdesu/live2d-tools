#include "warp_grid_fixture.h"
#include "real_core_model_fixture.h"

using namespace live2dstruct;
namespace Grid = WarpGridFixture;
using Builder = CMocDeformerStructureBuilder;

namespace {
QSet<int> affectedMeshes(const CSectionTable& table, int target)
{
    QSet<int> result;
    for (int mesh = 0; mesh < table.art_meshes.size(); ++mesh) {
        int parent = table.art_meshes[mesh]->parent_deformer_indices;
        QSet<int> visited;
        while (parent >= 0 && parent < table.deformers.size() && !visited.contains(parent)) {
            if (parent == target) {
                result.insert(mesh);
                break;
            }
            visited.insert(parent);
            parent = table.deformers[parent]->parent_deformer_indices;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto* glue : table.glue) {
            if (!result.contains(glue->art_mesh_indices_a) && !result.contains(glue->art_mesh_indices_b))
                continue;
            const int previous = result.size();
            result.insert(glue->art_mesh_indices_a);
            result.insert(glue->art_mesh_indices_b);
            changed = changed || result.size() != previous;
        }
    }
    return result;
}

struct Witness
{
    QByteArray bytes;
    QString target, mesh;
    QVector<float> input;
};

Witness witness(bool quad)
{
    Witness result;
    auto base = DeformerReplayFixture::read("Mark");
    auto reader = CReader::FromBytes(base);
    CSectionTable table(reader);
    QSet<int> glued;
    for (const auto* glue : table.glue) {
        glued.insert(glue->art_mesh_indices_a);
        glued.insert(glue->art_mesh_indices_b);
    }
    int selected = -1;
    for (int index = 0; index < table.art_meshes.size(); ++index)
        if (table.art_meshes[index]->vertex_counts >= 16 && !glued.contains(index)) {
            selected = index;
            break;
        }
    if (selected < 0)
        return { };
    const auto* mesh = table.art_meshes[selected];
    result.mesh = mesh->ids;
    result.target = "GridResizeWitness";
    Builder::Warp shape { 2, 2, quad, { } };
    for (int row = 0; row <= 2; ++row)
        for (int column = 0; column <= 2; ++column) {
            const float u = float(column) / 2, v = float(row) / 2;
            shape.positions << u + .13f * u * v + .17f * (row == 1 && column == 1)
                            << v + .23f * (column == 1) + .12f * (row == 1);
        }
    Builder::InsertRequest insert;
    insert.id = result.target;
    insert.partIndex = mesh->parent_part_indices;
    insert.shape = shape;
    Builder::Result added;
    if (!Builder::Insert(base, insert, added))
        return { };
    DeformerReplayFixture::RawColumns columns(added.bytes);
    columns.set("art_meshes.parent_deformer_indices", selected, added.impact.insertedIndex);
    const QVector<QPointF> points { { 0, 0 }, { .2, .2 }, { .5, .5 }, { .75, .25 }, { .4, .7 }, { .9, .8 }, { 1, 1 },
        { -.25, .25 }, { .2, -.5 }, { .25, 1.25 }, { 3.1, .5 }, { .125, .875 }, { .875, .125 }, { .25, .5 },
        { .5, .25 }, { 1, .3 } };
    for (int form = 0; form < mesh->keyform_sources_counts; ++form) {
        const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + form]
                              ->keyform_position_sources_begin_indices;
        for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
            const auto point = points[vertex % points.size()];
            columns.setFloat("keyform_positions.xys", begin + vertex * 2, point.x());
            columns.setFloat("keyform_positions.xys", begin + vertex * 2 + 1, point.y());
            if (!form)
                result.input << float(point.x()) << float(point.y());
        }
    }
    base = columns.encode();
    if (base.isEmpty() || !CMocDeformerOrder::Normalize(base, result.bytes))
        return { };
    return result;
}

QVector<float> cpuWitness(const QByteArray& bytes, const QString& id, const QVector<float>& input)
{
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    for (const auto* node : table.deformers) {
        if (node->ids != id)
            continue;
        const auto* warp = table.warp_deformers[node->specific_sources_indices];
        const auto values = Grid::positions(table, warp->keyform_sources_begin_indices, warp->vertex_counts);
        CWarpCoorType transform(
            warp->columns, warp->rows, std::vector<float>(values.cbegin(), values.cend()), warp->is_quad_source);
        auto source = input;
        QVector<float> result(input.size());
        if (!transform.transformLocalToCanvas(source.data(), result.data(), source.size() / 2, 0, 2))
            return { };
        for (int index = 1; index < result.size(); index += 2)
            result[index] = -result[index];
        return result;
    }
    return { };
}
}

class WarpGridStructureCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void sharedSpecificSourceRemainsValidInCore_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("blend");
        QTest::newRow("ordinary") << QString("Mark") << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            QTest::newRow("blend") << QString("Mao") << true;
    }
    void sharedSpecificSourceRemainsValidInCore()
    {
        QFETCH(QString, name);
        QFETCH(bool, blend);
        auto base = DeformerReplayFixture::read(name);
        auto initialReader = CReader::FromBytes(base);
        CSectionTable initial(initialReader);
        const int first = Grid::target(initial, blend);
        QVERIFY(first >= 0);
        int sibling = -1;
        for (int index = 0; index < initial.deformers.size(); ++index)
            if (index != first && initial.deformers[index]->types == 0) {
                sibling = index;
                break;
            }
        QVERIFY(sibling >= 0);
        const QString id = initial.deformers[first]->ids;
        DeformerReplayFixture::RawColumns shared(base);
        shared.set("deformers.specific_sources_indices", sibling, initial.deformers[first]->specific_sources_indices);
        shared.set("deformers.keyform_binding_sources_indices", sibling,
            initial.deformers[first]->keyform_binding_sources_indices);
        const auto prepared = shared.encode();
        QVERIFY(!prepared.isEmpty());
        QVERIFY(CMocDeformerOrder::Normalize(prepared, base));
        CoreModel original;
        QVERIFY2(original.load(base), "Shared-source fixture must pass real Core before testing the edit");
        auto reader = CReader::FromBytes(base);
        CSectionTable table(reader);
        int target = -1;
        for (int index = 0; index < table.deformers.size(); ++index)
            if (table.deformers[index]->ids == id)
                target = index;
        QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        Builder::Result resized;
        QString error;
        QVERIFY2(
            Builder::ResizeWarp(base, { id, warp->rows + 1, warp->columns + 1 }, resized, &error), qPrintable(error));
        QByteArray normalized;
        QVERIFY(CMocDeformerOrder::Normalize(resized.bytes, normalized));
        CoreModel candidate;
        QVERIFY(candidate.load(normalized));
        QCOMPARE(csmGetParameterCount(original.value), csmGetParameterCount(candidate.value));
        const auto affected = affectedMeshes(table, target);
        for (const auto& pose : poses(original.value)) {
            original.pose(pose);
            candidate.pose(pose);
            const auto before = frame(original.value), after = frame(candidate.value);
            QCOMPARE(after.definition, before.definition);
            for (int mesh = 0; mesh < before.values.size(); ++mesh) {
                if (affected.contains(mesh))
                    continue;
                QVERIFY(Grid::close(before.values[mesh], after.values[mesh]));
            }
        }
        QVERIFY(original.intact());
        QVERIFY(candidate.intact());
    }

    void actualModelsKeepBindingsAndUnaffectedCoreOutputs_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("blend");
        for (const QString name : { "Mark", "Rice", "Natori", "Wanko" })
            QTest::newRow(qPrintable(name)) << name << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            QTest::newRow("Mao-blend") << QString("Mao") << true;
    }
    void actualModelsKeepBindingsAndUnaffectedCoreOutputs()
    {
        QFETCH(QString, name);
        QFETCH(bool, blend);
        const auto base = DeformerReplayFixture::read(name);
        auto reader = CReader::FromBytes(base);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const int target = Grid::target(table, blend);
        QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        Builder::Result resized;
        QString error;
        QVERIFY2(Builder::ResizeWarp(
                     base, { table.deformers[target]->ids, warp->rows + 1, warp->columns + 2 }, resized, &error),
            qPrintable(error));
        QByteArray normalized;
        QVERIFY(CMocDeformerOrder::Normalize(resized.bytes, normalized, &error));
        CoreModel original, candidate;
        QVERIFY(original.load(base));
        QVERIFY(candidate.load(normalized));
        QCOMPARE(csmGetParameterCount(candidate.value), csmGetParameterCount(original.value));
        QCOMPARE(csmGetDrawableCount(candidate.value), csmGetDrawableCount(original.value));
        const int parameterCount = csmGetParameterCount(original.value);
        QCOMPARE(raw(csmGetParameterMinimumValues(candidate.value), parameterCount),
            raw(csmGetParameterMinimumValues(original.value), parameterCount));
        QCOMPARE(raw(csmGetParameterMaximumValues(candidate.value), parameterCount),
            raw(csmGetParameterMaximumValues(original.value), parameterCount));
        QCOMPARE(raw(csmGetParameterDefaultValues(candidate.value), parameterCount),
            raw(csmGetParameterDefaultValues(original.value), parameterCount));
        const auto affected = affectedMeshes(table, target);
        double maximum = 0;
        auto sequence = poses(original.value);
        for (int index = 0; index < csmGetParameterCount(original.value); ++index) {
            auto middle = sequence.first();
            middle[index] = (csmGetParameterMinimumValues(original.value)[index]
                                + csmGetParameterMaximumValues(original.value)[index])
                * .5f;
            sequence.append(std::move(middle));
        }
        for (const auto& pose : sequence) {
            original.pose(pose);
            candidate.pose(pose);
            const auto before = frame(original.value), after = frame(candidate.value);
            QCOMPARE(after.definition, before.definition);
            QCOMPARE(after.values.size(), before.values.size());
            for (int mesh = 0; mesh < before.values.size(); ++mesh) {
                QCOMPARE(after.values[mesh].size(), before.values[mesh].size());
                for (int component = 0; component < before.values[mesh].size(); ++component) {
                    const double a = before.values[mesh][component], b = after.values[mesh][component];
                    QVERIFY(std::isfinite(a) && std::isfinite(b));
                    if (affected.contains(mesh) && component < before.positionFields[mesh])
                        maximum = std::max(maximum, std::abs(a - b));
                    else
                        QVERIFY(std::abs(a - b) <= 1e-6 + 2e-6 * std::max(std::abs(a), std::abs(b)));
                }
            }
        }
        QVERIFY(original.intact());
        QVERIFY(candidate.intact());
        qInfo() << name << "poses" << sequence.size() << "affected meshes including Glue" << affected.size()
                << "maximum actual Core coordinate change" << maximum;
    }

    void interiorAndExteriorWitnessesMatchNewGridWithoutFalsePreservation_data()
    {
        QTest::addColumn<bool>("quad");
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("columns");
        QTest::addColumn<bool>("exactInterior");
        QTest::newRow("tri-equal-subdivision") << false << 4 << 4 << true;
        QTest::newRow("tri-anisotropic") << false << 4 << 6 << false;
        QTest::newRow("quad-anisotropic") << true << 4 << 6 << true;
        QTest::newRow("quad-reduction") << true << 1 << 1 << false;
    }
    void interiorAndExteriorWitnessesMatchNewGridWithoutFalsePreservation()
    {
        QFETCH(bool, quad);
        QFETCH(int, rows);
        QFETCH(int, columns);
        QFETCH(bool, exactInterior);
        const auto source = witness(quad);
        QVERIFY(!source.bytes.isEmpty());
        Builder::Result resized;
        QString error;
        QVERIFY2(
            Builder::ResizeWarp(source.bytes, { source.target, rows, columns }, resized, &error), qPrintable(error));
        QByteArray normalized;
        QVERIFY(CMocDeformerOrder::Normalize(resized.bytes, normalized, &error));
        CoreModel original, candidate;
        QVERIFY(original.load(source.bytes));
        QVERIFY(candidate.load(normalized));
        original.pose(poses(original.value).first());
        candidate.pose(poses(candidate.value).first());
        int mesh = -1;
        for (int index = 0; index < csmGetDrawableCount(original.value); ++index)
            if (QString::fromUtf8(csmGetDrawableIds(original.value)[index]) == source.mesh)
                mesh = index;
        QVERIFY(mesh >= 0);
        const auto before = cpuWitness(source.bytes, source.target, source.input);
        const auto after = cpuWitness(normalized, source.target, source.input);
        QCOMPARE(before.size(), source.input.size());
        QCOMPARE(after.size(), source.input.size());
        double inside = 0, outside = 0;
        const auto* a = csmGetDrawableVertexPositions(original.value)[mesh];
        const auto* b = csmGetDrawableVertexPositions(candidate.value)[mesh];
        for (int vertex = 0; vertex < source.input.size() / 2; ++vertex) {
            QVERIFY(std::abs(a[vertex].X - before[vertex * 2]) < .00002);
            QVERIFY(std::abs(a[vertex].Y - before[vertex * 2 + 1]) < .00002);
            QVERIFY(std::abs(b[vertex].X - after[vertex * 2]) < .00002);
            QVERIFY(std::abs(b[vertex].Y - after[vertex * 2 + 1]) < .00002);
            const double change = std::hypot(double(a[vertex].X) - b[vertex].X, double(a[vertex].Y) - b[vertex].Y);
            const double u = source.input[vertex * 2], v = source.input[vertex * 2 + 1];
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
                inside = std::max(inside, change);
            else
                outside = std::max(outside, change);
        }
        if (exactInterior)
            QVERIFY(inside < .00002);
        else
            QVERIFY(inside > .001);
        QVERIFY(outside > .001);
        QVERIFY(original.intact());
        QVERIFY(candidate.intact());
        qInfo() << "quad" << quad << "new grid" << rows << columns << "inside change" << inside << "outside change"
                << outside;
    }
};
QTEST_GUILESS_MAIN(WarpGridStructureCoreTests)
#include "warp_grid_structure_core_tests.moc"
