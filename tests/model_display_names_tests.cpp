#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include "deformer_snapshot_fixture.h"
#include <QJsonDocument>
#include <QtTest>

using namespace live2dstruct;
namespace {
QByteArray fixture(bool cycle = false)
{
    DeformerSnapshotData::Fixture f;
    const auto& o = *f.table.sectionOffsetTable;
    f.count(0, 2); f.count(6, 1);
    for (int index = 0; index < 2; ++index) {
        f.id(o.parts->ids, index, index ? "PartRoot" : "Mesh");
        f.column(o.parts->keyform_binding_sources_indices, index, -1);
        f.column(o.parts->keyform_sources_begin_indices, index, 0);
        f.column(o.parts->keyform_sources_counts, index, 1);
        f.column(o.parts->is_visible, index, 1); f.column(o.parts->is_enabled, index, 1);
        f.column(o.parts->parent_part_indices, index, index ? cycle ? 0 : -1 : 1);
    }
    f.column(o.art_meshes->parent_part_indices, 0, 0);
    return f.bytes;
}
CModelDisplayNames::State names()
{
    return {{{"Mesh", QStringLiteral("眼睛组")}}, {{"Mesh", QStringLiteral("高光 <&> 🦊")}}, {{"Warp", QStringLiteral("形变")}}};
}
QByteArray preview(const CModelDocument& document)
{
    QByteArray bytes; document.BuildPreviewMoc(bytes); return bytes;
}
QByteArray fingerprint(const CModelDocument& document)
{
    QByteArray bytes; CModelSaveFingerprint::Capture(document, bytes); return bytes;
}
CModelSession session(const CModelDocument& document)
{
    CModelSession result;
    result.version = 15; result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.objectDisplayNames = document.DisplayNames();
    return result;
}
}

