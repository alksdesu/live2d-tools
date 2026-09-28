#include "live2dstruct/cmodelexportsnapshot.h"
#include "live2dstruct/cdisplaynamemetadata.h"
#include "live2dstruct/cmodeldocument.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QFile>

using namespace live2dstruct;

namespace {
QByteArray sample()
{
    QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class ModelExportSnapshotTests : public QObject
{
    Q_OBJECT
private slots:
    void ambiguousRuntimeNamesCannotCapture()
    {
        DeformerSnapshotData::Fixture fixture;
        fixture.id(fixture.table.sectionOffsetTable->deformers->ids, 2, "Root");
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "duplicate", &error), qPrintable(error));
        CModelDisplayNames::State names; names.deformers.insert("Root", "ambiguous");
        QVERIFY(!document.SetDisplayNames(names, &error));
        CModelExportSnapshot snapshot;
        QVERIFY(!CModelExportSnapshot::Create(document.SourceDigest(), fixture.bytes, names, snapshot, &error));
        QVERIFY(!snapshot.IsValid());
    }
    void ownedNamesRemainAttachedToExactRuntime()
    {
        CModelDocument document; const auto bytes = sample(); QString error;
        QVERIFY2(document.LoadBytes(bytes, "model", &error), qPrintable(error));
        QString mesh = document.Drawables().first().id, name = QStringLiteral("眼睛 <&> 🌙");
        QByteArray source = document.SourceDigest();
        CModelDisplayNames::State names;
        names.artMeshes.insert(QString::fromRawData(mesh.constData(), mesh.size()), QString::fromRawData(name.constData(), name.size()));
        names.parts.insert(document.Parts().first().id, QStringLiteral("头部"));
        CModelExportSnapshot snapshot;
        QVERIFY2(CModelExportSnapshot::Create(QByteArray::fromRawData(source.constData(), source.size()), bytes, names, snapshot, &error), qPrintable(error));
        const auto id = QString(mesh.constData(), mesh.size()), expected = QString(name.constData(), name.size());
        mesh.fill('x'); name.fill('x'); source.fill('x'); names = {};
        QCOMPARE(snapshot.DisplayName("artmesh", id), expected);
        QCOMPARE(snapshot.DisplayName("part", id), id);
        QCOMPARE(snapshot.SourceDigest(), document.SourceDigest());
        QVERIFY(snapshot.Matches(document.SourceDigest(), document.SourceDigest(), &error));
        QVERIFY(!snapshot.Matches(QByteArray(32, 'x'), document.SourceDigest(), &error));
        QVERIFY(!snapshot.Matches(document.SourceDigest(), QByteArray(32, 'x'), &error));
        QVERIFY(snapshot.Matches(document.SourceDigest(), document.SourceDigest(), &error));
        QVERIFY(error.isEmpty());
    }

    void rejectedCaptureRetainsSnapshotAndChecksUnnormalizedIdentities()
    {
        CModelDocument document; const auto bytes = sample(); QString error;
        QVERIFY(document.LoadBytes(bytes, "model"));
        CModelDisplayNames::State names; names.artMeshes.insert(document.Drawables().first().id, "name");
        CModelExportSnapshot snapshot;
        QVERIFY(CModelExportSnapshot::Create(document.SourceDigest(), bytes, names, snapshot, &error));
        const auto retained = snapshot.DisplayNames();
        for (int failure = 0; failure < 5; ++failure) {
            auto rejected = names;
            if (failure == 0) rejected.parts.insert("unknown", "unknown");
            if (failure == 1) rejected.artMeshes.insert(document.Parts().first().id, "wrong type");
            if (failure == 2) rejected.deformers.insert("unknown", "display name");
            QVERIFY(!CModelExportSnapshot::Create(failure == 3 ? QByteArray() : document.SourceDigest(),
                failure == 4 ? QByteArray("invalid") : bytes, rejected, snapshot, &error));
            QCOMPARE(snapshot.DisplayNames(), retained);
        }
    }

    void metadataRoundTripsTypesAndAuthoritativeEmpty()
    {
        CModelDisplayNames::State names;
        names.parts.insert("same", "part"); names.artMeshes.insert("same", "mesh"); names.deformers.insert("same", "deformer");
        QJsonObject root{{"other", QJsonObject{{"opaque", 7}}}}; QString error;
        QVERIFY(CDisplayNameMetadata::Write(names, root, &error));
        QCOMPARE(root["other"].toObject()["opaque"].toInt(), 7);
        CDisplayNameMetadata::Snapshot parsed;
        QVERIFY(CDisplayNameMetadata::Read(root, parsed, &error));
        QVERIFY(parsed.names); QCOMPARE(*parsed.names, names);
        QVERIFY(parsed.deformerNames); QCOMPARE(*parsed.deformerNames, names.deformers);
        QVERIFY(CDisplayNameMetadata::Write({}, root, &error));
        QVERIFY(CDisplayNameMetadata::Read(root, parsed, &error));
        QVERIFY(parsed.names); QCOMPARE(*parsed.names, CModelDisplayNames::State{});
        QVERIFY(parsed.deformerNames->isEmpty());
        QVERIFY(CDisplayNameMetadata::Read({}, parsed, &error));
        QVERIFY(!parsed.names); QVERIFY(!parsed.deformerNames);
    }

    void metadataRejectsConflictsAndMalformedExtensionsAtomically()
    {
        CModelDisplayNames::State names; names.deformers.insert("same", "name");
        QJsonObject root; QVERIFY(CDisplayNameMetadata::Write(names, root));
        CDisplayNameMetadata::Snapshot parsed; QVERIFY(CDisplayNameMetadata::Read(root, parsed));
        for (int failure = 0; failure < 6; ++failure) {
            auto bad = root; auto full = bad["Live2DToolsDisplayNames"].toObject();
            if (failure == 0) full["Version"] = 2;
            if (failure == 1) full["Names"] = false;
            if (failure == 2) bad["Live2DToolsDeformerNames"] = QJsonObject{{"Version", 1}, {"Names", QJsonObject{}}};
            if (failure == 3) bad["Live2DToolsDeformerNames"] = QJsonValue(QJsonValue::Null);
            if (failure == 4) full["Names"] = QJsonObject{{"deformers", QJsonObject{}}};
            if (failure == 5) full["Version"] = "1";
            bad["Live2DToolsDisplayNames"] = full;
            QString error; QVERIFY(!CDisplayNameMetadata::Read(bad, parsed, &error)); QVERIFY(!error.isEmpty());
            QCOMPARE(*parsed.names, names); QCOMPARE(*parsed.deformerNames, names.deformers);
        }
    }
};

QTEST_APPLESS_MAIN(ModelExportSnapshotTests)
#include "model_export_snapshot_tests.moc"
