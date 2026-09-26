#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <limits>
#ifdef LIVE2D_TEST_CORE
#include "real_core_model_fixture.h"
#endif

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;
using Span = CSectionValidation::ColumnSpan;

namespace {
QByteArray modelBytes(const QString& name)
{
    QFile file(name == QStringLiteral("external-moc") ? qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC")
                                                     : QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QStringList modelNames()
{
    QStringList result{QStringLiteral("Mark"), QStringLiteral("Rice"), QStringLiteral("Natori"), QStringLiteral("Wanko")};
    if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty()) result.append(QStringLiteral("external-moc"));
    return result;
}
void put(QByteArray& bytes, qint64 offset, int value)
{
    qToLittleEndian(qint32(value), bytes.data() + offset);
}
Span named(const CSectionTable& table, const char* name)
{
    for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
        if (std::strcmp(span.name, name) == 0) return span;
    return {nullptr, 0, 0, 0, false, -1, -1};
}
QByteArray column(const QByteArray& bytes, const Span& span)
{
    return span.offset ? bytes.mid(span.offset, span.count * CSectionValidation::StorageWidth(span)) : QByteArray();
}
bool resizeGeneric(const QByteArray& source, int count, QByteArray& result, QString* error)
{
    auto reader = CReader::FromBytes(source);
    CSectionTable table(reader);
    if (!table.IsValid() || count < 1 || count > table.deformers.size() + 1) return false;
    Builder::ColumnState state;
    state.counts.insert(1, count);
    for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        if (span.countIndex != 1) continue;
        const auto width = CSectionValidation::StorageWidth(span);
        auto data = column(source, span);
        if (count > span.count) {
            auto row = data.left(width);
            if (QByteArray(span.name) == "deformers.ids") row = QByteArray("ColumnStateProbe").leftJustified(64, '\0');
            data.append(row);
        } else data.truncate(count * width);
        state.columns.insert(span.slot, data);
    }
    return Builder::BuildColumnState(source, state, result, error);
}
Builder::ColumnState sentinel()
{
    return {{{0x68, QByteArray("keep state")}}, {{1, 999}}};
}
}