class ModelDisplayNamesTests : public QObject
{
    Q_OBJECT
private slots:
    void typedNamesDoNotChangeModelAndAreOneHistoryTransaction()
    {
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(fixture(), "names.moc3", &error), qPrintable(error));
        const auto bytes = preview(document), saved = fingerprint(document), source = document.SourceBytes();
        CModelEdit command;
        QVERIFY2(document.MakeDisplayNamesEdit(names(), command, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(command));
        QCOMPARE(document.DisplayName("part", "Mesh"), names().parts.value("Mesh"));
        QCOMPARE(document.DisplayName("artmesh", "Mesh"), names().artMeshes.value("Mesh"));
        QCOMPARE(document.DisplayName("warp", "Warp"), names().deformers.value("Warp"));
        QVERIFY(document.DisplayName("rotation", "Warp").isEmpty());
        QCOMPARE(preview(document), bytes); QCOMPARE(document.SourceBytes(), source);
        QVERIFY(fingerprint(document) != saved);
        QVERIFY(document.UndoEdit()); QVERIFY(!document.CanUndoEdit());
        QCOMPARE(document.DisplayNames(), CModelDisplayNames::State{}); QCOMPARE(fingerprint(document), saved);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DisplayNames(), names()); QCOMPARE(preview(document), bytes);
        QVERIFY(document.MakeDisplayNameEdit("artmesh", "Mesh", "Mesh", command, &error));
        QVERIFY(document.ExecuteEdit(command)); QVERIFY(document.DisplayNames().artMeshes.isEmpty());
        QCOMPARE(document.DisplayName("part", "Mesh"), names().parts.value("Mesh"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DisplayNames(), names());
    }
    void locksAndPartCyclesRejectWithoutChangingHistory_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("id"); QTest::addColumn<bool>("cycle");
        QTest::newRow("mesh") << "artmesh" << "Mesh" << false;
        QTest::newRow("part") << "part" << "Mesh" << false;
        QTest::newRow("ancestor-part") << "part" << "PartRoot" << false;
        QTest::newRow("ancestor-deformer") << "rotation" << "Root" << false;
        QTest::newRow("part-cycle") << "part" << "unused" << true;
    }
    void locksAndPartCyclesRejectWithoutChangingHistory()
    {
        QFETCH(QString, type); QFETCH(QString, id); QFETCH(bool, cycle);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(fixture(cycle), "names.moc3", &error), qPrintable(error));
        if (!cycle) document.SetLocked(type, id, true);
        const auto state = document.AuthoringStructureState();
        CModelEdit edit;
        QVERIFY(!document.MakeDisplayNameEdit("artmesh", "Mesh", "renamed", edit, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(!document.CanUndoEdit()); QCOMPARE(document.AuthoringStructureState(), state);
    }
    void legacyDeformerApiAndBatchNamesShareOneState()
    {
        CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error)); QVERIFY(document.SetDisplayNames(names(), &error));
        QVERIFY(document.SetDeformerDisplayNames({{"Warp", "legacy"}}, &error));
        QCOMPARE(document.DisplayNames().deformers, document.DeformerDisplayNames());
        QCOMPARE(document.DisplayNames().parts, names().parts); QCOMPARE(document.DisplayNames().artMeshes, names().artMeshes);
        CModelEdit edit;
        QVERIFY(document.MakeDeformerDisplayNameEdit("Warp", "second", edit, &error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DisplayName("warp", "Warp"), QString("second"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DisplayName("warp", "Warp"), QString("legacy"));
    }
    void invalidOrStaleNamesCannotBeApplied_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"unknown-part", "unknown-mesh", "unknown-deformer", "newline", "stale", "coalesce", "request", "index"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void invalidOrStaleNamesCannotBeApplied()
    {
        QFETCH(QString, kind);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error));
        auto proposed = names();
        if (kind.startsWith("unknown")) {
            auto& map = kind == "unknown-part" ? proposed.parts : kind == "unknown-mesh" ? proposed.artMeshes : proposed.deformers;
            map.insert("Unknown", "Unknown");
        }
        if (kind == "newline") proposed.artMeshes["Mesh"] = "bad\nname";
        CModelEdit edit;
        if (kind.startsWith("unknown") || kind == "newline") {
            QVERIFY(!document.MakeDisplayNamesEdit(proposed, edit, &error));
            QVERIFY(!document.SetDisplayNames(proposed, &error));
            QCOMPARE(document.DisplayNames(), CModelDisplayNames::State{});
        } else {
            QVERIFY(document.MakeDisplayNamesEdit(proposed, edit, &error));
            if (kind == "stale") QVERIFY(document.SetDeformerDisplayNames({{"Warp", "intervening"}}, &error));
            if (kind == "coalesce") edit.coalesce = true;
            if (kind == "request") edit.parameterChange = CModelEdit::ParameterChange::Create;
            if (kind == "index") edit.index = 0;
            const auto before = document.AuthoringStructureState();
            QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), before);
        }
        QVERIFY(!document.CanUndoEdit());
    }
    void borrowedNamesAndCommandsOwnTheirStrings()
    {
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error));
        QString storage = "borrowed name";
        CModelDisplayNames::State requested;
        requested.artMeshes.insert("Mesh", QString::fromRawData(storage.constData(), storage.size()));
        CModelEdit edit; QVERIFY(document.MakeDisplayNamesEdit(requested, edit, &error));
        storage.fill('x');
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DisplayName("artmesh", "Mesh"), QString("borrowed name"));
        auto exported = document.DisplayNames(); exported.artMeshes["Mesh"] = "unrelated";
        QVERIFY(document.UndoEdit()); QVERIFY(document.RedoEdit());
        QCOMPARE(document.DisplayName("artmesh", "Mesh"), QString("borrowed name"));
    }
    void session15AndOlderSessionsPreserveTheirDistinctSemantics_data()
    {
        QTest::addColumn<bool>("old"); QTest::addColumn<bool>("empty");
        for (bool old : {false,true}) for (bool empty : {false,true})
            QTest::newRow(qPrintable(QString("%1/%2").arg(old).arg(empty))) << old << empty;
    }
    void session15AndOlderSessionsPreserveTheirDistinctSemantics()
    {
        QFETCH(bool, old); QFETCH(bool, empty);
        CModelDocument loaded; QString error; QVERIFY(loaded.LoadBytes(fixture(), "names.moc3", &error));
        QVERIFY(loaded.SetDisplayNames(names(), &error));
        auto saved = session(loaded);
        if (empty) saved.objectDisplayNames = CModelDisplayNames::State{};
        if (old) {
            saved.version = 13; saved.objectDisplayNames.reset();
            saved.deformerDisplayNames = empty ? CDeformerDisplayNames::State{} : names().deformers;
        }
        const auto bytes = saved.ToJson(); QVERIFY(!bytes.isEmpty());
        const auto json = QJsonDocument::fromJson(bytes).object(); QCOMPARE(json.value("version").toInt(), old ? 13 : 15);
        QCOMPARE(json.contains("objectDisplayNames"), !old); QCOMPARE(json.contains("deformerDisplayNames"), old);
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
        auto expected = empty ? CModelDisplayNames::State{} : names();
        if (old) { expected.parts = names().parts; expected.artMeshes = names().artMeshes; }
        QCOMPARE(restored.DisplayNames(), expected); QCOMPARE(preview(restored), preview(loaded));
    }
    void malformedSessionNamesAreAtomic_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"missing", "null", "legacy", "unknown", "bad-map", "conflict", "future"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void malformedSessionNamesAreAtomic()
    {
        QFETCH(QString, kind);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error));
        QVERIFY(document.SetDisplayNames(names(), &error)); auto saved = session(document);
        if (kind == "conflict") {
            saved.deformerDisplayNames = { {"Warp", "conflict"} }; QVERIFY(saved.ToJson().isEmpty()); return;
        }
        auto json = QJsonDocument::fromJson(saved.ToJson()).object();
        if (kind == "missing") json.remove("objectDisplayNames");
        if (kind == "null") json["objectDisplayNames"] = QJsonValue::Null;
        if (kind == "legacy") json["deformerDisplayNames"] = QJsonObject{};
        if (kind == "future") json["version"] = 16;
        if (kind == "unknown" || kind == "bad-map") {
            auto values = json["objectDisplayNames"].toObject();
            values["artMeshes"] = kind == "unknown" ? QJsonValue(QJsonObject{{"Unknown", "Unknown"}}) : QJsonValue("wrong");
            json["objectDisplayNames"] = values;
        }
        auto decoded = saved;
        if (kind == "unknown") {
            QVERIFY(CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error));
            CModelDocument output = document;
            QVERIFY(!decoded.RestoreDocument(document, output, &error)); QCOMPARE(output.DisplayNames(), names());
        } else {
            QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error));
            QCOMPARE(decoded.objectDisplayNames, saved.objectDisplayNames);
        }
    }
    void structureReplayPreservesAllNameTypes()
    {
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error));
        QVERIFY(document.SetDisplayNames(names(), &error));
        const auto before = document.AuthoringStructureState();
        CMocMeshStructureBuilder::Request request;
        CMocMeshStructureBuilder::MeshEdit mesh;
        mesh.artMeshId = "Mesh";
        mesh.vertices = {{{0}, {1}}, {{1}, {1}}, {{2}, {1}}, {{0, 1, 2}, {0.25, 0.25, 0.5}}};
        mesh.oldToNew = {0, 1, 2};
        mesh.indices = {0, 1, 3, 1, 2, 3, 2, 0, 3};
        request.meshes = {mesh};
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.DisplayNames(), names());
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DisplayNames(), names());
    }
    void unrelatedStructureCannotSmuggleNames_data()
    {
        QTest::addColumn<bool>("meshEdit"); QTest::addColumn<QString>("field");
        for (bool mesh : {false,true}) for (const char* field : {"partDisplayNames", "artMeshDisplayNames", "deformerDisplayNames"})
            QTest::newRow(qPrintable(QString("%1/%2").arg(mesh).arg(field))) << mesh << QString::fromLatin1(field);
    }
    void unrelatedStructureCannotSmuggleNames()
    {
        QFETCH(bool, meshEdit); QFETCH(QString, field);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(fixture(), "names.moc3", &error));
        QVERIFY(document.SetDisplayNames(names(), &error));
        if (!meshEdit) document.SetLocked("artmesh", "Mesh", true);
        const auto before = document.AuthoringStructureState(); CModelEdit edit;
        if (meshEdit) {
            CMocMeshStructureBuilder::MeshEdit mesh;
            mesh.artMeshId = "Mesh"; mesh.oldToNew = {0, 1, 2};
            mesh.vertices = {{{0}, {1}}, {{1}, {1}}, {{2}, {1}}, {{0, 1, 2}, {0.25, 0.25, 0.5}}};
            mesh.indices = {0, 1, 3, 1, 2, 3, 2, 0, 3};
            CMocMeshStructureBuilder::Request request; request.meshes = {mesh};
            QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
            edit.meshVerification = {};
        } else {
            CMocParameterStructureBuilder::CreateRequest request;
            request.parameter = {"NewParameter", -1, 1, 0, 2, false};
            QVERIFY2(document.MakeCreateParameterEdit(request, "parameter", edit, &error), qPrintable(error));
        }
        auto after = edit.after.toMap();
        auto values = after.value(field).value<CModelDisplayNames::Map>();
        values.insert(field == "deformerDisplayNames" ? "Warp" : "Mesh", "forged");
        after.insert(field, QVariant::fromValue(values)); edit.after = after;
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(!document.CanUndoEdit());
    }
};

QTEST_GUILESS_MAIN(ModelDisplayNamesTests)
#include "model_display_names_tests.moc"
