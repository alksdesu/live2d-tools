#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtEndian>
#include <QHash>
#include <QSet>
#include <array>
#include <limits>

using namespace live2dstruct;

namespace {
QByteArray sample(const QString& model)
{
    QFile file(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

qint32 integer(const QByteArray& bytes, int position)
{
    Q_ASSERT(position >= 0 && position <= bytes.size() - 4);
    return qFromLittleEndian<qint32>(bytes.constData() + position);
}

void setInteger(QByteArray& bytes, int position, qint32 value)
{
    Q_ASSERT(position >= 0 && position <= bytes.size() - 4);
    qToLittleEndian<qint32>(value, reinterpret_cast<uchar*>(bytes.data() + position));
}

bool save(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool noBulkData(const CSectionTable& table)
{
    return table.parts.empty() && table.deformers.empty() && table.warp_deformers.empty() &&
        table.rotation_deformers.empty() && table.art_meshes.empty() && table.parameters.empty() &&
        table.part_keyforms.empty() && table.warp_deformer_keyforms.empty() && table.rotation_deformer_keyforms.empty() &&
        table.art_mesh_keyforms.empty() && table.parameter_binding_indices.empty() && table.keyform_bindings.empty() &&
        table.parameter_bindings.empty() && table.drawable_masks.empty() && table.draw_order_groups.empty() &&
        table.draw_order_group_objects.empty() && table.glue.empty() && table.glue_info.empty() && table.glue_keyforms.empty() &&
        table.blend_shape_constraint_indices.empty() && table.blend_shape_constraint_values.empty() &&
        table.blend_shape_constraints.empty() && table.blend_shape_keyform_bindings.empty() &&
        table.blend_shape_parameter_bindings.empty() && table.blend_shapes_art_meshes.empty() &&
        table.blend_shapes_warp_deformers.empty() && table.keyform_multiply_colors.empty() && table.keyform_screen_colors.empty() &&
        (!table.keyform_positions || table.keyform_positions->xys.empty()) &&
        (!table.keys || table.keys->values.empty()) && (!table.uvs || table.uvs->uvs.empty()) &&
        (!table.position_indices || table.position_indices->indices.empty());
}

bool rejectedBeforeAllocation(const QString& path, const QByteArray& bytes, QString* detail)
{
    if (!save(path, bytes)) { *detail = "fixture write failed"; return false; }
    CReader reader(path);
    CSectionTable table(reader);
    *detail = QStringLiteral("tableValid=%1 readerValid=%2 bulkEmpty=%3 error=%4")
        .arg(table.IsValid()).arg(reader.IsValid()).arg(noBulkData(table)).arg(reader.Error());
    return !table.IsValid() && !reader.IsValid() && noBulkData(table) && !reader.Error().isEmpty();
}

void addModels()
{
    QTest::addColumn<QString>("model");
    for (const char* model : {"Mark", "Rice", "Natori", "Wanko"})
        QTest::newRow(model) << QString::fromLatin1(model);
}

struct ColumnFixture { int slot; int countIndex; int width; };
constexpr ColumnFixture columns[]{{0xc4, 4, 64}, {0xec, 4, 4}, {0x17c, 16, 2}, {0xe8, 4, 1}};
constexpr int parameterExtensions[]{0x1d8, 0x1dc, 0x1e0, 0x208, 0x20c, 0x210};

QStringList physicalColumnNames()
{
    struct Group { const char* section; const char* fields; };
    const Group groups[]{
        {"parts",
            "runtime_space_0 ids keyform_binding_sources_indices keyform_sources_begin_indices"
            " keyform_sources_counts is_visible is_enabled parent_part_indices"},
        {"deformers",
            "runtime_space_0 ids keyform_binding_sources_indices is_visible is_enabled"
            " parent_part_indices parent_deformer_indices types specific_sources_indices"},
        {"warp_deformers",
            "keyform_binding_sources_indices keyform_sources_begin_indices keyform_sources_counts"
            " vertex_counts rows columns"},
        {"rotation_deformers",
            "keyform_binding_sources_indices keyform_sources_begin_indices keyform_sources_counts"
            " base_angles"},
        {"art_meshes",
            "runtime_space_0 runtime_space_1 runtime_space_2 runtime_space_3 ids"
            " keyform_binding_sources_indices keyform_sources_begin_indices keyform_sources_counts"
            " is_visible is_enabled parent_part_indices parent_deformer_indices texture_nos"
            " drawable_flags vertex_counts uv_sources_begin_indices position_index_sources_begin_indices"
            " position_index_sources_counts drawable_mask_sources_begin_indices"
            " drawable_mask_sources_counts"},
        {"parameters",
            "runtime_space_0 ids max_values min_values default_values is_repeat decimal_places"
            " parameter_binding_sources_begin_indices parameter_binding_sources_counts"},
        {"part_keyforms",
            "draw_orders"},
        {"warp_deformer_keyforms",
            "opacities keyform_position_sources_begin_indices"},
        {"rotation_deformer_keyforms",
            "opacities angles origin_x origin_y scales is_reflect_x is_reflect_y"},
        {"art_mesh_keyforms",
            "opacities draw_orders keyform_position_sources_begin_indices"},
        {"keyform_positions",
            "xys"},
        {"parameter_binding_indices",
            "binding_sources_indices"},
        {"keyform_bindings",
            "parameter_binding_index_sources_begin_indices parameter_binding_index_sources_counts"},
        {"parameter_bindings",
            "keys_sources_begin_indices keys_sources_counts"},
        {"keys",
            "values"},
        {"uvs",
            "uvs"},
        {"position_indices",
            "indices"},
        {"drawable_masks",
            "art_mesh_sources_indices"},
        {"draw_order_groups",
            "object_sources_begin_indices object_sources_counts object_sources_total_counts"
            " maximum_draw_orders minimum_draw_orders"},
        {"draw_order_group_objects",
            "types indices self_indices"},
        {"glue",
            "runtime_space ids keyform_binding_sources_indices keyform_sources_begin_indices"
            " keyform_sources_counts art_mesh_indices_a art_mesh_indices_b"
            " glue_info_sources_begin_indices glue_info_sources_counts"},
        {"glue_info",
            "weights position_indices"},
        {"glue_keyforms",
            "intensities"},
        {"warp_deformers",
            "is_quad_source"},
        {"parameters",
            "extension_runtime_space_0 keys_sources_begin_indices keys_sources_counts"},
        {"warp_deformers",
            "keyform_color_sources_begin_indices"},
        {"rotation_deformers",
            "keyform_color_sources_begin_indices"},
        {"art_meshes",
            "keyform_color_sources_begin_indices"},
        {"keyform_multiply_colors",
            "r g b"},
        {"keyform_screen_colors",
            "r g b"},
        {"parameters",
            "parameter_types blend_shape_parameter_binding_sources_begin_indices"
            " blend_shape_parameter_binding_sources_counts"},
        {"blend_shape_parameter_bindings",
            "keys_sources_begin_indices keys_sources_counts base_key_indices"},
        {"blend_shape_keyform_bindings",
            "parameter_binding_sources_indices keyform_sources_blend_shape_indices"
            " keyform_sources_blend_shape_counts blend_shape_constraint_index_sources_begin_indices"
            " blend_shape_constraint_index_sources_counts"},
        {"blend_shapes_warp_deformers",
            "target_indices blend_shape_keyform_binding_sources_begin_indices"
            " blend_shape_keyform_binding_sources_counts"},
        {"blend_shapes_art_meshes",
            "target_indices blend_shape_keyform_binding_sources_begin_indices"
            " blend_shape_keyform_binding_sources_counts"},
        {"blend_shape_constraint_indices",
            "blend_shape_constraint_sources_indices"},
        {"blend_shape_constraints",
            "parameter_indices blend_shape_constraint_value_sources_begin_indices"
            " blend_shape_constraint_value_sources_counts"},
        {"blend_shape_constraint_values",
            "keys weights"},
    };
    QStringList names;
    for (const auto& group : groups)
        for (const QString& field : QString::fromLatin1(group.fields).split(QLatin1Char(' ')))
            names.append(QString::fromLatin1(group.section) + QLatin1Char('.') + field);
    return names;
}

QStringList countSections()
{
    return QStringLiteral(
        "parts deformers warp_deformers rotation_deformers art_meshes parameters part_keyforms"
        " warp_deformer_keyforms rotation_deformer_keyforms art_mesh_keyforms keyform_positions"
        " parameter_binding_indices keyform_bindings parameter_bindings keys uvs position_indices"
        " drawable_masks draw_order_groups draw_order_group_objects glue glue_info glue_keyforms"
        " keyform_multiply_colors keyform_screen_colors blend_shape_parameter_bindings"
        " blend_shape_keyform_bindings blend_shapes_warp_deformers blend_shapes_art_meshes"
        " blend_shape_constraint_indices blend_shape_constraints blend_shape_constraint_values"
    ).split(QLatin1Char(' '));
}

QSet<QString> optionalColumnNames()
{
    return {
        QStringLiteral("parts.runtime_space_0"),
        QStringLiteral("art_meshes.keyform_color_sources_begin_indices"),
        QStringLiteral("parameters.blend_shape_parameter_binding_sources_begin_indices"),
        QStringLiteral("parameters.blend_shape_parameter_binding_sources_counts"),
        QStringLiteral("parameters.extension_runtime_space_0"),
        QStringLiteral("parameters.keys_sources_begin_indices"),
        QStringLiteral("parameters.keys_sources_counts"),
        QStringLiteral("parameters.parameter_types"),
        QStringLiteral("rotation_deformers.keyform_color_sources_begin_indices"),
        QStringLiteral("warp_deformers.is_quad_source"),
        QStringLiteral("warp_deformers.keyform_color_sources_begin_indices"),
    };
}
}

class SectionPreflightTests : public QObject
{
    Q_OBJECT
private slots:
    void arraySpansMatchEveryPhysicalColumn_data() { addModels(); }

    void arraySpansMatchEveryPhysicalColumn()
    {
        QFETCH(QString, model);
        const QByteArray bytes = sample(model);
        QVERIFY(bytes.size() >= 0x264);
        CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        const auto names = physicalColumnNames();
        const auto sections = countSections();
        const auto optional = optionalColumnNames();
        QCOMPARE(spans.size(), 135);
        QCOMPARE(names.size(), spans.size());
        QCOMPARE(sections.size(), 32);
        QCOMPARE(optional.size(), 11);
        QHash<QString, CSectionValidation::ColumnSpan> byName;
        for (const auto& span : spans) {
            QVERIFY(span.name);
            const QString name = QString::fromLatin1(span.name);
            QVERIFY2(!byName.contains(name), qPrintable(name));
            byName.insert(name, span);
        }
        const int countOffset = integer(bytes, 0x40);
        for (int index = 0; index < names.size(); ++index) {
            const QString& name = names[index];
            QVERIFY2(byName.contains(name), qPrintable(name));
            const auto span = byName.value(name);
            QCOMPARE(span.offset, qint64(integer(bytes, 0x48 + index * 4)));
            const int countIndex = sections.indexOf(name.section(QLatin1Char('.'), 0, 0));
            QVERIFY2(countIndex >= 0, qPrintable(name));
            QCOMPARE(span.count, qint64(integer(bytes, countOffset + countIndex * 4)));
            const int width = name.endsWith(QStringLiteral(".ids")) ? 64
                : name == QStringLiteral("art_meshes.drawable_flags") ? 1
                : name == QStringLiteral("position_indices.indices") || name == QStringLiteral("glue_info.position_indices") ? 2 : 4;
            QCOMPARE(span.width, qint64(width));
            QCOMPARE(span.optional, optional.contains(name));
        }
        auto changed = spans;
        changed.first().offset = -1;
        changed.first().count = -1;
        const auto unchanged = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        QCOMPARE(unchanged.first().offset, spans.first().offset);
        QCOMPARE(unchanged.first().count, spans.first().count);
    }

    void arraySpansRejectMissingOffsetObjects()
    {
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        auto& offsets = *table.sectionOffsetTable;
        const auto checkMissing = [&](auto*& field) {
            auto* original = field;
            field = nullptr;
            const auto spans = CSectionValidation::ArraySpans(offsets, *table.count_info);
            field = original;
            QVERIFY(spans.isEmpty());
        };
        checkMissing(offsets.count_info);
        checkMissing(offsets.canvas_info);
        checkMissing(offsets.parts);
        checkMissing(offsets.deformers);
        checkMissing(offsets.warp_deformers);
        checkMissing(offsets.rotation_deformers);
        checkMissing(offsets.art_meshes);
        checkMissing(offsets.parameters);
        checkMissing(offsets.part_keyforms);
        checkMissing(offsets.warp_deformer_keyforms);
        checkMissing(offsets.rotation_deformer_keyforms);
        checkMissing(offsets.art_mesh_keyforms);
        checkMissing(offsets.keyform_positions);
        checkMissing(offsets.parameter_binding_indices);
        checkMissing(offsets.keyform_bindings);
        checkMissing(offsets.parameter_bindings);
        checkMissing(offsets.keys);
        checkMissing(offsets.uvs);
        checkMissing(offsets.position_indices);
        checkMissing(offsets.drawable_masks);
        checkMissing(offsets.draw_order_groups);
        checkMissing(offsets.draw_order_group_objects);
        checkMissing(offsets.glue);
        checkMissing(offsets.glue_info);
        checkMissing(offsets.glue_keyforms);
        checkMissing(offsets.keyform_multiply_colors);
        checkMissing(offsets.keyform_screen_colors);
        checkMissing(offsets.blend_shape_parameter_bindings);
        checkMissing(offsets.blend_shape_keyform_bindings);
        checkMissing(offsets.blend_shapes_warp_deformers);
        checkMissing(offsets.blend_shapes_art_meshes);
        checkMissing(offsets.blend_shape_constraint_indices);
        checkMissing(offsets.blend_shape_constraints);
        checkMissing(offsets.blend_shape_constraint_values);
        QCOMPARE(CSectionValidation::ArraySpans(offsets, *table.count_info).size(), 135);
    }

    void arraySpansPreservePhysicalAliases_data() { addModels(); }

    void arraySpansPreservePhysicalAliases()
    {
        QFETCH(QString, model);
        QByteArray bytes = sample(model);
        QVERIFY(bytes.size() >= 0x264);
        const auto names = physicalColumnNames();
        const int positionSlot = 0x48 + names.indexOf(QStringLiteral("keyform_positions.xys")) * 4;
        const int parameterSlot = 0x48 + names.indexOf(QStringLiteral("parameters.default_values")) * 4;
        QCOMPARE(positionSlot, 0x15c);
        QCOMPARE(parameterSlot, 0x114);
        const int positionOffset = integer(bytes, positionSlot);
        QVERIFY(positionOffset > 0);
        setInteger(bytes, parameterSlot, positionOffset);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("aliased.moc3"));
        QVERIFY(save(path, bytes));
        CReader reader(path);
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        QCOMPARE(spans.size(), 135);
        QSet<QString> aliases;
        for (const auto& span : spans) {
            if (span.offset != positionOffset || span.count == 0) continue;
            QCOMPARE(span.width, qint64(4));
            aliases.insert(QString::fromLatin1(span.name));
        }
        QVERIFY(aliases.contains(QStringLiteral("keyform_positions.xys")));
        QVERIFY(aliases.contains(QStringLiteral("parameters.default_values")));
        aliases.remove(QStringLiteral("keyform_positions.xys"));
        QVERIFY(aliases.contains(QStringLiteral("parameters.default_values")));
    }

    void originalSamplesPassIndependentCountChecks_data() { addModels(); }

    void originalSamplesPassIndependentCountChecks()
    {
        QFETCH(QString, model);
        const QByteArray bytes = sample(model);
        QVERIFY(bytes.size() >= 0x264);
        const int countOffset = integer(bytes, 0x40);
        const int canvasOffset = integer(bytes, 0x44);
        QVERIFY(countOffset >= 0x264 && countOffset <= bytes.size() - 128);
        QVERIFY(canvasOffset >= 0x264 && canvasOffset <= bytes.size() - 20);
        CReader reader(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QCOMPARE(table.parts.size(), integer(bytes, countOffset));
        QCOMPARE(table.art_meshes.size(), integer(bytes, countOffset + 16));
        QCOMPARE(table.parameters.size(), integer(bytes, countOffset + 20));
        QCOMPARE(table.position_indices->indices.size(), std::size_t(integer(bytes, countOffset + 64)));
    }

    void offsetTableRequiresEveryWordAndAdvancesConstraintColumns()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("offsets.moc3");
        QByteArray bytes = sample("Mark");
        QVERIFY(bytes.size() >= 0x264);
        const std::array<int, 5> sentinels{0x1001, 0x2002, 0x3003, 0x4004, 0x5005};
        for (int index = 0; index < 5; ++index) setInteger(bytes, 0x250 + index * 4, sentinels[index]);
        bytes.truncate(0x264);
        QVERIFY(save(path, bytes));
        {
            CReader reader(path);
            CSectionOffsetTable offsets(reader);
            QVERIFY2(reader.IsValid(), qPrintable(reader.Error()));
            QCOMPARE(reader.m_pos, 0x264);
            QCOMPARE(offsets.blend_shape_constraints->parameter_indices, sentinels[0]);
            QCOMPARE(offsets.blend_shape_constraints->blend_shape_constraint_value_sources_begin_indices, sentinels[1]);
            QCOMPARE(offsets.blend_shape_constraints->blend_shape_constraint_value_sources_counts, sentinels[2]);
            QCOMPARE(offsets.blend_shape_constraint_values->keys, sentinels[3]);
            QCOMPARE(offsets.blend_shape_constraint_values->weights, sentinels[4]);
        }
        bytes.chop(1);
        QVERIFY(save(path, bytes));
        {
            CReader reader(path);
            CSectionOffsetTable offsets(reader);
            QVERIFY(!reader.IsValid());
            QVERIFY(reader.m_pos <= bytes.size());
        }
        QString detail;
        QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
    }

    void metadataTruncationAndOverlapFailBeforeRows_data() { addModels(); }

    void metadataTruncationAndOverlapFailBeforeRows()
    {
        QFETCH(QString, model);
        const QByteArray source = sample(model);
        QVERIFY(source.size() >= 0x264);
        const int countOffset = integer(source, 0x40);
        const int canvasOffset = integer(source, 0x44);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("metadata.moc3");
        QString detail;
        for (int length : {0x263, countOffset + 127, canvasOffset + 19})
            QVERIFY2(rejectedBeforeAllocation(path, source.left(length), &detail), qPrintable(detail));
        const std::pair<int, int> invalid[]{{0x40, 0}, {0x40, 16}, {0x40, 0x80},
            {0x44, 0}, {0x44, 16}, {0x44, 0x260}, {0x44, countOffset},
            {0x44, countOffset + 124}, {0x40, canvasOffset}, {0x40, canvasOffset - 124}};
        for (const auto& mutation : invalid) {
            QByteArray bytes = source;
            setInteger(bytes, mutation.first, mutation.second);
            QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
        }
    }

    void requiredColumnsRejectInvalidSpansBeforeRows_data() { addModels(); }

    void requiredColumnsRejectInvalidSpansBeforeRows()
    {
        QFETCH(QString, model);
        const QByteArray source = sample(model);
        QVERIFY(source.size() >= 0x264);
        const int countOffset = integer(source, 0x40);
        const int canvasOffset = integer(source, 0x44);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("required.moc3");
        QString detail;
        for (const ColumnFixture& fixture : columns) {
            const int span = integer(source, countOffset + fixture.countIndex * 4) * fixture.width;
            QVERIFY(span > 1 && span < source.size());
            const std::array<int, 10> invalid{0, -1, 8, 0x44, countOffset, canvasOffset,
                static_cast<int>(source.size()), std::numeric_limits<int>::max() - 3,
                static_cast<int>(source.size()) - span + 1, countOffset - qMin(span - 1, 64)};
            for (int offset : invalid) {
                QByteArray bytes = source;
                setInteger(bytes, fixture.slot, offset);
                QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
            }
        }
        for (int countIndex = 0; countIndex < 32; ++countIndex) {
            for (int invalidCount : {-1, 1000001, std::numeric_limits<int>::max()}) {
                QByteArray bytes = source;
                setInteger(bytes, countOffset + countIndex * 4, invalidCount);
                QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
            }
        }
    }

    void columnWidthsAcceptExactEofAndRejectOneMissingByte_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<int>("column");
        for (const QString& model : {QString("Mark"), QString("Rice"), QString("Natori"), QString("Wanko")})
            for (int column = 0; column < 4; ++column)
                QTest::newRow(qPrintable(model + '-' + QString::number(columns[column].width))) << model << column;
    }

    void columnWidthsAcceptExactEofAndRejectOneMissingByte()
    {
        QFETCH(QString, model);
        QFETCH(int, column);
        const QByteArray source = sample(model);
        QVERIFY(source.size() >= 0x264);
        const auto fixture = columns[column];
        const int count = integer(source, integer(source, 0x40) + fixture.countIndex * 4);
        const int originalOffset = integer(source, fixture.slot);
        QVERIFY(count > 0 && originalOffset > 0 && originalOffset <= source.size() - count * fixture.width);
        QByteArray bytes = source;
        const int appendedOffset = bytes.size();
        bytes.append(source.mid(originalOffset, count * fixture.width));
        setInteger(bytes, fixture.slot, appendedOffset);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("width.moc3");
        QVERIFY(save(path, bytes));
        {
            CReader reader(path);
            CSectionTable table(reader);
            QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
            if (fixture.width == 64) {
                const QByteArray id = source.mid(originalOffset + (count - 1) * 64, 64);
                QCOMPARE(table.art_meshes.last()->ids, QString::fromUtf8(id.constData()));
            } else if (fixture.width == 4) {
                QCOMPARE(table.art_meshes.last()->vertex_counts, integer(source, originalOffset + (count - 1) * 4));
            } else if (fixture.width == 2) {
                QCOMPARE(table.position_indices->indices.back(), qFromLittleEndian<qint16>(source.constData() + originalOffset + (count - 1) * 2));
            } else {
                QCOMPARE(table.art_meshes.last()->drawable_flags, int(static_cast<quint8>(source[originalOffset + count - 1])));
            }
        }
        bytes.chop(1);
        QString detail;
        QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
    }

    void optionalColumnsAllowZeroAndCheckNonzeroSpans_data() { addModels(); }

    void optionalColumnsAllowZeroAndCheckNonzeroSpans()
    {
        QFETCH(QString, model);
        const QByteArray source = sample(model);
        QVERIFY(source.size() >= 0x264);
        QByteArray absent = source;
        for (int slot : parameterExtensions) setInteger(absent, slot, 0);
        setInteger(absent, 0x1d4, 0);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("optional.moc3");
        QVERIFY(save(path, absent));
        {
            CReader reader(path);
            CSectionTable table(reader);
            QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
            for (const auto* parameter : table.parameters) {
                QCOMPARE(parameter->parameter_types, 0);
                QCOMPARE(parameter->extension_runtime_space_0, 0);
                QCOMPARE(parameter->keys_sources_begin_indices, 0);
                QCOMPARE(parameter->keys_sources_counts, 0);
                QCOMPARE(parameter->blend_shape_parameter_binding_sources_begin_indices, 0);
                QCOMPARE(parameter->blend_shape_parameter_binding_sources_counts, 0);
            }
            for (const auto* warp : table.warp_deformers) QVERIFY(!warp->is_quad_source);
        }
        QString detail;
        for (int slot : {0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x208, 0x20c, 0x210}) {
            const int countIndex = slot == 0x1d4 ? 2 : 5;
            const int count = integer(source, integer(source, 0x40) + countIndex * 4);
            QVERIFY(count > 0);
            QByteArray present = absent;
            const int offset = present.size();
            present.append(QByteArray(count * 4, '\0'));
            setInteger(present, slot, offset);
            QVERIFY(save(path, present));
            {
                CReader reader(path);
                CSectionTable table(reader);
                QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
            }
            present.chop(1);
            QVERIFY2(rejectedBeforeAllocation(path, present, &detail), qPrintable(detail));
        }
    }

    void zeroCountColumnsStillValidateTheirNonzeroOffsets_data() { addModels(); }

    void zeroCountColumnsStillValidateTheirNonzeroOffsets()
    {
        QFETCH(QString, model);
        const QByteArray source = sample(model);
        QVERIFY(source.size() >= 0x264);
        QCOMPARE(integer(source, integer(source, 0x40) + 23 * 4), 0);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("empty.moc3");
        for (int offset : {0, 16, 0x40, static_cast<int>(source.size())}) {
            QByteArray bytes = source;
            setInteger(bytes, 0x1f0, offset);
            QVERIFY(save(path, bytes));
            CReader reader(path);
            CSectionTable table(reader);
            QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        }
        QString detail;
        for (int offset : {-1, static_cast<int>(source.size()) + 1, std::numeric_limits<int>::max()}) {
            QByteArray bytes = source;
            setInteger(bytes, 0x1f0, offset);
            QVERIFY2(rejectedBeforeAllocation(path, bytes, &detail), qPrintable(detail));
        }
    }

    void failedDocumentLoadPreservesTheCurrentDocument()
    {
        CModelDocument document;
        QString error;
        const QString originalPath = QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3");
        QVERIFY2(document.Load(originalPath, &error), qPrintable(error));
        QVERIFY(!document.Drawables().isEmpty());
        const QString id = document.Drawables().first().id;
        QVERIFY(document.SetDrawableOpacity(id, 0.375f));
        const auto opacity = document.DrawableOpacityOverrides();
        const auto canvas = document.CanvasPixels();
        const auto drawableCount = document.Drawables().size();
        const auto parameterCount = document.Parameters().size();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("invalid.moc3");
        for (const QString& model : {QString("Mark"), QString("Rice"), QString("Natori"), QString("Wanko")}) {
            QByteArray bytes = sample(model);
            QVERIFY(bytes.size() >= 0x264);
            setInteger(bytes, 0xc4, 0);
            QVERIFY(save(path, bytes));
            QVERIFY(!document.Load(path, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(document.IsLoaded());
            QCOMPARE(document.SourcePath(), originalPath);
            QCOMPARE(document.CanvasPixels(), canvas);
            QCOMPARE(document.Drawables().size(), drawableCount);
            QCOMPARE(document.Parameters().size(), parameterCount);
            QCOMPARE(document.Drawables().first().id, id);
            QCOMPARE(document.DrawableOpacityOverrides(), opacity);
        }
    }

    void readerSpanArithmeticAndErrorsAreSticky()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("reader.bin");
        QVERIFY(save(path, QByteArray(64, 'x')));
        const std::array<std::array<qint64, 3>, 5> valid{{{0, 64, 1}, {0, 1, 64}, {8, 7, 8}, {64, 0, 4}, {0, 0, std::numeric_limits<qint64>::max()}}};
        for (const auto& span : valid) {
            CReader reader(path);
            reader.seek(7);
            QVERIFY(reader.RequireSpan("valid", span[0], span[1], span[2]));
            QVERIFY(reader.IsValid());
            QCOMPARE(reader.m_pos, 7);
        }
        const qint64 maximum = std::numeric_limits<qint64>::max();
        const std::array<std::array<qint64, 3>, 10> invalid{{{-1, 1, 1}, {65, 0, 1}, {0, -1, 1}, {0, 1, 0}, {0, 1, -1},
            {63, 1, 2}, {0, maximum, 64}, {maximum, maximum, maximum}, {1, 2, maximum}, {64, 1, 1}}};
        for (const auto& span : invalid) {
            CReader reader(path);
            reader.seek(7);
            QVERIFY(!reader.RequireSpan("original-span", span[0], span[1], span[2]));
            const QString firstError = reader.Error();
            QVERIFY(firstError.contains("original-span"));
            QCOMPARE(reader.m_pos, 7);
            QVERIFY(!reader.Fail("second error"));
            reader.seek(-1);
            reader.seek(0);
            QCOMPARE(reader.readInt(), 0);
            QCOMPARE(reader.readUByte(), uint8_t(0));
            QVERIFY(!reader.RequireSpan("later-span", 0, 1, 1));
            QCOMPARE(reader.Error(), firstError);
            QCOMPARE(reader.m_pos, 7);
        }
    }

    void failedSeekAndReadsKeepTheFirstErrorAndCursor()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("reader.bin");
        QVERIFY(save(path, QByteArray(64, 'x')));
        for (int mode = 0; mode < 8; ++mode) {
            CReader reader(path);
            const int position = mode == 3 ? 64 : 63;
            reader.seek(position);
            switch (mode) {
            case 0: reader.readInt(); break;
            case 1: reader.readFloat(); break;
            case 2: reader.readShort(); break;
            case 3: reader.readUByte(); break;
            case 4: reader.readUShort(); break;
            case 5: reader.readBoolean(); break;
            case 6: reader.readString(); break;
            default: reader.seek(-1); break;
            }
            QVERIFY(!reader.IsValid());
            const QString firstError = reader.Error();
            QVERIFY(!firstError.isEmpty());
            QCOMPARE(reader.m_pos, position);
            reader.seek(std::numeric_limits<int>::max());
            reader.seek(0);
            QCOMPARE(reader.readFloat(), 0.0f);
            QCOMPARE(reader.readString(), QString());
            QVERIFY(!reader.Fail("later failure"));
            QCOMPARE(reader.Error(), firstError);
            QCOMPARE(reader.m_pos, position);
        }
    }

    void oversizedSparseFilesAreRejectedBeforeReading()
    {
        QTemporaryFile file;
        if (!file.open()) QSKIP("The platform could not create a temporary sparse file.");
        const qint64 size = qint64(std::numeric_limits<int>::max()) + 1;
        if (!file.resize(size)) QSKIP("The platform could not resize a sparse file beyond the reader's limit.");
        QCOMPARE(file.size(), size);
        const QString path = file.fileName();
        file.close();
        CReader reader(path);
        QVERIFY(!reader.IsValid());
        QVERIFY(reader.m_byte.isEmpty());
        QVERIFY(!reader.Error().isEmpty());
        QCOMPARE(reader.m_pos, 0);
    }
};

QTEST_GUILESS_MAIN(SectionPreflightTests)
#include "section_preflight_tests.moc"
