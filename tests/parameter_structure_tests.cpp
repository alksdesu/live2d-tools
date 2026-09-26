#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <limits>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
QByteArray source(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }

QByteArray extendedMetadata(QByteArray bytes)
{
    const int canvas = word(bytes, 0x44), header = word(bytes, 0x40);
    bytes.insert(canvas, QByteArray(64, '\0'));
    for (int slot = 0x40; slot < header; slot += 4) {
        const int offset = word(bytes, slot);
        if (offset >= canvas) put(bytes, slot, offset + 64);
    }
    return bytes;
}

void models()
{
    QTest::addColumn<QString>("name");
    for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
}

Builder::Owner select(const CSectionTable& table, Builder::Owner::Type type)
{
    CKeyformBindingGrid grid;
    if (!grid.Build(table)) return {};
    if (type == Builder::Owner::ArtMesh) {
        for (const auto* mesh : table.art_meshes) {
            if (grid.Diagnostic(CKeyformBindingGrid::ArtMesh, mesh->ids).isEmpty()) return {type, mesh->ids};
        }
    } else {
        for (const auto* deformer : table.deformers) {
            if (deformer->types != (type == Builder::Owner::Warp ? 0 : 1)) continue;
            if (grid.Diagnostic(type == Builder::Owner::Warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation, deformer->ids).isEmpty()) return {type, deformer->ids};
        }
    }
    return {};
}

bool sameLogicalColumns(const QByteArray& first, const QByteArray& second, QString& error)
{
    auto ar = CReader::FromBytes(first), br = CReader::FromBytes(second);
    CSectionTable a(ar), b(br);
    if (!a.IsValid() || !b.IsValid()) { error = ar.Error() + br.Error(); return false; }
    const auto ac = CSectionValidation::ArraySpans(*a.sectionOffsetTable, *a.count_info);
    const auto bc = CSectionValidation::ArraySpans(*b.sectionOffsetTable, *b.count_info);
    if (ac.size() != bc.size()) return false;
    for (int index = 0; index < ac.size(); ++index) {
        const auto& x = ac[index]; const auto& y = bc[index];
        if (x.slot != y.slot || x.count != y.count || bool(x.offset) != bool(y.offset) ||
            first.mid(x.offset, x.offset ? x.count * x.width : 0) != second.mid(y.offset, y.offset ? y.count * y.width : 0)) {
            error = QString::fromLatin1(x.name); return false;
        }
    }
    return true;
}
}

