#include "live2dstruct/cmocmaterialbuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "real_core_model_fixture.h"
#include <QtEndian>
using namespace live2dstruct;
namespace {
int drawable(csmModel* model, const QString& id)
{
    for (int index = 0; index < csmGetDrawableCount(model); ++index)
        if (QString::fromUtf8(csmGetDrawableIds(model)[index]) == id) return index;
    return -1;
}
void writeFloat(QByteArray& bytes, int offset, float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, bytes.data() + offset);
}
QByteArray uvBytes(const CModelDrawableMaterial& material)
{
    QByteArray bytes;
    for (int index = 0; index < material.uvs.size(); ++index) {
        float value = index % 2 ? 1.0f - material.uvs[index] : material.uvs[index];
        bytes.append(reinterpret_cast<const char*>(&value), 4);
    }
    return bytes;
}
}
class MocMaterialCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void modelMaterialsReachCoreWithoutChangingGeometry_data()
    {
        QTest::addColumn<QString>("path"); for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void modelMaterialsReachCoreWithoutChangingGeometry()
    {
        QFETCH(QString, path); const auto bytes = read(path); CModelMaterialOverrides source; QString error;
        QVERIFY2(CMocMaterialBuilder::Capture(bytes, source, &error), qPrintable(error)); QVERIFY(!source.isEmpty());
        const auto before = source.first(); auto changed = before; changed.textureIndex = 7;
        changed.uvs[0] = -.125f; changed.uvs[1] = 1.25f;
        CModelMaterialOverrides edits{{changed.id, changed}}; QByteArray output;
        QVERIFY2(CMocMaterialBuilder::Build(bytes, edits, output, &error), qPrintable(error));
        QVERIFY(CMocMaterialBuilder::ValidateTransition(bytes, output, {changed.id}, &error));
        CoreModel original, actual; QVERIFY(original.load(bytes)); QVERIFY(actual.load(output));
        const int target = drawable(original.value, changed.id); QVERIFY(target >= 0);
        QCOMPARE(csmGetDrawableTextureIndices(actual.value)[target], 7);
        QCOMPARE(raw(csmGetDrawableVertexUvs(actual.value)[target], changed.uvs.size() / 2), uvBytes(changed));
        for (const auto& pose : poses(original.value)) {
            original.pose(pose); actual.pose(pose);
            for (int mesh = 0; mesh < csmGetDrawableCount(original.value); ++mesh) {
                const int count = csmGetDrawableVertexCounts(original.value)[mesh]; QCOMPARE(csmGetDrawableVertexCounts(actual.value)[mesh], count);
                QCOMPARE(raw(csmGetDrawableVertexPositions(actual.value)[mesh], count), raw(csmGetDrawableVertexPositions(original.value)[mesh], count));
                QCOMPARE(raw(csmGetDrawableIndices(actual.value)[mesh], csmGetDrawableIndexCounts(actual.value)[mesh]), raw(csmGetDrawableIndices(original.value)[mesh], csmGetDrawableIndexCounts(original.value)[mesh]));
                QCOMPARE(csmGetDrawableOpacities(actual.value)[mesh], csmGetDrawableOpacities(original.value)[mesh]);
                QCOMPARE(csmGetDrawableDrawOrders(actual.value)[mesh], csmGetDrawableDrawOrders(original.value)[mesh]);
                if (mesh != target) {
                    QCOMPARE(csmGetDrawableTextureIndices(actual.value)[mesh], csmGetDrawableTextureIndices(original.value)[mesh]);
                    QCOMPARE(raw(csmGetDrawableVertexUvs(actual.value)[mesh], count), raw(csmGetDrawableVertexUvs(original.value)[mesh], count));
                }
            }
        }
        QVERIFY(original.intact()); QVERIFY(actual.intact());
    }
    void aliasedUvComponentsPreserveUntouchedCoreValues_data()
    {
        QTest::addColumn<int>("shift"); QTest::addColumn<bool>("third"); QTest::addColumn<bool>("tiny");
        for (int shift : {0, 1, 2}) for (bool third : {false, true}) for (bool tiny : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(shift).arg(third).arg(tiny))) << shift << third << tiny;
    }
    void aliasedUvComponentsPreserveUntouchedCoreValues()
    {
        QFETCH(int, shift); QFETCH(bool, third); QFETCH(bool, tiny);
        auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVector<int> meshes;
        for (int index = 0; index < table.art_meshes.size() && meshes.size() < 3; ++index) if (table.art_meshes[index]->vertex_counts >= 3) meshes.append(index);
        QCOMPARE(meshes.size(), 3); const auto* first = table.art_meshes[meshes[0]]; const auto* offsets = table.sectionOffsetTable->art_meshes;
        for (int index = 1; index <= (third ? 2 : 1); ++index) {
            qToLittleEndian(qint32(first->uv_sources_begin_indices + shift * index), bytes.data() + offsets->uv_sources_begin_indices + meshes[index] * 4);
            qToLittleEndian(qint32(3), bytes.data() + offsets->vertex_counts + meshes[index] * 4);
        }
        if (tiny) {
            for (int index = 0; index < 10; ++index)
                writeFloat(bytes, table.sectionOffsetTable->uvs->uvs + (first->uv_sources_begin_indices + index) * 4,
                           index % 3 == 0 ? -0.0f : index % 3 == 1 ? std::numeric_limits<float>::denorm_min() : -.125f);
        }
        const auto immutable = bytes; CoreModel original; QVERIFY(original.load(bytes));
        CModelDrawableMaterial changed; QString error; QVERIFY2(CMocMaterialBuilder::Capture(bytes, first->ids, changed, &error), qPrintable(error));
        changed.uvs[0] = .8125f; changed.uvs[1] = .15625f; changed.textureIndex = 2;
        QByteArray output; QVERIFY2(CMocMaterialBuilder::Build(bytes, {{changed.id, changed}}, output, &error), qPrintable(error));
        CoreModel actual; QVERIFY(actual.load(output)); const int target = drawable(original.value, changed.id); QVERIFY(target >= 0);
        for (int mesh = 0; mesh < csmGetDrawableCount(original.value); ++mesh) {
            const int count = csmGetDrawableVertexCounts(original.value)[mesh]; QCOMPARE(csmGetDrawableVertexCounts(actual.value)[mesh], count);
            if (mesh == target) QCOMPARE(raw(csmGetDrawableVertexUvs(actual.value)[mesh], count), uvBytes(changed));
            else QCOMPARE(raw(csmGetDrawableVertexUvs(actual.value)[mesh], count), raw(csmGetDrawableVertexUvs(original.value)[mesh], count));
        }
        QCOMPARE(bytes, immutable); QVERIFY(original.intact()); QVERIFY(actual.intact());
    }
    void transitionRejectsUnrelatedKeyformsAndAllowedSetMismatch()
    {
        const auto base = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); CModelMaterialOverrides all; QVERIFY(CMocMaterialBuilder::Capture(base, all));
        auto changed = all.first(); changed.uvs[0] += .25f; QByteArray material; QString error;
        QVERIFY(CMocMaterialBuilder::Build(base, {{changed.id, changed}}, material, &error));
        QVERIFY(!CMocMaterialBuilder::ValidateTransition(base, material, {"unknown"}, &error));
        auto reader = CReader::FromBytes(material); CSectionTable table(reader); QVERIFY(table.IsValid()); auto tampered = material;
        writeFloat(tampered, table.sectionOffsetTable->keyform_positions->xys, table.keyform_positions->xys[0] + .1f);
        QVERIFY(!CMocMaterialBuilder::ValidateTransition(base, tampered, {changed.id}, &error));
        QVERIFY(CMocMaterialBuilder::ValidateTransition(base, material, {changed.id}, &error));
    }
    void noOpAndTextureOnlyPreserveUvStorage()
    {
        const auto base = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); CModelMaterialOverrides all; QString error;
        QVERIFY(CMocMaterialBuilder::ValidateTransition(base, base, {}, &error));
        QVERIFY(CMocMaterialBuilder::Capture(base, all, &error)); QByteArray output;
        QVERIFY2(CMocMaterialBuilder::Build(base, all, output, &error), qPrintable(error)); QCOMPARE(output, base);
        auto changed = all.first(); changed.textureIndex += 1;
        QVERIFY(CMocMaterialBuilder::Build(base, {{changed.id, changed}}, output, &error));
        QVERIFY(!CMocMaterialBuilder::ValidateTransition(base, output, {}, &error));
        CMocParameterStructureBuilder::ColumnState differences;
        QVERIFY(CMocParameterStructureBuilder::CaptureColumnState(base, output, differences, &error));
        QCOMPARE(differences.columns.size(), 1); QVERIFY(differences.columns.contains(0xe4)); QVERIFY(differences.counts.isEmpty());
        const auto peer = std::next(all.cbegin())->id;
        QVERIFY(!CMocMaterialBuilder::ValidateTransition(base, output, {peer}, &error));
    }
    void emptyMeshWithoutTextureDoesNotBlockMaterialTransactions()
    {
        auto base = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); auto reader = CReader::FromBytes(base); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto* offsets = table.sectionOffsetTable->art_meshes;
        qToLittleEndian(qint32(0), base.data() + offsets->vertex_counts);
        qToLittleEndian(qint32(0), base.data() + offsets->position_index_sources_counts);
        qToLittleEndian(qint32(-1), base.data() + offsets->texture_nos);
        CModelMaterialOverrides all; QString error; QVERIFY(CMocMaterialBuilder::Capture(base, all, &error));
        QByteArray output; QVERIFY2(CMocMaterialBuilder::Build(base, all, output, &error), qPrintable(error)); QCOMPARE(output, base);
        QVERIFY2(CMocMaterialBuilder::ValidateTransition(base, base, {}, &error), qPrintable(error));
        const auto empty = all.value(table.art_meshes[0]->ids); QVERIFY(empty.uvs.isEmpty()); QCOMPARE(empty.textureIndex, -1);
        QJsonObject json; CModelMaterialOverrides restored;
        QVERIFY(CModelMaterialEdit::ToJson({{empty.id, empty}}, json, &error));
        QVERIFY(CModelMaterialEdit::FromJson(json, restored, &error)); QCOMPARE(restored.value(empty.id), empty);
        auto changed = all.value(table.art_meshes[1]->ids); changed.uvs[0] += .125f;
        QVERIFY2(CMocMaterialBuilder::Build(base, {{changed.id, changed}}, output, &error), qPrintable(error));
        CModelMaterialOverrides compiled;
        QVERIFY(CMocMaterialBuilder::Capture(output, compiled, &error)); QCOMPARE(compiled.value(empty.id), empty);
        QCOMPARE(compiled.value(changed.id), changed);
    }
    void physicalColumnAliasRejectsWithoutChangingOutput()
    {
        auto base = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); auto reader = CReader::FromBytes(base); CSectionTable table(reader); QVERIFY(table.IsValid());
        qToLittleEndian(qint32(table.sectionOffsetTable->keyform_positions->xys), base.data() + 0x178);
        CModelMaterialOverrides all; QString error; QVERIFY(CMocMaterialBuilder::Capture(base, all, &error));
        auto changed = all.first(); changed.uvs[0] += .125f; QByteArray output("sentinel");
        QVERIFY(!CMocMaterialBuilder::Build(base, {{changed.id, changed}}, output, &error));
        QCOMPARE(output, QByteArray("sentinel")); QVERIFY(!error.isEmpty());
    }
    void malformedBatchKeepsOutput()
    {
        const auto base = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"); CModelMaterialOverrides all; QVERIFY(CMocMaterialBuilder::Capture(base, all));
        auto changed = all.first(); changed.uvs[0] += .25f; auto invalid = all.last(); invalid.uvs.clear();
        QByteArray output("sentinel"); QString error;
        QVERIFY(!CMocMaterialBuilder::Build(base, {{changed.id, changed}, {invalid.id, invalid}}, output, &error));
        QCOMPARE(output, QByteArray("sentinel")); QVERIFY(!error.isEmpty());
    }
};
QTEST_GUILESS_MAIN(MocMaterialCoreTests)
#include "material_moc_core_tests.moc"
