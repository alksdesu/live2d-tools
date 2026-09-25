#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QtEndian>
#include <array>

using namespace live2dstruct;

namespace {
using ExtensionField = int CParameters::Offset::*;
const std::array<ExtensionField, 6> extensionFields{
    &CParameters::Offset::parameter_types,
    &CParameters::Offset::extension_runtime_space_0,
    &CParameters::Offset::keys_sources_begin_indices,
    &CParameters::Offset::keys_sources_counts,
    &CParameters::Offset::blend_shape_parameter_binding_sources_begin_indices,
    &CParameters::Offset::blend_shape_parameter_binding_sources_counts
};

std::array<int, 6> extensions(const CParameters& parameter)
{
    return {parameter.parameter_types, parameter.extension_runtime_space_0,
            parameter.keys_sources_begin_indices, parameter.keys_sources_counts,
            parameter.blend_shape_parameter_binding_sources_begin_indices,
            parameter.blend_shape_parameter_binding_sources_counts};
}

int appendIntegers(CReader& reader, const std::array<qint32, 2>& values)
{
    const int offset = static_cast<int>(reader.m_byte.size());
    for (qint32 value : values) {
        const qint32 littleEndian = qToLittleEndian(value);
        reader.m_byte.append(reinterpret_cast<const char*>(&littleEndian), sizeof(littleEndian));
    }
    return offset;
}
}

class MocOptionalFieldsTests : public QObject
{
    Q_OBJECT

private slots:
    void offsetConstructorsDefaultOptionalMembers()
    {
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        QVERIFY(reader.IsValid());
        const int offset = static_cast<int>(reader.m_byte.size());
        reader.m_byte.append(QByteArray(9 * 4, '\0'));
        reader.seek(offset);
        const CParameters::Offset parameters(reader);
        for (ExtensionField field : extensionFields) QCOMPARE(parameters.*field, 0);
        reader.seek(offset);
        const CWarp_deformers::Offset warp(reader);
        QCOMPARE(warp.is_quad_source, 0);
        QCOMPARE(warp.keyform_color_sources_begin_indices, 0);
        QVERIFY(reader.IsValid());
    }

    void originalSamplesDefaultAbsentParameterExtensions_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void originalSamplesDefaultAbsentParameterExtensions()
    {
        QFETCH(QString, model);
        const QString path = QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3";
        CReader reader(path);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QVERIFY(!table.parameters.isEmpty());
        for (ExtensionField field : extensionFields) QCOMPARE(table.sectionOffsetTable->parameters->*field, 0);
        for (const CParameters* parameter : table.parameters)
            for (int value : extensions(*parameter)) QCOMPARE(value, 0);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        QCOMPARE(document.Parameters().size(), table.parameters.size());
        for (const auto& parameter : document.Parameters()) QCOMPARE(parameter.type, 0);
    }

    void originalWarpFlagsRespectMissingAndPresentArrays_data()
    {
        originalSamplesDefaultAbsentParameterExtensions_data();
    }

    void originalWarpFlagsRespectMissingAndPresentArrays()
    {
        QFETCH(QString, model);
        CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QVERIFY(!table.warp_deformers.isEmpty());
        const int offset = table.sectionOffsetTable->warp_deformers->is_quad_source;
        const int version = static_cast<quint8>(reader.m_byte[4]);
        if (version == 1) {
            QCOMPARE(offset, 0);
            for (const CWarp_deformers* warp : table.warp_deformers) QVERIFY(!warp->is_quad_source);
        } else {
            QCOMPARE(version, 3);
            QVERIFY(offset > 0);
            QVERIFY(static_cast<qint64>(offset) + static_cast<qint64>(table.warp_deformers.size()) * 4 <= reader.m_byte.size());
            for (qsizetype index = 0; index < table.warp_deformers.size(); ++index) {
                const qint32 raw = qFromLittleEndian<qint32>(reader.m_byte.constData() + offset + index * 4);
                QCOMPARE(table.warp_deformers[index]->is_quad_source, raw != 0);
            }
        }
    }

    void nonzeroParameterExtensionArraysPreserveEachFieldAndIndex()
    {
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(table.parameters.size() >= 2);
        CParameters::Offset offsets = *table.sectionOffsetTable->parameters;
        const std::array<std::array<qint32, 2>, 6> values{{{0, 1}, {11, 12}, {21, 22}, {31, 32}, {41, 42}, {51, 52}}};
        for (std::size_t field = 0; field < extensionFields.size(); ++field)
            offsets.*extensionFields[field] = appendIntegers(reader, values[field]);
        for (int index : {0, 1}) {
            const CParameters parameter(reader, offsets, index);
            QVERIFY(reader.IsValid());
            const auto actual = extensions(parameter);
            for (std::size_t field = 0; field < actual.size(); ++field) QCOMPARE(actual[field], values[field][index]);
            QCOMPARE(parameter.ids, table.parameters[index]->ids);
            QCOMPARE(parameter.default_values, table.parameters[index]->default_values);
        }
        for (std::size_t missing = 0; missing < extensionFields.size(); ++missing) {
            CParameters::Offset partial = offsets;
            partial.*extensionFields[missing] = 0;
            const CParameters parameter(reader, partial, 1);
            QVERIFY(reader.IsValid());
            const auto actual = extensions(parameter);
            for (std::size_t field = 0; field < actual.size(); ++field)
                QCOMPARE(actual[field], field == missing ? 0 : values[field][1]);
        }
    }

    void presentWarpFlagsPreserveBothModes()
    {
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(table.warp_deformers.size() >= 2);
        CWarp_deformers::Offset offsets = *table.sectionOffsetTable->warp_deformers;
        for (const std::array<qint32, 2>& flags : {std::array<qint32, 2>{0, 1}, std::array<qint32, 2>{1, 0}}) {
            offsets.is_quad_source = appendIntegers(reader, flags);
            for (int index : {0, 1}) {
                const CWarp_deformers warp(reader, offsets, index);
                QVERIFY(reader.IsValid());
                QCOMPARE(warp.is_quad_source, flags[index] != 0);
                QCOMPARE(warp.columns, table.warp_deformers[index]->columns);
                QCOMPARE(warp.rows, table.warp_deformers[index]->rows);
            }
        }
        offsets.is_quad_source = 0;
        for (int index : {0, 1}) {
            const CWarp_deformers warp(reader, offsets, index);
            QVERIFY(!warp.is_quad_source);
            QVERIFY(reader.IsValid());
        }
    }
};

QTEST_APPLESS_MAIN(MocOptionalFieldsTests)
#include "optional_moc_fields_tests.moc"
