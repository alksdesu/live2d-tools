#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/cblendshaperelations.h"
#include "deformer_order_fixture.h"
#include "real_core_model_fixture.h"
#include <numeric>

using namespace live2dstruct;
namespace Fixture = DeformerOrderFixture;
namespace {
int drawableIndex(csmModel* model, const QString& id)
{
    for (int index = 0; index < csmGetDrawableCount(model); ++index)
        if (QString::fromUtf8(csmGetDrawableIds(model)[index]) == id) return index;
    return -1;
}
}

class DeformerOrderMocCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void reversedHierarchyMatchesSortedSourceEachFrame_data()
    {
        QTest::addColumn<QString>("path");
        for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void reversedHierarchyMatchesSortedSourceEachFrame()
    {
        QFETCH(QString, path);
        const auto original = Fixture::read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QSet<int> edgeTypes;
        for (int index = 0; index < table.deformers.size(); ++index) {
            const auto* child = table.deformers[index];
            QVERIFY(child->parent_deformer_indices < index);
            if (child->parent_deformer_indices >= 0)
                edgeTypes.insert(table.deformers[child->parent_deformer_indices]->types * 2 + child->types);
        }
        QVERIFY(!edgeTypes.isEmpty());
        if (QFileInfo(path).baseName() == "Mark") QCOMPARE(edgeTypes.size(), 4);
        const auto reversed = Fixture::permute(original, Fixture::reverseOrder(table)); QVERIFY(!reversed.isEmpty());
        CoreModel acceptedSource;
        QVERIFY(acceptedSource.load(reversed));
        QByteArray normalized; QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(reversed, normalized, &error), qPrintable(error));
        QCOMPARE(Fixture::semanticDifference(reversed, normalized), QString());
        CoreModel expected, actual;
        QVERIFY(expected.load(original)); QVERIFY(actual.load(normalized));
        const auto sequence = poses(expected.value);
        for (const auto& pose : sequence) {
            expected.pose(pose); actual.pose(pose);
            const auto message = difference(frame(expected.value), frame(actual.value));
            QVERIFY2(message.isEmpty(), qPrintable(message));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact()); QVERIFY(acceptedSource.intact());
        if (table.sectionOffsetTable->mocVersion >= 5) {
            QVERIFY(!table.blend_shapes_warp_deformers.isEmpty());
            QVERIFY(!table.blend_shapes_art_meshes.isEmpty());
            QVERIFY(!table.glue.isEmpty());
            auto nextReader = CReader::FromBytes(normalized); CSectionTable next(nextReader); QVERIFY(next.IsValid());
            CBlendShapeRelations before, after;
            QVERIFY2(before.Build(table, &error), qPrintable(error));
            QVERIFY2(after.Build(next, &error), qPrintable(error));
            QCOMPARE(before.BlendShapeParameterIndices(), after.BlendShapeParameterIndices());
        }
        QCOMPARE(Fixture::read(path), original);
    }

    void forwardRiceParentMatchesIndependentAdjacentOrder()
    {
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Rice/Rice.moc3");
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        int child = -1, parent = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (table.deformers[index]->ids == "Rotation29") child = index;
            if (table.deformers[index]->ids == "Rotation43") parent = index;
        }
        QVERIFY(child >= 0); QCOMPARE(parent, child + 1);
        QCOMPARE(table.deformers[child]->parent_part_indices, table.deformers[parent]->parent_part_indices);
        auto source = original;
        Fixture::put(source, table.sectionOffsetTable->deformers->parent_deformer_indices + child * 4, parent);
        QVector<int> adjacent(table.deformers.size()); std::iota(adjacent.begin(), adjacent.end(), 0);
        std::swap(adjacent[child], adjacent[parent]);
        const auto oracle = Fixture::permute(source, adjacent); QVERIFY(!oracle.isEmpty());
        QByteArray result; QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(source, result, &error), qPrintable(error));
        CoreModel unsorted, expected, actual;
        QVERIFY(unsorted.load(source)); QVERIFY(expected.load(oracle)); QVERIFY(actual.load(result));
        const int target = drawableIndex(actual.value, "ArtMesh182"); QVERIFY(target >= 0);
        int sizeParameter = -1;
        for (int index = 0; index < csmGetParameterCount(actual.value); ++index)
            if (QByteArray(csmGetParameterIds(actual.value)[index]) == "ParamMagicPowersBSize") sizeParameter = index;
        QVERIFY(sizeParameter >= 0);
        QVector<float> pose(csmGetParameterDefaultValues(actual.value), csmGetParameterDefaultValues(actual.value) + csmGetParameterCount(actual.value));
        bool observedLag = false, observedSettledMismatch = false;
        for (float size : {1.0f, 0.0f, 1.0f, 0.0f}) {
            pose[sizeParameter] = size;
            unsorted.pose(pose); expected.pose(pose); actual.pose(pose);
            const auto first = frame(unsorted.value), correct = frame(expected.value);
            QVERIFY2(difference(correct, frame(actual.value)).isEmpty(), qPrintable(difference(correct, frame(actual.value))));
            for (int step = 0; step < 3; ++step) { unsorted.pose(pose); expected.pose(pose); actual.pose(pose); }
            const auto settled = frame(unsorted.value);
            observedLag |= changed(first, settled).contains(target);
            observedSettledMismatch |= changed(correct, settled).contains(target);
            QVERIFY(difference(correct, frame(expected.value)).isEmpty());
            QVERIFY(difference(correct, frame(actual.value)).isEmpty());
        }
        QVERIFY(observedLag); QVERIFY(observedSettledMismatch);
        QVERIFY(unsorted.intact()); QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void reviveTransformsAndUnsafeAliasesAreKeptSeparate_data()
    {
        QTest::addColumn<bool>("uv");
        QTest::newRow("uv") << true;
        QTest::newRow("ebo") << false;
    }
    void reviveTransformsAndUnsafeAliasesAreKeptSeparate()
    {
        QFETCH(bool, uv);
        const auto original = Fixture::read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        const auto visible = std::find_if(spans.cbegin(), spans.cend(), [](const auto& span) { return QString::fromLatin1(span.name) == "deformers.is_visible"; });
        QVERIFY(visible != spans.cend());
        const auto* mesh = table.art_meshes.front();
        const int offset = uv ? table.sectionOffsetTable->uvs->uvs + mesh->uv_sources_begin_indices * 4
                             : table.sectionOffsetTable->position_indices->indices + mesh->position_index_sources_begin_indices * 2;
        std::vector<unsigned char> originalMemory(original.size() + csmAlignofMoc + 128, 0xa5);
        auto* originalAligned = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(originalMemory.data()) + csmAlignofMoc - 1) & ~(uintptr_t(csmAlignofMoc) - 1));
        std::memcpy(originalAligned, original.constData(), original.size());
        QVERIFY(csmHasMocConsistency(originalAligned, original.size()));
        std::memcpy(originalAligned, original.constData(), original.size());
        QVERIFY(csmReviveMocInPlace(originalAligned, original.size()));
        QVERIFY(std::memcmp(originalAligned + offset, original.constData() + offset, table.deformers.size() * 4) != 0);
        for (bool ordered : {true, false}) {
            auto source = ordered ? original : Fixture::permute(original, Fixture::reverseOrder(table));
            Fixture::put(source, visible->slot, offset);
            std::vector<unsigned char> allocation(source.size() + csmAlignofMoc + 128, 0xa5);
            auto* aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(allocation.data()) + csmAlignofMoc - 1) & ~(uintptr_t(csmAlignofMoc) - 1));
            std::memcpy(aligned, source.constData(), source.size());
            QVERIFY(!csmHasMocConsistency(aligned, source.size()));
            QByteArray result("keep"); QString error;
            QCOMPARE(CMocDeformerOrder::Normalize(source, result, &error), ordered);
            if (ordered) QCOMPARE(result, source);
            else { QCOMPARE(result, QByteArray("keep")); QVERIFY(error.contains(uv ? QStringLiteral("UV") : QStringLiteral("三角索引"))); }
        }
    }

    void reservedRuntimeRowsHaveEightByteStorage_data() { reversedHierarchyMatchesSortedSourceEachFrame_data(); }
    void reservedRuntimeRowsHaveEightByteStorage()
    {
        QFETCH(QString, path);
        const auto original = Fixture::read(path);
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        const int count = table.deformers.size(), offset = table.sectionOffsetTable->deformers->runtime_space_0;
        QVERIFY(count > 0);
        QCOMPARE(table.sectionOffsetTable->deformers->ids - offset, count * 8);
        std::vector<unsigned char> allocation(original.size() + csmAlignofMoc + 128, 0xa5);
        auto* aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(allocation.data()) + csmAlignofMoc - 1) & ~(uintptr_t(csmAlignofMoc) - 1));
        std::memcpy(aligned, original.constData(), original.size());
        QVERIFY(csmHasMocConsistency(aligned, original.size()));
        std::memcpy(aligned, original.constData(), original.size());
        QVERIFY(csmReviveMocInPlace(aligned, original.size()));
        QVERIFY(std::memcmp(aligned + offset + count * 4, original.constData() + offset + count * 4, count * 4) != 0);
        QVERIFY(std::all_of(aligned + original.size(), aligned + original.size() + 128, [](unsigned char value) { return value == 0xa5; }));
    }
};

QTEST_GUILESS_MAIN(DeformerOrderMocCoreTests)
#include "deformer_order_moc_core_tests.moc"
