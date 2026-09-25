#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QtEndian>
#include <QFile>
#include <QCryptographicHash>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
constexpr int columnSlots[]{0x10c, 0x110, 0x114, 0x118, 0x11c};

QByteArray source(const QString& model)
{
    QFile file(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

quint32 word(const QByteArray& bytes, int at) { return qFromLittleEndian<quint32>(bytes.constData() + at); }
void put(QByteArray& bytes, int at, quint32 value) { qToLittleEndian(value, bytes.data() + at); }
quint32 bits(float value) { quint32 encoded; std::memcpy(&encoded, &value, 4); return encoded; }

CModelParameterMetadata metadata(const CParameters& parameter)
{
    return {parameter.ids, parameter.min_values, parameter.max_values, parameter.default_values,
            parameter.decimal_places, parameter.is_repeat};
}

quint32 fieldValue(const CModelParameterMetadata& value, int field)
{
    switch (field) {
    case 0: return bits(value.maximum);
    case 1: return bits(value.minimum);
    case 2: return bits(value.defaultValue);
    case 3: return value.repeat ? 1 : 0;
    default: return quint32(value.decimalPlaces);
    }
}

void change(CModelParameterMetadata& value, int field)
{
    switch (field) {
    case 0: value.maximum += 7; break;
    case 1: value.minimum -= 9; break;
    case 2: value.defaultValue = value.defaultValue == value.minimum ? value.maximum : value.minimum; break;
    case 3: value.repeat = !value.repeat; break;
    default: value.decimalPlaces = value.decimalPlaces == 7 ? 5 : 7; break;
    }
}

void models()
{
    QTest::addColumn<QString>("model");
    for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
}
}

class MocParameterPreviewTests : public QObject
{
    Q_OBJECT
private slots:
    void realModelsPatchEachRuntimeFieldIndependently_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<int>("field");
        for (const char* model : {"Mark", "Rice", "Natori", "Wanko"}) for (int field = 0; field < 5; ++field)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(model).arg(field))) << QString::fromLatin1(model) << field;
    }

    void realModelsPatchEachRuntimeFieldIndependently()
    {
        QFETCH(QString, model);
        QFETCH(int, field);
        const QByteArray baseline = source(model);
        QVERIFY(!baseline.isEmpty());
        QCOMPARE(quint8(baseline[4]), quint8(model == "Mark" || model == "Rice" ? 3 : 1));
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QVERIFY(table.parameters.size() > 2);
        const int index = table.parameters.size() / 2;
        const auto original = metadata(*table.parameters[index]);
        QVERIFY(original.minimum < original.maximum);
        auto edited = original;
        change(edited, field);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(baseline, &error), qPrintable(error));
        CMocPreviewBuilder::Overrides overrides;
        overrides.parameterMetadata.insert(table.parameters[index]->ids, edited);
        QByteArray output("unchanged");
        QVERIFY2(builder.BuildOverrides(overrides, output, &error), qPrintable(error));
        QByteArray expected = baseline;
        put(expected, word(baseline, columnSlots[field]) + index * 4, fieldValue(edited, field));
        QCOMPARE(output, expected);
        CReader updatedReader = CReader::FromBytes(output);
        CSectionTable updated(updatedReader);
        QVERIFY2(updated.IsValid(), qPrintable(updatedReader.Error()));
        QCOMPARE(updated.parameters.size(), table.parameters.size());
        for (int row = 0; row < table.parameters.size(); ++row) {
            QVERIFY(metadata(*updated.parameters[row]) == (row == index ? edited : metadata(*table.parameters[row])));
            QCOMPARE(updated.parameters[row]->parameter_types, table.parameters[row]->parameter_types);
            QCOMPARE(updated.parameters[row]->parameter_binding_sources_begin_indices, table.parameters[row]->parameter_binding_sources_begin_indices);
            QCOMPARE(updated.parameters[row]->parameter_binding_sources_counts, table.parameters[row]->parameter_binding_sources_counts);
        }
        overrides.parameterMetadata[table.parameters[index]->ids] = original;
        QVERIFY(builder.BuildOverrides(overrides, output, &error));
        QCOMPARE(output, baseline);
        QCOMPARE(builder.SourceDigest(), QCryptographicHash::hash(baseline, QCryptographicHash::Sha256));
    }

    void unchangedAndDisplayNameOnlyPreserveEveryByte_data() { models(); }
    void unchangedAndDisplayNameOnlyPreserveEveryByte()
    {
        QFETCH(QString, model);
        const auto baseline = source(model);
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline));
        CMocPreviewBuilder::Overrides overrides;
        for (const auto* parameter : table.parameters) overrides.parameterMetadata.insert(parameter->ids, metadata(*parameter));
        QByteArray output;
        QVERIFY(builder.BuildOverrides(overrides, output));
        QCOMPARE(output, baseline);
        for (auto it = overrides.parameterMetadata.begin(); it != overrides.parameterMetadata.end(); ++it)
            it->displayName = QStringLiteral("自定义名称 <parameter> é");
        QVERIFY(builder.BuildOverrides(overrides, output));
        QCOMPARE(output, baseline);
    }

    void negativeZeroAndNoncanonicalTrueKeepOriginalEncoding()
    {
        auto baseline = source("Mark");
        QVERIFY(!baseline.isEmpty());
        put(baseline, word(baseline, 0x114), 0x80000000u);
        put(baseline, word(baseline, 0x118), 0x80000002u);
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto original = metadata(*table.parameters[0]);
        QVERIFY(std::signbit(original.defaultValue));
        QVERIFY(original.repeat);
        auto edited = original;
        edited.defaultValue = 0.0f;
        edited.decimalPlaces = 8;
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline));
        CMocPreviewBuilder::Overrides overrides;
        overrides.parameterMetadata.insert(table.parameters[0]->ids, edited);
        QByteArray output;
        QVERIFY(builder.BuildOverrides(overrides, output));
        auto expected = baseline;
        put(expected, word(expected, 0x11c), 8);
        QCOMPARE(output, expected);
        overrides.parameterMetadata[table.parameters[0]->ids].repeat = false;
        QVERIFY(builder.BuildOverrides(overrides, output));
        put(expected, word(expected, 0x118), 0);
        QCOMPARE(output, expected);
        overrides.parameterMetadata[table.parameters[0]->ids] = original;
        QVERIFY(builder.BuildOverrides(overrides, output));
        QCOMPARE(output, baseline);
    }

    void aliasedColumnsDetachWithoutTouchingOtherFields_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<int>("sharedSlot");
        QTest::addColumn<int>("byteShift");
        QTest::newRow("maximum-default") << 0 << 0x114 << 0;
        QTest::newRow("minimum-default") << 1 << 0x114 << 0;
        QTest::newRow("default-maximum") << 2 << 0x10c << 0;
        QTest::newRow("repeat-decimal") << 3 << 0x11c << 0;
        QTest::newRow("decimal-repeat") << 4 << 0x118 << 0;
        QTest::newRow("partial-byte-overlap") << 3 << 0x11c << 2;
    }

    void aliasedColumnsDetachWithoutTouchingOtherFields()
    {
        QFETCH(int, field);
        QFETCH(int, sharedSlot);
        QFETCH(int, byteShift);
        auto baseline = source("Mark");
        QVERIFY(!baseline.isEmpty());
        put(baseline, columnSlots[field], word(baseline, sharedSlot) + byteShift);
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        const int row = 0;
        const auto original = metadata(*table.parameters[row]);
        auto edited = original;
        change(edited, field);
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY2(builder.Initialize(baseline, &error), qPrintable(error));
        CMocPreviewBuilder::Overrides overrides;
        overrides.parameterMetadata.insert(table.parameters[row]->ids, edited);
        QByteArray output;
        QVERIFY2(builder.BuildOverrides(overrides, output, &error), qPrintable(error));
        const quint32 destination = word(output, columnSlots[field]);
        QVERIFY(destination >= quint32(baseline.size()));
        QCOMPARE(destination % 64, 0u);
        auto unchangedPrefix = output.left(baseline.size());
        put(unchangedPrefix, columnSlots[field], word(baseline, columnSlots[field]));
        QCOMPARE(unchangedPrefix, baseline);
        for (int index = 0; index < table.parameters.size(); ++index)
            QCOMPARE(word(output, destination + index * 4), index == row ? fieldValue(edited, field) : word(baseline, word(baseline, columnSlots[field]) + index * 4));
        CReader updatedReader = CReader::FromBytes(output);
        CSectionTable updated(updatedReader);
        QVERIFY(updated.IsValid());
        QVERIFY(metadata(*updated.parameters[row]) == edited);
        overrides.parameterMetadata[table.parameters[row]->ids] = original;
        QVERIFY(builder.BuildOverrides(overrides, output));
        QCOMPARE(output, baseline);
    }

    void narrowedRangesKeepBindingsAndSourceKeys_data() { models(); }
    void narrowedRangesKeepBindingsAndSourceKeys()
    {
        QFETCH(QString, model);
        const auto baseline = source(model);
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(!table.parameters.isEmpty());
        const auto* parameter = table.parameters[0];
        QCOMPARE(parameter->min_values, -30.0f);
        QCOMPARE(parameter->max_values, 30.0f);
        QVERIFY(parameter->parameter_binding_sources_counts > 0);
        const int firstBinding = parameter->parameter_binding_sources_begin_indices;
        const auto* binding = table.parameter_bindings[firstBinding];
        QVERIFY(binding->keys_sources_counts >= 3);
        QCOMPARE(table.keys->GetData(binding->keys_sources_begin_indices), -30.0f);
        auto edited = metadata(*parameter);
        edited.minimum = -5;
        edited.maximum = 5;
        edited.defaultValue = 1;
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline));
        CMocPreviewBuilder::Overrides overrides;
        overrides.parameterMetadata.insert(parameter->ids, edited);
        QByteArray output;
        QVERIFY(builder.BuildOverrides(overrides, output));
        auto expected = baseline;
        put(expected, word(expected, 0x110), bits(-5));
        put(expected, word(expected, 0x10c), bits(5));
        put(expected, word(expected, 0x114), bits(1));
        QCOMPARE(output, expected);
        CReader updatedReader = CReader::FromBytes(output);
        CSectionTable updated(updatedReader);
        QVERIFY(updated.IsValid());
        QVERIFY(updated.keys->values == table.keys->values);
        CKeyformBindingGrid originalGrid, updatedGrid;
        QVERIFY(originalGrid.Build(table));
        QVERIFY(updatedGrid.Build(updated));
        bool found = false;
        const auto check = [&](CKeyformBindingGrid::ObjectType type, const QString& id) {
            const auto axes = originalGrid.Axes(type, id);
            for (const auto& axis : axes) if (axis.parameterId == parameter->ids) {
                found = true;
                QCOMPARE(updatedGrid.Diagnostic(type, id), originalGrid.Diagnostic(type, id));
                const auto updatedAxes = updatedGrid.Axes(type, id);
                QCOMPARE(updatedAxes.size(), axes.size());
                for (int index = 0; index < axes.size(); ++index) QCOMPARE(updatedAxes[index].keys, axes[index].keys);
            }
        };
        for (const auto* mesh : table.art_meshes) check(CKeyformBindingGrid::ArtMesh, mesh->ids);
        for (const auto* deformer : table.deformers)
            check(deformer->types == 0 ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation, deformer->ids);
        QVERIFY(found);
    }

    void metadataAndGeometryComposeFromOneSnapshot()
    {
        const auto baseline = source("Mark");
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(baseline, "snapshot.moc3", &error), qPrintable(error));
        auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
                                 [](const CModelDrawable& item) { return !item.keyforms.isEmpty() && !item.keyforms[0].positions.isEmpty(); });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        auto positions = mesh->keyforms[0].positions;
        positions[0] += 0.125f;
        auto metadata = document.ParameterMetadata(document.Parameters()[0].id);
        metadata.maximum += 15;
        metadata.defaultValue = 2;
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline));
        CMocPreviewBuilder::Overrides overrides;
        overrides.artMeshPositions.insert(id, {positions});
        overrides.parameterMetadata.insert(document.Parameters()[0].id, metadata);
        QByteArray output;
        QVERIFY2(builder.BuildOverrides(overrides, output, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(restored.LoadBytes(output, "preview.moc3", &error), qPrintable(error));
        QCOMPARE(restored.DrawableKeyformPositions(id, 0), positions);
        QVERIFY(restored.ParameterMetadata(document.Parameters()[0].id) == metadata);
        QCOMPARE(document.SourceBytes(), baseline);
        QVERIFY(builder.BuildOverrides({}, output, &error));
        QCOMPARE(output, baseline);
    }

    void invalidEditsLeaveOutputAndSnapshotUntouched()
    {
        const auto baseline = source("Mark");
        CReader reader = CReader::FromBytes(baseline);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline));
        const auto original = metadata(*table.parameters[0]);
        QVector<CModelParameterMetadata> invalid;
        auto bad = original; bad.minimum = std::numeric_limits<float>::quiet_NaN(); invalid.append(bad);
        bad = original; bad.maximum = std::numeric_limits<float>::infinity(); invalid.append(bad);
        bad = original; bad.defaultValue = std::numeric_limits<float>::infinity(); invalid.append(bad);
        bad = original; bad.minimum = bad.maximum + 1; invalid.append(bad);
        bad = original; bad.defaultValue = bad.maximum + 1; invalid.append(bad);
        bad = original; bad.decimalPlaces = -1; invalid.append(bad);
        bad = original; bad.decimalPlaces = 10; invalid.append(bad);
        for (const auto& edit : invalid) {
            CMocPreviewBuilder::Overrides overrides;
            overrides.parameterMetadata.insert(table.parameters[0]->ids, edit);
            QByteArray output("retained");
            QString error;
            QVERIFY(!builder.BuildOverrides(overrides, output, &error));
            QCOMPARE(output, QByteArray("retained"));
            QVERIFY(!error.isEmpty());
        }
        for (const QString& id : {QString(), QStringLiteral("missing")}) {
            CMocPreviewBuilder::Overrides overrides;
            overrides.parameterMetadata.insert(id, original);
            QByteArray output("retained");
            QVERIFY(!builder.BuildOverrides(overrides, output));
            QCOMPARE(output, QByteArray("retained"));
        }
        QByteArray output;
        QVERIFY(builder.BuildOverrides({}, output));
        QCOMPARE(output, baseline);
        auto ambiguous = baseline;
        const int ids = word(ambiguous, 0x108);
        std::memcpy(ambiguous.data() + ids + 64, ambiguous.constData() + ids, 64);
        CMocPreviewBuilder duplicate;
        QVERIFY(duplicate.Initialize(ambiguous));
        QVERIFY(duplicate.BuildOverrides({}, output));
        QCOMPARE(output, ambiguous);
        CMocPreviewBuilder::Overrides overrides;
        overrides.parameterMetadata.insert(table.parameters[0]->ids, original);
        output = "retained";
        QVERIFY(!duplicate.BuildOverrides(overrides, output));
        QCOMPARE(output, QByteArray("retained"));
    }
};

QTEST_GUILESS_MAIN(MocParameterPreviewTests)
#include "parameter_moc_preview_tests.moc"