class ParameterStructureTests : public QObject
{
    Q_OBJECT
private slots:
    void createAndDeleteUnboundParameter_data() { models(); }
    void createAndDeleteUnboundParameter()
    {
        QFETCH(QString, name);
        const auto original = source(name);
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("ParamStructureNew");
        Builder::Result created, deleted;
        QString error;
        QVERIFY2(Builder::Create(original, request, created, &error), qPrintable(error));
        auto reader = CReader::FromBytes(created.bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QCOMPARE(table.parameters.last()->ids, request.parameter.id);
        QCOMPARE(table.parameters.size(), created.oldParameterToNew.size() + 1);
        QCOMPARE(table.parameters.last()->parameter_binding_sources_counts, 0);
        QVector<Builder::Reference> references;
        QVERIFY2(Builder::AnalyzeDelete(created.bytes, request.parameter.id, references, &error), qPrintable(error));
        QVERIFY(references.isEmpty());
        QVERIFY2(Builder::DeleteUnreferenced(created.bytes, request.parameter.id, deleted, &error), qPrintable(error));
        QCOMPARE(deleted.oldParameterToNew.last(), -1);
        QVERIFY2(sameLogicalColumns(original, deleted.bytes, error), qPrintable(error));
        Builder::ColumnState state;
        QVERIFY2(Builder::CaptureColumnState(original, deleted.bytes, state, &error), qPrintable(error));
        QVERIFY(state.columns.isEmpty());
        QVERIFY(state.counts.isEmpty());
        QByteArray restored;
        QVERIFY(Builder::BuildColumnState(original, state, restored, &error));
        QCOMPARE(restored, original);
    }

    void createsSlowAxisAndStableFlatState_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("keyCount");
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) for (int type = 0; type < 3; ++type)
            for (int count : {2, 3, 4}) QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(name).arg(type).arg(count)))
                << QString::fromLatin1(name) << type << count;
    }

    void createsSlowAxisAndStableFlatState()
    {
        QFETCH(QString, name); QFETCH(int, type); QFETCH(int, keyCount);
        const auto original = source(name);
        auto reader = CReader::FromBytes(original);
        CSectionTable before(reader);
        const auto owner = select(before, Builder::Owner::Type(type));
        QVERIFY(!owner.id.isEmpty());
        QVector<float> keys = keyCount == 2 ? QVector<float>{-1, 1} : keyCount == 3 ? QVector<float>{-1, 0, 1} : QVector<float>{-1, -.4f, .2f, 1};
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("ParamSlowAxis");
        request.bindings.append({owner, keys});
        Builder::Result result;
        QString error;
        QVERIFY2(Builder::Create(original, request, result, &error), qPrintable(error));
        QCOMPARE(result.keyforms.size(), 1);
        const auto& remap = result.keyforms.first();
        QCOMPARE(remap.newKeyformCount, remap.oldKeyformCount * keyCount);
        for (int index = 0; index < remap.newLocalToOldLocal.size(); ++index)
            QCOMPARE(remap.newLocalToOldLocal[index], index % remap.oldKeyformCount);
        auto changedReader = CReader::FromBytes(result.bytes);
        CSectionTable changed(changedReader);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(changed));
        const auto axes = grid.Axes(CKeyformBindingGrid::ObjectType(type), owner.id);
        QVERIFY(!axes.isEmpty());
        QCOMPARE(axes.last().parameterId, request.parameter.id);
        QCOMPARE(axes.last().keys, keys);
        QVector<Builder::Reference> references;
        QVERIFY(Builder::AnalyzeDelete(result.bytes, request.parameter.id, references, &error));
        QVERIFY(!references.isEmpty());
        Builder::Result retained = result;
        QVERIFY(!Builder::DeleteUnreferenced(result.bytes, request.parameter.id, retained, &error));
        QCOMPARE(retained.bytes, result.bytes);
        Builder::ColumnState state, secondState;
        QVERIFY2(Builder::CaptureColumnState(original, result.bytes, state, &error), qPrintable(error));
        QByteArray rebuilt, twice;
        QVERIFY2(Builder::BuildColumnState(original, state, rebuilt, &error), qPrintable(error));
        QVERIFY2(sameLogicalColumns(result.bytes, rebuilt, error), qPrintable(error));
        QVERIFY(Builder::CaptureColumnState(original, rebuilt, secondState, &error));
        QVERIFY(state == secondState);
        QVERIFY(Builder::BuildColumnState(original, secondState, twice, &error));
        QCOMPARE(twice, rebuilt);
    }

    void bindExistingParameterRemapsOtherParameterBindings()
    {
        const auto original = source("Mark");
        auto reader = CReader::FromBytes(original);
        CSectionTable table(reader);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(table));
        const QString parameter = table.parameters.first()->ids;
        Builder::Owner owner;
        for (const auto* mesh : table.art_meshes) {
            const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, mesh->ids);
            bool found = false;
            for (const auto& axis : axes) found = found || axis.parameterId == parameter;
            if (!found && grid.Diagnostic(CKeyformBindingGrid::ArtMesh, mesh->ids).isEmpty()) { owner = {Builder::Owner::ArtMesh, mesh->ids}; break; }
        }
        QVERIFY(!owner.id.isEmpty());
        Builder::Result result;
        QString error;
        QVERIFY2(Builder::BindExistingParameter(original, parameter, {{owner, {-10, 5, 20}}}, result, &error), qPrintable(error));
        auto updatedReader = CReader::FromBytes(result.bytes);
        CSectionTable updated(updatedReader);
        CKeyformBindingGrid updatedGrid;
        QVERIFY(updatedGrid.Build(updated));
        for (const auto* mesh : table.art_meshes) {
            const auto before = grid.Axes(CKeyformBindingGrid::ArtMesh, mesh->ids);
            const auto after = updatedGrid.Axes(CKeyformBindingGrid::ArtMesh, mesh->ids);
            QCOMPARE(after.size(), before.size() + (mesh->ids == owner.id ? 1 : 0));
            for (int index = 0; index < before.size(); ++index) {
                QCOMPARE(after[index].parameterId, before[index].parameterId);
                QCOMPARE(after[index].keys, before[index].keys);
            }
        }
    }

    void invalidRequestsAndStatesAreAtomic()
    {
        const auto original = source("Mark");
        Builder::Result result;
        result.bytes = "sentinel";
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("Bad");
        request.parameter.maximum = std::numeric_limits<float>::infinity();
        QString error;
        QVERIFY(!Builder::Create(original, request, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        request.parameter.maximum = 1;
        request.bindings = {{{Builder::Owner::ArtMesh, QStringLiteral("missing")}, {-1, 0, 1}}};
        QVERIFY(!Builder::Create(original, request, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        Builder::ColumnState state;
        state.counts.insert(32, 1);
        QByteArray bytes("sentinel");
        QVERIFY(!Builder::BuildColumnState(original, state, bytes, &error));
        QCOMPARE(bytes, QByteArray("sentinel"));
        state.counts.clear();
        state.columns.insert(0x264, QByteArray(4, '\0'));
        QVERIFY(!Builder::BuildColumnState(original, state, bytes, &error));
        QCOMPARE(bytes, QByteArray("sentinel"));
        auto changed = original;
        changed[7] = 1;
        Builder::ColumnState retained;
        retained.columns.insert(0x11c, "sentinel");
        QVERIFY(!Builder::CaptureColumnState(original, changed, retained, &error));
        QCOMPARE(retained.columns.value(0x11c), QByteArray("sentinel"));
    }

    void unknownActiveExtensionsRejectStructureChanges_data()
    {
        QTest::addColumn<bool>("activeCount");
        QTest::newRow("unknown-count") << true;
        QTest::newRow("unknown-payload") << false;
    }

    void unknownActiveExtensionsRejectStructureChanges()
    {
        QFETCH(bool, activeCount);
        auto original = extendedMetadata(source("Mark"));
        if (activeCount) put(original, word(original, 0x40) + 32 * 4, 1);
        else put(original, 0x264, word(original, 0x174));
        auto reader = CReader::FromBytes(original);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        Builder::Result retained;
        retained.bytes = "sentinel";
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("ParamSafeUnknown");
        QString error;
        QVERIFY(!Builder::Create(original, request, retained, &error));
        QVERIFY(error.contains(QStringLiteral("未解释")));
        QCOMPARE(retained.bytes, QByteArray("sentinel"));
        QVERIFY(!Builder::BindExistingParameter(original, table.parameters.first()->ids, {}, retained, &error));
        QCOMPARE(retained.bytes, QByteArray("sentinel"));
        QVector<Builder::Reference> references{{"sentinel", {}, {}, {}}};
        QVERIFY(!Builder::AnalyzeDelete(original, table.parameters.first()->ids, references, &error));
        QCOMPARE(references.first().kind, QStringLiteral("sentinel"));
        QVERIFY(!Builder::DeleteUnreferenced(original, table.parameters.first()->ids, retained, &error));
        QCOMPARE(retained.bytes, QByteArray("sentinel"));
    }

    void emptyUnknownMarkersRemainEmptyAfterRepeatedRebuilds()
    {
        auto original = extendedMetadata(source("Mark"));
        original[4] = 5;
        for (int slot : {0x294, 0x298, 0x29c}) put(original, slot, original.size());
        Builder::Result first, second;
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("ParamFirst");
        QString error;
        QVERIFY2(Builder::Create(original, request, first, &error), qPrintable(error));
        request.parameter.id = QStringLiteral("ParamSecond");
        QVERIFY2(Builder::Create(first.bytes, request, second, &error), qPrintable(error));
        Builder::ColumnState state;
        QVERIFY2(Builder::CaptureColumnState(original, second.bytes, state, &error), qPrintable(error));
        QByteArray rebuilt;
        QVERIFY2(Builder::BuildColumnState(original, state, rebuilt, &error), qPrintable(error));
        QVERIFY2(sameLogicalColumns(second.bytes, rebuilt, error), qPrintable(error));
        for (int slot : {0x294, 0x298, 0x29c}) {
            QCOMPARE(word(first.bytes, slot), first.bytes.size());
            QCOMPARE(word(second.bytes, slot), second.bytes.size());
            QCOMPARE(word(rebuilt, slot), rebuilt.size());
        }
    }

    void unboundTypeOneDeletionIsNotAdvertisedAsSupported()
    {
        Builder::CreateRequest request;
        request.parameter.id = QStringLiteral("ParamTypeOne");
        Builder::Result created, retained;
        QString error;
        QVERIFY(Builder::Create(source("Mark"), request, created, &error));
        const int parameterCount = word(created.bytes, word(created.bytes, 0x40) + 5 * 4);
        if (!word(created.bytes, 0x208)) {
            while (created.bytes.size() % 64) created.bytes.append('\0');
            put(created.bytes, 0x208, created.bytes.size());
            created.bytes.append(QByteArray(parameterCount * 4, '\0'));
            while (created.bytes.size() % 64) created.bytes.append('\0');
        }
        put(created.bytes, word(created.bytes, 0x208) + (parameterCount - 1) * 4, 1);
        retained.bytes = "sentinel";
        QVERIFY(!Builder::DeleteUnreferenced(created.bytes, request.parameter.id, retained, &error));
        QVERIFY(error.contains(QStringLiteral("普通参数")));
        QCOMPARE(retained.bytes, QByteArray("sentinel"));
    }

    void versionFiveSchemaPreservesAllExtendedColumns()
    {
        auto original = extendedMetadata(source("Mark"));
        original[4] = 5;
        auto reader = CReader::FromBytes(original);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        QCOMPARE(spans.size(), 147);
        QSet<int> indices;
        Builder::ColumnState state;
        state.counts.insert(32, 17);
        state.counts.insert(33, 19);
        for (const auto& span : spans) {
            QCOMPARE(word(original, span.slot), span.offset);
            QCOMPARE(word(original, word(original, 0x40) + span.countIndex * 4), span.count);
            QVERIFY(!indices.contains(span.slot));
            indices.insert(span.slot);
            if (span.slot < 0x264) continue;
            QByteArray values(state.counts.value(span.countIndex, span.count) * 4, '\0');
            for (int index = 0; index < values.size() / 4; ++index) put(values, index * 4, span.slot == 0x288 ? 0 : index);
            state.columns.insert(span.slot, values);
        }
        QByteArray expanded;
        QString error;
        QVERIFY2(Builder::BuildColumnState(original, state, expanded, &error), qPrintable(error));
        auto expandedReader = CReader::FromBytes(expanded);
        CSectionTable parsed(expandedReader);
        QVERIFY(parsed.IsValid());
        QCOMPARE(parsed.blend_shapes_parts.size(), 17);
        QCOMPARE(parsed.blend_shapes_rotation_deformers.size(), 19);
        for (const auto& span : CSectionValidation::ArraySpans(*parsed.sectionOffsetTable, *parsed.count_info))
            if (span.slot >= 0x264) QCOMPARE(expanded.mid(span.offset, span.count * span.width), state.columns.value(span.slot));
        Builder::ColumnState captured;
        QVERIFY(Builder::CaptureColumnState(original, expanded, captured, &error));
        QCOMPARE(captured.columns, state.columns);
        QCOMPARE(captured.counts, state.counts);
        CKeyformBindingGrid grid;
        QVERIFY(grid.Build(parsed));
        bool found = false;
        for (const auto* deformer : parsed.deformers) if (deformer->types == 1 && deformer->specific_sources_indices == 0) {
            found = true;
            QVERIFY(grid.Diagnostic(CKeyformBindingGrid::Rotation, deformer->ids).contains(QStringLiteral("BlendShape")));
        }
        QVERIFY(found);
    }
};

QTEST_APPLESS_MAIN(ParameterStructureTests)
#include "parameter_structure_tests.moc"
