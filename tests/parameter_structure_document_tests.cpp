#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmotiontimeline.h"
#include "live2dstruct/csectionvalidation.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
void models()
{
    QTest::addColumn<QString>("name");
    for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
}

QString path(const QString& name) { return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3"; }
int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }

Builder::CreateRequest request(const QString& id)
{
    Builder::CreateRequest result;
    result.parameter.id = id;
    result.parameter.minimum = -2;
    result.parameter.maximum = 3;
    result.parameter.defaultValue = 0.25f;
    result.parameter.decimalPlaces = 4;
    result.parameter.repeat = true;
    return result;
}

QByteArray fiveOwnerFixture()
{
    DeformerSnapshotData::Fixture fixture;
    const auto& offsets = *fixture.table.sectionOffsetTable;
    fixture.BindRoot(false);
    fixture.column(offsets.rotation_deformers->keyform_binding_sources_indices, 0, 0);
    fixture.column(offsets.art_meshes->keyform_binding_sources_indices, 0, 0);
    fixture.column(offsets.art_meshes->keyform_sources_counts, 0, 2);
    fixture.count(0, 2); fixture.count(6, 2);
    for (int index = 0; index < 2; ++index) {
        fixture.id(offsets.parts->ids, index, index ? "PartChild" : "PartParent");
        fixture.column(offsets.parts->keyform_binding_sources_indices, index, index ? 0 : -1);
        fixture.column(offsets.parts->keyform_sources_begin_indices, index, 0);
        fixture.column(offsets.parts->keyform_sources_counts, index, index ? 2 : 1);
        fixture.column(offsets.parts->is_visible, index, 1); fixture.column(offsets.parts->is_enabled, index, 1);
        fixture.column(offsets.parts->parent_part_indices, index, index - 1);
    }
    fixture.appendColumn(0x128, {0, 0});
    DeformerSnapshotData::real(fixture.bytes, word(fixture.bytes, 0x128), 100);
    DeformerSnapshotData::real(fixture.bytes, word(fixture.bytes, 0x128) + 4, 200);
    fixture.count(4, 2);
    for (const auto& span : CSectionValidation::ArraySpans(offsets, *fixture.table.count_info))
        if (QByteArray(span.name).startsWith("art_meshes.") && span.offset)
            fixture.bytes.replace(span.offset + span.width, span.width, fixture.bytes.mid(span.offset, span.width));
    fixture.id(offsets.art_meshes->ids, 1, "MeshOther");
    for (int mesh = 0; mesh < 2; ++mesh) fixture.column(offsets.art_meshes->parent_part_indices, mesh, 1);
    fixture.count(7, 2);
    fixture.column(offsets.deformers->keyform_binding_sources_indices, 1, 0);
    fixture.column(offsets.warp_deformers->keyform_binding_sources_indices, 0, 0);
    fixture.column(offsets.warp_deformers->keyform_sources_counts, 0, 2);
    fixture.column(offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, 1, 12);
    DeformerSnapshotData::real(fixture.bytes, offsets.warp_deformer_keyforms->opacities + 4, 0.5f);
    fixture.count(20, 1); fixture.count(21, 1); fixture.count(22, 2);
    fixture.appendColumn(0x1a4, {0}); fixture.appendColumn(0x1a8, QVector<int>(16, 0));
    fixture.id(word(fixture.bytes, 0x1a8), 0, "GlueOne");
    fixture.appendColumn(0x1ac, {0}); fixture.appendColumn(0x1b0, {0}); fixture.appendColumn(0x1b4, {2});
    fixture.appendColumn(0x1b8, {0}); fixture.appendColumn(0x1bc, {1});
    fixture.appendColumn(0x1c0, {0}); fixture.appendColumn(0x1c4, {1});
    fixture.appendColumn(0x1c8, {0}); fixture.appendColumn(0x1cc, {0}); fixture.appendColumn(0x1d0, {0, 0});
    DeformerSnapshotData::real(fixture.bytes, word(fixture.bytes, 0x1c8), 0.75f);
    DeformerSnapshotData::real(fixture.bytes, word(fixture.bytes, 0x1d0), 0.25f);
    DeformerSnapshotData::real(fixture.bytes, word(fixture.bytes, 0x1d0) + 4, 0.75f);
    return fixture.bytes;
}

