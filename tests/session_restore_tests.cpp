#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmotiontimeline.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <functional>
#include <limits>

using namespace live2dstruct;

namespace {
QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}

bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

CModelSession state(const CModelDocument& document)
{
    CModelSession session;
    session.sourceMoc = document.SourcePath();
    session.sourceDigest = document.SourceDigest();
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it)
        session.parameters.insert(it.key(), it.value());
    session.parameterMetadata = document.ParameterMetadataOverrides();
    session.drawableOpacity = document.DrawableOpacityOverrides();
    session.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    session.drawableIndices = document.DrawableIndexOverrides();
    session.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    session.deformerParentIndices = document.DeformerParentOverrides();
    session.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    session.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    for (const auto& part : document.Parts()) session.visibility.insert("part|" + part.id, part.visible);
    for (const auto& deformer : document.Deformers()) {
        const QString type = deformer.type == CModelDeformer::Warp ? "warp|" : "rotation|";
        session.visibility.insert(type + deformer.id, deformer.visible);
    }
    for (const auto& mesh : document.Drawables()) session.visibility.insert("artmesh|" + mesh.id, mesh.visible);
    for (const auto& key : document.LockedKeys()) session.locked.insert(key, true);
    return session;
}
}

class SessionRestoreTests : public QObject
{
    Q_OBJECT
private:
    CModelDocument loaded;
    CModelDocument withoutAngle;
    int meshIndex = -1;
    int rotationIndex = -1;
    int warpIndex = -1;
    int childIndex = -1;
    int parentIndex = -1;

    const CModelDrawable& mesh() const { return loaded.Drawables()[meshIndex]; }
    const CModelDeformer& rotation() const { return loaded.Deformers()[rotationIndex]; }
    const CModelDeformer& warp() const { return loaded.Deformers()[warpIndex]; }

