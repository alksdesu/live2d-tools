#include "live2dstruct/cmodelsavefingerprint.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include <QtTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>

using namespace live2dstruct;

namespace {
QByteArray imageBytes(QColor color, QSize size = {32, 32})
{
    QImage image(size, QImage::Format_RGBA8888); image.fill(color); QBuffer output;
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG")) return {};
    return output.data();
}
bool initializeTextures(CModelDocument& document, const QString& directory, QSize size, QString* error)
{
    const auto bytes = imageBytes(Qt::red, size); CModelMaterialState materials; CTextureAssets::Catalog sources;
    if (bytes.isEmpty() || !document.CaptureDrawableMaterials(materials, error)) return false;
    for (const auto& material : materials) if (!material.uvs.isEmpty() && !sources.contains(material.textureIndex)) {
        CTextureAssets::Source source;
        if (!CTextureAssets::MakeSource(directory + QString("/atlas-%1.png").arg(material.textureIndex), bytes, source, error)) return false;
        sources.insert(material.textureIndex, source);
    }
    return document.InitializeTextureWorkspace(sources, {}, error);
}
bool matchesWorkspace(const CModelDocument& document)
{
    QByteArray computed;
    return CTextureWorkspace::Fingerprint(document.TextureSources(), document.TextureWorkspace(), computed) &&
        computed == document.TextureWorkspaceFingerprint();
}
}

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

    void textureFingerprintFollowsEditUndoRestoreFailureAndReload()
    {
        auto document = m_original; QTemporaryDir directory; QString error; QVERIFY(directory.isValid());
        QVERIFY2(initializeTextures(document, directory.path(), {32, 32}, &error), qPrintable(error));
        QVERIFY(matchesWorkspace(document)); const auto original = document.TextureWorkspaceFingerprint();
        QByteArray clean; QVERIFY(CModelSaveFingerprint::Capture(document, clean));
        CModelAtlasState before; QVERIFY(document.CaptureAtlasState(before)); auto after = before;
        after.workspace.atlases.first().png = imageBytes(Qt::blue); CModelEdit edit;
        QVERIFY2(document.MakeAtlasEdit(before, after, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(matchesWorkspace(document)); QVERIFY(document.TextureWorkspaceFingerprint() != original);
        const auto changed = document.TextureWorkspaceFingerprint(); QByteArray dirty;
        QVERIFY(CModelSaveFingerprint::Capture(document, dirty)); QVERIFY(dirty != clean);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.TextureWorkspaceFingerprint(), original); QVERIFY(matchesWorkspace(document));
        QByteArray restored; QVERIFY(CModelSaveFingerprint::Capture(document, restored)); QCOMPARE(restored, clean);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.TextureWorkspaceFingerprint(), changed); QVERIFY(matchesWorkspace(document));
        auto invalid = document.TextureWorkspace(); invalid.atlases.first().png = "invalid PNG";
        QVERIFY(!document.SetTextureWorkspace(invalid, &error)); QCOMPARE(document.TextureWorkspaceFingerprint(), changed);
        QVERIFY(document.SetTextureWorkspace(before.workspace)); QCOMPARE(document.TextureWorkspaceFingerprint(), original);
        CModelSession session; session.version = 11; session.sourceMoc = document.SourcePath(); session.sourceDigest = document.SourceDigest();
        CTextureWorkspace::Draft draft; QVERIFY(CTextureWorkspace::CaptureDraft(document.TextureSources(), after.workspace, draft));
        session.textureWorkspace = draft; CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error)); CModelDocument recovered;
        QVERIFY2(decoded.RestoreDocument(document, recovered, &error), qPrintable(error));
        QCOMPARE(recovered.TextureWorkspaceFingerprint(), changed); QVERIFY(matchesWorkspace(recovered));
        QVERIFY(recovered.LoadBytes(m_original.SourceBytes(), m_original.SourcePath())); QVERIFY(recovered.TextureWorkspaceFingerprint().isEmpty());
        QVERIFY(CModelSaveFingerprint::Capture(recovered, restored)); QCOMPARE(restored, m_baseline);
    }

    void unchangedLargeTextureDoesNotChangeDuringParameterSaveChecks()
    {
        auto document = m_original; QTemporaryDir directory; QString error; QVERIFY(directory.isValid());
        QVERIFY2(initializeTextures(document, directory.path(), {2048, 2048}, &error), qPrintable(error));
        const auto original = document.TextureWorkspaceFingerprint(); QByteArray clean;
        QVERIFY(CModelSaveFingerprint::Capture(document, clean)); const auto parameter = document.Parameters().first();
        const float initial = document.ParameterValue(parameter.id);
        const float changed = initial == parameter.maximum ? parameter.minimum : parameter.maximum;
        QElapsedTimer timer; timer.start();
        for (int index = 0; index < 12; ++index) {
            const float value = index % 2 == 0 ? changed : initial;
            QVERIFY(document.SetParameterValue(parameter.id, value)); QByteArray saved;
            QVERIFY(CModelSaveFingerprint::Capture(document, saved)); QCOMPARE(saved == clean, value == initial);
            QCOMPARE(document.TextureWorkspaceFingerprint(), original);
        }
        qInfo() << "12 parameter edits and save checks with 2048x2048 atlas:" << timer.elapsed() << "ms";
        QVERIFY(matchesWorkspace(document));
    }
};
QTEST_APPLESS_MAIN(ModelSaveFingerprintTests)
#include "model_save_fingerprint_tests.moc"
