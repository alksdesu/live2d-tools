#include "geometry_batch_fixture.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include <QtTest>
#include <functional>
#include <stdexcept>

using namespace GeometryBatchData;

namespace {
struct Preview
{
    int attempts = 0, accepted = 0;
    bool reject = false, throwing = false;
    QByteArray bytes;
    QMap<QString, float> values;
    bool editing = false;
    std::function<void()> callback;

    void connect(CKeyformEditController& controller)
    {
        controller.SetPublisher([this](const CModelDocument& candidate, const QMap<QString, float>& pose,
                                      bool active, QString* error) {
            ++attempts;
            if (callback) callback();
            if (throwing) throw std::runtime_error("controlled geometry publisher exception");
            if (reject) { if (error) *error = "controlled geometry publisher rejection"; return false; }
            QByteArray output;
            if (!candidate.BuildRuntimeMoc(output, error)) return false;
            bytes = std::move(output); values = pose; editing = active; ++accepted;
            return true;
        }, [](QString*) { return true; });
    }
};
}

class GeometryBatchControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void onePublisherAndOneHistoryForAllOwners()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto initial = document.AuthoringStructureState();
        const auto expected = changed(all(document));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.connect(controller); QString error;
        auto edit = command(document);
        QVERIFY(CKeyformEditController::Handles(edit));
        QVERIFY2(controller.Submit(edit, pose(document), 1, &error), qPrintable(error));
        QCOMPARE(preview.attempts, 1); QCOMPARE(preview.accepted, 1);
        QCOMPARE(all(document), expected); QCOMPARE(historyCount(document), 1);
        QCOMPARE(controller.Target(), QString("Mesh")); QCOMPARE(controller.Type(), QString("artmesh"));
        QCOMPARE(controller.Keyform(), 0); QCOMPARE(controller.Pose().value("P"), 0.0f);
        QVERIFY(preview.editing); QCOMPARE(preview.values, controller.Pose());
        CModelDocument compiled; QVERIFY(compiled.LoadBytes(preview.bytes, "compiled")); QCOMPARE(all(compiled), expected);
        CModelEdit undone;
        QVERIFY2(controller.History(true, undone, &error), qPrintable(error));
        QCOMPARE(preview.attempts, 2); QCOMPARE(document.AuthoringStructureState(), initial);
        QVERIFY(controller.History(false, undone, &error)); QCOMPARE(preview.attempts, 3);
        QCOMPARE(all(document), expected);
    }

    void failuresPreserveDocumentHistoryAndLogicalPreview_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 5; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void failuresPreserveDocumentHistoryAndLogicalPreview()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document));
        QVERIFY(document.ExecuteEdit(command(document))); QVERIFY(document.UndoEdit());
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.connect(controller); QString error;
        QVERIFY(controller.Activate("Root", "rotation", 1, pose(document), &error));
        const auto logicalPose = controller.Pose(); const auto originalBytes = preview.bytes;
        auto edit = command(document);
        if (failure == 0) preview.reject = true;
        if (failure == 1) preview.throwing = true;
        if (failure == 2) {
            auto after = edit.after.value<State>(); std::get<QVector<float>>(after[3].value).removeLast();
            edit.after = QVariant::fromValue(after);
        }
        if (failure == 3) document.SetLocked("warp", "Warp", true);
        if (failure == 4) {
            auto before = edit.before.value<State>(); std::get<CModelRotationTransform>(before[5].value).angle += 1;
            edit.before = QVariant::fromValue(before);
        }
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        const int attempts = preview.attempts;
        QVERIFY(!controller.Submit(edit, pose(document), 0, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(preview.attempts, attempts + (failure < 2 ? 1 : 0));
        QCOMPARE(preview.bytes, originalBytes); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
        QCOMPARE(controller.Target(), QString("Root")); QCOMPARE(controller.Keyform(), 1); QCOMPARE(controller.Pose(), logicalPose);
    }

    void failedHistoryDoesNotAdvanceOrReturnACommand_data()
    {
        QTest::addColumn<bool>("undo"); QTest::addColumn<bool>("throwing");
        QTest::newRow("undo-reject") << true << false;
        QTest::newRow("redo-reject") << false << false;
        QTest::newRow("undo-throw") << true << true;
        QTest::newRow("redo-throw") << false << true;
    }
    void failedHistoryDoesNotAdvanceOrReturnACommand()
    {
        QFETCH(bool, undo); QFETCH(bool, throwing);
        CModelDocument document; QVERIFY(load(document)); CKeyformEditController controller;
        controller.SetDocument(&document); Preview preview; preview.connect(controller);
        QString error; QVERIFY(controller.Submit(command(document), &error));
        CModelEdit previous;
        if (!undo) QVERIFY(controller.History(true, previous, &error));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        const auto bytes = preview.bytes; const auto values = controller.Pose(); const int attempts = preview.attempts;
        preview.reject = !throwing; preview.throwing = throwing;
        CModelEdit output{"sentinel", "type", CModelEdit::Opacity, 1.0f, 0.5f};
        QVERIFY(!controller.History(undo, output, &error)); QCOMPARE(output.name, QString("sentinel"));
        QCOMPARE(preview.attempts, attempts + 1); QCOMPARE(preview.bytes, bytes);
        QCOMPARE(document.AuthoringStructureState(), baseline); QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.CanUndoEdit(), undo); QCOMPARE(document.CanRedoEdit(), !undo); QCOMPARE(controller.Pose(), values);
    }

    void lockedUnchangedSourceSuppliesThePreviewPose()
    {
        CModelDocument document; QVERIFY(load(document)); auto before = all(document), after = before;
        std::get<CModelRotationTransform>(after[5].value).originX += 0.2f;
        document.SetLocked("artmesh", "Mesh", true);
        CModelEdit edit; QString error;
        QVERIFY(document.MakeGeometryEdit(before, after, before[1], edit, &error));
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller);
        QVERIFY2(controller.Submit(edit, pose(document), 0, &error), qPrintable(error));
        QCOMPARE(preview.attempts, 1); QCOMPARE(controller.Target(), QString("Mesh"));
        QCOMPARE(controller.Keyform(), 1); QCOMPARE(controller.Pose().value("P"), 1.0f);
        QCOMPARE(all(document), after); QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
    }

    void reentrantSubmitCannotPublishOrReplaceDocument()
    {
        CModelDocument document; QVERIFY(load(document)); const auto edit = command(document);
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller);
        bool nested = true; CModelDocument other; QVERIFY(load(other));
        preview.callback = [&] {
            nested = controller.Submit(edit);
            controller.SetDocument(&other);
        };
        QString error; QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(!nested); QCOMPARE(preview.attempts, 1); QCOMPARE(all(document), changed(all(other)));
        QVERIFY(!other.CanUndoEdit()); QCOMPARE(historyCount(document), 1);
    }

    void sharedSpecificSourcesPublishOnlyCompatibleChanges_data()
    {
        QTest::addColumn<bool>("same");
        QTest::newRow("same-owner-values") << true; QTest::newRow("different-owner-values") << false;
    }
    void sharedSpecificSourcesPublishOnlyCompatibleChanges()
    {
        QFETCH(bool, same);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(sharedFixture(true), "shared-specific", &error), qPrintable(error));
        Frame source, target;
        QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, source));
        QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Twin", 0, target));
        const State before{source, target}; State after = before;
        std::get<CModelRotationTransform>(after[0].value).angle += 10;
        std::get<CModelRotationTransform>(after[1].value).angle += same ? 10 : 20;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, source, edit, &error));
        CKeyformEditController controller; controller.SetDocument(&document); Preview preview; preview.connect(controller);
        const auto initial = document.AuthoringStructureState(); const auto revision = document.Revision();
        QCOMPARE(controller.Submit(edit, &error), same); QCOMPARE(preview.attempts, 1);
        if (same) {
            QCOMPARE(preview.accepted, 1); QCOMPARE(historyCount(document), 1);
            CModelDocument compiled; QVERIFY(compiled.LoadBytes(preview.bytes, "compiled", &error));
            Frame observed; QVERIFY(compiled.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, observed)); QCOMPARE(observed, after[0]);
            QVERIFY(compiled.CaptureGeometryKeyform(Frame::Rotation, "Twin", 0, observed)); QCOMPARE(observed, after[1]);
        } else {
            QVERIFY(error.contains("共享专用")); QCOMPARE(preview.accepted, 0); QVERIFY(preview.bytes.isEmpty());
            QCOMPARE(document.AuthoringStructureState(), initial); QCOMPARE(document.Revision(), revision);
            QVERIFY(!document.CanUndoEdit()); QVERIFY(!controller.IsActive());
        }
    }

    void activeDragRejectsBatchBeforePublication()
    {
        CModelDocument document; QVERIFY(load(document)); CKeyformEditController controller;
        controller.SetDocument(&document); Preview preview; preview.connect(controller); QString error;
        QVERIFY(controller.Activate("Root", "rotation", 0, pose(document), &error));
        QVERIFY2(controller.BeginDeformerDrag(CKeyformEditController::DeformerHandle::RotationOrigin, {}, &error), qPrintable(error));
        const int attempts = preview.attempts; const auto baseline = document.AuthoringStructureState();
        QVERIFY(!controller.Submit(command(document), &error)); QCOMPARE(preview.attempts, attempts);
        QCOMPARE(document.AuthoringStructureState(), baseline); QVERIFY(controller.IsDragging());
    }
};

QTEST_APPLESS_MAIN(GeometryBatchControllerTests)
#include "geometry_batch_controller_tests.moc"
