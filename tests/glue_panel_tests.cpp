#include "ui/cgluepanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QIODevice>
#include <QPushButton>
#include <QTableWidget>
#include <QtTest>

using namespace live2dstruct;

class GluePanelTests : public QObject
{
    Q_OBJECT
private slots:
    void editsWeightAndSupportsHistory()
    {
        QFile file(QStringLiteral(TEST_RESOURCES) + QStringLiteral("/Rice/Rice.moc3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(file.readAll(), file.fileName(), &error), qPrintable(error));
        CGluePanel panel;
        panel.SetPublisher([&](const QByteArray& before, const CGlueEditController::Request& request,
                               const QByteArray&, QString* detail) {
            QByteArray current;
            if (!document.BuildPreviewMoc(current, detail) || current != before) return false;
            CModelEdit edit;
            if (!document.MakeMeshStructureEdit(request, edit, detail)) return false;
            return document.ExecuteEdit(edit);
        });
        panel.SetDocument(&document);
        auto* selector = panel.findChild<QComboBox*>(QStringLiteral("glueSelector"));
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("gluePairs"));
        auto* weightA = panel.findChild<QDoubleSpinBox*>(QStringLiteral("glueWeightA"));
        auto* apply = panel.findChild<QPushButton*>(QStringLiteral("applyGlueWeights"));
        QVERIFY(selector && table && weightA && apply);
        QVERIFY(selector->count() > 0); QVERIFY(table->rowCount() > 0);
        const float original = panel.CurrentGlues().first().pairs.first().a.weight;
        const double replacement = qFuzzyCompare(double(original) + 1.0, 1.25) ? .5 : .25;
        weightA->setValue(replacement);
        apply->click();
        QCOMPARE(panel.CurrentGlues().first().pairs.first().a.weight, float(replacement));
        QVERIFY(document.CanUndoEdit());
        QVERIFY(panel.Undo(&error));
        QCOMPARE(panel.CurrentGlues().first().pairs.first().a.weight, original);
        QVERIFY(panel.Redo(&error));
        QCOMPARE(panel.CurrentGlues().first().pairs.first().a.weight, float(replacement));
    }

    void switchingDocumentsResetsHistoryEvenWhenBytesMatch()
    {
        QFile file(QStringLiteral(TEST_RESOURCES) + QStringLiteral("/Rice/Rice.moc3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        CModelDocument first, second;
        QString error;
        QVERIFY2(first.LoadBytes(bytes, QStringLiteral("first.moc3"), &error), qPrintable(error));
        QVERIFY2(second.LoadBytes(bytes, QStringLiteral("second.moc3"), &error), qPrintable(error));
        CModelDocument* active = &first;
        CGluePanel panel;
        panel.SetPublisher([&](const QByteArray& before, const CGlueEditController::Request& request,
                               const QByteArray&, QString* detail) {
            QByteArray current;
            if (!active->BuildPreviewMoc(current, detail) || current != before) return false;
            CModelEdit edit;
            if (!active->MakeMeshStructureEdit(request, edit, detail)) return false;
            return active->ExecuteEdit(edit);
        });
        panel.SetDocument(active);
        auto* weightA = panel.findChild<QDoubleSpinBox*>(QStringLiteral("glueWeightA"));
        auto* apply = panel.findChild<QPushButton*>(QStringLiteral("applyGlueWeights"));
        QVERIFY(weightA && apply);
        const float original = panel.CurrentGlues().first().pairs.first().a.weight;
        weightA->setValue(original == .25f ? .5 : .25);
        apply->click();
        QVERIFY(panel.CanUndo());

        active = &second;
        panel.SetDocument(active);
        QVERIFY(!panel.CanUndo());
        QCOMPARE(panel.CurrentGlues().first().pairs.first().a.weight, original);
    }
};

QTEST_MAIN(GluePanelTests)
#include "glue_panel_tests.moc"
