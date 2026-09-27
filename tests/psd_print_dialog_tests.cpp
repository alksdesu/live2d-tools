#include "ui/cpsdprintdialog.h"
#include <QtTest>
#include <QDialogButtonBox>

class PsdPrintDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void startsUnconfirmed()
    {
        CPsdPrintDialog dialog;
        QVERIFY(!dialog.m_IsPrint);
        dialog.reject();
        QVERIFY(!dialog.m_IsPrint);
    }

    void acceptsOptions()
    {
        CPsdPrintDialog dialog;
        const auto checks = dialog.findChildren<QCheckBox*>();
        QCOMPARE(checks.size(), 5);
        checks.at(0)->setChecked(true);
        checks.at(1)->setChecked(true);
        checks.at(2)->setChecked(true);
        checks.at(4)->setChecked(true);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(dialog.m_IsPrint);
        QVERIFY(dialog.m_IsAll);
        QVERIFY(dialog.m_IsFenZu);
        QVERIFY(dialog.m_IsTexExtract);
        QVERIFY(!dialog.m_IsSnapshot);
        QVERIFY(dialog.m_IsCubismCompat);
    }

    void textureSamplingPreservesParameterSnapshotChoice()
    {
        CPsdPrintDialog dialog;
        const auto checks = dialog.findChildren<QCheckBox*>();
        QCOMPARE(checks.size(), 5);
        auto* texture = checks.at(2);
        auto* snapshot = checks.at(3);
        snapshot->setChecked(true);
        texture->setChecked(true);
        QVERIFY(snapshot->isEnabled());
        QVERIFY(snapshot->isChecked());
        texture->setChecked(false);
        QVERIFY(snapshot->isChecked());
        texture->setChecked(true);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(dialog.m_IsPrint);
        QVERIFY(dialog.m_IsTexExtract);
        QVERIFY(dialog.m_IsSnapshot);
    }
};

QTEST_MAIN(PsdPrintDialogTests)
#include "psd_print_dialog_tests.moc"
