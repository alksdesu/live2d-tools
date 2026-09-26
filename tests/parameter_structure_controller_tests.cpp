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
    bool reject = false, throws = false, keep = false, restoring = false, mutateRejectedRuntime = false;
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
            if (mutateRejectedRuntime && (reject || throws)) {
                command.runtime->before = QStringLiteral("rejected before");
                command.runtime->after = QStringLiteral("rejected after");
                command.runtime->beforeEditor = QStringLiteral("rejected editor before");
                command.runtime->afterEditor = QStringLiteral("rejected editor after");
            }
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

Builder::Owner bindingOwner(int type)
{
    return {Builder::Owner::Type(type), type == 0 ? QStringLiteral("Mesh")
        : type == 1 ? QStringLiteral("Warp") : QStringLiteral("Root")};
}

QString bindingType(int type)
{
    return type == 0 ? QStringLiteral("artmesh") : type == 1 ? QStringLiteral("warp") : QStringLiteral("rotation");
}

Builder::BindingKey retained(float value, int index)
{
    return {value, index, Builder::BindingKey::InterpolateOldAxis, -1};
}

Builder::BindingKey inserted(float value, int copy = -1)
{
    return {value, -1, copy < 0 ? Builder::BindingKey::InterpolateOldAxis : Builder::BindingKey::CopyOldSlice, copy};
}

Builder::EditBindingKeysRequest bindingRequest(int type, int axis, const QString& operation)
{
    Builder::EditBindingKeysRequest result{bindingOwner(type), axis, QStringLiteral("Axis%1").arg(axis), {-1, 0, 1}, {}};
    if (operation == QLatin1String("move")) result.keys = {retained(-1, 0), retained(.25f, 1), retained(1, 2)};
    else if (operation == QLatin1String("delete-other")) result.keys = {retained(-1, 0), retained(0, 1)};
    else if (operation == QLatin1String("delete-current")) result.keys = {retained(-1, 0), retained(1, 2)};
    else if (operation == QLatin1String("replace-copy")) result.keys = {retained(-1, 0), inserted(0, 1), inserted(.5f), retained(1, 2)};
    else result.keys = {retained(-1, 0), inserted(-.5f, operation == QLatin1String("copy") ? 0 : -1), retained(0, 1), retained(1, 2)};
    return result;
}

bool bindingDocument(int type, CModelDocument& document, QString* error, bool repeated = false)
{
    DeformerSnapshotData::Fixture fixture;
    if (!fixture.table.IsValid()) return false;
    fixture.column(fixture.table.sectionOffsetTable->art_meshes->keyform_binding_sources_indices, 0, -1);
    fixture.column(fixture.table.sectionOffsetTable->art_meshes->keyform_sources_counts, 0, 1);
    auto bytes = fixture.bytes;
    for (int axis = 0; axis < 3; ++axis) {
        Builder::CreateRequest create;
        create.parameter.id = QStringLiteral("Axis%1").arg(axis);
        create.bindings.append({bindingOwner(type), repeated && axis == 1 ? QVector<float>{0, .5f, 1} : QVector<float>{-1, 0, 1}});
        Builder::Result result;
        if (!Builder::Create(bytes, create, result, error)) return false;
        bytes = result.bytes;
    }
    if (repeated) {
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        if (!table.IsValid()) return false;
        const auto* parameters = table.sectionOffsetTable->parameters;
        DeformerSnapshotData::put(bytes, parameters->parameter_binding_sources_counts + 4, 2);
        DeformerSnapshotData::put(bytes, parameters->parameter_binding_sources_counts + 8, 0);
    }
    return document.LoadBytes(bytes, QStringLiteral("binding-controller.moc3"), error);
}

int bindingCorner(int axis, int digit, int axisCount = 3)
{
    QVector<int> digits{2, 0, 1}, counts{3, 3, 3};
    digits[axis] = digit;
    counts[axis] = axisCount;
    return digits[0] + counts[0] * (digits[1] + counts[1] * digits[2]);
}

QVariantMap editorState(const CKeyformEditController& controller)
{
    return {{"target", controller.Target()}, {"type", controller.Type()}, {"keyform", controller.Keyform()},
            {"pose", QVariant::fromValue(controller.Pose())}};
}