CModelSession session(const CModelDocument& document)
{
    CModelSession result;
    result.sourceMoc = document.SourcePath();
    result.sourceDigest = document.SourceDigest();
    result.structure = document.ParameterStructure();
    result.version = result.structure.columns.isEmpty() && result.structure.counts.isEmpty() ? 1 : 2;
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it) result.parameters.insert(it.key(), it.value());
    result.parameterMetadata = document.ParameterMetadataOverrides();
    result.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) result.locked.insert(key, true);
    result.drawableOpacity = document.DrawableOpacityOverrides();
    result.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    result.drawableIndices = document.DrawableIndexOverrides();
    result.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    result.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    result.deformerParentIndices = document.DeformerParentOverrides();
    result.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    result.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    return result;
}
}

class ParameterStructureDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void deletionFreezesFiveOwnerKindsAndRestoresExactHistory()
    {
        CModelDocument document;
        QString error;
        const auto bytes = fiveOwnerFixture();
        QVERIFY2(document.LoadBytes(bytes, "five-owner.moc3", &error), qPrintable(error));
        auto positions = document.DrawableKeyformPositions("Mesh", 1);
        QVERIFY(!positions.isEmpty());
        positions[0] += 0.2f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, positions));
        QVERIFY(document.SetParameterValue("P", 0.75f));
        const auto before = document.ParameterStructureState();
        const auto first = document.DrawableKeyformPositions("Mesh", 0);
        CModelEdit removed;
        QVERIFY2(document.MakeDeleteParameterEdit(Builder::DeleteRequest{"P", 0.5f}, removed, &error), qPrintable(error));
        QCOMPARE(removed.parameterChange, CModelEdit::ParameterChange::Delete);
        QVERIFY(removed.freezeValue.has_value()); QCOMPARE(*removed.freezeValue, 0.5f);
        QCOMPARE(document.ParameterStructureState(), before);
        QVERIFY(document.ExecuteEdit(removed));
        QVERIFY(document.Parameters().isEmpty());
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 0)[0], (first[0] + positions[0]) * 0.5f);
        QByteArray frozen;
        QVERIFY(document.BuildPreviewMoc(frozen, &error));
        auto reader = CReader::FromBytes(frozen); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QCOMPARE(table.parts[1]->keyform_sources_counts, 1);
        QCOMPARE(table.part_keyforms[table.parts[1]->keyform_sources_begin_indices]->draw_orders, 150.0f);
        QCOMPARE(table.glue[0]->keyform_sources_counts, 1);
        QCOMPARE(table.glue_keyforms[table.glue[0]->keyform_sources_begin_indices]->intensities, 0.5f);
        QCOMPARE(table.glue_info[0]->weights, 0.75f);
        QCOMPARE(table.glue[0]->art_mesh_indices_a, 0); QCOMPARE(table.glue[0]->art_mesh_indices_b, 1);
        QCOMPARE(document.SourceBytes(), bytes);
        const auto after = document.ParameterStructureState();
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterStructureState(), before);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterStructureState(), after);
        CModelDocument restored;
        QVERIFY2(session(document).RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.ParameterStructureState(), after);
    }

    void deletionChecksEveryOwnerAndItsAncestors_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("id"); QTest::addColumn<int>("auxiliaryOnly");
        QTest::newRow("mesh") << QString("artmesh") << QString("Mesh") << 0;
        QTest::newRow("warp") << QString("warp") << QString("Warp") << 0;
        QTest::newRow("rotation") << QString("rotation") << QString("Root") << 0;
        QTest::newRow("part") << QString("part") << QString("PartChild") << 0;
        QTest::newRow("part-ancestor") << QString("part") << QString("PartParent") << 0;
        QTest::newRow("glue") << QString("glue") << QString("GlueOne") << 0;
        QTest::newRow("glue-other-mesh") << QString("artmesh") << QString("MeshOther") << 0;
        QTest::newRow("deformer-ancestor") << QString("rotation") << QString("Child") << 0;
        QTest::newRow("only-part") << QString("part") << QString("PartChild") << 1;
        QTest::newRow("only-glue-endpoint") << QString("artmesh") << QString("MeshOther") << 2;
    }

    void deletionChecksEveryOwnerAndItsAncestors()
    {
        QFETCH(QString, type); QFETCH(QString, id); QFETCH(int, auxiliaryOnly);
        CModelDocument document;
        QString error;
        auto bytes = fiveOwnerFixture();
        if (auxiliaryOnly) {
            auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
            for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
                const QByteArray name(span.name);
                const bool preserve = auxiliaryOnly == 1 ? name.startsWith("parts.") : name.startsWith("glue.");
                if (preserve) continue;
                if (name.endsWith(".keyform_binding_sources_indices"))
                    for (int row = 0; row < span.count; ++row) put(bytes, span.offset + row * 4, -1);
                if (name.endsWith(".keyform_sources_counts"))
                    for (int row = 0; row < span.count; ++row) put(bytes, span.offset + row * 4, 1);
            }
            if (auxiliaryOnly == 1)
                for (int row = 0; row < table.art_meshes.size(); ++row)
                    put(bytes, table.sectionOffsetTable->art_meshes->parent_part_indices + row * 4, -1);
        }
        QVERIFY2(document.LoadBytes(bytes, "locked.moc3", &error), qPrintable(error));
        document.SetLocked(type, id, true);
        const auto state = document.ParameterStructureState(); const auto revision = document.Revision();
        CModelEdit output; output.name = "sentinel";
        QVERIFY(!document.MakeDeleteParameterEdit(Builder::DeleteRequest{"P", 0.5f}, output, &error));
        QVERIFY(error.contains(QStringLiteral("锁定")));
        QCOMPARE(output.name, QString("sentinel")); QCOMPARE(document.ParameterStructureState(), state); QCOMPARE(document.Revision(), revision);
    }

    void auxiliaryIdentityCannotBeRewrittenByAState_data()
    {
        QTest::addColumn<int>("slot");
        for (int slot : {0x4c, 0x5c, 0x60, 0x64, 0x1a8, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc})
            QTest::newRow(qPrintable(QString::number(slot, 16))) << slot;
    }

    void auxiliaryIdentityCannotBeRewrittenByAState()
    {
        QFETCH(int, slot);
        CModelDocument document;
        QString error;
        const auto original = fiveOwnerFixture();
        QVERIFY(document.LoadBytes(original, "identity.moc3", &error));
        Builder::ColumnState state;
        if (slot == 0x4c) state.columns[slot] = original.mid(word(original, slot), 128);
        else if (slot == 0x1a8) state.columns[slot] = original.mid(word(original, slot), 64);
        else state.columns[slot] = original.mid(word(original, slot), slot == 0x1cc ? 2 : (slot < 0x100 ? 8 : 4));
        state.columns[slot][0] = char(quint8(state.columns[slot][0]) ^ 1);
        const auto before = document.ParameterStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.SetParameterStructure(state, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(document.ParameterStructureState(), before); QCOMPARE(document.Revision(), revision);
    }

    void auxiliaryKeyformRangesAndNumbersAreChecked_data()
    {
        QTest::addColumn<int>("slot"); QTest::addColumn<int>("bits");
        QTest::newRow("part-begin") << 0x54 << std::numeric_limits<int>::max();
        QTest::newRow("part-count") << 0x58 << -1;
        QTest::newRow("part-nan") << 0x128 << 0x7fc00000;
        QTest::newRow("glue-begin") << 0x1b0 << std::numeric_limits<int>::max();
        QTest::newRow("glue-count") << 0x1b4 << -1;
        QTest::newRow("glue-infinity") << 0x1d0 << 0x7f800000;
    }

    void auxiliaryKeyformRangesAndNumbersAreChecked()
    {
        QFETCH(int, slot); QFETCH(int, bits);
        const auto bytes = fiveOwnerFixture();
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "ranges.moc3", &error));
        Builder::ColumnState state;
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            if (span.slot == slot) state.columns[slot] = bytes.mid(span.offset, span.count * span.width);
        QVERIFY(state.columns.contains(slot)); put(state.columns[slot], 0, bits);
        const auto original = document.ParameterStructureState();
        QVERIFY(!document.SetParameterStructure(state, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(document.ParameterStructureState(), original);
    }

    void parameterChangeIntentMustMatchTheChangedCatalog()
    {
        CModelDocument document;
        QVERIFY(document.Load(path("Mark")));
        CModelEdit create;
        QVERIFY(document.MakeCreateParameterEdit(request("ParamIntent"), {}, create));
        QCOMPARE(create.parameterChange, CModelEdit::ParameterChange::Create);
        QVERIFY(!create.freezeValue);
        const auto before = document.ParameterStructureState();
        auto forged = create; forged.parameterChange = CModelEdit::ParameterChange::Delete;
        QVERIFY(!document.ExecuteEdit(forged));
        forged = create; forged.parameterChange = CModelEdit::ParameterChange::Bind;
        QVERIFY(!document.ExecuteEdit(forged));
        forged = create; forged.freezeValue = 0;
        QVERIFY(!document.ExecuteEdit(forged));
        QCOMPARE(document.ParameterStructureState(), before);
        QVERIFY(document.ExecuteEdit(create));
        CModelEdit removed;
        QVERIFY(document.MakeDeleteParameterEdit("ParamIntent", removed));
        QCOMPARE(removed.parameterChange, CModelEdit::ParameterChange::Delete);
        QVERIFY(!removed.freezeValue);
        QVERIFY(document.ExecuteEdit(removed));
        CModelEdit restored;
        QVERIFY(document.UndoEdit(&restored));
        QCOMPARE(restored.parameterChange, CModelEdit::ParameterChange::Delete);
        QVERIFY(document.UndoEdit(&restored));
        QCOMPARE(restored.parameterChange, CModelEdit::ParameterChange::Create);
    }

    void malformedParameterHistoryIsRejectedAtomically_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 6; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }

    void malformedParameterHistoryIsRejectedAtomically()
    {
        QFETCH(int, kind);
        CModelDocument document;
        QVERIFY(document.Load(path("Mark")));
        const auto first = document.Parameters()[0], second = document.Parameters()[1];
        CModelEdit valid{first.id, "parameter", CModelEdit::ParameterValue, first.defaultValue, first.maximum, -1, false};
        QVERIFY(document.ExecuteEdit(valid));
        CModelEdit command;
        command.name = "invalid";
        command.type = "parameters";
        command.field = CModelEdit::ParameterValues;
        QMap<QString, float> before{{first.id, first.defaultValue}}, after{{first.id, first.minimum}};
        if (kind == 0) after = {{second.id, second.maximum}};
        if (kind == 1) before.insert(second.id, second.defaultValue);
        if (kind == 2) before[first.id] = std::numeric_limits<float>::quiet_NaN();
        if (kind == 3) before = {{"Missing", 0}}, after = {{"Missing", 1}};
        command.before = QVariant::fromValue(before);
        command.after = QVariant::fromValue(after);
        if (kind >= 4) {
            command.name = first.id;
            command.type = "parameter";
            command.field = CModelEdit::ParameterValue;
            command.before = kind == 4 ? std::numeric_limits<float>::infinity() : first.maximum + 1;
            command.after = first.minimum;
        }
        const auto state = document.ParameterStructureState();
        const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(command));
        QCOMPARE(document.ParameterStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        CModelEdit pending;
        QVERIFY(document.PeekUndoEdit(&pending));
        QCOMPARE(pending.name, first.id);
        QCOMPARE(pending.after.toFloat(), first.maximum);
        QVERIFY(document.UndoEdit());
        QVERIFY(!document.CanUndoEdit());
    }

    void ordinaryHistoryRestoresSparseStorageBeforeStructuralUndo_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<bool>("explicitBefore");
        for (const char* field : {"parameter", "parameters", "metadata", "opacity", "drawOrder", "positions", "indices",
                                 "masks", "inverted", "parent", "warp", "rotation"}) {
            QTest::newRow(field) << QString::fromLatin1(field) << false;
            if (QStringList{"parameter", "parameters", "metadata", "opacity", "drawOrder", "indices"}.contains(QString::fromLatin1(field)))
                QTest::newRow(qPrintable(QString::fromLatin1(field) + "-explicit")) << QString::fromLatin1(field) << true;
        }
    }

    void ordinaryHistoryRestoresSparseStorageBeforeStructuralUndo()
    {
        QFETCH(QString, field);
        QFETCH(bool, explicitBefore);
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path("Mark"), &error));
        const auto mesh = document.Drawables().front();
        const QString parameter = document.Parameters().front().id;
        const QString other = document.Parameters()[1].id;
        if (explicitBefore) {
            if (field == "parameter" || field == "parameters") {
                QVERIFY(document.SetParameterValue(parameter, document.ParameterValue(parameter)));
                if (field == "parameters") QVERIFY(document.SetParameterValue(other, document.ParameterValue(other)));
            } else if (field == "metadata") {
                auto metadata = document.ParameterMetadata(parameter); metadata.displayName = "保留名称";
                QVERIFY(document.SetParameterMetadata(parameter, metadata));
            } else if (field == "opacity") QVERIFY(document.SetDrawableOpacity(mesh.id, document.DrawableOpacity(mesh)));
            else if (field == "drawOrder") QVERIFY(document.SetDrawableDrawOrder(mesh.id, 37));
            else if (field == "indices") QVERIFY(document.SetDrawableIndices(mesh.id, mesh.indices));
        }
        const auto original = document.ParameterStructureState();
        CModelEdit create;
        QVERIFY(document.MakeCreateParameterEdit(request("ParamHistoryBoundary"), {}, create, &error));
        QVERIFY(document.ExecuteEdit(create));
        const auto boundary = document.ParameterStructureState();
        CModelEdit edit;
        edit.name = mesh.id;
        edit.type = "artmesh";
        edit.coalesce = false;
        if (field == "parameter") {
            edit.name = parameter; edit.type = "parameter"; edit.field = CModelEdit::ParameterValue;
            edit.before = document.ParameterValue(parameter); edit.after = document.ParameterMetadata(parameter).maximum;
        } else if (field == "parameters") {
            edit.name = "batch"; edit.type = "parameters"; edit.field = CModelEdit::ParameterValues;
            edit.before = QVariant::fromValue(QMap<QString, float>{{parameter, document.ParameterValue(parameter)}, {other, document.ParameterValue(other)}});
            edit.after = QVariant::fromValue(QMap<QString, float>{{parameter, document.ParameterMetadata(parameter).maximum}, {other, document.ParameterMetadata(other).maximum}});
        } else if (field == "metadata") {
            auto metadata = document.ParameterMetadata(parameter); metadata.maximum += 1;
            QVERIFY(document.MakeParameterMetadataEdit(parameter, metadata, edit, &error));
        } else if (field == "opacity") {
            edit.field = CModelEdit::Opacity; edit.before = document.DrawableOpacity(mesh); edit.after = 0.375f;
        } else if (field == "drawOrder") {
            edit.field = CModelEdit::DrawOrder; edit.before = document.DrawableDrawOrder(mesh); edit.after = 47;
        } else if (field == "positions") {
            edit.field = CModelEdit::KeyformPositions; edit.index = 0;
            auto positions = document.DrawableKeyformPositions(mesh.id, 0); edit.before = QVariant::fromValue(positions);
            positions[0] += 0.125f; edit.after = QVariant::fromValue(positions);
        } else if (field == "indices") {
            edit.field = CModelEdit::MeshIndices;
            auto indices = document.DrawableIndices(mesh.id); edit.before = QVariant::fromValue(indices);
            QVERIFY(indices.size() >= 3); std::rotate(indices.begin(), indices.begin() + 1, indices.begin() + 3);
            edit.after = QVariant::fromValue(indices);
        } else if (field == "masks") {
            edit.field = CModelEdit::MaskIndices; edit.before = QVariant::fromValue(document.DrawableMaskIndices(mesh.id));
            edit.after = QVariant::fromValue(QVector<int>{1, 2});
        } else if (field == "inverted") {
            edit.field = CModelEdit::MaskInverted; edit.before = document.DrawableMaskInverted(mesh.id); edit.after = !edit.before.toBool();
        } else {
            auto target = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& value) {
                return field == "parent" ? value.parentDeformerIndex >= 0 :
                    field == "warp" ? value.type == CModelDeformer::Warp && !value.warpKeyforms.isEmpty() :
                                      value.type == CModelDeformer::Rotation && !value.rotationKeyforms.isEmpty();
            });
            QVERIFY(target != document.Deformers().cend());
            edit.name = target->id; edit.type = target->type == CModelDeformer::Warp ? "warp" : "rotation";
            if (field == "parent") {
                edit.field = CModelEdit::DeformerParent; edit.before = document.DeformerParentIndex(edit.name); edit.after = -1;
            } else if (field == "warp") {
                edit.field = CModelEdit::WarpKeyforms;
                auto forms = document.DeformerWarpKeyforms(edit.name); edit.before = QVariant::fromValue(forms);
                forms[0].positions[0] += 0.125f; edit.after = QVariant::fromValue(forms);
            } else {
                edit.field = CModelEdit::RotationKeyforms;
                auto forms = document.DeformerRotationKeyforms(edit.name); edit.before = QVariant::fromValue(forms);
                forms[0].angle += 1; edit.after = QVariant::fromValue(forms);
            }
        }
        QVERIFY(document.ExecuteEdit(edit));
        const auto changed = document.ParameterStructureState();
        QVERIFY(changed != boundary);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterStructureState(), boundary);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterStructureState(), original);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterStructureState(), boundary);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterStructureState(), changed);
    }

    void createsAndBakesOverlaysWithExactUndo_data() { models(); }
    void createsAndBakesOverlaysWithExactUndo()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path(name), &error), qPrintable(error));
        const auto source = document.SourceBytes(), digest = document.SourceDigest();
        const auto mesh = document.Drawables().front();
        const auto parameter = document.Parameters().front();
        auto positions = mesh.keyforms.front().positions;
        positions[0] += 0.03125f;
        QVERIFY(document.SetDrawableKeyformPositions(mesh.id, 0, positions));
        QVERIFY(document.SetDrawableMaskIndices(mesh.id, {1}));
        QVERIFY(document.SetDrawableMaskInverted(mesh.id, true));
        QVERIFY(document.SetDrawableIndices(mesh.id, mesh.indices));
        QVERIFY(document.SetDrawableOpacity(mesh.id, 0.625f));
        QVERIFY(document.SetDrawableDrawOrder(mesh.id, 37));
        QVERIFY(document.SetDrawableVisible(mesh.id, !mesh.visible));
        auto metadata = document.ParameterMetadata(parameter.id);
        metadata.maximum += 1;
        metadata.displayName = QStringLiteral("保留的名称");
        QVERIFY(document.SetParameterMetadata(parameter.id, metadata));
        QVERIFY(document.SetParameterValue(parameter.id, -0.0f));
        document.SetLocked("artmesh", document.Drawables()[1].id, true);
        const auto before = document.ParameterStructureState();
        const auto revision = document.Revision();
        auto create = request("ParamDocumentCreated");
        create.bindings.append(Builder::BindingRequest{{Builder::Owner::ArtMesh, mesh.id}, {-2, 3}});
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(create, QStringLiteral("创建参数"), edit, &error), qPrintable(error));
        QCOMPARE(document.ParameterStructureState(), before);
        QCOMPARE(document.Revision(), revision);
        edit.runtime = std::make_shared<CModelEditRuntimeState>();
        edit.runtime->before = QStringLiteral("runtime-only");
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.SourceBytes(), source);
        QCOMPARE(document.SourceDigest(), digest);
        QCOMPARE(document.SourcePath(), path(name));
        QCOMPARE(document.Parameters().last().id, create.parameter.id);
        QCOMPARE(document.ParameterValue(create.parameter.id), create.parameter.defaultValue);
        QCOMPARE(document.ParameterMetadata(create.parameter.id).displayName, QStringLiteral("创建参数"));
        QCOMPARE(document.ParameterMetadata(parameter.id).displayName, metadata.displayName);
        QVERIFY(std::signbit(document.ParameterValue(parameter.id)));
        QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
        QVERIFY(document.DrawableMaskIndexOverrides().isEmpty());
        QVERIFY(document.DrawableMaskInvertedOverrides().isEmpty());
        QCOMPARE(document.DrawableIndexOverrides().value(mesh.id), mesh.indices);
        QCOMPARE(document.DrawableOpacityOverrides().value(mesh.id), 0.625f);
        QCOMPARE(document.DrawableDrawOrderOverrides().value(mesh.id), 37);
        QCOMPARE(document.DrawableMaskIndices(mesh.id), QVector<int>{1});
        QVERIFY(document.DrawableMaskInverted(mesh.id));
        QCOMPARE(document.DrawableKeyformPositions(mesh.id, 0), positions);
        QCOMPARE(document.Drawables().front().keyforms.size(), mesh.keyforms.size() * 2);
        CModelEdit stored;
        QVERIFY(document.PeekUndoEdit(&stored));
        QCOMPARE(stored.runtime, edit.runtime);
        const auto after = document.ParameterStructureState();
        stored.runtime->after = QStringLiteral("runtime-later");
        QCOMPARE(document.ParameterStructureState(), after);
        QVERIFY(!session(document).ToJson().contains("runtime-only"));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterStructureState(), before);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterStructureState(), after);
    }

    void sessionVersionTwoRebuildsFromOriginalSource_data() { models(); }
    void sessionVersionTwoRebuildsFromOriginalSource()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path(name), &error));
        const auto source = document.SourceBytes(), digest = document.SourceDigest();
        CModelEdit create;
        QVERIFY2(document.MakeCreateParameterEdit(request("ParamSessionCreated"), "会话参数", create, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(create));
        QVERIFY(document.SetParameterValue("ParamSessionCreated", 0.375f));
        QByteArray expected;
        QVERIFY(document.BuildPreviewMoc(expected, &error));
        const auto saved = session(document);
        const auto json = saved.ToJson();
        QCOMPARE(QJsonDocument::fromJson(json).object().value("version").toInt(), 2);
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(json, &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.structure, saved.structure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(saved.Save(directory.filePath("project.session"), &error));
        CModelSession loaded;
        QVERIFY(CModelSession::Load(directory.filePath("project.session"), &loaded, &error));
        CModelDocument original, restored;
        QVERIFY(original.Load(path(name), &error));
        QVERIFY2(loaded.RestoreDocument(original, restored, &error), qPrintable(error));
        const auto restoredState = restored.ParameterStructureState();
        const auto expectedState = document.ParameterStructureState();
        for (auto it = expectedState.cbegin(); it != expectedState.cend(); ++it) {
            if (it.key() == QLatin1String("fingerprint")) continue;
            QVERIFY2(restoredState.value(it.key()) == it.value(), qPrintable(it.key()));
        }
        QCOMPARE(restoredState, expectedState);
        QCOMPARE(restored.SourceBytes(), source);
        QCOMPARE(restored.SourceDigest(), digest);
        QCOMPARE(restored.ParameterValue("ParamSessionCreated"), 0.375f);
        QByteArray actual;
        QVERIFY(restored.BuildPreviewMoc(actual, &error));
        QCOMPARE(actual, expected);
        QVERIFY(!restored.CanUndoEdit());
    }

    void invalidSessionEncodingAndVersionsAreAtomic_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 10; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void invalidSessionEncodingAndVersionsAreAtomic()
    {
        QFETCH(int, kind);
        QJsonObject columns{{"268", QString::fromLatin1(QByteArray(4, '\0').toBase64())}};
        QJsonObject counts;
        int version = 2;
        if (kind == 0) version = 1;
        if (kind == 1) version = 5;
        if (kind == 2) columns["268"] = "AA==garbage";
        if (kind == 3) columns["268"] = "AA";
        if (kind == 4) columns["268"] = "AA==\n";
        if (kind == 5) columns = {{"64", "AA=="}};
        if (kind == 6) columns = {{"0268", "AA=="}};
        if (kind == 7) columns = {{"269", "AA=="}};
        if (kind == 8) counts = {{"34", 0}};
        if (kind == 9) counts = {{"5", 1000001}};
        QJsonObject object{{"version", version}, {"structure", QJsonObject{{"columns", columns}, {"counts", counts}}}};
        CModelSession output;
        output.sourceMoc = "unchanged";
        QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(object).toJson(), &output, &error));
        QCOMPARE(output.sourceMoc, QString("unchanged"));
        QVERIFY(!error.isEmpty());
        CModelSession legacy;
        QVERIFY(CModelSession::FromJson(R"({"version":1})", &legacy, &error));
        QVERIFY(legacy.structure.columns.isEmpty());
    }

    void staleMalformedAndProtectedStatesDoNotCommit()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path("Mark"), &error));
        CModelEdit edit;
        QVERIFY(document.MakeCreateParameterEdit(request("ParamStale"), {}, edit, &error));
        QVERIFY(document.SetParameterValue(document.Parameters().front().id, 1.0f));
        const auto state = document.ParameterStructureState();
        const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.ParameterStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(document.MakeCreateParameterEdit(request("ParamBadState"), {}, edit, &error));
        auto broken = edit;
        auto after = broken.after.toMap();
        after.insert("unexpected", true);
        broken.after = after;
        QVERIFY(!document.ExecuteEdit(broken));
        QCOMPARE(document.ParameterStructureState(), state);
        QVERIFY(document.ExecuteEdit(edit));
        const auto accepted = document.ParameterStructureState();
        auto columns = document.ParameterStructure();
        const auto& source = document.SourceBytes();
        const int uvCount = word(source, word(source, 0x40) + 15 * 4);
        columns.columns[0x178] = source.mid(word(source, 0x178), uvCount * 4);
        columns.columns[0x178][0] = char(quint8(columns.columns[0x178][0]) ^ 0x40);
        QVERIFY(!document.SetParameterStructure(columns, &error));
        QCOMPARE(document.ParameterStructureState(), accepted);
        auto invalidSession = session(document);
        invalidSession.structure = columns;
        CModelDocument target = document;
        QVERIFY(!invalidSession.RestoreDocument(document, target, &error));
        QCOMPARE(target.ParameterStructureState(), accepted);
    }

    void ownerAndAncestorLocksAreCheckedBeforeBinding()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path("Natori"), &error));
        int index = -1;
        for (int mesh = 0; mesh < document.Drawables().size(); ++mesh)
            if (document.Drawables()[mesh].parentDeformerIndex >= 0 && document.Drawables()[mesh].parentPartIndex >= 0) { index = mesh; break; }
        QVERIFY(index >= 0);
        const auto target = document.Drawables()[index];
        const auto parent = document.Deformers()[target.parentDeformerIndex];
        const QString parentType = parent.type == CModelDeformer::Warp ? "warp" : "rotation";
        auto create = request("ParamLocked");
        create.bindings.append(Builder::BindingRequest{{Builder::Owner::ArtMesh, target.id}, {-2, 3}});
        for (const auto& lock : {qMakePair(QString("artmesh"), target.id), qMakePair(parentType, parent.id),
                                qMakePair(QString("part"), document.Parts()[target.parentPartIndex].id)}) {
            document.SetLocked(lock.first, lock.second, true);
            CModelEdit output; output.name = "unchanged";
            const auto before = document.ParameterStructureState();
            QVERIFY(!document.MakeCreateParameterEdit(create, {}, output, &error));
            QCOMPARE(output.name, QString("unchanged"));
            QCOMPARE(document.ParameterStructureState(), before);
            document.SetLocked(lock.first, lock.second, false);
        }
    }

    void originalCrossPartParentRemainsRestorableAfterBaking_data() { models(); }
    void originalCrossPartParentRemainsRestorableAfterBaking()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path(name), &error));
        int index = -1;
        for (int row = 0; row < document.Deformers().size(); ++row) {
            const auto& value = document.Deformers()[row];
            if (value.parentDeformerIndex >= 0 && document.Deformers()[value.parentDeformerIndex].parentPartIndex != value.parentPartIndex) { index = row; break; }
        }
        QVERIFY(index >= 0);
        const auto child = document.Deformers()[index];
        QVERIFY(document.SetDeformerParentIndex(child.id, -1));
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request("ParamWithParent"), {}, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.DeformerParentIndex(child.id), -1);
        QCOMPARE(document.SourceDeformerParentIndex(child.id), child.parentDeformerIndex);
        QVERIFY(document.SetDeformerParentIndex(child.id, child.parentDeformerIndex));
        QByteArray output;
        QVERIFY2(document.BuildPreviewMoc(output, &error), qPrintable(error));
        for (const auto& diagnostic : document.Validate()) QVERIFY(diagnostic.code != "deformer_parent_override");
        CModelDocument restored;
        QVERIFY2(session(document).RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerParentIndex(child.id), child.parentDeformerIndex);
    }

    void forgedCrossPartParentInStructureIsRejected()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path("Natori"), &error));
        int child = -1, parent = -1;
        for (int a = 0; a < document.Deformers().size() && child < 0; ++a)
            for (int b = 0; b < a; ++b)
                if (document.Deformers()[a].parentPartIndex != document.Deformers()[b].parentPartIndex &&
                    document.Deformers()[a].parentDeformerIndex != b) { child = a; parent = b; break; }
        QVERIFY(child >= 0);
        const auto before = document.ParameterStructureState();
        Builder::ColumnState structure;
        const auto& source = document.SourceBytes();
        structure.columns[0x80] = source.mid(word(source, 0x80), document.Deformers().size() * 4);
        put(structure.columns[0x80], child * 4, parent);
        QVERIFY(!document.SetParameterStructure(structure, &error));
        QVERIFY(error.contains(QStringLiteral("跨 Part")));
        QCOMPARE(document.ParameterStructureState(), before);
        CModelSession saved;
        saved.version = 2;
        saved.structure = structure;
        CModelDocument restored = document;
        QVERIFY(!saved.RestoreDocument(document, restored, &error));
        QCOMPARE(restored.ParameterStructureState(), before);
    }

    void removedSourceParametersCannotRemainInSessionTimeline()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(path("Mark"), &error));
        CModelEdit edit;
        QVERIFY(document.MakeCreateParameterEdit(request("ParamTimeline"), {}, edit, &error));
        QVERIFY(document.ExecuteEdit(edit));
        const auto loaded = document;
        QVERIFY(document.MakeDeleteParameterEdit("ParamTimeline", edit, &error));
        QVERIFY(document.ExecuteEdit(edit));
        auto saved = session(document);
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("ParamTimeline", 0.0f, 1.0f));
        QVERIFY(timeline.SaveState(&saved.animation, false, 0, &error));
        CModelDocument restored = loaded;
        QVERIFY2(saved.RestoreDocument(loaded, restored, &error), qPrintable(error));
        timeline.Clear();
        QVERIFY(timeline.AddPoint("LegacyVirtualParameter", 0.0f, 1.0f));
        QVERIFY(timeline.SaveState(&saved.animation, false, 0, &error));
        QVERIFY2(saved.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QVERIFY2(document.MakeDeleteParameterEdit(Builder::DeleteRequest{"ParamAngleX", 0}, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        saved = session(document);
        timeline.Clear();
        QVERIFY(timeline.AddPoint("ParamAngleX", 0.0f, 1.0f));
        QVERIFY(timeline.SaveState(&saved.animation, false, 0, &error));
        restored = loaded;
        const auto before = restored.ParameterStructureState();
        QVERIFY(!saved.RestoreDocument(loaded, restored, &error));
        QCOMPARE(restored.ParameterStructureState(), before);
    }
};

QTEST_GUILESS_MAIN(ParameterStructureDocumentTests)
#include "parameter_structure_document_tests.moc"
