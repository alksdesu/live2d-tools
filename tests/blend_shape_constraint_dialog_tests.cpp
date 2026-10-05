#include "ui/cblendshapeconstraintdialog.h"
#include <QtTest>
#include <QTableView>
#include <QDialogButtonBox>

using namespace live2dstruct;

class BlendShapeConstraintDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void validatesRangeAndOrdering()
    {
        CModelBlendShapeConstraintEntry before;
        before.parameterId = QStringLiteral("Limiter"); before.parameterMinimum = -1; before.parameterMaximum = 1;
        before.values = {{-1, 0}, {0, .5f}, {1, 1}};
        CBlendShapeConstraintDialog dialog(before);
        auto* table = dialog.findChild<QTableView*>(QStringLiteral("blendShapeConstraintTable")); QVERIFY(table);
        auto* model = table->model();
        QVERIFY(model->setData(model->index(1, 1), QStringLiteral("0.25")));
        QVERIFY(model->setData(model->index(1, 2), QStringLiteral("0.75")));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVector<CMocBlendShapeConstraintBuilder::Value> submitted;
        dialog.SetSubmissionHandler([&](const auto& values, QString*) { submitted = values; return true; });
        dialog.show();
        QMetaObject::invokeMethod(&dialog, "done", Qt::DirectConnection, Q_ARG(int, int(QDialog::Accepted)));
        QCOMPARE(accepted.count(), 1); QCOMPARE(submitted.size(), 3); QCOMPARE(submitted[1].key, .25f); QCOMPARE(submitted[1].weight, .75f);
    }

    void invalidInputDoesNotSubmit()
    {
        CModelBlendShapeConstraintEntry before;
        before.parameterId = QStringLiteral("Limiter"); before.parameterMinimum = 0; before.parameterMaximum = 1;
        before.values = {{0, 0}, {1, 1}};
        CBlendShapeConstraintDialog dialog(before);
        auto* table = dialog.findChild<QTableView*>(QStringLiteral("blendShapeConstraintTable")); QVERIFY(table);
        auto* model = table->model(); QVERIFY(model->setData(model->index(1, 1), QStringLiteral("-0.2")));
        int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        QMetaObject::invokeMethod(&dialog, "done", Qt::DirectConnection, Q_ARG(int, int(QDialog::Accepted)));
        QCOMPARE(calls, 0); QVERIFY(!dialog.Error().isEmpty());
    }

    void cancelDoesNotSubmit()
    {
        CModelBlendShapeConstraintEntry before;
        before.parameterId = QStringLiteral("Limiter"); before.parameterMinimum = 0; before.parameterMaximum = 1;
        before.values = {{0, 0}, {1, 1}};
        CBlendShapeConstraintDialog dialog(before); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        QMetaObject::invokeMethod(&dialog, "done", Qt::DirectConnection, Q_ARG(int, int(QDialog::Rejected)));
        QCOMPARE(calls, 0); QVERIFY(!dialog.isVisible());
    }
};

QTEST_MAIN(BlendShapeConstraintDialogTests)
#include "blend_shape_constraint_dialog_tests.moc"
