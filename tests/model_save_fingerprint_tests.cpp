#include "live2dstruct/cmodelsavefingerprint.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <algorithm>
#include <cmath>

using namespace live2dstruct;

class ModelSaveFingerprintTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
    QByteArray m_baseline;
private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        QVERIFY2(CModelSaveFingerprint::Capture(m_original, m_baseline, &error), qPrintable(error));
    }

    void parameterUndoAndSeparateReturnToOriginalRecoverClean()
    {
        auto document = m_original; const auto& parameter = document.Parameters().front();
        const float before = document.ParameterValue(parameter.id);
        const float after = before == parameter.maximum ? parameter.minimum : parameter.maximum;
        const QMap<QString, float> oldValues{{parameter.id, before}}, newValues{{parameter.id, after}};
        QVERIFY(document.ExecuteEdit({"values", "parameters", CModelEdit::ParameterValues,
            QVariant::fromValue(oldValues), QVariant::fromValue(newValues), -1, false}));
        QByteArray changed; QVERIFY(CModelSaveFingerprint::Capture(document, changed)); QVERIFY(changed != m_baseline);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterValue(parameter.id), before);
        QCOMPARE(document.AuthoringStructureState(), m_original.AuthoringStructureState());
        QByteArray restored; QVERIFY(CModelSaveFingerprint::Capture(document, restored)); QCOMPARE(restored, m_baseline);
        QVERIFY(document.ExecuteEdit({"values", "parameters", CModelEdit::ParameterValues,
            QVariant::fromValue(oldValues), QVariant::fromValue(newValues), -1, false}));
        QVERIFY(document.ExecuteEdit({"values", "parameters", CModelEdit::ParameterValues,
            QVariant::fromValue(newValues), QVariant::fromValue(oldValues), -1, false}));
        QCOMPARE(document.ParameterValue(parameter.id), before);
        QVERIFY(document.AuthoringStructureState() != m_original.AuthoringStructureState());
        QVERIFY(CModelSaveFingerprint::Capture(document, restored)); QCOMPARE(restored, m_baseline);
    }

    void neutralOverridesDoNotChangeSavedContent_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto* field : {"parameter", "metadata", "opacity", "order", "positions", "indices", "masks", "inverted", "parent", "rotation", "warp"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void neutralOverridesDoNotChangeSavedContent()
    {
        QFETCH(QString, field); auto document = m_original; const auto& parameter = document.Parameters().front();
        bool changed = false;
        if (field == "parameter") changed = document.SetParameterValue(parameter.id, document.ParameterValue(parameter.id));
        if (field == "metadata") changed = document.SetParameterMetadata(parameter.id, document.ParameterMetadata(parameter.id));
        for (const auto& mesh : document.Drawables()) {
            if (changed) break;
            if (field == "opacity" && !mesh.keyforms.isEmpty() && std::all_of(mesh.keyforms.cbegin(), mesh.keyforms.cend(), [&](const auto& form) { return form.opacity == mesh.opacity; }))
                changed = document.SetDrawableOpacity(mesh.id, mesh.opacity);
            if (field == "order" && !mesh.keyforms.isEmpty() && std::all_of(mesh.keyforms.cbegin(), mesh.keyforms.cend(), [&](const auto& form) { return form.drawOrder == document.DrawableDrawOrder(mesh); }))
                changed = document.SetDrawableDrawOrder(mesh.id, document.DrawableDrawOrder(mesh));
            if (field == "positions" && !mesh.keyforms.isEmpty()) changed = document.SetDrawableKeyformPositions(mesh.id, 0, mesh.keyforms[0].positions);
            if (field == "indices") changed = document.SetDrawableIndices(mesh.id, mesh.indices);
            if (field == "masks") changed = document.SetDrawableMaskIndices(mesh.id, mesh.maskIndices);
            if (field == "inverted") changed = document.SetDrawableMaskInverted(mesh.id, document.DrawableMaskInverted(mesh.id));
        }
        for (const auto& deformer : document.Deformers()) {
            if (changed) break;
            if (field == "parent") changed = document.SetDeformerParentIndex(deformer.id, deformer.parentDeformerIndex);
            if (field == "rotation" && deformer.type == CModelDeformer::Rotation) changed = document.SetDeformerRotationKeyforms(deformer.id, deformer.rotationKeyforms);
            if (field == "warp" && deformer.type == CModelDeformer::Warp) changed = document.SetDeformerWarpKeyforms(deformer.id, deformer.warpKeyforms);
        }
        QVERIFY(changed); QByteArray current; QString error;
        QVERIFY2(CModelSaveFingerprint::Capture(document, current, &error), qPrintable(error)); QCOMPARE(current, m_baseline);
    }

    void nonMocMetadataAndAuthorStateAffectFingerprint_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto* field : {"name", "precision", "lock", "visibility", "organization", "sourcePath"}) QTest::newRow(field) << QString::fromLatin1(field);
    }
    void nonMocMetadataAndAuthorStateAffectFingerprint()
    {
        QFETCH(QString, field); auto document = m_original; const auto& parameter = document.Parameters().front();
        if (field == "name" || field == "precision") {
            auto metadata = document.ParameterMetadata(parameter.id);
            if (field == "name") metadata.displayName = "Renamed";
            else metadata.decimalPlaces = metadata.decimalPlaces == 9 ? 8 : metadata.decimalPlaces + 1;
            QVERIFY(document.SetParameterMetadata(parameter.id, metadata));
        } else if (field == "lock") document.SetLocked("artmesh", document.Drawables().front().id, true);
        else if (field == "visibility") QVERIFY(document.SetDrawableVisible(document.Drawables().front().id, !document.Drawables().front().visible));
        else if (field == "organization") {
            CParameterOrganization::State state; state.configured = true;
            for (const auto& value : document.Parameters()) state.nodes.append({CParameterOrganization::Node::Parameter, value.id, {}, {}, {}});
            QVERIFY(document.SetParameterOrganization(state));
        } else QVERIFY(document.LoadBytes(document.SourceBytes(), "/different/source.moc3"));
        QByteArray current; QVERIFY(CModelSaveFingerprint::Capture(document, current)); QVERIFY(current != m_baseline);
    }

    void hiddenOpacityIsAnAuthorChangeEvenWhenRuntimeBytesMatch()
    {
        auto document = m_original; const auto id = document.Drawables().front().id;
        QVERIFY(document.SetDrawableVisible(id, false));
        QByteArray before, runtimeBefore; QVERIFY(CModelSaveFingerprint::Capture(document, before)); QVERIFY(document.BuildRuntimeMoc(runtimeBefore));
        QVERIFY(document.SetDrawableOpacity(id, 0.37f));
        QByteArray after, runtimeAfter; QVERIFY(CModelSaveFingerprint::Capture(document, after)); QVERIFY(document.BuildRuntimeMoc(runtimeAfter));
        QCOMPARE(runtimeAfter, runtimeBefore); QVERIFY(after != before);
    }

    void subFloatEpsilonChangesRemainDirty()
    {
        auto document = m_original; const auto& parameter = document.Parameters().front();
        const float value = std::nextafter(document.ParameterValue(parameter.id), parameter.maximum);
        QVERIFY(document.SetParameterValue(parameter.id, value));
        QByteArray current; QVERIFY(CModelSaveFingerprint::Capture(document, current)); QVERIFY(current != m_baseline);
    }

    void unloadedFailureLeavesOutputUnchanged()
    {
        CModelDocument document; QByteArray output = "sentinel"; QString error;
        QVERIFY(!CModelSaveFingerprint::Capture(document, output, &error)); QCOMPARE(output, QByteArray("sentinel"));
    }
};
QTEST_APPLESS_MAIN(ModelSaveFingerprintTests)
#include "model_save_fingerprint_tests.moc"
