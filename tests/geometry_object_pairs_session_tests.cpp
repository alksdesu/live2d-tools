#include "geometry_object_pairs_document_fixture.h"
#include <QtTest>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonArray>

using namespace GeometryPairsData;

class GeometryObjectPairsSessionTests : public QObject
{
    Q_OBJECT
private slots:
    void legacyVersionsRestoreWithoutPairMetadata_data()
    {
        QTest::addColumn<int>("version");
        for (int version = 1; version <= 8; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void legacyVersionsRestoreWithoutPairMetadata()
    {
        QFETCH(int, version);
        CModelDocument document; QVERIFY(load(document)); auto session = capture(document); session.version = version;
        QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        const auto json = QJsonDocument::fromJson(bytes).object(); QCOMPARE(json.value("version").toInt(), version);
        QVERIFY(!json.contains("geometryObjectPairs"));
        CModelSession parsed; QString error; QVERIFY2(CModelSession::FromJson(bytes, &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.geometryObjectPairs, Pairs::State{});
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), Pairs::State{});
    }

    void pairStateUpgradesToNineAndRoundTrips()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); const auto state = paired(document);
        QVERIFY(document.SetGeometryObjectPairs(state)); auto session = capture(document);
        QCOMPARE(session.version, 1); const auto bytes = session.ToJson(); QVERIFY(!bytes.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(bytes).object().value("version").toInt(), 9);
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(bytes, &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.version, 9); QCOMPARE(parsed.geometryObjectPairs, state);
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), state); QVERIFY(!restored.CanUndoEdit());
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
    }

    void metadataInstallsAfterTopologyOverrides()
    {
        CModelDocument document; QVERIFY(load(document));
        for (const auto& id : {QString("Mesh"), QString("Twin")}) {
            auto indices = document.DrawableIndices(id); std::swap(indices[0], indices[1]);
            QVERIFY(document.SetDrawableIndices(id, indices));
        }
        const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state)); QVERIFY(!state.pairs[0].requiresReview);
        CModelSession parsed; QString error; QVERIFY(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), state); QCOMPARE(restored.DrawableIndices("Mesh"), document.DrawableIndices("Mesh"));
        QVERIFY(Pairs::Validate(restored.GeometryObjectPairs(), restored));
    }

    void meshStructureRoundTripRetainsMigratedCorrespondence()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        CModelEdit edit; QVERIFY2(document.MakeMeshStructureEdit(reorder(document, true), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); const auto state = document.GeometryObjectPairs();
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), state); QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
    }

    void reviewStatePersistsWithoutBecomingEnabledAgain()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        auto indices = document.DrawableIndices("Mesh"); std::swap(indices[0], indices[1]); QVERIFY(document.SetDrawableIndices("Mesh", indices));
        const auto state = document.GeometryObjectPairs(); QVERIFY(state.pairs[0].requiresReview);
        CModelSession parsed; QString error; QVERIFY(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), state); QVERIFY(restored.GeometryObjectPairs().pairs[0].requiresReview);
    }

    void malformedJsonPreservesOutput_data()
    {
        QTest::addColumn<int>("failure");
        for (int failure = 0; failure < 8; ++failure) QTest::newRow(qPrintable(QString::number(failure))) << failure;
    }
    void malformedJsonPreservesOutput()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        auto json = QJsonDocument::fromJson(capture(document).ToJson()).object();
        if (failure == 0) json["version"] = 8;
        if (failure == 1) json.remove("geometryObjectPairs");
        if (failure == 2) json["geometryObjectPairs"] = QJsonArray{};
        if (failure == 3) json["sourceDigest"] = QString(64, '1');
        if (failure == 4) json.remove("sourceDigest");
        if (failure == 5) json["version"] = 14;
        if (failure == 6) json["sourceMoc"] = QString("bad") + QChar::Null;
        if (failure == 7) {
            auto state = json["geometryObjectPairs"].toObject(); auto rows = state["pairs"].toArray();
            auto row = rows[0].toObject(); row["vertices"] = QJsonArray{0, 0, 2}; rows[0] = row; state["pairs"] = rows; json["geometryObjectPairs"] = state;
        }
        CModelSession result; result.sourceMoc = "sentinel"; QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &result, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(result.sourceMoc, QString("sentinel"));
    }

    void directInvalidRestoreKeepsExistingDestination_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("old-version") << 0; QTest::newRow("other-source") << 1; QTest::newRow("other-object") << 2;
    }
    void directInvalidRestoreKeepsExistingDestination()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        auto session = capture(document); session.version = 9;
        if (failure == 0) session.version = 8;
        if (failure == 1) session.geometryObjectPairs.sourceDigest[0] ^= 1;
        if (failure == 2) session.geometryObjectPairs.pairs[0].first.object.id = "missing";
        CModelDocument destination = document; QVERIFY(destination.SetParameterValue("P", 0.3f));
        const auto baseline = destination.AuthoringStructureState(); const auto revision = destination.Revision(); QString error;
        QVERIFY(!session.RestoreDocument(document, destination, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(destination.AuthoringStructureState(), baseline); QCOMPARE(destination.Revision(), revision);
    }

    void inMemorySourceAndExplicitEmptyNineAreSupported()
    {
        CModelDocument document; QVERIFY(document.LoadBytes(GeometryBatchData::sharedFixture(false), {}));
        QVERIFY(document.SetGeometryObjectPairs(paired(document))); CModelSession parsed; QString error;
        QVERIFY(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.GeometryObjectPairs(), document.GeometryObjectPairs());
        auto empty = capture(document); empty.version = 9; empty.geometryObjectPairs = {};
        QVERIFY(CModelSession::FromJson(empty.ToJson(), &parsed, &error)); QVERIFY(parsed.RestoreDocument(document, restored, &error));
        QCOMPARE(restored.GeometryObjectPairs(), Pairs::State{});
    }
};

QTEST_APPLESS_MAIN(GeometryObjectPairsSessionTests)
#include "geometry_object_pairs_session_tests.moc"
