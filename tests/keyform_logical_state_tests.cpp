#include "live2dstruct/ckeyformeditcontroller.h"
#include <QtTest>
#include <limits>

using namespace live2dstruct;
using Controller = CKeyformEditController;

namespace {
struct Fixture {
    CModelDocument document;
    Controller controller;
    QString target;
    int publishes = 0, ends = 0;
    bool Open(QString* error)
    {
        if (!document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", error)) return false;
        controller.SetDocument(&document);
        controller.SetPublisher([this](const auto&, const auto&, bool, QString*) { ++publishes; return true; },
                                [this](QString*) { ++ends; return true; });
        QMap<QString, float> values;
        for (const auto& parameter : document.Parameters()) values.insert(parameter.id, document.ParameterValue(parameter.id));
        for (const auto& mesh : document.Drawables()) {
            if (mesh.keyforms.size() < 2 || mesh.vertexCount < 3) continue;
            if (controller.Activate(mesh.id, 0, values, error)) { target = mesh.id; return true; }
        }
        return false;
    }
    Controller::LogicalState Empty() const { return {{}, document.SourceDigest()}; }
};
void compare(const Controller::LogicalState& a, const Controller::LogicalState& b)
{
    QCOMPARE(a.editor.target, b.editor.target); QCOMPARE(a.editor.type, b.editor.type);
    QCOMPARE(a.editor.keyform, b.editor.keyform); QCOMPARE(a.editor.pose, b.editor.pose);
    QCOMPARE(a.sourceDigest, b.sourceDigest);
}
}

class KeyformLogicalStateTests : public QObject
{
    Q_OBJECT
private slots:
    void preparedSwitchAndRestoreDoNotPublishOrEditDocument()
    {
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        const auto revision = f.document.Revision();
        Controller::LogicalState active; QVERIFY(f.controller.CaptureLogicalState(active, &error));
        Controller::PreparedLogicalState clear;
        QVERIFY(f.controller.PrepareLogicalState(f.Empty(), clear, &error));
        QVERIFY(f.controller.IsActive()); QVERIFY(clear.IsValid());
        QVERIFY(f.controller.ValidatePreparedLogicalState(clear, &error));
        f.controller.AdoptLogicalState(clear);
        QVERIFY(!clear.IsValid()); QVERIFY(!f.controller.IsActive());
        QVERIFY(!f.controller.ValidatePreparedLogicalState(clear, &error));
        Controller::PreparedLogicalState restore;
        QVERIFY(f.controller.PrepareLogicalState(active, restore, &error));
        QVERIFY(f.controller.ValidatePreparedLogicalState(restore, &error));
        f.controller.AdoptLogicalState(restore);
        Controller::LogicalState after; QVERIFY(f.controller.CaptureLogicalState(after, &error));
        compare(after, active);
        QCOMPARE(f.document.Revision(), revision); QVERIFY(!f.document.CanUndoEdit());
        QCOMPARE(f.publishes, 1); QCOMPARE(f.ends, 0);
        static_assert(noexcept(f.controller.AdoptLogicalState(restore)));
    }
    void invalidRequestedStatePreservesPreparedCandidate_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 7; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void invalidRequestedStatePreservesPreparedCandidate()
    {
        QFETCH(int, kind); Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::LogicalState state; QVERIFY(f.controller.CaptureLogicalState(state));
        Controller::PreparedLogicalState prepared; QVERIFY(f.controller.PrepareLogicalState(f.Empty(), prepared));
        switch (kind) {
        case 0: state.sourceDigest = QByteArray(32, 'x'); break;
        case 1: state.editor.target = "missing"; break;
        case 2: state.editor.type = "part"; break;
        case 3: state.editor.keyform = std::numeric_limits<int>::max(); break;
        case 4: state.editor.pose.clear(); break;
        case 5: state.editor.pose[state.editor.pose.firstKey()] = std::numeric_limits<float>::quiet_NaN(); break;
        case 6: state.editor.target.clear(); break;
        }
        QVERIFY(!f.controller.PrepareLogicalState(state, prepared, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(prepared.IsValid()); QVERIFY(f.controller.ValidatePreparedLogicalState(prepared));
        f.controller.AdoptLogicalState(prepared); QVERIFY(!f.controller.IsActive());
        QCOMPARE(f.publishes, 1); QCOMPARE(f.ends, 0);
    }
    void documentOrControllerChangesInvalidatePreparedState_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 3; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void documentOrControllerChangesInvalidatePreparedState()
    {
        QFETCH(int, kind); Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::PreparedLogicalState prepared; QVERIFY(f.controller.PrepareLogicalState(f.Empty(), prepared));
        if (kind == 0) f.document.SetLocked("artmesh", f.target, true);
        if (kind == 1) QVERIFY(f.controller.End());
        if (kind == 2) f.controller.SetDocument(&f.document);
        QVERIFY(!f.controller.ValidatePreparedLogicalState(prepared, &error)); QVERIFY(!error.isEmpty());
    }
    void restoringAlreadyActiveLockedTargetDoesNotBecomeAGeometryEdit()
    {
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::LogicalState active; QVERIFY(f.controller.CaptureLogicalState(active));
        f.document.SetLocked("artmesh", f.target, true);
        Controller::PreparedLogicalState clear; QVERIFY(f.controller.PrepareLogicalState(f.Empty(), clear));
        f.controller.AdoptLogicalState(clear);
        Controller::PreparedLogicalState restore; QVERIFY(f.controller.PrepareLogicalState(active, restore));
        f.controller.AdoptLogicalState(restore);
        QCOMPARE(f.controller.Target(), f.target); QCOMPARE(f.publishes, 1); QCOMPARE(f.ends, 0);
    }
    void activeDragRejectsAllPreparationAndCapture()
    {
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(f.document, f.target, 0, f.controller.Pose(), &error), qPrintable(error));
        QVector<float> runtime;
        QVERIFY(mapping.ToModel(f.document.DrawableKeyformPositions(f.target, 0), runtime, &error));
        Controller::LogicalState active; QVERIFY(f.controller.CaptureLogicalState(active));
        Controller::PreparedLogicalState prepared; QVERIFY(f.controller.PrepareLogicalState(f.Empty(), prepared));
        QVERIFY2(f.controller.BeginDrag({0}, runtime, &error), qPrintable(error));
        auto sentinel = active;
        QVERIFY(!f.controller.CaptureLogicalState(sentinel, &error)); compare(sentinel, active);
        QVERIFY(!f.controller.PrepareLogicalState(f.Empty(), prepared, &error));
        QVERIFY(!f.controller.ValidatePreparedLogicalState(prepared, &error));
        QVERIFY(f.controller.CancelDrag(&error));
        QVERIFY(f.controller.ValidatePreparedLogicalState(prepared, &error));
    }
    void unloadedAndOtherControllerAreRejected()
    {
        Controller empty; Controller::LogicalState state{{"sentinel", "artmesh", 2, {}}, QByteArray(32, 's')};
        auto before = state; QString error;
        QVERIFY(!empty.CaptureLogicalState(state, &error)); compare(state, before);
        Fixture a, b; QVERIFY(a.Open(&error)); QVERIFY(b.Open(&error));
        Controller::PreparedLogicalState prepared; QVERIFY(a.controller.PrepareLogicalState(a.Empty(), prepared));
        QVERIFY(!b.controller.ValidatePreparedLogicalState(prepared, &error));
        QVERIFY(a.controller.ValidatePreparedLogicalState(prepared, &error));
    }
    void preparedStateOwnsCallerBuffers_data()
    {
        QTest::addColumn<int>("field");
        QTest::newRow("target") << 0;
        QTest::newRow("type") << 1;
        QTest::newRow("pose-key") << 2;
        QTest::newRow("pose-value") << 3;
        QTest::newRow("source-digest") << 4;
    }
    void preparedStateOwnsCallerBuffers()
    {
        QFETCH(int, field);
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::LogicalState expected; QVERIFY(f.controller.CaptureLogicalState(expected));
        QVERIFY(!expected.editor.pose.isEmpty()); QVERIFY(!expected.sourceDigest.isEmpty());
        QString target(expected.editor.target.constData(), expected.editor.target.size());
        QString type(expected.editor.type.constData(), expected.editor.type.size());
        const auto firstKey = expected.editor.pose.firstKey();
        QString poseKey(firstKey.constData(), firstKey.size());
        QByteArray digest(expected.sourceDigest.constData(), expected.sourceDigest.size());
        QChar* targetData = target.data(); QChar* typeData = type.data(); QChar* keyData = poseKey.data();
        char* digestData = digest.data();
        Controller::LogicalState requested;
        requested.editor.target = QString::fromRawData(targetData, target.size());
        requested.editor.type = QString::fromRawData(typeData, type.size());
        requested.editor.keyform = expected.editor.keyform;
        for (auto value = expected.editor.pose.cbegin(); value != expected.editor.pose.cend(); ++value) {
            const QString key = value.key() == firstKey ? QString::fromRawData(keyData, poseKey.size())
                                                       : QString(value.key().constData(), value.key().size());
            requested.editor.pose.insert(key, value.value());
        }
        float* poseValue = &requested.editor.pose.begin().value();
        requested.sourceDigest = QByteArray::fromRawData(digestData, digest.size());
        Controller::PreparedLogicalState prepared;
        QVERIFY2(f.controller.PrepareLogicalState(requested, prepared, &error), qPrintable(error));
        switch (field) {
        case 0: targetData[0] = QChar('!'); break;
        case 1: typeData[0] = QChar('!'); break;
        case 2: keyData[0] = QChar('!'); break;
        case 3: *poseValue += 1000.0f; break;
        case 4: digestData[0] ^= 0x7f; break;
        }
        QVERIFY2(f.controller.ValidatePreparedLogicalState(prepared, &error), qPrintable(error));
        f.controller.AdoptLogicalState(prepared);
        Controller::LogicalState actual;
        QVERIFY2(f.controller.CaptureLogicalState(actual, &error), qPrintable(error));
        compare(actual, expected);
        QCOMPARE(f.publishes, 1); QCOMPARE(f.ends, 0);
    }
    void decodedDeformerStateOwnsRuntimeBuffers_data()
    {
        QTest::addColumn<bool>("forward"); QTest::addColumn<int>("field");
        for (bool forward : {false, true}) for (int field = 0; field < 4; ++field)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(forward ? "after" : "before").arg(field))) << forward << field;
    }
    void decodedDeformerStateOwnsRuntimeBuffers()
    {
        QFETCH(bool, forward); QFETCH(int, field);
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::LogicalState expected; QVERIFY(f.controller.CaptureLogicalState(expected));
        QVERIFY(!expected.editor.pose.isEmpty());
        QString target(expected.editor.target.constData(), expected.editor.target.size());
        QString type(expected.editor.type.constData(), expected.editor.type.size());
        const auto firstKey = expected.editor.pose.firstKey();
        QString poseKey(firstKey.constData(), firstKey.size());
        QChar* targetData = target.data(); QChar* typeData = type.data(); QChar* keyData = poseKey.data();
        QMap<QString, float> pose;
        for (auto value = expected.editor.pose.cbegin(); value != expected.editor.pose.cend(); ++value) {
            const QString key = value.key() == firstKey ? QString::fromRawData(keyData, poseKey.size())
                                                       : QString(value.key().constData(), value.key().size());
            pose.insert(key, value.value());
        }
        float* poseValue = &pose.begin().value();
        CModelEdit command; command.field = CModelEdit::DeformerStructure;
        command.runtime = std::make_shared<CModelEditRuntimeState>();
        const QVariantMap encoded{{QStringLiteral("target"), QString::fromRawData(targetData, target.size())},
            {QStringLiteral("type"), QString::fromRawData(typeData, type.size())},
            {QStringLiteral("keyform"), expected.editor.keyform}, {QStringLiteral("pose"), QVariant::fromValue(pose)}};
        (forward ? command.runtime->afterEditor : command.runtime->beforeEditor) = encoded;
        Controller::LogicalState actual; actual.sourceDigest = expected.sourceDigest;
        QVERIFY2(Controller::ReadDeformerEditorState(command, forward, actual.editor, &error), qPrintable(error));
        switch (field) {
        case 0: targetData[0] = QChar('!'); break;
        case 1: typeData[0] = QChar('!'); break;
        case 2: keyData[0] = QChar('!'); break;
        case 3: *poseValue += 1000.0f; break;
        }
        compare(actual, expected);
        QCOMPARE(f.publishes, 1); QCOMPARE(f.ends, 0);
    }
    void preparedAdoptionDuringPublicationIsDeferredWithoutConsumption()
    {
        Fixture f; QString error; QVERIFY2(f.Open(&error), qPrintable(error));
        Controller::LogicalState expected; QVERIFY(f.controller.CaptureLogicalState(expected));
        Controller::PreparedLogicalState clear;
        QVERIFY(f.controller.PrepareLogicalState(f.Empty(), clear, &error));
        bool called = false, unchanged = false, retained = false;
        f.controller.SetPublisher([&](const auto&, const auto&, bool, QString*) {
            ++f.publishes; called = true;
            f.controller.AdoptLogicalState(clear);
            unchanged = f.controller.Target() == expected.editor.target && f.controller.Type() == expected.editor.type &&
                f.controller.Keyform() == expected.editor.keyform && f.controller.Pose() == expected.editor.pose;
            retained = clear.IsValid();
            return true;
        }, [&](QString*) { ++f.ends; return true; });
        const auto revision = f.document.Revision();
        QVERIFY2(f.controller.Activate(expected.editor.target, expected.editor.type, expected.editor.keyform,
                                       expected.editor.pose, &error), qPrintable(error));
        QVERIFY(called); QVERIFY(unchanged); QVERIFY(retained); QVERIFY(clear.IsValid());
        QVERIFY2(f.controller.ValidatePreparedLogicalState(clear, &error), qPrintable(error));
        f.controller.AdoptLogicalState(clear);
        QVERIFY(!f.controller.IsActive()); QVERIFY(!clear.IsValid());
        QCOMPARE(f.document.Revision(), revision); QCOMPARE(f.publishes, 2); QCOMPARE(f.ends, 0);
    }

};
QTEST_GUILESS_MAIN(KeyformLogicalStateTests)
#include "keyform_logical_state_tests.moc"
