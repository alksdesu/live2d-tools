#include "geometry_batch_fixture.h"
#include <QtTest>
#include <limits>

using namespace GeometryBatchData;

class GeometryBatchDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void oneHistoryRestoresEveryOwnerAndSparseStorage()
    {
        CModelDocument document; QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const auto before = all(document), after = changed(before);
        QCOMPARE(before.size(), 6);
        const auto baseline = document.AuthoringStructureState();
        const auto originalWarp = document.DeformerWarpKeyforms("Warp");
        const auto originalRotation = document.DeformerRotationKeyforms("Root");
        CModelEdit edit;
        QVERIFY2(document.MakeGeometryEdit(before, after, before[0], edit, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(all(document), after);
        QCOMPARE(historyCount(document), 1);
        QVERIFY(!document.CanRedoEdit());
        for (int index = 0; index < 2; ++index) {
            QCOMPARE(document.DeformerWarpKeyforms("Warp")[index].opacity, originalWarp[index].opacity);
            QCOMPARE(document.DeformerRotationKeyforms("Root")[index].opacity, originalRotation[index].opacity);
        }
        CModelEdit archived;
        QVERIFY(document.UndoEdit(&archived));
        QCOMPARE(archived.field, CModelEdit::GeometryKeyforms);
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
        QVERIFY(document.DeformerWarpKeyformOverrides().isEmpty());
        QVERIFY(document.DeformerRotationKeyformOverrides().isEmpty());
        QVERIFY(document.RedoEdit());
        QCOMPARE(all(document), after);
        QCOMPARE(historyCount(document), 1);
    }

    void rejectsWholeBatch_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 15; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void rejectsWholeBatch()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document));
        const auto initial = command(document); QVERIFY(document.ExecuteEdit(initial)); QVERIFY(document.UndoEdit());
        auto before = all(document), after = changed(before);
        auto driver = before[0];
        if (failure == 0) std::get<QVector<float>>(before[3].value)[0] += 1;
        if (failure == 1) after[3].id = "missing";
        if (failure == 2) { before.append(before[3]); after.append(after[3]); }
        if (failure == 3) after[3].value = CModelRotationTransform{};
        if (failure == 4) std::get<QVector<float>>(after[3].value)[0] = std::numeric_limits<float>::quiet_NaN();
        if (failure == 5) std::get<QVector<float>>(after[3].value).removeLast();
        if (failure == 6) { before[3].keyform = 20; after[3].keyform = 20; }
        if (failure == 7) { before[3].type = Frame::Type(99); after[3].type = Frame::Type(99); }
        if (failure == 8) driver.id = "other";
        if (failure == 9) after = before;
        if (failure == 10) after.removeLast();
        if (failure == 11) document.SetLocked("warp", "Warp", true);
        if (failure == 12) document.SetLocked("rotation", "Root", true);
        if (failure == 13) std::get<CModelRotationTransform>(after[4].value).scale = std::numeric_limits<float>::infinity();
        if (failure == 14) { before[3].id = "Root"; after[3].id = "Root"; }
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit output{"sentinel", "type", CModelEdit::Opacity, 1.0f, 0.5f};
        QString error;
        QVERIFY(!document.MakeGeometryEdit(before, after, driver, output, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(output.name, QString("sentinel"));
        CModelEdit raw{driver.id, driver.TypeName(), CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), driver.keyform, false};
        QVERIFY(!document.ExecuteEdit(raw));
        QCOMPARE(document.AuthoringStructureState(), baseline); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
    }

    void malformedCommandIsRejected_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 7; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void malformedCommandIsRejected()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document)); auto edit = command(document);
        if (failure == 0) edit.coalesce = true;
        if (failure == 1) edit.before = QVariantList{};
        if (failure == 2) edit.after = QVariantMap{};
        if (failure == 3) edit.parameterChange = CModelEdit::ParameterChange::Create;
        if (failure == 4) edit.freezeValue = 0.5f;
        if (failure == 5) edit.type = "objects";
        if (failure == 6) edit.index = -1;
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
    }

    void unchangedLockedDriverCanBeRead()
    {
        CModelDocument document; QVERIFY(load(document));
        auto before = all(document); auto after = before;
        std::get<QVector<float>>(after[2].value)[0] += 0.1f;
        document.SetLocked("artmesh", "Mesh", true);
        CModelEdit edit; QString error;
        QVERIFY2(document.MakeGeometryEdit(before, after, before[0], edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(all(document), after);
        QVERIFY(!document.DrawableKeyformPositionOverrides().contains("Mesh"));
        QVERIFY(document.UndoEdit()); QCOMPARE(all(document), before);
        QVERIFY(document.IsLocked("artmesh", "Mesh"));
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), after);
    }

    void signedZeroIsAChangeAndStaleSignIsRejected()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms("Root"); forms[0].scale = 0.0f;
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, frame));
        State before{frame}, after = before;
        std::get<CModelRotationTransform>(after[0].value).scale = -0.0f;
        QVERIFY(before != after);
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, frame, edit));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
        QVERIFY(!document.ExecuteEdit(edit));
        QVERIFY(document.UndoEdit()); QVERIFY(!std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
        QVERIFY(document.RedoEdit()); QVERIFY(std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
    }

    void locksAddedLaterDoNotTrapGeometryHistory()
    {
        CModelDocument document; QVERIFY(load(document)); const auto before = all(document);
        QVERIFY(document.ExecuteEdit(command(document)));
        document.SetLocked("rotation", "Root", true);
        QVERIFY(document.UndoEdit()); QCOMPARE(all(document), before);
        QVERIFY(document.IsLocked("rotation", "Root"));
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), changed(before));
    }

    void rawRotationReflectionRemainsAnAllowedStoredGeometry()
    {
        CModelDocument document; QVERIFY(load(document));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, frame));
        const State before{frame}; State after = before;
        auto& transform = std::get<CModelRotationTransform>(after[0].value);
        transform.reflectX = !transform.reflectX; transform.reflectY = !transform.reflectY; transform.scale = -0.75f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, frame, edit));
        QVERIFY(document.ExecuteEdit(edit));
        Frame actual; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, actual));
        QCOMPARE(actual, after[0]); QVERIFY(document.UndoEdit());
        QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, actual)); QCOMPARE(actual, before[0]);
    }

    void historyRejectsInterveningOwnerMutation()
    {
        CModelDocument document; QVERIFY(load(document)); auto edit = command(document);
        QVERIFY(document.ExecuteEdit(edit));
        auto positions = document.DrawableKeyformPositions("Mesh", 1); positions[0] += 1;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, positions));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(document.CanUndoEdit());
    }

    void savedSessionAndCompiledModelRetainBothKeyforms()
    {
        CModelDocument document; QString error; QVERIFY(load(document));
        const auto source = document; const auto before = all(document), expected = changed(before);
        QVERIFY(document.ExecuteEdit(command(document)));
        QByteArray bytes; QVERIFY2(document.BuildRuntimeMoc(bytes, &error), qPrintable(error));
        CModelDocument compiled; QVERIFY2(compiled.LoadBytes(bytes, "compiled", &error), qPrintable(error));
        QCOMPARE(all(compiled), expected);
        QCOMPARE(compiled.Drawables()[0].uvs, source.Drawables()[0].uvs);
        QCOMPARE(compiled.DrawableIndices("Mesh"), source.DrawableIndices("Mesh"));
        QCOMPARE(compiled.DeformerWarpKeyforms("Warp")[1].opacity, source.DeformerWarpKeyforms("Warp")[1].opacity);
        CModelSession stored; QVERIFY2(CModelSession::FromJson(session(document).ToJson(), &stored, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(stored.RestoreDocument(source, restored, &error), qPrintable(error));
        QCOMPARE(all(restored), expected); QVERIFY(!restored.CanUndoEdit());
        QVERIFY(restored.BuildRuntimeMoc(bytes, &error));
    }

    void exactStorageRejectsSignedZeroDrift()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms("Root"); forms[0].scale = 0.0f;
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        Frame driver; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, driver));
        State before{driver}, after = before;
        std::get<CModelRotationTransform>(after[0].value).scale = -0.0f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, driver, edit));
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(document.CanUndoEdit());
    }

    void unrelatedParameterChangesDoNotInvalidateGeometryStorage()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = all(document); QVERIFY(document.ExecuteEdit(command(document)));
        QVERIFY(document.SetParameterValue("P", 0.4f)); QVERIFY(document.UndoEdit());
        QCOMPARE(all(document), before); QCOMPARE(document.ParameterValue("P"), 0.4f);
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), changed(before));
    }

    void partAncestorsAndEffectiveDeformerParentsAreRespected_data()
    {
        QTest::addColumn<bool>("deformerPart");
        QTest::newRow("mesh-part") << false; QTest::newRow("ancestor-deformer-part") << true;
    }
    void partAncestorsAndEffectiveDeformerParentsAreRespected()
    {
        QFETCH(bool, deformerPart);
        DeformerSnapshotData::Fixture source; const auto& o = *source.table.sectionOffsetTable;
        source.count(0, 2);
        for (int index = 0; index < 2; ++index) {
            source.id(o.parts->ids, index, index ? "PartChild" : "PartRoot");
            source.column(o.parts->keyform_binding_sources_indices, index, -1);
            source.column(o.parts->keyform_sources_begin_indices, index, 0);
            source.column(o.parts->keyform_sources_counts, index, 0);
            source.column(o.parts->is_visible, index, 1); source.column(o.parts->is_enabled, index, 1);
            source.column(o.parts->parent_part_indices, index, index - 1);
        }
        source.column(deformerPart ? o.deformers->parent_part_indices : o.art_meshes->parent_part_indices,
                      deformerPart ? 1 : 0, 1);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source.bytes, "parts", &error), qPrintable(error));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, frame));
        const State before{frame}, after = changed(before);
        document.SetLocked("part", "PartRoot", true);
        CModelEdit edit; QVERIFY(!document.MakeGeometryEdit(before, after, frame, edit, &error));
        QVERIFY(error.contains("PartRoot"));
        if (deformerPart) {
            QVERIFY(document.SetDeformerParentIndex("Child", -1));
            QVERIFY2(document.MakeGeometryEdit(before, after, frame, edit, &error), qPrintable(error));
            QVERIFY(document.SetDeformerParentIndex("Child", 1));
            QVERIFY(!document.ExecuteEdit(edit));
        }
    }

    void sharedNormalRowsDetachWithoutChangingTheOtherOwner()
    {
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(sharedFixture(false), "shared", &error), qPrintable(error));
        Frame source, target;
        QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, source));
        QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Twin", 0, target));
        const State before{source, target}; auto after = changed(before);
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, source, edit, &error));
        QVERIFY(document.ExecuteEdit(edit)); QByteArray compiled;
        QVERIFY2(document.BuildRuntimeMoc(compiled, &error), qPrintable(error));
        CModelDocument rebuilt; QVERIFY(rebuilt.LoadBytes(compiled, "rebuilt", &error));
        Frame observed;
        QVERIFY(rebuilt.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, observed)); QCOMPARE(observed, after[0]);
        QVERIFY(rebuilt.CaptureGeometryKeyform(Frame::ArtMesh, "Twin", 0, observed)); QCOMPARE(observed, after[1]);
        QCOMPARE(rebuilt.DrawableKeyformPositions("Mesh", 1), document.Drawables()[0].keyforms[1].positions);
        QCOMPARE(rebuilt.DrawableKeyformPositions("Twin", 1), document.Drawables()[1].keyforms[1].positions);
        QVERIFY(document.UndoEdit()); QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
    }

    void captureRejectsWrongTypesWithoutTouchingOutput()
    {
        CModelDocument document; QVERIFY(load(document));
        Frame output{Frame::ArtMesh, "sentinel", 33, QVector<float>{7, 9}}; const auto before = output;
        QVERIFY(!document.CaptureGeometryKeyform(Frame::Rotation, "Warp", 0, output)); QCOMPARE(output, before);
        QVERIFY(!document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 4, output)); QCOMPARE(output, before);
        QVERIFY(!document.CaptureGeometryKeyform(Frame::Type(44), "Mesh", 0, output)); QCOMPARE(output, before);
    }
};

QTEST_APPLESS_MAIN(GeometryBatchDocumentTests)
#include "geometry_batch_document_tests.moc"