QVariantMap runtimeState(const CModelEdit& edit)
{
    return edit.runtime ? QVariantMap{{"before", edit.runtime->before}, {"after", edit.runtime->after},
        {"beforeEditor", edit.runtime->beforeEditor}, {"afterEditor", edit.runtime->afterEditor}} : QVariantMap{};
}
}

class ParameterStructureControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void bindingKeysPreserveOrEndTheSelectedIdentity_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("axis");
        QTest::addColumn<QString>("operation");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis)
            for (const QString& operation : {QString("move"), QString("delete-other"), QString("delete-current"),
                                            QString("insert"), QString("copy"), QString("replace-copy")})
                QTest::newRow(qPrintable(QString("type%1-axis%2-%3").arg(type).arg(axis).arg(operation))) << type << axis << operation;
    }

    void bindingKeysPreserveOrEndTheSelectedIdentity()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(QString, operation);
        CModelDocument document; QString error;
        QVERIFY2(bindingDocument(type, document, &error), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        const auto owner = bindingOwner(type);
        const int oldCorner = bindingCorner(axis, 1);
        QVERIFY2(controller.Activate(owner.id, bindingType(type), oldCorner, DeformerSnapshotData::pose(document), &error), qPrintable(error));
        const auto before = document.ParameterStructureState(), selected = editorState(controller);
        const auto beforePose = controller.Pose();
        const auto source = document.SourceBytes();
        CModelEdit edit;
        const auto request = bindingRequest(type, axis, operation);
        QVERIFY2(document.MakeBindingKeysEdit(request, edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        const bool survives = operation != QLatin1String("delete-current") && operation != QLatin1String("replace-copy");
        QCOMPARE(controller.IsActive(), survives); QCOMPARE(preview.keep, survives);
        if (survives) {
            const int nextDigit = operation == QLatin1String("insert") || operation == QLatin1String("copy") ? 2 : 1;
            QCOMPARE(controller.Keyform(), bindingCorner(axis, nextDigit, request.keys.size()));
            auto expected = beforePose;
            if (operation == QLatin1String("move")) expected[request.parameterId] = .25f;
            QCOMPARE(controller.Pose(), expected);
            QMap<QString, float> corner;
            QVERIFY(document.KeyformParameterValues(owner.id, bindingType(type), controller.Keyform(), corner));
            for (auto value = corner.cbegin(); value != corner.cend(); ++value) QCOMPARE(controller.Pose().value(value.key()), value.value());
        } else {
            QCOMPARE(controller.Keyform(), -1); QVERIFY(controller.Pose().isEmpty());
        }
        const auto after = document.ParameterStructureState(), afterSelection = editorState(controller);
        CModelEdit history;
        for (int cycle = 0; cycle < 2; ++cycle) {
            QVERIFY2(controller.History(true, history, &error), qPrintable(error));
            QCOMPARE(document.ParameterStructureState(), before); QCOMPARE(editorState(controller), selected);
            QVERIFY2(controller.History(false, history, &error), qPrintable(error));
            QCOMPARE(document.ParameterStructureState(), after); QCOMPARE(editorState(controller), afterSelection);
        }
        QCOMPARE(document.SourceBytes(), source); QCOMPARE(preview.ended, 0);
    }

    void addedCornersRestoreFromTheDestinationArchive_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("axis"); QTest::addColumn<bool>("copy");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis) for (bool copy : {false, true})
            QTest::newRow(qPrintable(QString("type%1-axis%2-%3").arg(type).arg(axis).arg(copy ? "copy" : "interpolate"))) << type << axis << copy;
    }

    void addedCornersRestoreFromTheDestinationArchive()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(bool, copy);
        CModelDocument document; QString error; QVERIFY2(bindingDocument(type, document, &error), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        const auto owner = bindingOwner(type);
        QVERIFY(controller.Activate(owner.id, bindingType(type), bindingCorner(axis, 1), DeformerSnapshotData::pose(document), &error));
        CModelEdit edit;
        QVERIFY2(document.MakeBindingKeysEdit(bindingRequest(type, axis, copy ? "copy" : "insert"), edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        const int added = bindingCorner(axis, 1, 4);
        QVERIFY2(controller.Activate(owner.id, bindingType(type), added, controller.Pose(), &error), qPrintable(error));
        const auto selected = editorState(controller);
        QCOMPARE(controller.Pose().value(QStringLiteral("Axis%1").arg(axis)), -.5f);
        for (int cycle = 0; cycle < 2; ++cycle) {
            QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
            QVERIFY(!controller.IsActive()); QVERIFY(!preview.keep);
            QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
            QCOMPARE(editorState(controller), selected); QVERIFY(preview.keep);
        }
    }

    void repeatedParameterAxesNeverJumpToAnotherReachableCorner_data()
    {
        QTest::addColumn<int>("type");
        for (int type = 0; type < 3; ++type) QTest::newRow(qPrintable(QString::number(type))) << type;
    }

    void repeatedParameterAxesNeverJumpToAnotherReachableCorner()
    {
        QFETCH(int, type);
        CModelDocument document; QString error; QVERIFY2(bindingDocument(type, document, &error, true), qPrintable(error));
        const auto owner = bindingOwner(type);
        QMap<QString, float> corner;
        QVERIFY(!document.KeyformParameterValues(owner.id, bindingType(type), 0, corner));
        QVERIFY(document.KeyformParameterValues(owner.id, bindingType(type), 19, corner));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        QVERIFY2(controller.Activate(owner.id, bindingType(type), 19, DeformerSnapshotData::pose(document), &error), qPrintable(error));
        const auto selected = editorState(controller);
        auto request = bindingRequest(type, 0, "move"); request.keys[1].value = .5f;
        CModelEdit edit;
        QVERIFY2(document.MakeBindingKeysEdit(request, edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(!controller.IsActive()); QVERIFY(!preview.keep);
        QVERIFY(!document.KeyformParameterValues(owner.id, bindingType(type), 19, corner));
        QVERIFY(document.KeyformParameterValues(owner.id, bindingType(type), 22, corner));
        QCOMPARE(corner.value("Axis0"), .5f);
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QCOMPARE(editorState(controller), selected);
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QVERIFY(!controller.IsActive());
    }

    void genericStructureLookupSkipsEarlierUnreachableCorners()
    {
        CModelDocument document; QString error; QVERIFY2(bindingDocument(0, document, &error, true), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        QVERIFY2(controller.Activate("Mesh", "artmesh", 19, DeformerSnapshotData::pose(document), &error), qPrintable(error));
        CModelEdit edit;
        QVERIFY2(document.MakeCreateParameterEdit(request(), "New", edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(controller.IsActive()); QCOMPARE(controller.Keyform(), 19); QVERIFY(preview.keep);
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QVERIFY(controller.IsActive()); QCOMPARE(controller.Keyform(), 19);
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QVERIFY(controller.IsActive()); QCOMPARE(controller.Keyform(), 19);
    }

    void bindingKeysDoNotReplaceAnotherActiveObjectOrStartAnEditor_data()
    {
        QTest::addColumn<bool>("active"); QTest::newRow("inactive") << false; QTest::newRow("other-object") << true;
    }

    void bindingKeysDoNotReplaceAnotherActiveObjectOrStartAnEditor()
    {
        QFETCH(bool, active);
        CModelDocument document; QString error; QVERIFY2(bindingDocument(0, document, &error), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        if (active) QVERIFY(controller.Activate("Child", "rotation", 0, DeformerSnapshotData::pose(document), &error));
        const auto selected = editorState(controller);
        CModelEdit edit;
        QVERIFY2(document.MakeBindingKeysEdit(bindingRequest(0, 1, "move"), edit, &error), qPrintable(error));
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QCOMPARE(editorState(controller), selected); QCOMPARE(preview.keep, active);
        QVERIFY2(controller.History(true, edit, &error), qPrintable(error));
        QCOMPARE(editorState(controller), selected);
        QVERIFY2(controller.History(false, edit, &error), qPrintable(error));
        QCOMPARE(editorState(controller), selected);
    }

    void rejectedBindingKeyPublicationPreservesHistoryAndRuntime_data()
    {
        QTest::addColumn<int>("stage"); QTest::addColumn<int>("fault");
        for (int stage = 0; stage < 3; ++stage) for (int fault = 0; fault < 3; ++fault)
            QTest::newRow(qPrintable(QString("stage%1-fault%2").arg(stage).arg(fault))) << stage << fault;
    }

    void rejectedBindingKeyPublicationPreservesHistoryAndRuntime()
    {
        QFETCH(int, stage); QFETCH(int, fault);
        CModelDocument document; QString error; QVERIFY2(bindingDocument(0, document, &error), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        QVERIFY(controller.Activate("Mesh", "artmesh", bindingCorner(1, 1), DeformerSnapshotData::pose(document), &error));
        CModelEdit command;
        QVERIFY2(document.MakeBindingKeysEdit(bindingRequest(0, 1, "move"), command, &error), qPrintable(error));
        if (stage) QVERIFY2(controller.Submit(command, &error), qPrintable(error));
        if (stage == 2) QVERIFY2(controller.History(true, command, &error), qPrintable(error));
        CModelEdit pending;
        if (stage == 1) QVERIFY(document.PeekUndoEdit(&pending));
        if (stage == 2) QVERIFY(document.PeekRedoEdit(&pending));
        const auto recorded = runtimeState(pending), selected = editorState(controller), state = document.ParameterStructureState();
        const auto revision = document.Revision();
        const auto bytes = preview.bytes;
        const auto values = preview.values;
        const int accepted = preview.accepted;
        if (!fault) controller.SetStructurePublisher({});
        preview.reject = fault == 1; preview.throws = fault == 2; preview.mutateRejectedRuntime = true;
        CModelEdit output; output.name = QStringLiteral("sentinel"); output.runtime = std::make_shared<CModelEditRuntimeState>();
        output.runtime->before = QStringLiteral("unchanged");
        const auto outputRuntime = output.runtime;
        const bool result = stage ? controller.History(stage == 1, output, &error) : controller.Submit(command, &error);
        QVERIFY(!result); QVERIFY(!error.isEmpty());
        QCOMPARE(document.ParameterStructureState(), state); QCOMPARE(document.Revision(), revision);
        QCOMPARE(editorState(controller), selected); QCOMPARE(preview.bytes, bytes); QCOMPARE(preview.values, values);
        QCOMPARE(preview.accepted, accepted); QCOMPARE(runtimeState(pending), recorded);
        QCOMPARE(output.name, QStringLiteral("sentinel")); QVERIFY(output.runtime == outputRuntime);
        QCOMPARE(output.runtime->before.toString(), QStringLiteral("unchanged"));
        CModelEdit stillPending;
        if (stage == 1) QVERIFY(document.PeekUndoEdit(&stillPending));
        if (stage == 2) QVERIFY(document.PeekRedoEdit(&stillPending));
        if (stage) {
            QVERIFY(stillPending.runtime == pending.runtime); QCOMPARE(stillPending.before, pending.before); QCOMPARE(stillPending.after, pending.after);
        } else QVERIFY(!document.CanUndoEdit());
        preview.reject = preview.throws = false; preview.mutateRejectedRuntime = false; preview.Connect(controller);
        QVERIFY2(stage ? controller.History(stage == 1, output, &error) : controller.Submit(command, &error), qPrintable(error));
    }

    void bindingKeyPublicationRejectsReentry()
    {
        CModelDocument document; QString error; QVERIFY2(bindingDocument(0, document, &error), qPrintable(error));
        CKeyformEditController controller; controller.SetDocument(&document);
        Preview preview; preview.Connect(controller);
        QVERIFY(controller.Activate("Mesh", "artmesh", bindingCorner(1, 1), DeformerSnapshotData::pose(document), &error));
        CModelEdit command;
        QVERIFY2(document.MakeBindingKeysEdit(bindingRequest(0, 1, "move"), command, &error), qPrintable(error));
        bool submitted = true, history = true, activated = true, restored = true;
        preview.during = [&] {
            CModelEdit result;
            submitted = controller.Submit(command); history = controller.History(true, result);
            activated = controller.Activate("Child", "rotation", 0, {}); restored = controller.Restore(document);
        };
        QVERIFY2(controller.Submit(command, &error), qPrintable(error));
        QVERIFY(!submitted && !history && !activated && !restored);
        QCOMPARE(preview.attempts, 1); QCOMPARE(preview.accepted, 1);
        QCOMPARE(controller.Target(), QStringLiteral("Mesh")); QCOMPARE(controller.Pose().value("Axis1"), .25f);
    }

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