class DeformerColumnStateTests : public QObject
{
    Q_OBJECT
private slots:
    void noOpRetainsCompleteSource_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : modelNames()) QTest::newRow(qPrintable(name)) << name;
    }
    void noOpRetainsCompleteSource()
    {
        QFETCH(QString, name);
        const auto bytes = modelBytes(name);
        QVERIFY(!bytes.isEmpty());
        Builder::ColumnState state = sentinel();
        QString error;
        QVERIFY2(Builder::CaptureColumnState(bytes, bytes, state, &error), qPrintable(error));
        QVERIFY(state.columns.isEmpty() && state.counts.isEmpty());
        QByteArray rebuilt;
        QVERIFY2(Builder::BuildColumnState(bytes, state, rebuilt, &error), qPrintable(error));
        QCOMPARE(rebuilt, bytes);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            const bool runtime = QByteArray(span.name).contains("runtime_space");
            QCOMPARE(CSectionValidation::StorageWidth(span), runtime ? qint64(8) : span.width);
            if (runtime) QCOMPARE(span.width, qint64(4));
        }
    }

    void upperStorageBytesRoundTrip_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("field");
        for (const QString& name : modelNames()) {
            auto reader = CReader::FromBytes(modelBytes(name)); CSectionTable table(reader);
            QVERIFY(table.IsValid());
            for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
                if (QByteArray(span.name).contains("runtime_space") && span.offset && span.count)
                    QTest::newRow(qPrintable(name + '/' + span.name)) << name << QByteArray(span.name);
        }
    }
    void upperStorageBytesRoundTrip()
    {
        QFETCH(QString, name); QFETCH(QByteArray, field);
        const auto bytes = modelBytes(name);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const auto scratch = named(table, field.constData());
        QVERIFY(scratch.slot >= 0 && scratch.count > 0);
        auto edited = bytes;
        edited[scratch.offset + scratch.count * 8 - 1] = char(0x6d);
        Builder::ColumnState state;
        QString error;
        QVERIFY2(Builder::CaptureColumnState(bytes, edited, state, &error), qPrintable(error));
        QCOMPARE(state.columns.size(), 1);
        QCOMPARE(state.columns.value(scratch.slot).size(), scratch.count * 8);
        QCOMPARE(state.columns.value(scratch.slot), column(edited, scratch));
        QByteArray rebuilt;
        QVERIFY2(Builder::BuildColumnState(bytes, state, rebuilt, &error), qPrintable(error));
        auto rebuiltReader = CReader::FromBytes(rebuilt); CSectionTable after(rebuiltReader);
        QVERIFY(after.IsValid());
        for (const auto& original : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            QCOMPARE(column(rebuilt, named(after, original.name)), column(edited, original));
        QCOMPARE(modelBytes(name), bytes);
    }

    void genericRowGrowthAndRemovalCaptureAllEightBytes_data() { noOpRetainsCompleteSource_data(); }
    void genericRowGrowthAndRemovalCaptureAllEightBytes()
    {
        QFETCH(QString, name);
        const auto bytes = modelBytes(name);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QByteArray grown, restored;
        QString error;
        QVERIFY2(resizeGeneric(bytes, table.deformers.size() + 1, grown, &error), qPrintable(error));
        Builder::ColumnState state;
        QVERIFY2(Builder::CaptureColumnState(bytes, grown, state, &error), qPrintable(error));
        QCOMPARE(state.counts.value(1), table.deformers.size() + 1);
        const auto scratch = named(table, "deformers.runtime_space_0");
        QCOMPARE(state.columns.value(scratch.slot).size(), (table.deformers.size() + 1) * 8);
        QByteArray rebuilt;
        QVERIFY2(Builder::BuildColumnState(bytes, state, rebuilt, &error), qPrintable(error));
        auto rebuiltReader = CReader::FromBytes(rebuilt); CSectionTable next(rebuiltReader);
        QCOMPARE(column(rebuilt, named(next, scratch.name)), state.columns.value(scratch.slot));
        QVERIFY2(resizeGeneric(grown, table.deformers.size(), restored, &error), qPrintable(error));
        QVERIFY2(Builder::CaptureColumnState(bytes, restored, state, &error), qPrintable(error));
        QVERIFY(state.columns.isEmpty() && state.counts.isEmpty());
        QVERIFY(Builder::BuildColumnState(bytes, state, rebuilt, &error));
        QCOMPARE(rebuilt, bytes);
    }

    void shortOrMetadataStorageFailsAtomically_data()
    {
        QTest::addColumn<bool>("metadata");
        QTest::newRow("upper-half-past-eof") << false;
        QTest::newRow("upper-half-overlaps-canvas") << true;
    }
    void shortOrMetadataStorageFailsAtomically()
    {
        QFETCH(bool, metadata);
        const auto original = modelBytes("Mark");
        auto reader = CReader::FromBytes(original); CSectionTable table(reader);
        const auto scratch = named(table, "deformers.runtime_space_0");
        auto bytes = original;
        if (metadata) {
            const auto canvas = original.mid(table.sectionOffsetTable->canvas_info->CanvasOffset, 20);
            bytes.append(QByteArray(512, '\0'));
            const int canvasOffset = bytes.size();
            bytes.append(canvas); bytes.append(QByteArray((64 - bytes.size() % 64) % 64, '\0'));
            put(bytes, 0x44, canvasOffset);
            put(bytes, scratch.slot, canvasOffset - scratch.count * 4);
        } else put(bytes, scratch.slot, bytes.size() - scratch.count * 4);
        auto invalidReader = CReader::FromBytes(bytes); CSectionTable readable(invalidReader);
        QVERIFY(readable.IsValid());
        QString error; QByteArray output("keep bytes"); auto state = sentinel(); const auto old = state;
        QVERIFY(!Builder::BuildColumnState(bytes, {}, output, &error));
        QCOMPARE(output, QByteArray("keep bytes"));
        QVERIFY(error.contains(QString::fromLatin1(scratch.name)));
        QVERIFY(!Builder::CaptureColumnState(original, bytes, state, &error));
        QVERIFY(state == old);
        QVERIFY(!Builder::CaptureColumnState(bytes, original, state, &error));
        QVERIFY(state == old);
    }

    void aliasedWritableStorageIsNotSilentlyDetached_data()
    {
        QTest::addColumn<int>("overlap");
        QTest::newRow("same-start") << 0;
        QTest::newRow("upper-half") << 1;
        QTest::newRow("partial-upper-tail") << 2;
    }
    void aliasedWritableStorageIsNotSilentlyDetached()
    {
        QFETCH(int, overlap);
        auto bytes = modelBytes("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const auto scratch = named(table, "deformers.runtime_space_0");
        const auto enabled = named(table, "deformers.is_enabled");
        const int shift = overlap == 0 ? 0 : overlap == 1 ? scratch.count * 4 : scratch.count * 8 - 4;
        put(bytes, enabled.slot, scratch.offset + shift);
        auto aliasedReader = CReader::FromBytes(bytes); CSectionTable aliased(aliasedReader);
        QVERIFY(aliased.IsValid());
        QString error; QByteArray output; Builder::ColumnState state;
        QVERIFY2(Builder::CaptureColumnState(bytes, bytes, state, &error), qPrintable(error));
        QVERIFY(state.columns.isEmpty());
        state.columns.insert(scratch.slot, column(bytes, scratch));
        QVERIFY2(Builder::BuildColumnState(bytes, state, output, &error), qPrintable(error));
        QCOMPARE(output, bytes);
        state.columns[scratch.slot][shift] = char(0x5a);
        output = "keep bytes";
        QVERIFY(!Builder::BuildColumnState(bytes, state, output, &error));
        QCOMPARE(output, QByteArray("keep bytes"));
        QVERIFY(error.contains("deformers.is_enabled") && error.contains("deformers.runtime_space_0"));
        auto edited = bytes; edited[scratch.offset + shift] = char(0x5a);
        auto captured = sentinel(); const auto old = captured;
        QVERIFY(!Builder::CaptureColumnState(bytes, edited, captured, &error));
        QVERIFY(captured == old);
        const auto decimals = named(aliased, "parameters.decimal_places");
        state = {}; state.columns.insert(decimals.slot, column(bytes, decimals));
        put(state.columns[decimals.slot], 0, 7);
        QVERIFY2(Builder::BuildColumnState(bytes, state, output, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(output); CSectionTable after(afterReader);
        QCOMPARE(named(after, "deformers.is_enabled").offset - named(after, scratch.name).offset, qint64(shift));
        QCOMPARE(column(output, named(after, scratch.name)), column(bytes, scratch));
    }

    void captureRejectsNewMutableAliasEvenWhenColumnValuesMatch()
    {
        auto original = modelBytes("Mark");
        auto reader = CReader::FromBytes(original); CSectionTable table(reader);
        const auto scratch = named(table, "deformers.runtime_space_0"), enabled = named(table, "deformers.is_enabled");
        std::memset(original.data() + enabled.offset, 0, enabled.count * enabled.width);
        auto changed = original;
        put(changed, enabled.slot, scratch.offset + scratch.count * 4);
        auto state = sentinel(); const auto before = state; QString error;
        QVERIFY(!Builder::CaptureColumnState(original, changed, state, &error));
        QVERIFY(state == before);
        QVERIFY(error.contains(QStringLiteral("共享布局")));
        QVERIFY(!Builder::CaptureColumnState(changed, original, state, &error));
        QVERIFY(state == before);
    }

    void previewPatchesProtectEveryRuntimeUpperHalf_data()
    {
        QTest::addColumn<QByteArray>("runtime");
        QTest::addColumn<bool>("positions");
        for (const QByteArray& runtime : {QByteArray("parts.runtime_space_0"), QByteArray("deformers.runtime_space_0"),
             QByteArray("art_meshes.runtime_space_0"), QByteArray("art_meshes.runtime_space_1"),
             QByteArray("art_meshes.runtime_space_2"), QByteArray("art_meshes.runtime_space_3"), QByteArray("glue.runtime_space")})
            for (bool positions : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1(runtime) + (positions ? "/positions" : "/scalar"))) << runtime << positions;
    }

    void previewPatchesProtectEveryRuntimeUpperHalf()
    {
        QFETCH(QByteArray, runtime); QFETCH(bool, positions);
        auto source = modelBytes(runtime == "glue.runtime_space" ? "Natori" : "Mark");
        auto reader = CReader::FromBytes(source); CSectionTable table(reader);
        const auto scratch = named(table, runtime.constData());
        QVERIFY(scratch.offset && scratch.count);
        const auto* mesh = table.art_meshes.first();
        const int row = mesh->keyform_sources_begin_indices;
        const int first = table.art_mesh_keyforms[row]->keyform_position_sources_begin_indices;
        const qint64 editedAddress = positions ? table.sectionOffsetTable->keyform_positions->xys + first * 4
                                               : table.sectionOffsetTable->parameters->min_values;
        const qint64 scratchOffset = editedAddress - scratch.count * 4;
        QVERIFY(scratchOffset > table.sectionOffsetTable->canvas_info->CanvasOffset + 20);
        put(source, scratch.slot, scratchOffset);
        const auto preserved = source.mid(scratchOffset, scratch.count * 8);
        CMocPreviewBuilder builder; QString error;
        QVERIFY2(builder.Initialize(source, &error), qPrintable(error));
        CMocPreviewBuilder::Overrides edits;
        QVector<float> values;
        CModelDocument document;
        QVERIFY2(document.LoadBytes(source, "runtime-alias.moc3", &error), qPrintable(error));
        if (positions) {
            values = document.DrawableKeyformPositions(mesh->ids, 0);
            values[0] += .0125f;
            edits.artMeshPositions.insert(mesh->ids, {values});
        } else {
            auto metadata = document.ParameterMetadata(table.parameters.first()->ids);
            metadata.minimum -= 1;
            edits.parameterMetadata.insert(table.parameters.first()->ids, metadata);
        }
        QByteArray output;
        QVERIFY2(builder.BuildOverrides(edits, output, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(output); CSectionTable after(afterReader);
        QVERIFY(after.IsValid());
        QCOMPARE(column(output, named(after, runtime.constData())), preserved);
        if (positions) {
            const int start = after.art_mesh_keyforms[after.art_meshes.first()->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
            QVector<float> actual;
            for (int index = 0; index < values.size(); ++index) actual.append(after.keyform_positions->xys[start + index]);
            QCOMPARE(actual, values);
            values[0] = std::numeric_limits<float>::quiet_NaN();
            edits.artMeshPositions[mesh->ids] = {values};
        } else {
            QCOMPARE(after.parameters.first()->min_values, table.parameters.first()->min_values - 1);
            edits.parameterMetadata[table.parameters.first()->ids].minimum = std::numeric_limits<float>::infinity();
        }
        output = "keep bytes";
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QCOMPARE(output, QByteArray("keep bytes"));
    }

    void previewRejectsTruncatedStorageWithoutReplacingItsBaseline()
    {
        const auto bytes = modelBytes("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(bytes));
        const auto digest = builder.SourceDigest();
        auto truncated = bytes;
        const auto scratch = named(table, "deformers.runtime_space_0");
        put(truncated, scratch.slot, truncated.size() - scratch.count * 4);
        QString error;
        QVERIFY(!builder.Initialize(truncated, &error));
        QVERIFY(error.contains(QString::fromLatin1(scratch.name)));
        QCOMPARE(builder.SourceDigest(), digest);
        QByteArray output;
        QVERIFY(builder.BuildOverrides({}, output, &error));
        QCOMPARE(output, bytes);
    }

#ifdef LIVE2D_TEST_CORE
    void realCorePreservesExistingDrawablesAfterGenericGrowthAndRemoval_data() { noOpRetainsCompleteSource_data(); }
    void realCorePreservesExistingDrawablesAfterGenericGrowthAndRemoval()
    {
        QFETCH(QString, name);
        const auto bytes = modelBytes(name);
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QByteArray grown, restored; QString error;
        QVERIFY2(resizeGeneric(bytes, table.deformers.size() + 1, grown, &error), qPrintable(error));
        QVERIFY2(resizeGeneric(grown, table.deformers.size(), restored, &error), qPrintable(error));
        CoreModel source, added, removed;
        QVERIFY(source.load(bytes)); QVERIFY(added.load(grown)); QVERIFY(removed.load(restored));
        for (const auto& pose : poses(source.value)) {
            source.pose(pose); added.pose(pose); removed.pose(pose);
            const auto expected = frame(source.value);
            QVERIFY2(changed(expected, frame(added.value)).isEmpty(), qPrintable(difference(expected, frame(added.value))));
            QVERIFY2(changed(expected, frame(removed.value)).isEmpty(), qPrintable(difference(expected, frame(removed.value))));
        }
        QVERIFY(source.intact() && added.intact() && removed.intact());
    }
#endif
};
QTEST_GUILESS_MAIN(DeformerColumnStateTests)
#include "deformer_column_state_tests.moc"
