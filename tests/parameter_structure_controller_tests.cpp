#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QTemporaryDir>
#include <stdexcept>

using namespace live2dstruct;

namespace {
using Builder = CMocParameterStructureBuilder;

struct Preview {
    int attempts = 0, accepted = 0, ended = 0, definitions = 0;
    bool reject = false, throws = false, keep = false, restoring = false;
    QByteArray bytes;
    QMap<QString, float> values;
    std::function<void()> during;

    void Connect(CKeyformEditController& controller)
    {
        controller.SetPublisher([this](const CModelDocument& document, const auto& pose, bool, QString* error) {
            values = pose;
            return document.BuildPreviewMoc(bytes, error);
        }, [this](QString*) { ++ended; return true; });
        controller.SetDefinitionPublisher([this](const auto&, const auto&, bool, QString*) { ++definitions; return false; });
        controller.SetStructurePublisher([this](const CModelDocument& document, const CModelEdit& command,
            bool forward, bool session, const auto& baseline, bool editing, auto& pose, QString* error) {
            ++attempts;
            if (during) during();
            if (throws) throw std::runtime_error("publication failed");
            if (reject) { if (error) *error = "publication rejected"; return false; }
            QByteArray candidate;
            if (!document.BuildPreviewMoc(candidate, error)) return false;
            QMap<QString, float> next;
            for (const auto& parameter : document.Parameters())
                next.insert(parameter.id, pose.value(parameter.id, document.ParameterValue(parameter.id)));
            const QVariant& archived = forward ? command.runtime->after : command.runtime->before;
            const auto saved = archived.value<QMap<QString, float>>();
            for (auto value = saved.cbegin(); value != saved.cend(); ++value)
                if (!values.contains(value.key()) && next.contains(value.key())) next[value.key()] = value.value();
            for (auto value = baseline.cbegin(); value != baseline.cend(); ++value) next[value.key()] = value.value();
            QVariant captured = QVariant::fromValue(values);
            (forward ? command.runtime->before : command.runtime->after).swap(captured);
            bytes.swap(candidate);
            values.swap(next);
            pose = values;
            keep = editing;
            restoring = session;
            ++accepted;
            return true;
        }, [this](const CModelDocument&, const CModelEdit& command, bool forward, auto& pose, QString*) {
            const auto archived = (forward ? command.runtime->after : command.runtime->before).value<QMap<QString, float>>();
            for (auto value = archived.cbegin(); value != archived.cend(); ++value)
                if (!values.contains(value.key()) && pose.contains(value.key())) pose[value.key()] = value.value();
            return true;
        });
    }
};

Builder::CreateRequest request(bool bound = false, int keys = 3)
{
    Builder::CreateRequest result;
    result.parameter.id = QStringLiteral("NewParameter");
    if (bound) result.bindings.append({{Builder::Owner::ArtMesh, QStringLiteral("Mesh")},
        keys == 3 ? QVector<float>{-1, 0, 1} : QVector<float>{-1, 1}});
    return result;
}
}

class ParameterStructureControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void rejectionPreservesDocumentHistoryAndPreview_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("missing publisher") << 0;
        QTest::newRow("rejected") << 1;
        QTest::newRow("exception") << 2;
    }

    void rejectionPreservesDocumentHistoryAndPreview()
    {
        QFETCH(int, fault);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(DeformerSnapshotData::sample("Mark"), &error), qPrintable(error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        if (!fault) controller.SetStructurePublisher({});
        preview.reject = fault == 1;
        preview.throws = fault == 2;
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request(), QStringLiteral("新参数"), edit, &error), qPrintable(error));
        const auto state = document.ParameterStructureState();
        const auto revision = document.Revision();
        QVERIFY(!controller.Submit(edit, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(document.ParameterStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.PeekUndoEdit());
        QCOMPARE(preview.accepted, 0);
        QVERIFY(preview.bytes.isEmpty());
    }

    void createBindUndoRedoRetainsOriginalIdentity()
    {
        QTemporaryDir directory;
        DeformerSnapshotData::Fixture fixture;
        const auto path = directory.filePath("source.moc3");
        QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto original = document.ParameterStructureState();
        const auto digest = document.SourceDigest();
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        CModelEdit create;
        QVERIFY2(document.MakeCreateParameterEdit(request(true), "New", create, &error), qPrintable(error));
        QVERIFY2(controller.Submit(create, &error), qPrintable(error));
        QCOMPARE(document.Parameters().size(), 2);
        QCOMPARE(document.Drawables().front().keyforms.size(), 6);
        QCOMPARE(document.SourceBytes(), fixture.bytes);
        QCOMPARE(document.SourceDigest(), digest);
        const auto created = document.ParameterStructureState();
        const auto createdBytes = preview.bytes;
        CModelEdit history;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QVERIFY(history.runtime);
        QCOMPARE(document.ParameterStructureState(), original);
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QCOMPARE(document.ParameterStructureState(), created);
        QCOMPARE(preview.bytes, createdBytes);
        QCOMPARE(preview.definitions, 0);
        QCOMPARE(preview.ended, 0);
    }

    void undoReentersEditorAfterBindingExits()
    {
        QTemporaryDir directory;
        DeformerSnapshotData::Fixture fixture;
        const auto path = directory.filePath("source.moc3");
        QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        QVERIFY(controller.Activate("Mesh", 1, {{"P", 1}}, &error));
        const auto originalPose = controller.Pose();
        CModelEdit edit;
        QVERIFY(document.MakeCreateParameterEdit(request(true, 2), "New", edit, &error));
        QVERIFY(controller.Submit(edit, &error));
        QVERIFY(!controller.IsActive());
        CModelEdit history;
        preview.reject = true;
        QVERIFY(!controller.History(true, history, &error));
        QVERIFY(!controller.IsActive());
        QVERIFY(document.Parameters().size() == 2);
        preview.reject = false;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QVERIFY(controller.IsActive());
        QCOMPARE(controller.Target(), QStringLiteral("Mesh"));
        QCOMPARE(controller.Type(), QStringLiteral("artmesh"));
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), originalPose);
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QVERIFY(!controller.IsActive());
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(controller.Pose(), originalPose);
    }

    void boundCornerRemapsOrExitsAtomically_data()
    {
        QTest::addColumn<int>("keyCount");
        QTest::newRow("default key preserves editor") << 3;
        QTest::newRow("interpolated default exits editor") << 2;
    }

    void boundCornerRemapsOrExitsAtomically()
    {
        QFETCH(int, keyCount);
        QTemporaryDir directory;
        DeformerSnapshotData::Fixture fixture;
        const auto path = directory.filePath("source.moc3");
        QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        QVERIFY2(controller.Activate("Mesh", 1, {{"P", 1}}, &error), qPrintable(error));
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request(true, keyCount), "New", edit, &error), qPrintable(error));
        preview.reject = true;
        QVERIFY(!controller.Submit(edit, &error));
        QVERIFY(controller.IsActive());
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), (QMap<QString, float>{{"P", 1}}));
        preview.reject = false;
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QCOMPARE(controller.IsActive(), keyCount == 3);
        QCOMPARE(preview.keep, keyCount == 3);
        if (keyCount == 3) {
            QCOMPARE(controller.Keyform(), 3);
            QCOMPARE(controller.Pose().value("NewParameter"), 0.0f);
            QCOMPARE(controller.Pose().value("P"), 1.0f);
        }
        QCOMPARE(preview.ended, 0);
    }

    void newlyBoundCornerCanBeEditedAndMappedAcrossUndo()
    {
        QTemporaryDir directory;
        DeformerSnapshotData::Fixture fixture;
        const auto path = directory.filePath("source.moc3");
        QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto original = document.DrawableKeyformPositions("Mesh", 1);
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request(true), "New", edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY2(controller.Activate("Mesh", 3, {{"P", 1}, {"NewParameter", 0}}, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 3), original);
        auto changed = original;
        changed[0] += .125f;
        edit = {"Mesh", "artmesh", CModelEdit::KeyformPositions,
                QVariant::fromValue(original), QVariant::fromValue(changed), 3, false};
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 3), changed);
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 1), original);
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 5), original);
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(document, "Mesh", 3, controller.Pose(), &error), qPrintable(error));
        QVector<float> world, roundTrip;
        QVERIFY2(mapping.ToModel(changed, world, &error), qPrintable(error));
        QVERIFY2(mapping.FromModel(world, roundTrip, &error), qPrintable(error));
        QVERIFY(DeformerSnapshotData::near(roundTrip, changed));
        CModelEdit history;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 3), original);
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 1), original);
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QCOMPARE(controller.Keyform(), 3);
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions("Mesh", 3), changed);
    }

    void restoredRuntimePoseSelectsItsCornerBeforePublication()
    {
        QTemporaryDir directory;
        DeformerSnapshotData::Fixture fixture;
        const auto path = directory.filePath("source.moc3");
        QVERIFY(DeformerSnapshotData::write(path, fixture.bytes));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request(true), "New", edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY2(controller.Activate("Mesh", 5, {{"P", 1}, {"NewParameter", 1}}, &error), qPrintable(error));
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QCOMPARE(controller.Keyform(), 1);
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QCOMPARE(controller.Keyform(), 5);
        QCOMPARE(controller.Pose().value("NewParameter"), 1.0f);
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues("Mesh", "artmesh", controller.Keyform(), corner, &error));
        for (auto value = corner.cbegin(); value != corner.cend(); ++value)
            QCOMPARE(controller.Pose().value(value.key()), value.value());
    }

    void eachCommandKeepsIndependentRuntimeState()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(DeformerSnapshotData::sample("Mark"), &error), qPrintable(error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        CModelEdit edit;
        QVERIFY(document.MakeCreateParameterEdit(request(), "Old", edit, &error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        preview.values["NewParameter"] = .7f;
        QVERIFY(document.MakeDeleteParameterEdit("NewParameter", edit, &error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        CModelEdit deletion;
        QVERIFY(document.PeekUndoEdit(&deletion));
        QVERIFY(document.MakeCreateParameterEdit(request(), "New", edit, &error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        preview.values["NewParameter"] = .1f;
        CModelEdit recreation;
        QVERIFY(document.PeekUndoEdit(&recreation));
        QVERIFY(deletion.runtime != recreation.runtime);
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QCOMPARE(preview.values.value("NewParameter"), .7f);
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QCOMPARE(preview.values.value("NewParameter"), .1f);
    }

    void restoringStructureUsesDedicatedPublisherAndRejectsReentry()
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(DeformerSnapshotData::sample("Mark"), &error));
        CModelDocument candidate = document;
        CModelEdit edit;
        QVERIFY(candidate.MakeCreateParameterEdit(request(), "New", edit, &error));
        QVERIFY(candidate.ExecuteEdit(edit));
        candidate.ClearEditHistory();
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Preview preview;
        preview.Connect(controller);
        bool reentered = true;
        preview.during = [&] { reentered = controller.Restore(document); };
        QVERIFY2(controller.Restore(candidate, &error), qPrintable(error));
        QVERIFY(!reentered);
        QVERIFY(preview.restoring);
        QCOMPARE(preview.definitions, 0);
        QCOMPARE(document.ParameterStructure(), candidate.ParameterStructure());
        QVERIFY(!document.PeekUndoEdit());
    }
};

QTEST_GUILESS_MAIN(ParameterStructureControllerTests)
#include "parameter_structure_controller_tests.moc"
