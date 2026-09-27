#include "live2d/cwarpgridpreview.h"
#include "warp_grid_fixture.h"
#include "deformer_snapshot_fixture.h"
#include "real_core_model_fixture.h"
#include "std/cexportcontrol.h"
#include <QMap>
#include <QSet>

using namespace live2dstruct;
namespace {
CWarpGridPreview::Request request(const QByteArray& before, const QByteArray& after)
{
    CWarpGridPreview::Request result; result.beforeMoc = before; result.afterMoc = after;
    CoreModel model;
    if (!model.load(before)) return result;
    for (int i = 0; i < csmGetParameterCount(model.value); ++i)
        result.parameters.insert(QString::fromUtf8(csmGetParameterIds(model.value)[i]), csmGetParameterDefaultValues(model.value)[i]);
    for (int i = 0; i < csmGetPartCount(model.value); ++i)
        result.partOpacities.insert(QString::fromUtf8(csmGetPartIds(model.value)[i]), csmGetPartOpacities(model.value)[i]);
    return result;
}
void pose(CoreModel& model, const CWarpGridPreview::Request& input)
{
    QVector<float> values;
    for (int i = 0; i < csmGetParameterCount(model.value); ++i) values.append(input.parameters.value(QString::fromUtf8(csmGetParameterIds(model.value)[i])));
    for (int i = 0; i < csmGetPartCount(model.value); ++i)
        csmGetPartOpacities(model.value)[i] = input.partOpacities.value(QString::fromUtf8(csmGetPartIds(model.value)[i]));
    model.pose(values);
}
QByteArray small(int indexCount = 3)
{
    auto bytes = DeformerReplayFixture::read("Mark");
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    if (!table.IsValid()) return {};
    const auto& o = *table.sectionOffsetTable;
    int target = -1;
    for (int i = 0; i < table.art_meshes.size(); ++i)
        if (table.art_meshes[i]->keyform_sources_counts == 1 && table.art_meshes[i]->vertex_counts >= 3 && table.art_meshes[i]->position_index_sources_counts >= 6) { target = i; break; }
    if (target < 0) return {};
    for (int i = 0; i < table.art_meshes.size(); ++i)
        DeformerSnapshotData::put(bytes, o.art_meshes->position_index_sources_counts + i * 4, i == target ? indexCount : 0);
    DeformerSnapshotData::put(bytes, o.art_meshes->parent_deformer_indices + target * 4, -1);
    DeformerSnapshotData::put(bytes, o.art_meshes->parent_part_indices + target * 4, -1);
    DeformerSnapshotData::put(bytes, o.art_meshes->is_visible + target * 4, 1);
    DeformerSnapshotData::put(bytes, o.art_meshes->is_enabled + target * 4, 1);
    const auto* mesh = table.art_meshes[target];
    DeformerSnapshotData::real(bytes, o.art_mesh_keyforms->opacities + mesh->keyform_sources_begin_indices * 4, 1);
    for (int i = 0; i < indexCount; ++i)
        qToLittleEndian(quint16(i % 3), bytes.data() + o.position_indices->indices + (mesh->position_index_sources_begin_indices + i) * 2);
    QByteArray normalized; CMocDeformerOrder::Normalize(bytes, normalized); return normalized;
}
int triangleMesh(csmModel* model)
{
    for (int i = 0; i < csmGetDrawableCount(model); ++i) if (csmGetDrawableIndexCounts(model)[i] > 0) return i;
    return -1;
}

CWarpGridPreview::Result sentinel()
{
    CWarpGridPreview::Result result;
    result.beforeContours = {QLineF(1, 2, 3, 4)}; result.afterContours = {QLineF(5, 6, 7, 8)};
    result.maximumDisplacementPixels = 17; result.changedVertices = 9; result.changedDrawables = {"keep"};
    result.closedMeshEdges = true; result.canvasPixels = {123, 456}; return result;
}
bool equal(const CWarpGridPreview::Result& a, const CWarpGridPreview::Result& b)
{
    return a.beforeContours == b.beforeContours && a.afterContours == b.afterContours &&
        a.maximumDisplacementPixels == b.maximumDisplacementPixels && a.changedVertices == b.changedVertices &&
        a.changedDrawables == b.changedDrawables && a.closedMeshEdges == b.closedMeshEdges && a.canvasPixels == b.canvasPixels;
}
}

class WarpGridPreviewCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void resizedCurrentPoseMatchesIndependentCore_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<bool>("blend");
        QTest::newRow("Mark") << QString("Mark") << false;
        QTest::newRow("Mao-Blend") << QString("Mao") << true;
    }
    void resizedCurrentPoseMatchesIndependentCore()
    {
        QFETCH(QString, model); QFETCH(bool, blend);
        const auto bytes = DeformerReplayFixture::read(model);
        if (bytes.isEmpty() && model == "Mao") QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao.moc3");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const int target = WarpGridFixture::target(table, blend); QVERIFY(target >= 0);
        const auto* warp = table.warp_deformers[table.deformers[target]->specific_sources_indices];
        CMocDeformerStructureBuilder::Result resized; QString error;
        QVERIFY2(CMocDeformerStructureBuilder::ResizeWarp(bytes, {table.deformers[target]->ids, warp->rows + 1, warp->columns + 2}, resized, &error), qPrintable(error));
        QByteArray before, after; QVERIFY(CMocDeformerOrder::Normalize(bytes, before)); QVERIFY(CMocDeformerOrder::Normalize(resized.bytes, after));
        auto input = request(before, after);
        CoreModel a, b; QVERIFY(a.load(before)); QVERIFY(b.load(after));
        for (int i = 0; i < csmGetParameterCount(a.value); ++i)
            input.parameters[QString::fromUtf8(csmGetParameterIds(a.value)[i])] = csmGetParameterMinimumValues(a.value)[i] * .37f + csmGetParameterMaximumValues(a.value)[i] * .63f;
        for (auto item = input.partOpacities.begin(); item != input.partOpacities.end(); ++item) item.value() = .63f;
        pose(a, input); pose(b, input); const auto original = frame(a.value), candidate = frame(b.value);
        CoreModel untouched; QVERIFY(untouched.load(before)); const auto otherPose = request(before, before); pose(untouched, otherPose);
        const auto held = frame(untouched.value);
        const auto heldParameters = raw(csmGetParameterValues(untouched.value), csmGetParameterCount(untouched.value));
        CWarpGridPreview::Result output; QVERIFY2(CWarpGridPreview::Build(input, output, &error), qPrintable(error));
        double maximum = 0; qsizetype count = 0; QStringList changed;
        for (int mesh = 0; mesh < csmGetDrawableCount(a.value); ++mesh) {
            bool any = false;
            for (int i = 0; i < original.positionFields[mesh]; i += 2) {
                const auto& x = original.values[mesh]; const auto& y = candidate.values[mesh];
                const double distance = std::sqrt(std::pow(double(x[i]) - y[i], 2) + std::pow(double(x[i + 1]) - y[i + 1], 2)) * original.pixelsPerUnit;
                if (distance != 0) { ++count; any = true; maximum = std::max(maximum, distance); }
            }
            if (any) changed.append(QString::fromUtf8(csmGetDrawableIds(a.value)[mesh]));
        }
        QCOMPARE(output.changedVertices, count); QCOMPARE(output.changedDrawables, changed);
        QVERIFY(std::abs(output.maximumDisplacementPixels - maximum) <= std::max(1.0, maximum) * 1e-12);
        QCOMPARE(output.beforeContours.size(), output.afterContours.size()); QVERIFY(!output.beforeContours.isEmpty());
        QCOMPARE(frame(a.value).definition, original.definition); QCOMPARE(frame(a.value).values, original.values);
        QCOMPARE(frame(b.value).values, candidate.values); QVERIFY(a.intact() && b.intact());
        QCOMPARE(input.beforeMoc, before); QCOMPARE(input.afterMoc, after);
        QCOMPARE(frame(untouched.value).definition, held.definition); QCOMPARE(frame(untouched.value).values, held.values);
        QCOMPARE(raw(csmGetParameterValues(untouched.value), csmGetParameterCount(untouched.value)), heldParameters);
        QVERIFY(untouched.intact());
        qInfo() << model << "changed vertices" << count << "maximum source pixels" << maximum;
    }
    void oneTriangleContoursUseOriginalPixelScaleAndYUp()
    {
        const auto bytes = small(); QVERIFY(!bytes.isEmpty());
        auto input = request(bytes, bytes);
        CoreModel model; QVERIFY(model.load(bytes)); pose(model, input);
        csmVector2 canvas, origin; float ppu; csmReadCanvasInfo(model.value, &canvas, &origin, &ppu);
        CWarpGridPreview::Result result; QString error; QVERIFY2(CWarpGridPreview::Build(input, result, &error), qPrintable(error));
        QCOMPARE(result.beforeContours.size(), 3); QCOMPARE(result.beforeContours, result.afterContours);
        QCOMPARE(result.changedVertices, qsizetype(0)); QCOMPARE(result.maximumDisplacementPixels, 0.0); QVERIFY(result.changedDrawables.isEmpty());
        QVERIFY(!result.closedMeshEdges); QCOMPARE(result.canvasPixels, QSizeF(canvas.X, canvas.Y));
        const int mesh = triangleMesh(model.value); QVERIFY(mesh >= 0);
        const auto* points = csmGetDrawableVertexPositions(model.value)[mesh];
        const auto vertex = [&](int index) { return QPointF(double(points[index].X) * ppu, double(points[index].Y) * ppu); };
        QCOMPARE(result.beforeContours, QVector<QLineF>({QLineF(vertex(0), vertex(1)), QLineF(vertex(0), vertex(2)), QLineF(vertex(1), vertex(2))}));
        input.parameters.insert("VirtualOrRetired", 31); input.partOpacities.insert("RetiredPart", .25f);
        CWarpGridPreview::Result extra; QVERIFY(CWarpGridPreview::Build(input, extra, &error)); QVERIFY(equal(extra, result));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const auto* source = table.art_meshes[mesh];
        const int position = table.art_mesh_keyforms[source->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
        DeformerSnapshotData::real(input.afterMoc, table.sectionOffsetTable->keyform_positions->xys + position * 4,
                                  table.keyform_positions->GetData(position) + .125f);
        CoreModel modified; QVERIFY(modified.load(input.afterMoc)); pose(modified, input);
        QVERIFY2(CWarpGridPreview::Build(input, extra, &error), qPrintable(error));
        const auto* newPoints = csmGetDrawableVertexPositions(modified.value)[mesh];
        const auto newVertex = [&](int index) { return QPointF(double(newPoints[index].X) * ppu, double(newPoints[index].Y) * ppu); };
        QCOMPARE(extra.beforeContours, result.beforeContours);
        QCOMPARE(extra.afterContours, QVector<QLineF>({QLineF(newVertex(0), newVertex(1)), QLineF(newVertex(0), newVertex(2)), QLineF(newVertex(1), newVertex(2))}));
        QVERIFY(extra.afterContours != extra.beforeContours); QVERIFY(extra.changedVertices > 0); QVERIFY(extra.maximumDisplacementPixels > 0);
    }
    void closedEdgesAndIsolatedVertices()
    {
        const auto bytes = small(6); CoreModel baseline; QVERIFY(baseline.load(bytes));
        auto input = request(bytes, bytes); CWarpGridPreview::Result result; QString error;
        QVERIFY2(CWarpGridPreview::Build(input, result, &error), qPrintable(error));
        QVERIFY(result.closedMeshEdges); QCOMPARE(result.beforeContours.size(), 3);
        const int meshIndex = triangleMesh(baseline.value); QVERIFY(meshIndex >= 0);
        input = request(small(0), small(0));
        auto reader = CReader::FromBytes(input.afterMoc); CSectionTable table(reader);
        const auto* mesh = table.art_meshes[meshIndex];
        const int first = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
        const int position = table.sectionOffsetTable->keyform_positions->xys + first * 4;
        DeformerSnapshotData::real(input.afterMoc, position, table.keyform_positions->GetData(first) + .2f);
        QVERIFY2(CWarpGridPreview::Build(input, result, &error), qPrintable(error));
        QVERIFY(result.beforeContours.isEmpty()); QVERIFY(result.afterContours.isEmpty()); QVERIFY(!result.closedMeshEdges);
        QVERIFY(result.changedVertices > 0); QVERIFY(result.maximumDisplacementPixels > 0);
    }

    void partSnapshotsAndDuplicateIdsAreChecked_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"missing", "nan", "duplicate"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void partSnapshotsAndDuplicateIdsAreChecked()
    {
        QFETCH(QString, kind);
        const auto bytes = DeformerReplayFixture::read("Mark"); auto input = request(bytes, bytes);
        QVERIFY(input.partOpacities.size() > 1);
        if (kind == "missing") input.partOpacities.remove(input.partOpacities.firstKey());
        if (kind == "nan") input.partOpacities.first() = std::numeric_limits<float>::quiet_NaN();
        if (kind == "duplicate") {
            auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
            const int offset = table.sectionOffsetTable->parts->ids;
            input.beforeMoc.replace(offset + 64, 64, input.beforeMoc.mid(offset, 64)); input.afterMoc = input.beforeMoc;
        }
        auto output = sentinel(); const auto saved = output; QString error;
        QVERIFY(!CWarpGridPreview::Build(input, output, &error)); QVERIFY(equal(saved, output));
    }
    void failuresPreserveOutput_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"empty", "invalid-moc", "missing-parameter", "unknown-only", "nan-pose", "bad-id", "uv", "indices", "position-nan", "parameter-default", "opacity", "texture", "id", "ppu", "moc-limit", "memory-limit", "vertex-limit", "index-limit", "segment-limit", "work-limit", "object-limit", "raised-limit"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void failuresPreserveOutput()
    {
        QFETCH(QString, kind);
        const auto bytes = small(); CoreModel baseline; QVERIFY(baseline.load(bytes));
        auto input = request(bytes, bytes); CWarpGridPreview::Result valid; QString baselineError;
        QVERIFY2(CWarpGridPreview::Build(input, valid, &baselineError), qPrintable(baselineError));
        if (kind == "empty") input.beforeMoc.clear();
        if (kind == "invalid-moc") input.afterMoc[0] = 'X';
        if (kind == "missing-parameter") input.parameters.remove(input.parameters.firstKey());
        if (kind == "unknown-only") input.parameters = {{"Unknown", .3f}};
        if (kind == "nan-pose") input.parameters.first() = std::numeric_limits<float>::quiet_NaN();
        if (kind == "bad-id") input.parameters.insert(QString("bad") + QChar::Null, 1);
        if (kind == "moc-limit") input.limits.maximumMocBytes = bytes.size();
        if (kind == "memory-limit") input.limits.maximumMemoryBytes = 1;
        if (kind == "vertex-limit") input.limits.maximumVertices = 2;
        if (kind == "index-limit") input.limits.maximumIndices = 2;
        if (kind == "segment-limit") input.limits.maximumSegments = 2;
        if (kind == "work-limit") input.limits.maximumWork = 1;
        if (kind == "object-limit") input.limits.maximumObjects = 0;
        if (kind == "raised-limit") ++input.limits.maximumVertices;
        auto reader = CReader::FromBytes(input.afterMoc); CSectionTable table(reader);
        if (table.IsValid()) {
            const auto& o = *table.sectionOffsetTable;
            const int index = triangleMesh(baseline.value); QVERIFY(index >= 0); const auto* mesh = table.art_meshes[index];
            if (kind == "uv") DeformerSnapshotData::real(input.afterMoc, o.uvs->uvs + mesh->uv_sources_begin_indices * 4, .37f);
            if (kind == "position-nan") DeformerSnapshotData::real(input.afterMoc, o.keyform_positions->xys + table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]->keyform_position_sources_begin_indices * 4, std::numeric_limits<float>::quiet_NaN());
            if (kind == "parameter-default") DeformerSnapshotData::real(input.afterMoc, o.parameters->default_values,
                table.parameters.first()->min_values * .75f + table.parameters.first()->max_values * .25f);
            if (kind == "indices") { const int start = o.position_indices->indices + mesh->position_index_sources_begin_indices * 2; qToLittleEndian(quint16(2), input.afterMoc.data() + start); qToLittleEndian(quint16(0), input.afterMoc.data() + start + 4); }
            if (kind == "opacity") DeformerSnapshotData::real(input.afterMoc, o.art_mesh_keyforms->opacities + mesh->keyform_sources_begin_indices * 4, .5f);
            if (kind == "texture") DeformerSnapshotData::put(input.afterMoc, o.art_meshes->texture_nos + index * 4, mesh->texture_nos + 1);
            if (kind == "id") input.afterMoc.replace(o.art_meshes->ids + index * 64, 64, QByteArray("AnotherMesh").leftJustified(64, '\0'));
            if (kind == "ppu") DeformerSnapshotData::real(input.afterMoc, o.canvas_info->CanvasOffset, table.canvas_info->pixels_per_unit * 2);
        }
        auto output = sentinel(); const auto expected = output; QString error;
        QVERIFY(!CWarpGridPreview::Build(input, output, &error)); QVERIFY(!error.isEmpty()); QVERIFY(equal(output, expected));
    }
    void cancelAndBorrowedInputs()
    {
        const auto bytes = small(); auto input = request(bytes, bytes);
        QByteArray rawBefore = bytes, rawAfter = bytes;
        const auto id = input.parameters.firstKey();
        QVector<QChar> rawId(id.cbegin(), id.cend());
        input.beforeMoc = QByteArray::fromRawData(rawBefore.data(), rawBefore.size());
        input.afterMoc = QByteArray::fromRawData(rawAfter.data(), rawAfter.size());
        const auto value = input.parameters.take(id);
        input.parameters.insert(QString::fromRawData(rawId.data(), rawId.size()), value);
        CWarpGridPreview::Result expected; QString error; QVERIFY(CWarpGridPreview::Build(input, expected, &error));
        CExportControl control; bool modified = false;
        QVERIFY(control.SetHandler([&](const auto&, QString*) { if (!modified) { rawBefore.fill('x'); rawAfter.fill('y'); rawId[0] = 'Q'; input.parameters.clear(); input.limits.maximumWork = 1; modified = true; } return true; }));
        CWarpGridPreview::Result actual; QVERIFY2(CWarpGridPreview::Build(input, actual, &error, &control), qPrintable(error)); QVERIFY(modified); QVERIFY(equal(expected, actual));
        input = request(bytes, bytes);
        for (int stop : {0, 1, 2, 3, 4, 5, 6}) {
            CExportControl canceled; int calls = 0;
            QVERIFY(canceled.SetHandler([&](const auto&, QString*) { return calls++ != stop; }));
            auto output = sentinel(); const auto saved = output;
            QVERIFY(!CWarpGridPreview::Build(input, output, &error, &canceled)); QVERIFY(equal(saved, output));
        }
    }
};
QTEST_APPLESS_MAIN(WarpGridPreviewCoreTests)
#include "warp_grid_preview_core_tests.moc"
