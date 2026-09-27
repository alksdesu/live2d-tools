#include "live2dstruct/cdeformerdisplaynames.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include "live2dstruct/cmodelsession.h"
#include "deformer_snapshot_fixture.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace live2dstruct;
using Names = CDeformerDisplayNames;
namespace {
QByteArray preview(const CModelDocument& document)
{
    QByteArray bytes;
    document.BuildPreviewMoc(bytes);
    return bytes;
}
QByteArray fingerprint(const CModelDocument& document)
{
    QByteArray bytes;
    CModelSaveFingerprint::Capture(document, bytes);
    return bytes;
}
bool load(CModelDocument& document, QString* error = nullptr, bool withPath = true)
{
    DeformerSnapshotData::Fixture fixture;
    return document.LoadBytes(fixture.bytes, withPath ? QStringLiteral("names-fixture.moc3") : QString(), error);
}
CModelSession session(const CModelDocument& document)
{
    CModelSession result;
    result.version = 13;
    result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.deformerDisplayNames = document.DeformerDisplayNames();
    result.structure = document.ParameterStructure(); result.meshStructure = document.HasMeshStructure();
    result.materialStructure = document.HasMaterialStructure(); result.drawableMaterials = document.DrawableMaterialOverrides();
    result.deformerStructure = document.HasDeformerStructure();
    if (result.deformerStructure) result.sourceDeformerIds = document.SourceDeformerIds();
    result.parameterMetadata = document.ParameterMetadataOverrides();
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it) result.parameters.insert(it.key(), it.value());
    result.parameterOrganization = document.ParameterOrganization(); result.geometryObjectPairs = document.GeometryObjectPairs();
    result.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys()) result.locked.insert(key, true);
    result.drawableOpacity = document.DrawableOpacityOverrides(); result.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides(); result.drawableIndices = document.DrawableIndexOverrides();
    result.drawableMaskIndices = document.DrawableMaskIndexOverrides(); result.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    result.deformerParentIndices = document.DeformerParentOverrides(); result.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    result.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    return result;
}
CDeformerPreserveRequest anchored(const CModelDocument& document, const CMocDeformerStructureBuilder::InsertRequest& request)
{
    return {request, document.SourceDigest(), QCryptographicHash::hash(preview(document), QCryptographicHash::Sha256)};
}
CDeformerPreserveRequest removal(const CModelDocument& document, const QString& id)
{
    return {CMocDeformerStructureBuilder::RemoveRequest{id}, document.SourceDigest(), QCryptographicHash::hash(preview(document), QCryptographicHash::Sha256)};
}
QByteArray partFixture(bool cycle = false)
{
    DeformerSnapshotData::Fixture f;
    const auto& o = *f.table.sectionOffsetTable;
    f.count(0, 2); f.count(6, 1);
    for (int i = 0; i < 2; ++i) {
        f.id(o.parts->ids, i, i ? "PartChild" : "PartRoot");
        f.column(o.parts->keyform_binding_sources_indices, i, -1);
        f.column(o.parts->keyform_sources_begin_indices, i, 0);
        f.column(o.parts->keyform_sources_counts, i, 1);
        f.column(o.parts->is_visible, i, 1); f.column(o.parts->is_enabled, i, 1);
        f.column(o.parts->parent_part_indices, i, i ? 0 : cycle ? 1 : -1);
    }
    f.column(o.deformers->parent_part_indices, 0, 1);
    return f.bytes;
}
}

class DeformerNamesDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void invalidNames_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& pair : {qMakePair(QStringLiteral("empty"), QString()), qMakePair(QStringLiteral("blank"), QStringLiteral(" \t ")),
                qMakePair(QStringLiteral("newline"), QStringLiteral("a\nb")), qMakePair(QStringLiteral("null"), QString(QChar::Null)),
                qMakePair(QStringLiteral("line-separator"), QString(QChar(0x2028))), qMakePair(QStringLiteral("paragraph"), QString(QChar(0x2029))),
                qMakePair(QStringLiteral("noncharacter"), QString(QChar(0xffff))), qMakePair(QStringLiteral("surrogate"), QString(QChar(0xd800))),
                qMakePair(QStringLiteral("too-long"), QString(1025, QLatin1Char('x')))})
            QTest::newRow(qPrintable(pair.first)) << pair.second;
    }
    void invalidNames()
    {
        QFETCH(QString, name);
        Names::State target{{"keep", "unchanged"}}, expected = target;
        QString error;
        QVERIFY(!Names::ValidateName(name, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!Names::Normalize({{"Warp", name}}, target, &error)); QCOMPARE(target, expected);
        QVERIFY(!Names::FromJson(QJsonObject{{"Warp", name}}, target, &error)); QCOMPARE(target, expected);
    }
    void unicodeBudgetOwnershipAndRawJsonIdentity()
    {
        QString id = QStringLiteral("Warp"), name = QStringLiteral("  双眼 <&> \"🦊\" é  ");
        QVector<QChar> rawId(id.cbegin(), id.cend()), rawName(name.cbegin(), name.cend());
        Names::State raw{{QString::fromRawData(rawId.constData(), rawId.size()), QString::fromRawData(rawName.constData(), rawName.size())}}, owned;
        QString error;
        QVERIFY2(Names::Normalize(raw, owned, &error), qPrintable(error));
        QVERIFY(owned.firstKey().constData() != rawId.constData()); QVERIFY(owned.first().constData() != rawName.constData());
        const auto expected = owned;
        rawId[0] = QLatin1Char('X'); rawName[2] = QLatin1Char('X'); QCOMPARE(owned, expected);
        Names::State decoded;
        QVERIFY(Names::FromJson(QJsonObject{{"Unknown", "Unknown"}}, decoded));
        QCOMPARE(decoded, Names::State({{"Unknown", "Unknown"}}));
        QJsonObject encoded;
        QVERIFY(Names::ToJson({{"Warp", "Warp"}}, encoded)); QVERIFY(encoded.isEmpty());
        QVERIFY(Names::ValidateName(QString(1024, QLatin1Char('x'))));
        Names::State excessive;
        const QString large(1024, QChar(0x4e2d));
        for (int i = 0; i < 6000; ++i) excessive.insert(QStringLiteral("D%1").arg(i), large);
        QVERIFY(!Names::Validate(excessive, &error)); QVERIFY(error.contains(QStringLiteral("限制")));
        QJsonObject oversized;
        for (auto item = excessive.cbegin(); item != excessive.cend(); ++item) oversized.insert(item.key(), item.value());
        oversized.insert("ZZAfterBudget", 42);
        const auto original = decoded;
        QVERIFY(!Names::FromJson(oversized, decoded, &error));
        QVERIFY2(error.contains(QStringLiteral("限制")), qPrintable(error));
        QCOMPARE(decoded, original);
        for (const QString& key : {QString(), QString(65, QLatin1Char('x')), QString(30, QChar(0x4e2d)), QString("a") + QChar::Null})
            QVERIFY(!Names::Validate({{key, "name"}}, &error));
        QVERIFY(!Names::FromJson(QJsonObject{{"Warp", 42}}, decoded, &error));
    }
    void renameHistoryDirtyAndFrozenCommand()
    {
        CModelDocument document; QString error; QVERIFY2(load(document, &error), qPrintable(error));
        const auto original = document.SourceBytes(), moc = preview(document), saved = fingerprint(document);
        QVERIFY(!saved.isEmpty());
        const auto revision = document.Revision();
        CModelEdit edit;
        QVERIFY(document.MakeDeformerDisplayNameEdit("Warp", "Warp", edit, &error));
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
        QString id = "Warp", before = "Warp", after = QStringLiteral("网格 🐼 <&>");
        QVERIFY(document.MakeDeformerDisplayNameEdit(id, after, edit, &error));
        QVector<QChar> rawId(id.cbegin(), id.cend()), rawBefore(before.cbegin(), before.cend()), rawAfter(after.cbegin(), after.cend());
        edit.name = QString::fromRawData(rawId.constData(), rawId.size());
        edit.before = QString::fromRawData(rawBefore.constData(), rawBefore.size());
        edit.after = QString::fromRawData(rawAfter.constData(), rawAfter.size());
        const QString wanted = after;
        QVERIFY(document.ExecuteEdit(edit));
        rawId[0] = 'X'; rawBefore[0] = 'X'; rawAfter[0] = 'X';
        QCOMPARE(document.DeformerDisplayName("Warp"), wanted);
        QVERIFY(fingerprint(document) != saved); QCOMPARE(preview(document), moc); QCOMPARE(document.SourceBytes(), original);
        document.SetLocked("rotation", "Root", true);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), QString("Warp"));
        QVERIFY(document.DeformerDisplayNames().isEmpty());
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), wanted);
        document.ClearLocked(); QVERIFY(document.UndoEdit()); QCOMPARE(fingerprint(document), saved);
        QCOMPARE(document.DeformerDisplayName("missing"), QString());
    }
    void invalidAndStaleCommandsStayAtomic_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"type", "index", "coalesce", "value-type", "unknown", "stale", "late-parent-lock", "extra-request"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void invalidAndStaleCommandsStayAtomic()
    {
        QFETCH(QString, kind);
        CModelDocument document; QString error; QVERIFY(load(document, &error));
        CModelEdit edit; QVERIFY(document.MakeDeformerDisplayNameEdit("Warp", "next", edit, &error));
        if (kind == "type") edit.type = "rotation";
        if (kind == "index") edit.index = 0;
        if (kind == "coalesce") edit.coalesce = true;
        if (kind == "value-type") edit.after = 42;
        if (kind == "unknown") edit.name = "Unknown";
        if (kind == "stale") QVERIFY(document.SetDeformerDisplayNames({{"Warp", "intervening"}}, &error));
        if (kind == "late-parent-lock") document.SetLocked("rotation", "Root", true);
        if (kind == "extra-request") edit.meshRequest = std::make_shared<CMocMeshStructureBuilder::Request>();
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(!document.CanRedoEdit());
        QVERIFY(!document.SetDeformerDisplayNames({{"Unknown", "Unknown"}}, &error)); QCOMPARE(document.AuthoringStructureState(), state);
    }
    void inheritedLocksAndCycles_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"direct", "deformer-parent", "ancestor-part", "part-parent", "part-cycle", "deformer-cycle"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void inheritedLocksAndCycles()
    {
        QFETCH(QString, kind);
        auto bytes = partFixture(kind == "part-cycle");
        if (kind == "deformer-cycle") {
            auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
            DeformerSnapshotData::put(bytes, table.sectionOffsetTable->deformers->parent_deformer_indices, 2);
        }
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(bytes, "locks.moc3", &error), qPrintable(error));
        if (kind == "direct") document.SetLocked("rotation", "Child", true);
        if (kind == "deformer-parent") document.SetLocked("warp", "Warp", true);
        if (kind == "ancestor-part") document.SetLocked("part", "PartChild", true);
        if (kind == "part-parent") document.SetLocked("part", "PartRoot", true);
        CModelEdit edit; QVERIFY(!document.MakeDeformerDisplayNameEdit("Child", "new", edit, &error)); QVERIFY(!error.isEmpty());
        edit = {"Child", "rotation", CModelEdit::DeformerDisplayName, "Child", "new", -1, false};
        QVERIFY(!document.ExecuteEdit(edit)); QVERIFY(!document.CanUndoEdit());
    }
    void effectiveParentOverridesControlInheritedLocks()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error));
        document.SetLocked("rotation", "Root", true);
        QVERIFY(document.SetDeformerParentIndex("Warp", -1));
        CModelEdit edit; QVERIFY2(document.MakeDeformerDisplayNameEdit("Child", "detached parent", edit, &error), qPrintable(error));
        QVERIFY(document.SetDeformerParentIndex("Warp", 0));
        QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.DeformerDisplayName("Child"), QString("Child"));
    }
    void controlCharacterSourceIdCanBeRenamedAndUndone()
    {
        DeformerSnapshotData::Fixture fixture;
        fixture.id(fixture.table.sectionOffsetTable->deformers->ids, 1, "Warp\nId");
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "control.moc3", &error), qPrintable(error));
        CModelEdit edit; QVERIFY(document.MakeDeformerDisplayNameEdit("Warp\nId", "显示名", edit, &error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DeformerDisplayName("Warp\nId"), QStringLiteral("显示名"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerDisplayName("Warp\nId"), QString("Warp\nId"));
        QVERIFY(document.RedoEdit());
    }
    void sessionVersionsSeedAndExplicitEmpty_data()
    {
        QTest::addColumn<bool>("oldVersion"); QTest::addColumn<bool>("empty");
        for (bool oldVersion : {false, true}) for (bool empty : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(oldVersion).arg(empty))) << oldVersion << empty;
    }
    void sessionVersionsSeedAndExplicitEmpty()
    {
        QFETCH(bool, oldVersion); QFETCH(bool, empty);
        CModelDocument loaded; QString error; QVERIFY(load(loaded, &error));
        QVERIFY(loaded.SetDeformerDisplayNames({{"Warp", "loaded seed"}}, &error));
        auto snapshot = session(loaded);
        if (oldVersion) { snapshot.version = 12; snapshot.deformerDisplayNames.reset(); }
        else snapshot.deformerDisplayNames = empty ? Names::State{} : Names::State{{"Warp", "saved name"}};
        const auto json = snapshot.ToJson(); QVERIFY(!json.isEmpty());
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(json, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.version, oldVersion ? 12 : 13);
        QCOMPARE(decoded.deformerDisplayNames.has_value(), !oldVersion);
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerDisplayName("Warp"), oldVersion ? QString("loaded seed") : empty ? QString("Warp") : QString("saved name"));
        QVERIFY(!restored.CanUndoEdit()); QCOMPARE(loaded.DeformerDisplayName("Warp"), QString("loaded seed"));
        QCOMPARE(restored.SourceBytes(), loaded.SourceBytes());
        QTemporaryDir temporary; QVERIFY(snapshot.Save(temporary.filePath("session.json"), &error));
        QVERIFY(CModelSession::Load(temporary.filePath("session.json"), &decoded, &error));
        QVERIFY(decoded.RestoreDocument(loaded, restored, &error));
    }
    void invalidSessionNamesAndSourcesStayAtomic_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"missing", "null", "array", "old-with-names", "unknown", "unknown-equals-id", "wrong-digest", "empty-digest", "nul-path"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void invalidSessionNamesAndSourcesStayAtomic()
    {
        QFETCH(QString, kind);
        CModelDocument loaded; QString error; QVERIFY(load(loaded, &error));
        auto snapshot = session(loaded);
        auto json = QJsonDocument::fromJson(snapshot.ToJson()).object();
        if (kind == "missing") json.remove("deformerDisplayNames");
        if (kind == "null") json.insert("deformerDisplayNames", QJsonValue(QJsonValue::Null));
        if (kind == "array") json.insert("deformerDisplayNames", QJsonArray{});
        if (kind == "old-with-names") json.insert("version", 12);
        if (kind.startsWith("unknown")) json.insert("deformerDisplayNames", QJsonObject{{"Unknown", kind == "unknown" ? "name" : "Unknown"}});
        if (kind == "wrong-digest") json.insert("sourceDigest", QString(64, 'a'));
        if (kind == "empty-digest") json.insert("sourceDigest", QString());
        if (kind == "nul-path") json.insert("sourceMoc", QString("x") + QChar::Null);
        CModelSession decoded = session(loaded);
        const bool parsed = CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error);
        if (kind.startsWith("unknown") || kind == "wrong-digest") {
            QVERIFY2(parsed, qPrintable(error));
            CModelDocument result = loaded; const auto state = result.AuthoringStructureState(); const auto revision = result.Revision();
            QVERIFY(!decoded.RestoreDocument(loaded, result, &error)); QCOMPARE(result.AuthoringStructureState(), state); QCOMPARE(result.Revision(), revision);
            if (kind.startsWith("unknown")) {
                snapshot.deformerDisplayNames = Names::State{{"Unknown", "Unknown"}};
                QVERIFY(CModelSession::FromJson(snapshot.ToJson(), &decoded, &error));
                QVERIFY(decoded.deformerDisplayNames->contains("Unknown"));
                QVERIFY(!decoded.RestoreDocument(loaded, result, &error));
            }
        } else QVERIFY(!parsed);
    }
    void pathlessParameterTransactionKeepsNames()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error, false));
        QVERIFY(document.SetDeformerDisplayNames({{"Warp", "source name"}}, &error));
        CMocParameterStructureBuilder::CreateRequest request;
        request.parameter = {"NewParameter", -1, 1, 0, 2, false};
        CModelEdit edit; QVERIFY2(document.MakeCreateParameterEdit(request, "parameter", edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DeformerDisplayName("Warp"), QString("source name"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), QString("source name"));
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), QString("source name"));
        auto snapshot = session(document); CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(snapshot.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerDisplayNames(), document.DeformerDisplayNames());
    }
    void preparedMeshPreservesNamesAndRejectsStaleNames()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error));
        QVERIFY(document.SetDeformerDisplayNames({{"Warp", "name"}}, &error));
        CMocMeshStructureBuilder::MeshEdit mesh;
        mesh.artMeshId = document.Drawables().first().id;
        for (int i = 0; i < 3; ++i) { mesh.vertices.append({{i}, {1}}); mesh.oldToNew.append(i); }
        mesh.vertices.append({{0, 1}, {.5, .5}}); mesh.indices = document.DrawableIndices(mesh.artMeshId);
        CMocMeshStructureBuilder::Request request; request.meshes = {mesh};
        CModelEdit edit; QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        QVERIFY(!edit.meshVerification.IsEmpty());
        CModelDocument stale = document; QVERIFY(stale.SetDeformerDisplayNames({{"Warp", "changed"}}, &error));
        const auto before = stale.AuthoringStructureState(); QVERIFY(!stale.ExecuteEdit(edit)); QCOMPARE(stale.AuthoringStructureState(), before);
        QVector<QChar> rawName{QChar('n'), QChar('a'), QChar('m'), QChar('e')};
        auto supplied = edit.after.toMap();
        supplied["deformerDisplayNames"] = QVariant::fromValue(Names::State{{"Warp", QString::fromRawData(rawName.constData(), rawName.size())}});
        edit.after = supplied;
        QVERIFY(document.ExecuteEdit(edit)); rawName[0] = 'X';
        QCOMPARE(document.DeformerDisplayName("Warp"), QString("name"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), QString("name"));
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerDisplayName("Warp"), QString("name"));
    }
    void legacySessionSeedDoesNotRenameARecreatedSourceId()
    {
        DeformerSnapshotData::Fixture fixture;
        const auto& offsets = *fixture.table.sectionOffsetTable;
        fixture.count(12, 2);
        fixture.column(offsets.keyform_bindings->parameter_binding_index_sources_begin_indices, 1, 1);
        fixture.column(offsets.keyform_bindings->parameter_binding_index_sources_counts, 1, 0);
        fixture.column(offsets.deformers->keyform_binding_sources_indices, 0, 1);
        fixture.column(offsets.rotation_deformers->keyform_binding_sources_indices, 0, 1);
        CModelDocument loaded; QString error; QVERIFY(loaded.LoadBytes(fixture.bytes, "source.moc3", &error));
        QVERIFY(loaded.SetDeformerDisplayNames({{"Root", "old source name"}, {"Child", "surviving source"}}, &error));
        CModelDocument document = loaded;
        CModelEdit edit; QVERIFY2(document.MakeDeformerStructureEdit(removal(document, "Root"), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        CMocDeformerStructureBuilder::InsertRequest request;
        request.id = "Root"; request.shape = CMocDeformerStructureBuilder::Rotation{};
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(!document.SourceDeformerIds().contains("Root"));
        QCOMPARE(document.DeformerDisplayName("Root"), QString("Root"));
        auto old = session(document); old.version = 12; old.deformerDisplayNames.reset();
        CModelDocument restored; QVERIFY2(old.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerDisplayName("Root"), QString("Root"));
        QCOMPARE(restored.DeformerDisplayName("Child"), QString("surviving source"));
        old.version = 13; old.deformerDisplayNames = Names::State{{"Root", "explicit new name"}};
        QVERIFY(old.RestoreDocument(loaded, restored, &error));
        QCOMPARE(restored.DeformerDisplayName("Root"), QString("explicit new name"));
    }
    void deletedNamesRestoreOnUndoButDoNotLeakIntoReusedIds()
    {
        CModelDocument document; QString error; QVERIFY(load(document, &error));
        CMocDeformerStructureBuilder::InsertRequest request;
        request.id = "Created"; request.shape = CMocDeformerStructureBuilder::Rotation{};
        CModelEdit edit; QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DeformerDisplayName("Created"), QString("Created"));
        QVERIFY(document.MakeDeformerDisplayNameEdit("Created", "authored name", edit, &error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY2(document.MakeDeformerStructureEdit(removal(document, "Created"), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(!document.DeformerDisplayNames().contains("Created"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerDisplayName("Created"), QString("authored name"));
        QVERIFY(document.RedoEdit()); QVERIFY(!document.DeformerDisplayNames().contains("Created"));
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DeformerDisplayName("Created"), QString("Created"));
        QVERIFY(!document.DeformerDisplayNames().contains("Created"));
    }
};
QTEST_APPLESS_MAIN(DeformerNamesDocumentTests)
#include "deformer_names_document_tests.moc"
