#include "live2dstruct/cparametervalueeditcontroller.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace live2dstruct;
namespace {
using Controller = CParameterValueEditController;
using Phase = Controller::Phase;
const QString A = QStringLiteral("ParamAngleX"), B = QStringLiteral("ParamAngleY");
bool load(CModelDocument& document)
{
    return document.Load(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
}
}

class ParameterValueEditControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void singleBatchIsOneHistoryAndPreservesOriginalStorage()
    {
        CModelDocument document; QVERIFY(load(document));
        Controller controller; controller.SetDocument(&document);
        Controller::Result result; QString error;
        int published = 0;
        controller.SetPublisher([&](const auto& p, QString*) {
            if (p.phase != Phase::Single || p.before.value(A) != 0 || p.after.value(A) != 10 || document.ParameterValue(A) != 0) return false;
            ++published; return true;
        });
        const auto originalOverrides = document.ParameterValues();
        QVERIFY2(controller.Apply({Phase::Single, 0, {{A, 10}, {B, -5}}}, result, &error), qPrintable(error));
        QCOMPARE(published, 1); QVERIFY(result.changed && result.historyChanged);
        CModelEdit edit; QVERIFY(document.PeekUndoEdit(&edit)); QCOMPARE(edit.field, CModelEdit::ParameterValues); QVERIFY(!edit.coalesce);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterValues(), originalOverrides); QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterValue(A), 10.0f); QCOMPARE(document.ParameterValue(B), -5.0f);
    }

    void repeatedPreviewsCommitOnceAndSeparateGesturesStaySeparate()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document);
        Controller::Result result; const auto revision = document.Revision();
        QVERIFY(controller.Apply({Phase::Begin, 1, {{A, 0}}}, result));
        for (float value : {1.f, 2.f, 3.f}) QVERIFY(controller.Apply({Phase::Preview, 1, {{A, value}}}, result));
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit()); QVERIFY(controller.HasChanges());
        QVERIFY(controller.Apply({Phase::Finish, 1, {}}, result)); QCOMPARE(document.ParameterValue(A), 3.0f);
        QVERIFY(controller.Apply({Phase::Begin, 2, {{A, 3}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 2, {{A, 4}}}, result));
        QVERIFY(controller.Apply({Phase::Finish, 2, {}}, result));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterValue(A), 3.0f); QVERIFY(document.CanUndoEdit());
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterValue(A), 0.0f); QVERIFY(!document.CanUndoEdit());
    }

    void returnToStartAndCancelLeaveNoHistory()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document); Controller::Result result;
        QVector<Phase> phases;
        controller.SetPublisher([&](const auto& p, QString*) { phases.append(p.phase); return true; });
        QVERIFY(controller.Apply({Phase::Begin, 1, {{A, 0}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, 15}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, 0}}}, result));
        QVERIFY(!controller.HasChanges()); QVERIFY(controller.Apply({Phase::Finish, 1, {}}, result));
        QVERIFY(!result.changed && !result.historyChanged); QVERIFY(!document.CanUndoEdit());
        QVERIFY(controller.Apply({Phase::Begin, 2, {{A, 0}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 2, {{A, 15}}}, result));
        QVERIFY(controller.Apply({Phase::Cancel, 2, {}}, result));
        QCOMPARE(result.acceptedValues.value(A), 0.0f); QVERIFY(!controller.IsActive()); QVERIFY(!document.CanUndoEdit());
        QCOMPARE(phases.last(), Phase::Cancel);
    }

    void rejectionAndExceptionLeaveOutputAndGestureRetryable_data()
    {
        QTest::addColumn<bool>("throws"); QTest::newRow("false") << false; QTest::newRow("throw") << true;
    }
    void rejectionAndExceptionLeaveOutputAndGestureRetryable()
    {
        QFETCH(bool, throws); CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document);
        bool reject = false;
        controller.SetPublisher([&](const auto&, QString*) { if (reject && throws) throw std::runtime_error("reject"); return !reject; });
        Controller::Result result; QVERIFY(controller.Apply({Phase::Begin, 1, {{A, 0}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, 7}}}, result)); const auto original = result.acceptedValues;
        reject = true; QString error;
        QVERIFY(!controller.Apply({Phase::Preview, 1, {{A, 8}}}, result, &error)); QCOMPARE(result.acceptedValues, original);
        QVERIFY(!controller.Apply({Phase::Finish, 1, {}}, result)); QVERIFY(controller.IsActive()); QVERIFY(!document.CanUndoEdit());
        QVERIFY(!controller.Apply({Phase::Cancel, 1, {}}, result)); QVERIFY(controller.IsActive());
        reject = false; QVERIFY(controller.Apply({Phase::Finish, 1, {}}, result)); QCOMPARE(document.ParameterValue(A), 7.0f);
    }

    void rulesAndRangesInvalidateGestureWithoutPublishing()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document);
        QByteArray context("one"); controller.SetContextDigestProvider([&] { return context; });
        int published = 0; controller.SetPublisher([&](const auto&, QString*) { ++published; return true; });
        Controller::Result result; QVERIFY(controller.Apply({Phase::Begin, 1, {{A, 0}}}, result));
        context = "two"; QVERIFY(!controller.Apply({Phase::Preview, 1, {{A, 2}}}, result)); QCOMPARE(published, 1);
        context = "one"; QVERIFY(document.SetParameterValue(B, 1));
        QVERIFY(!controller.Apply({Phase::Finish, 1, {}}, result)); QCOMPARE(published, 1); QVERIFY(!document.CanUndoEdit());
    }

    void pairExpansionAndBaselineAreCapturedTogether()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document);
        controller.SetExpander([](const auto&, const auto& input, auto& output, QString*) {
            output = input; output[B] = -input.value(A); return true;
        });
        controller.SetBaselineProvider([](auto& values, QString*) { values = {{A, 2}, {B, -2}}; return true; });
        Controller::Result result; QVERIFY(controller.Apply({Phase::Begin, 1, {{A, 2}}}, result));
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, 5}}}, result)); QCOMPARE(result.acceptedValues.value(B), -5.0f);
        QVERIFY(controller.Apply({Phase::Finish, 1, {}}, result));
        CModelEdit edit; QVERIFY(document.PeekUndoEdit(&edit));
        QCOMPARE((edit.before.value<QMap<QString, float>>()), (QMap<QString, float>{{A, 2}, {B, -2}}));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterValue(A), 0.0f); QCOMPARE(document.ParameterValue(B), 0.0f);
    }

    void returningToOriginalDisplayRestoresContextWithoutSavingEffects()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document);
        controller.SetBaselineProvider([](auto& values, QString*) { values = {{A, .1f}}; return true; });
        float runtime = .8f;
        QVector<bool> restored;
        controller.SetPublisher([&](const auto& p, QString*) {
            if (p.phase != Phase::Begin) runtime = p.restoreContext ? .8f : p.after.value(A);
            restored.append(p.restoreContext); return true;
        });
        Controller::Result result;
        QVERIFY(controller.Apply({Phase::Begin, 1, {{A, .8f}}}, result)); QCOMPARE(runtime, .8f);
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, .9f}}}, result)); QCOMPARE(runtime, .9f); QVERIFY(result.changed);
        QVERIFY(controller.Apply({Phase::Preview, 1, {{A, .8f}}}, result)); QCOMPARE(runtime, .8f); QVERIFY(result.restoreContext); QVERIFY(!result.changed);
        QVERIFY(controller.Apply({Phase::Finish, 1, {}}, result)); QVERIFY(result.restoreContext); QVERIFY(!document.CanUndoEdit());
        QCOMPARE(document.ParameterValue(A), 0.0f);
        QVERIFY(controller.Apply({Phase::Single, 0, {{A, .1f}}}, result)); QVERIFY(!result.restoreContext); QCOMPARE(runtime, .1f);
    }

    void realOrganizationExpandsAndRejectsConflictsAtomically_data()
    {
        QTest::addColumn<bool>("reverse"); QTest::newRow("same") << false; QTest::newRow("reverse") << true;
    }
    void realOrganizationExpandsAndRejectsConflictsAtomically()
    {
        QFETCH(bool, reverse); CModelDocument document; QVERIFY(load(document));
        auto organization = document.ParameterOrganization(); organization.configured = true;
        for (const auto& parameter : document.Parameters()) organization.nodes.append({CParameterOrganization::Node::Parameter, parameter.id, {}, {}, {}});
        organization.pairs.append({A, B, reverse, true});
        QString error; QVERIFY2(document.SetParameterOrganization(organization, &error), qPrintable(error));
        Controller controller; controller.SetDocument(&document); Controller::Result result;
        QVERIFY2(controller.Apply({Phase::Single, 0, {{A, 15}}}, result, &error), qPrintable(error));
        QCOMPARE(result.acceptedValues.value(B), reverse ? -15.f : 15.f);
        const auto revision = document.Revision(); const auto previous = result.acceptedValues;
        QVERIFY(!controller.Apply({Phase::Single, 0, {{A, 5}, {B, 12}}}, result, &error));
        QCOMPARE(document.Revision(), revision); QCOMPARE(result.acceptedValues, previous);
        QVERIFY(controller.Apply({Phase::Single, 0, {{B, 6}}}, result)); QCOMPARE(result.acceptedValues.value(A), reverse ? -6.f : 6.f);
    }

    void invalidInputAndReentryNeverPartiallyPublish()
    {
        CModelDocument document; QVERIFY(load(document)); Controller controller; controller.SetDocument(&document); Controller::Result result;
        int calls = 0; controller.SetPublisher([&](const auto&, QString*) {
            Controller::Result nested; if (controller.Apply({Phase::Single, 0, {{B, 2}}}, nested)) return false;
            ++calls; return true;
        });
        for (const auto& values : QVector<QMap<QString, float>>{{}, {{"missing", 0}}, {{A, std::numeric_limits<float>::infinity()}}, {{A, 1000}}})
            QVERIFY(!controller.Apply({Phase::Single, 0, values}, result));
        QCOMPARE(calls, 0); QVERIFY(!document.CanUndoEdit());
        QVERIFY(controller.Apply({Phase::Single, 0, {{A, 3}}}, result)); QCOMPARE(calls, 1); QCOMPARE(document.ParameterValue(B), 0.0f);
    }

    void ownerCanBeDestroyedDuringPublisher()
    {
        CModelDocument document; QVERIFY(load(document)); auto controller = std::make_unique<Controller>(); controller->SetDocument(&document);
        controller->SetPublisher([&](const auto&, QString*) { controller.reset(); return true; });
        Controller::Result result; result.acceptedValues = {{"sentinel", 2}};
        QVERIFY(!controller->Apply({Phase::Single, 0, {{A, 3}}}, result));
        QCOMPARE(result.acceptedValues, (QMap<QString, float>{{"sentinel", 2}})); QVERIFY(!document.CanUndoEdit());
    }
};
QTEST_GUILESS_MAIN(ParameterValueEditControllerTests)
#include "parameter_value_edit_controller_tests.moc"