    void rejectsWithoutMutation(const CModelSession& session, const QString& expectedField,
                                const CModelDocument* source = nullptr)
    {
        CModelDocument result = loaded;
        const auto& parameter = loaded.Parameters().first();
        const float oldValue = loaded.ParameterValue(parameter.id);
        const float newValue = oldValue == parameter.maximum ? parameter.minimum : parameter.maximum;
        QVERIFY(result.ExecuteEdit({parameter.id, "parameter", CModelEdit::ParameterValue, oldValue, newValue, -1, false}));
        QVERIFY(result.ExecuteEdit({mesh().id, "artmesh", CModelEdit::Opacity,
                                    result.DrawableOpacity(mesh()), 0.375f, -1, false}));
        QVERIFY(result.UndoEdit());
        result.SetLocked("artmesh", mesh().id, true);
        const auto before = state(result).ToJson();
        const auto bytes = result.SourceBytes();
        const auto revision = result.Revision();
        QString error;
        QVERIFY2(!session.RestoreDocument(source ? *source : loaded, result, &error), qPrintable(expectedField));
        QVERIFY2(error.contains(expectedField), qPrintable(error));
        QCOMPARE(state(result).ToJson(), before);
        QCOMPARE(result.SourceBytes(), bytes);
        QCOMPARE(result.Revision(), revision);
        QVERIFY(result.CanUndoEdit());
        QVERIFY(result.CanRedoEdit());
        QVERIFY(result.UndoEdit());
        QCOMPARE(result.ParameterValue(parameter.id), oldValue);
        QVERIFY(result.RedoEdit());
        QCOMPARE(result.ParameterValue(parameter.id), newValue);
        QVERIFY(result.RedoEdit());
        QCOMPARE(result.DrawableOpacity(mesh()), 0.375f);
        QVERIFY(!result.CanRedoEdit());
    }

private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(loaded.Load(sample("Mark"), &error), qPrintable(error));
        QVERIFY(!loaded.Parameters().isEmpty());
        QVERIFY(!loaded.Parts().isEmpty());
        for (int index = 0; index < loaded.Drawables().size(); ++index) {
            const auto& drawable = loaded.Drawables()[index];
            if (drawable.vertexCount >= 3 && drawable.keyforms.size() >= 3) { meshIndex = index; break; }
        }
        QVERIFY(meshIndex >= 0);
        for (int index = 0; index < loaded.Deformers().size(); ++index) {
            const auto& deformer = loaded.Deformers()[index];
            if (rotationIndex < 0 && deformer.type == CModelDeformer::Rotation && !deformer.rotationKeyforms.isEmpty())
                rotationIndex = index;
            if (warpIndex < 0 && deformer.type == CModelDeformer::Warp && !deformer.warpKeyforms.isEmpty())
                warpIndex = index;
            if (childIndex < 0 && deformer.parentDeformerIndex >= 0 &&
                deformer.parentPartIndex == loaded.Deformers()[deformer.parentDeformerIndex].parentPartIndex) {
                childIndex = index;
                parentIndex = deformer.parentDeformerIndex;
            }
        }
        QVERIFY(rotationIndex >= 0);
        QVERIFY(warpIndex >= 0);
        QVERIFY(childIndex >= 0);
        QVERIFY(parentIndex >= 0);
        withoutAngle = loaded;
        CModelEdit deletion;
        QVERIFY2(withoutAngle.MakeDeleteParameterEdit({QStringLiteral("ParamAngleX"), 0}, deletion, &error), qPrintable(error));
        QVERIFY(withoutAngle.ExecuteEdit(deletion));
        QCOMPARE(withoutAngle.Parameters().size(), loaded.Parameters().size() - 1);
        QCOMPARE(withoutAngle.SourceBytes(), loaded.SourceBytes());
    }

    void newPhysicsDraftWritesV4AndRestoresFromTheModelSnapshot_data()
    {
        QTest::addColumn<int>("version"); QTest::addColumn<bool>("dirty");
        for (int version : {1, 2, 3, 4}) for (bool dirty : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("v%1-%2").arg(version).arg(dirty))) << version << dirty;
    }

    void newPhysicsDraftWritesV4AndRestoresFromTheModelSnapshot()
    {
        QFETCH(int, version); QFETCH(bool, dirty);
        CModelSession session; session.version = version;
        session.sourceMoc = loaded.SourcePath(); session.sourceDigest = loaded.SourceDigest();
        const QJsonObject draft{{"Version", 3}, {"PhysicsSettings", QJsonArray{}}, {"Custom", "preserved"}};
        session.physicsDraft = CModelSession::PhysicsDraft{QString(), draft, dirty};
        const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(), 4); QCOMPARE(session.version, version);
        CModelSession decoded; QString error;
        QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.version, 4); QVERIFY(decoded.physicsDraft); QVERIFY(decoded.physicsDraft->sourcePath.isEmpty());
        QCOMPARE(decoded.physicsDraft->document, draft); QCOMPARE(decoded.physicsDraft->dirty, dirty);
        QCOMPARE(decoded.sourceMoc, session.sourceMoc); QCOMPARE(decoded.sourceDigest, session.sourceDigest);
        QTemporaryDir directory; const auto path = directory.filePath("new-physics.session.json");
        QVERIFY2(session.Save(path, &error), qPrintable(error));
        CModelSession saved; QVERIFY2(CModelSession::Load(path, &saved, &error), qPrintable(error));
        QCOMPARE(saved.ToJson(), bytes);
        CModelDocument restored = loaded;
        QVERIFY(restored.ExecuteEdit({mesh().id, "artmesh", CModelEdit::Opacity, restored.DrawableOpacity(mesh()), .375f}));
        QVERIFY2(saved.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.SourceBytes(), loaded.SourceBytes()); QCOMPARE(restored.SourceDigest(), loaded.SourceDigest());
        QCOMPARE(state(restored).ToJson(), state(loaded).ToJson()); QVERIFY(!restored.CanUndoEdit()); QVERIFY(!restored.CanRedoEdit());
    }

    void sourcedPhysicsDraftKeepsLegacyReferenceOnlyValidation_data()
    {
        QTest::addColumn<int>("version");
        for (int version : {1, 2, 3, 4}) QTest::newRow(qPrintable(QString::number(version))) << version;
    }

    void sourcedPhysicsDraftKeepsLegacyReferenceOnlyValidation()
    {
        QFETCH(int, version);
        const QJsonObject draft{{"PhysicsSettings", QJsonArray{QJsonObject{
            {"Input", QJsonArray{QJsonObject{{"Source", QJsonObject{{"Target", "Parameter"}, {"Id", "ExternalVirtualParameter"}}}}}},
            {"Output", QJsonArray{}}}}}};
        CModelSession session; session.version = version;
        session.physicsDraft = CModelSession::PhysicsDraft{QStringLiteral("/unread/original.physics3.json"), draft, true};
        CModelSession decoded; QString error; const auto bytes = session.ToJson();
        QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.version, std::max(3, version)); QVERIFY(decoded.sourceMoc.isEmpty()); QVERIFY(decoded.sourceDigest.isEmpty());
        QCOMPARE(decoded.physicsDraft->document, draft);
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
        session.physicsDraft->sourcePath.clear(); session.sourceMoc = loaded.SourcePath(); session.sourceDigest = loaded.SourceDigest();
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.version, 4); QCOMPARE(decoded.physicsDraft->document, draft);
        QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
    }

    void v4DraftCannotBeReadOrRestoredAsAnOlderVersion_data()
    {
        QTest::addColumn<int>("version");
        for (int version : {1, 2, 3}) QTest::newRow(qPrintable(QString::number(version))) << version;
    }

    void v4DraftCannotBeReadOrRestoredAsAnOlderVersion()
    {
        QFETCH(int, version);
        CModelSession session; session.version = 4; session.sourceMoc = loaded.SourcePath(); session.sourceDigest = loaded.SourceDigest();
        session.physicsDraft = CModelSession::PhysicsDraft{QStringLiteral(""), QJsonObject{{"PhysicsSettings", QJsonArray{}}}, true};
        auto root = QJsonDocument::fromJson(session.ToJson()).object(); root["version"] = version;
        CModelSession result; result.viewScale = 2; const auto before = result.ToJson(); QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(root).toJson(), &result, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(result.ToJson(), before);
        session.version = version; rejectsWithoutMutation(session, version < 3 ? QStringLiteral("version") : QStringLiteral("physicsDraft"));
        QVERIFY2(CModelSession::FromJson(QJsonDocument(QJsonObject{{"version", version}}).toJson(), &result, &error), qPrintable(error));
        QCOMPARE(result.version, version); QVERIFY(!result.physicsDraft);
    }

    void invalidNewDraftJsonKeepsTheExistingSession_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"missing-source", "empty-source", "null-source", "nul-source", "missing-digest", "empty-digest", "null-digest",
             "short-digest", "long-digest", "bad-digest", "missing-path", "null-path", "nul-path", "empty-document", "null-document", "missing-document", "bad-dirty"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void invalidNewDraftJsonKeepsTheExistingSession()
    {
        QFETCH(QString, field);
        CModelSession source; source.version = 4; source.sourceMoc = loaded.SourcePath(); source.sourceDigest = loaded.SourceDigest();
        source.physicsDraft = CModelSession::PhysicsDraft{QStringLiteral(""), QJsonObject{{"PhysicsSettings", QJsonArray{}}}, true};
        auto root = QJsonDocument::fromJson(source.ToJson()).object(); auto draft = root["physicsDraft"].toObject();
        if (field == "missing-source") root.remove("sourceMoc");
        else if (field == "empty-source") root["sourceMoc"] = "";
        else if (field == "null-source") root["sourceMoc"] = QJsonValue();
        else if (field == "nul-source") root["sourceMoc"] = QString("model") + QChar::Null;
        else if (field == "missing-digest") root.remove("sourceDigest");
        else if (field == "empty-digest") root["sourceDigest"] = "";
        else if (field == "null-digest") root["sourceDigest"] = QJsonValue();
        else if (field == "short-digest") root["sourceDigest"] = QString(62, 'a');
        else if (field == "long-digest") root["sourceDigest"] = QString(66, 'a');
        else if (field == "bad-digest") root["sourceDigest"] = QString(64, 'z');
        else if (field == "missing-path") draft.remove("sourcePath");
        else if (field == "null-path") draft["sourcePath"] = QJsonValue();
        else if (field == "nul-path") draft["sourcePath"] = QString("rig") + QChar::Null;
        else if (field == "empty-document") draft["document"] = QJsonObject{};
        else if (field == "null-document") draft["document"] = QJsonValue();
        else if (field == "missing-document") draft.remove("document");
        else if (field == "bad-dirty") draft["dirty"] = "true";
        root["physicsDraft"] = draft;
        CModelSession result; result.parameters["unchanged"] = .25f; result.viewScale = 3;
        const auto before = result.ToJson(); QString error;
        const auto bytes = QJsonDocument(root).toJson();
        QVERIFY(!CModelSession::FromJson(bytes, &result, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(result.ToJson(), before);
        QTemporaryDir directory; const auto path = directory.filePath("bad.session.json"); QVERIFY(write(path, bytes));
        QVERIFY(!CModelSession::Load(path, &result, &error)); QCOMPARE(result.ToJson(), before);
    }

    void invalidNewDraftCannotOverwriteFilesOrDocumentHistory_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"source", "nul-source", "digest", "short-digest", "long-digest", "nul-path", "document", "version"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void invalidNewDraftCannotOverwriteFilesOrDocumentHistory()
    {
        QFETCH(QString, field);
        CModelSession session; session.version = 4; session.sourceMoc = loaded.SourcePath(); session.sourceDigest = loaded.SourceDigest();
        session.physicsDraft = CModelSession::PhysicsDraft{QString(), QJsonObject{{"PhysicsSettings", QJsonArray{}}}, true};
        if (field == "source") session.sourceMoc.clear();
        else if (field == "nul-source") session.sourceMoc.append(QChar::Null);
        else if (field == "digest") session.sourceDigest.clear();
        else if (field == "short-digest") session.sourceDigest = QByteArray(31, 'x');
        else if (field == "long-digest") session.sourceDigest = QByteArray(33, 'x');
        else if (field == "nul-path") session.physicsDraft->sourcePath = QString("rig") + QChar::Null;
        else if (field == "document") session.physicsDraft->document = {};
        else session.version = 11;
        QVERIFY(session.ToJson().isEmpty());
        QTemporaryDir directory; const auto path = directory.filePath("existing.json"); QVERIFY(write(path, "unchanged")); QString error;
        QVERIFY(!session.Save(path, &error)); QVERIFY(!error.isEmpty());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("unchanged"));
        const auto fresh = directory.filePath("new.json"); QVERIFY(!session.Save(fresh, &error)); QVERIFY(!QFileInfo::exists(fresh));
        const QString expected = field == "version" ? QStringLiteral("version")
            : field == "document" || field == "nul-path" ? QStringLiteral("物理草稿") : QStringLiteral("physicsDraft");
        rejectsWithoutMutation(session, expected);
    }

    void newDraftRequiresTheSameModelDigestAtRestore()
    {
        CModelSession session; session.version = 4; session.sourceMoc = loaded.SourcePath(); session.sourceDigest = QByteArray(32, 'x');
        session.physicsDraft = CModelSession::PhysicsDraft{QString(), QJsonObject{{"PhysicsSettings", QJsonArray{}}}, true};
        CModelSession decoded; QString error;
        QVERIFY(CModelSession::FromJson(session.ToJson(), &decoded, &error));
        rejectsWithoutMutation(decoded, QStringLiteral("sourceDigest"));
    }

    void legacyDefaultsRestoreSourceAndClearPreviousEdits_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void legacyDefaultsRestoreSourceAndClearPreviousEdits()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QVERIFY(document.Load(sample(name)));
        const auto baseline = state(document).ToJson();
        const auto& drawable = document.Drawables().first();
        const auto id = drawable.id;
        QVERIFY(document.ExecuteEdit({id, "artmesh", CModelEdit::Opacity, document.DrawableOpacity(drawable), 0.3f}));
        QVERIFY(document.SetDrawableVisible(id, false));
        document.SetLocked("artmesh", id, true);
        CModelSession legacy;
        QString error;
        QVERIFY(CModelSession::FromJson("{}", &legacy, &error));
        QVERIFY2(legacy.RestoreDocument(document, document, &error), qPrintable(error));
        QCOMPARE(state(document).ToJson(), baseline);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QVERIFY(error.isEmpty());
    }

    void restoresAllFieldsAndSparseFormsAtomically()
    {
        CModelSession session;
        session.sourceDigest = loaded.SourceDigest();
        session.sourceMoc = loaded.SourcePath();
        const auto& parameter = loaded.Parameters().first();
        auto metadata = loaded.ParameterMetadata(parameter.id);
        metadata.displayName = "Restored parameter";
        metadata.maximum += 10.0f;
        metadata.decimalPlaces = 3;
        metadata.repeat = !metadata.repeat;
        session.parameterMetadata.insert(parameter.id, metadata);
        session.parameters.insert(parameter.id, metadata.maximum);
        session.visibility = {{"part|" + loaded.Parts().first().id, false}, {"artmesh|" + mesh().id, false},
                              {"rotation|" + rotation().id, false}, {"warp|" + warp().id, false}};
        session.locked = {{"part|" + loaded.Parts().first().id, true}, {"artmesh|" + mesh().id, true},
                          {"rotation|" + rotation().id, true}, {"warp|" + warp().id, false}};
        session.drawableOpacity.insert(mesh().id, 0.625f);
        session.drawableDrawOrder.insert(mesh().id, 456);
        const int editedIndex = mesh().keyforms.size() / 2;
        auto positions = mesh().keyforms[editedIndex].positions;
        positions[0] += 0.125f;
        auto& forms = session.drawableKeyformPositions[mesh().id];
        forms.resize(editedIndex + 1);
        forms[editedIndex] = positions;
        session.drawableIndices.insert(mesh().id, {0, 1, 2});
        const int mask = (meshIndex + 1) % loaded.Drawables().size();
        session.drawableMaskIndices.insert(mesh().id, {mask});
        session.drawableMaskInverted.insert(mesh().id, true);
        session.deformerParentIndices.insert(loaded.Deformers()[parentIndex].id, childIndex);
        session.deformerParentIndices.insert(loaded.Deformers()[childIndex].id, -1);
        CModelDocument sequential = loaded;
        QVERIFY(!sequential.SetDeformerParentIndex(loaded.Deformers()[parentIndex].id, childIndex));
        auto rotationForms = rotation().rotationKeyforms;
        rotationForms[0].angle += 12.0f;
        rotationForms[0].reflectX = !rotationForms[0].reflectX;
        session.deformerRotationKeyforms.insert(rotation().id, rotationForms);
        auto warpForms = warp().warpKeyforms;
        warpForms[0].positions[0] += 0.25f;
        session.deformerWarpKeyforms.insert(warp().id, warpForms);
        session.viewScale = 25.0f;
        session.viewPosition = QPointF(-4.0, 7.0);
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(parameter.id, 2.0f, parameter.defaultValue));
        QVERIFY(timeline.SaveState(&session.animation, true, 1.0));
        CModelSession decoded;
        QString error;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument restored = loaded;
        restored.SetLocked("warp", warp().id, true);
        QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.SourceDigest(), loaded.SourceDigest());
        QCOMPARE(restored.SourceBytes(), loaded.SourceBytes());
        QVERIFY(restored.ParameterMetadata(parameter.id) == metadata);
        QCOMPARE(restored.ParameterValue(parameter.id), metadata.maximum);
        QCOMPARE(restored.ParameterMetadataOverrides().size(), 1);
        QCOMPARE(restored.DrawableOpacityOverrides(), session.drawableOpacity);
        QCOMPARE(restored.DrawableDrawOrderOverrides(), session.drawableDrawOrder);
        QCOMPARE(restored.DrawableIndexOverrides(), session.drawableIndices);
        QCOMPARE(restored.DrawableMaskIndexOverrides(), session.drawableMaskIndices);
        QCOMPARE(restored.DrawableMaskInvertedOverrides(), session.drawableMaskInverted);
        QCOMPARE(restored.DeformerParentOverrides(), session.deformerParentIndices);
        QCOMPARE(restored.DeformerRotationKeyformOverrides(), session.deformerRotationKeyforms);
        QCOMPARE(restored.DeformerWarpKeyformOverrides(), session.deformerWarpKeyforms);
        const auto restoredState = state(restored);
        for (auto it = session.visibility.cbegin(); it != session.visibility.cend(); ++it)
            QCOMPARE(restoredState.visibility.value(it.key()), it.value());
        for (auto it = session.locked.cbegin(); it != session.locked.cend(); ++it)
            QCOMPARE(restoredState.locked.value(it.key(), false), it.value());
        QCOMPARE(restored.LockedKeys().size(), 3);
        for (int index = 0; index < mesh().keyforms.size(); ++index) {
            QCOMPARE(restored.DrawableKeyformPositions(mesh().id, index),
                     index == editedIndex ? positions : mesh().keyforms[index].positions);
        }
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY(!restored.CanRedoEdit());
        QVERIFY(loaded.DrawableKeyformPositionOverrides().isEmpty());
        QCOMPARE(loaded.ParameterValue(parameter.id), parameter.defaultValue);
        QVERIFY(error.isEmpty());
    }

    void rejectsUnknownObjectsWithoutDiscardingHistory_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"parameters", "parameterMetadata", "visibility", "locked", "drawableOpacity",
                                 "drawableDrawOrder", "drawableKeyformPositions", "drawableIndices", "drawableMaskIndices",
                                 "drawableMaskInverted", "deformerParentIndices", "deformerRotationKeyforms", "deformerWarpKeyforms"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void rejectsUnknownObjectsWithoutDiscardingHistory()
    {
        QFETCH(QString, field);
        CModelSession session;
        const QString missing = "missing-object";
        if (field == "parameters") session.parameters.insert(missing, 0);
        if (field == "parameterMetadata") session.parameterMetadata.insert(missing, loaded.ParameterMetadata(loaded.Parameters().first().id));
        if (field == "visibility") session.visibility.insert("part|" + missing, true);
        if (field == "locked") session.locked.insert("artmesh|" + missing, false);
        if (field == "drawableOpacity") session.drawableOpacity.insert(missing, 0.5f);
        if (field == "drawableDrawOrder") session.drawableDrawOrder.insert(missing, 0);
        if (field == "drawableKeyformPositions") session.drawableKeyformPositions.insert(missing, {});
        if (field == "drawableIndices") session.drawableIndices.insert(missing, {0, 1, 2});
        if (field == "drawableMaskIndices") session.drawableMaskIndices.insert(missing, {});
        if (field == "drawableMaskInverted") session.drawableMaskInverted.insert(missing, false);
        if (field == "deformerParentIndices") session.deformerParentIndices.insert(missing, -1);
        if (field == "deformerRotationKeyforms") session.deformerRotationKeyforms.insert(missing, rotation().rotationKeyforms);
        if (field == "deformerWarpKeyforms") session.deformerWarpKeyforms.insert(missing, warp().warpKeyforms);
        rejectsWithoutMutation(session, field);
    }

    void finiteSignedRotationScaleSurvivesSessionRestore_data()
    {
        QTest::addColumn<float>("scale");
        QTest::newRow("negative") << -2.0f;
        QTest::newRow("negative-zero") << -0.0f;
        QTest::newRow("zero") << 0.0f;
        QTest::newRow("positive") << 2.0f;
    }

    void finiteSignedRotationScaleSurvivesSessionRestore()
    {
        QFETCH(float, scale);
        auto forms = rotation().rotationKeyforms;
        forms[0].scale = scale;
        auto session = state(loaded);
        session.deformerRotationKeyforms.insert(rotation().id, forms);
        CModelSession parsed;
        QString error;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.deformerRotationKeyforms.value(rotation().id), forms);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("signed-scale.session.json");
        QVERIFY2(parsed.Save(path, &error), qPrintable(error));
        CModelSession saved;
        QVERIFY2(CModelSession::Load(path, &saved, &error), qPrintable(error));
        CModelDocument restored = loaded;
        QVERIFY(restored.ExecuteEdit({mesh().id, "artmesh", CModelEdit::Opacity,
                                      restored.DrawableOpacity(mesh()), .375f, -1, false}));
        QVERIFY2(saved.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerRotationKeyforms(rotation().id), forms);
        QCOMPARE(restored.SourceBytes(), loaded.SourceBytes());
        QCOMPARE(restored.SourceDigest(), loaded.SourceDigest());
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY(!restored.CanRedoEdit());
        QByteArray preview;
        QVERIFY2(restored.BuildPreviewMoc(preview, &error), qPrintable(error));
        auto reader = CReader::FromBytes(preview);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto* definition = table.deformers[rotationIndex];
        const auto* source = table.rotation_deformers[definition->specific_sources_indices];
        QCOMPARE(table.rotation_deformer_keyforms[source->keyform_sources_begin_indices]->scales, scale);
    }

    void rejectsInvalidValuesWithoutClampingOrLosingHistory()
    {
        const auto& parameter = loaded.Parameters().first();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        using Mutation = std::function<void(CModelSession&)>;
        struct Case { const char* field; const char* name; Mutation mutate; };
        const QVector<Case> cases = {
            {"version", "unsupported", [](auto& s) { s.version = 99; }},
            {"parameters", "out-of-range", [&](auto& s) { s.parameters.insert(parameter.id, parameter.maximum + 1); }},
            {"parameters", "nan", [&](auto& s) { s.parameters.insert(parameter.id, nan); }},
            {"parameterMetadata", "reversed-range", [&](auto& s) {
                auto m = loaded.ParameterMetadata(parameter.id); m.minimum = m.maximum + 1; s.parameterMetadata.insert(parameter.id, m);
            }},
            {"parameterMetadata", "default-out-of-range", [&](auto& s) {
                auto m = loaded.ParameterMetadata(parameter.id); m.defaultValue = m.maximum + 1; s.parameterMetadata.insert(parameter.id, m);
            }},
            {"parameterMetadata", "bad-precision", [&](auto& s) {
                auto m = loaded.ParameterMetadata(parameter.id); m.decimalPlaces = 10; s.parameterMetadata.insert(parameter.id, m);
            }},
            {"parameterMetadata", "infinite-range", [&](auto& s) {
                auto m = loaded.ParameterMetadata(parameter.id); m.maximum = infinity; s.parameterMetadata.insert(parameter.id, m);
            }},
            {"visibility", "wrong-kind", [&](auto& s) { s.visibility.insert("rotation|" + warp().id, false); }},
            {"visibility", "missing-separator", [&](auto& s) { s.visibility.insert(mesh().id, false); }},
            {"locked", "wrong-kind", [&](auto& s) { s.locked.insert("warp|" + rotation().id, true); }},
            {"drawableOpacity", "negative", [&](auto& s) { s.drawableOpacity.insert(mesh().id, -0.1f); }},
            {"drawableOpacity", "above-one", [&](auto& s) { s.drawableOpacity.insert(mesh().id, 1.1f); }},
            {"drawableOpacity", "nan", [&](auto& s) { s.drawableOpacity.insert(mesh().id, nan); }},
            {"drawableDrawOrder", "negative", [&](auto& s) { s.drawableDrawOrder.insert(mesh().id, -1); }},
            {"drawableDrawOrder", "above-maximum", [&](auto& s) { s.drawableDrawOrder.insert(mesh().id, 1001); }},
            {"drawableKeyformPositions", "wrong-vertex-count", [&](auto& s) { s.drawableKeyformPositions.insert(mesh().id, {{0, 0}}); }},
            {"drawableKeyformPositions", "nan", [&](auto& s) {
                auto positions = mesh().keyforms[0].positions; positions[0] = nan; s.drawableKeyformPositions.insert(mesh().id, {positions});
            }},
            {"drawableKeyformPositions", "excess-empty-slots", [&](auto& s) { s.drawableKeyformPositions[mesh().id].resize(mesh().keyforms.size() + 1); }},
            {"drawableIndices", "empty", [&](auto& s) { s.drawableIndices.insert(mesh().id, {}); }},
            {"drawableIndices", "incomplete-triangle", [&](auto& s) { s.drawableIndices.insert(mesh().id, {0, 1}); }},
            {"drawableIndices", "out-of-bounds", [&](auto& s) { s.drawableIndices.insert(mesh().id, {0, 1, quint16(mesh().vertexCount)}); }},
            {"drawableIndices", "unsigned-overflow", [&](auto& s) { s.drawableIndices.insert(mesh().id, {0, 1, 65535}); }},
            {"drawableMaskIndices", "self", [&](auto& s) { s.drawableMaskIndices.insert(mesh().id, {meshIndex}); }},
            {"drawableMaskIndices", "negative", [&](auto& s) { s.drawableMaskIndices.insert(mesh().id, {-1}); }},
            {"drawableMaskIndices", "out-of-bounds", [&](auto& s) { s.drawableMaskIndices.insert(mesh().id, {int(loaded.Drawables().size())}); }},
            {"drawableMaskIndices", "duplicate", [&](auto& s) {
                const int other = (meshIndex + 1) % loaded.Drawables().size(); s.drawableMaskIndices.insert(mesh().id, {other, other});
            }},
            {"deformerParentIndices", "self", [&](auto& s) { s.deformerParentIndices.insert(warp().id, warpIndex); }},
            {"deformerParentIndices", "out-of-bounds", [&](auto& s) { s.deformerParentIndices.insert(warp().id, loaded.Deformers().size()); }},
            {"deformerParentIndices", "cycle", [&](auto& s) { s.deformerParentIndices.insert(loaded.Deformers()[parentIndex].id, childIndex); }},
            {"deformerRotationKeyforms", "wrong-kind", [&](auto& s) { s.deformerRotationKeyforms.insert(warp().id, rotation().rotationKeyforms); }},
            {"deformerRotationKeyforms", "empty", [&](auto& s) { s.deformerRotationKeyforms.insert(rotation().id, {}); }},
            {"deformerRotationKeyforms", "wrong-count", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms.append(forms.first()); s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerRotationKeyforms", "nan-scale", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms[0].scale = nan; s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerRotationKeyforms", "infinite-scale", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms[0].scale = infinity; s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerRotationKeyforms", "negative-infinite-scale", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms[0].scale = -infinity; s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerRotationKeyforms", "bad-opacity", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms[0].opacity = 2; s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerRotationKeyforms", "infinite-origin", [&](auto& s) {
                auto forms = rotation().rotationKeyforms; forms[0].originX = infinity; s.deformerRotationKeyforms.insert(rotation().id, forms);
            }},
            {"deformerWarpKeyforms", "wrong-kind", [&](auto& s) { s.deformerWarpKeyforms.insert(rotation().id, warp().warpKeyforms); }},
            {"deformerWarpKeyforms", "empty", [&](auto& s) { s.deformerWarpKeyforms.insert(warp().id, {}); }},
            {"deformerWarpKeyforms", "wrong-count", [&](auto& s) {
                auto forms = warp().warpKeyforms; forms.append(forms.first()); s.deformerWarpKeyforms.insert(warp().id, forms);
            }},
            {"deformerWarpKeyforms", "wrong-position-count", [&](auto& s) {
                auto forms = warp().warpKeyforms; forms[0].positions.removeLast(); s.deformerWarpKeyforms.insert(warp().id, forms);
            }},
            {"deformerWarpKeyforms", "nan", [&](auto& s) {
                auto forms = warp().warpKeyforms; forms[0].positions[0] = nan; s.deformerWarpKeyforms.insert(warp().id, forms);
            }},
            {"deformerWarpKeyforms", "bad-opacity", [&](auto& s) {
                auto forms = warp().warpKeyforms; forms[0].opacity = -1; s.deformerWarpKeyforms.insert(warp().id, forms);
            }},
            {"view", "zero-scale", [](auto& s) { s.viewScale = 0; }},
            {"view", "negative-scale", [](auto& s) { s.viewScale = -1; }},
            {"view", "infinite-scale", [&](auto& s) { s.viewScale = infinity; }},
            {"view", "nan-position", [&](auto& s) { s.viewPosition.setX(nan); }},
            {"view", "position-overflow", [](auto& s) { s.viewPosition.setY(double(std::numeric_limits<float>::max()) * 2); }},
            {"sourceDigest", "wrong-model", [](auto& s) { s.sourceDigest = QByteArray(32, 'X'); }},
            {"sourceDigest", "wrong-width", [](auto& s) { s.sourceDigest = QByteArray(31, 'X'); }},
            {"动画", "invalid-timeline", [](auto& s) { s.animation.insert("version", 42); }}
        };
        for (const auto& test : cases) {
            CModelSession session;
            session.drawableOpacity.insert(mesh().id, 0.625f);
            test.mutate(session);
            rejectsWithoutMutation(session, QString::fromUtf8(test.field));
            if (QTest::currentTestFailed()) {
                qWarning() << test.field << test.name;
                return;
            }
        }
    }

    void restoreUsesLoadedBytesAfterFileReplacementAndDeletion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("model.moc3");
        QVERIFY(write(path, loaded.SourceBytes()));
        CModelDocument document;
        QVERIFY(document.Load(path));
        CModelSession session;
        session.sourceMoc = path;
        session.sourceDigest = document.SourceDigest();
        session.drawableOpacity.insert(mesh().id, 0.4f);
        QVERIFY(write(path, QByteArrayLiteral("replaced file")));
        CModelDocument result;
        QString error;
        QVERIFY2(session.RestoreDocument(document, result, &error), qPrintable(error));
        QCOMPARE(result.SourceBytes(), loaded.SourceBytes());
        QCOMPARE(result.SourcePath(), path);
        QCOMPARE(result.DrawableOpacity(mesh()), 0.4f);
        QVERIFY(QFile::remove(path));
        const QString savedPath = directory.filePath("backup.live2d-session.json");
        QVERIFY2(session.Save(savedPath, &error), qPrintable(error));
        CModelSession saved;
        QVERIFY2(CModelSession::Load(savedPath, &saved, &error), qPrintable(error));
        QVERIFY2(saved.RestoreDocument(document, result, &error), qPrintable(error));
        QCOMPARE(result.SourceDigest(), loaded.SourceDigest());
        QCOMPARE(result.DrawableOpacity(mesh()), 0.4f);
        CModelDocument empty;
        const auto before = state(result).ToJson();
        QVERIFY(!session.RestoreDocument(empty, result, &error));
        QCOMPARE(state(result).ToJson(), before);
    }

    void deletedSourceParameterCannotRemainInTimeline_data()
    {
        QTest::addColumn<bool>("alreadyDeleted");
        QTest::newRow("source-directory") << false;
        QTest::newRow("edited-directory") << true;
    }

    void deletedSourceParameterCannotRemainInTimeline()
    {
        QFETCH(bool, alreadyDeleted);
        CModelSession session;
        session.version = 2;
        session.structure = withoutAngle.ParameterStructure();
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleX"), 0, 0));
        QVERIFY(timeline.SaveState(&session.animation, false, 0));
        rejectsWithoutMutation(session, QStringLiteral("animation"), alreadyDeleted ? &withoutAngle : &loaded);
    }

    void deletedSourceParameterCannotRemainInPhysicsDraft_data()
    {
        QTest::addColumn<bool>("input");
        QTest::addColumn<bool>("alreadyDeleted");
        QTest::newRow("input-source-directory") << true << false;
        QTest::newRow("output-source-directory") << false << false;
        QTest::newRow("input-edited-directory") << true << true;
        QTest::newRow("output-edited-directory") << false << true;
    }

    void deletedSourceParameterCannotRemainInPhysicsDraft()
    {
        QFETCH(bool, input);
        QFETCH(bool, alreadyDeleted);
        const QJsonObject endpoint{{"Target", "Parameter"}, {"Id", "ParamAngleX"}};
        const QJsonObject group{{"Input", input ? QJsonArray{QJsonObject{{"Source", endpoint}}} : QJsonArray{}},
                                {"Output", input ? QJsonArray{} : QJsonArray{QJsonObject{{"Destination", endpoint}}}}};
        CModelSession session;
        session.version = 3;
        session.structure = withoutAngle.ParameterStructure();
        session.physicsDraft = CModelSession::PhysicsDraft{QStringLiteral("/unread/rig.physics3.json"),
            QJsonObject{{"PhysicsSettings", QJsonArray{group}}}, true};
        rejectsWithoutMutation(session, QStringLiteral("physicsDraft"), alreadyDeleted ? &withoutAngle : &loaded);
    }

    void virtualReferencesUseImmutableSourceDirectory_data()
    {
        QTest::addColumn<bool>("createdInWorkingDocument");
        QTest::addColumn<bool>("physics");
        QTest::addColumn<int>("version");
        QTest::newRow("v1-existing-virtual") << false << false << 1;
        QTest::newRow("v1-new-working-parameter") << true << false << 1;
        QTest::newRow("v2-existing-virtual") << false << false << 2;
        QTest::newRow("v2-new-working-parameter") << true << false << 2;
        QTest::newRow("v3-physics-existing-virtual") << false << true << 3;
        QTest::newRow("v3-physics-new-working-parameter") << true << true << 3;
    }

    void virtualReferencesUseImmutableSourceDirectory()
    {
        QFETCH(bool, createdInWorkingDocument);
        QFETCH(bool, physics);
        QFETCH(int, version);
        const QString virtualId = QStringLiteral("SessionOnlyParameter");
        CModelDocument working = withoutAngle;
        QString error;
        if (createdInWorkingDocument) {
            CMocParameterStructureBuilder::CreateRequest request;
            request.parameter.id = virtualId;
            CModelEdit edit;
            QVERIFY2(working.MakeCreateParameterEdit(request, {}, edit, &error), qPrintable(error));
            QVERIFY(working.ExecuteEdit(edit));
        }
        CModelSession session;
        session.version = version;
        if (version > 1) session.structure = withoutAngle.ParameterStructure();
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(virtualId, 0, 0));
        QVERIFY(timeline.AddPoint({CMotionTarget::PartOpacity, QStringLiteral("ParamAngleX")}, 0, .5f));
        QVERIFY(timeline.SaveState(&session.animation, false, 0));
        if (physics) {
            const QJsonObject endpoint{{"Target", "Parameter"}, {"Id", virtualId}};
            session.physicsDraft = CModelSession::PhysicsDraft{QStringLiteral("/unread/rig.physics3.json"),
                QJsonObject{{"PhysicsSettings", QJsonArray{QJsonObject{
                    {"Input", QJsonArray{QJsonObject{{"Source", endpoint}}}},
                    {"Output", QJsonArray{QJsonObject{{"Destination", endpoint}}}}}}}}, true};
        }
        CModelDocument restored;
        QVERIFY2(session.RestoreDocument(working, restored, &error), qPrintable(error));
        QVERIFY(restored.ParameterStructure() == session.structure);
        QCOMPARE(restored.SourceBytes(), loaded.SourceBytes());
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY(!restored.CanRedoEdit());
    }

    void sparseEmptySlotsAndEmptyMasksRemainValid()
    {
        CModelSession session;
        session.drawableKeyformPositions[mesh().id].resize(mesh().keyforms.size());
        session.drawableMaskIndices.insert(mesh().id, {});
        session.drawableMaskInverted.insert(mesh().id, true);
        CModelDocument restored;
        QVERIFY(session.RestoreDocument(loaded, restored));
        QVERIFY(restored.DrawableKeyformPositionOverrides().isEmpty());
        QVERIFY(restored.DrawableMaskIndices(mesh().id).isEmpty());
        QVERIFY(restored.DrawableMaskInverted(mesh().id));
        for (int index = 0; index < mesh().keyforms.size(); ++index)
            QCOMPARE(restored.DrawableKeyformPositions(mesh().id, index), mesh().keyforms[index].positions);
    }

    void sharedRowSessionRestoresIndependentPreview()
    {
        QByteArray bytes = loaded.SourceBytes();
        CReader reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int owner = -1, alias = -1;
        for (int first = 0; first < table.art_meshes.size() && owner < 0; ++first) {
            for (int second = first + 1; second < table.art_meshes.size(); ++second) {
                if (table.art_meshes[first]->vertex_counts > 0 &&
                    table.art_meshes[first]->vertex_counts == table.art_meshes[second]->vertex_counts &&
                    table.art_meshes[first]->keyform_sources_counts == table.art_meshes[second]->keyform_sources_counts &&
                    table.art_meshes[first]->keyform_sources_counts > 0) {
                    owner = first;
                    alias = second;
                    break;
                }
            }
        }
        QVERIFY(owner >= 0);
        QVERIFY(alias >= 0);
        const int offset = table.sectionOffsetTable->art_meshes->keyform_sources_begin_indices + alias * 4;
        QVERIFY(offset >= 0 && offset <= bytes.size() - 4);
        qToLittleEndian<qint32>(table.art_meshes[owner]->keyform_sources_begin_indices,
                               reinterpret_cast<uchar*>(bytes.data() + offset));
        CModelDocument source;
        QString error;
        QVERIFY2(source.LoadBytes(bytes, "shared-row.moc3", &error), qPrintable(error));
        const QString id = source.Drawables()[owner].id;
        auto positions = source.DrawableKeyformPositions(id, 0);
        QVERIFY(!positions.isEmpty());
        positions[0] += 0.125f;
        CModelSession session;
        session.sourceDigest = source.SourceDigest();
        session.drawableKeyformPositions.insert(id, {positions});
        CModelDocument restored;
        QVERIFY2(session.RestoreDocument(source, restored, &error), qPrintable(error));
        QCOMPARE(restored.DrawableKeyformPositions(id, 0), positions);
        QByteArray preview;
        QVERIFY2(restored.BuildPreviewMoc(preview, &error), qPrintable(error));
        CModelDocument evaluated;
        QVERIFY2(evaluated.LoadBytes(preview, "preview.moc3", &error), qPrintable(error));
        QCOMPARE(evaluated.Drawables().size(), source.Drawables().size());
        for (const auto& mesh : source.Drawables()) {
            for (int keyform = 0; keyform < mesh.keyforms.size(); ++keyform) {
                const auto expected = mesh.id == id && keyform == 0 ? positions : source.DrawableKeyformPositions(mesh.id, keyform);
                QCOMPARE(evaluated.DrawableKeyformPositions(mesh.id, keyform), expected);
            }
        }
        QCOMPARE(restored.SourceBytes(), bytes);
    }
};

QTEST_GUILESS_MAIN(SessionRestoreTests)
#include "session_restore_tests.moc"
