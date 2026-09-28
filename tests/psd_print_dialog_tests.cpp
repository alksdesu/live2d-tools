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

    void scopeAndEmptyIdentityNoticeDoesNotChangeDefaultOptions()
    {
        CPsdPrintDialog dialog;
        const auto* notice = dialog.findChild<QLabel*>("psdCoverageNotice");
        QVERIFY(notice); QCOMPARE(notice->textFormat(), Qt::PlainText); QVERIFY(notice->wordWrap());
        QVERIFY(notice->text().contains(QStringLiteral("不代表原始素材已恢复")));
        QVERIFY(notice->text().contains(QStringLiteral("合成预览按导出的图层生成")));
        QVERIFY(notice->text().contains(QStringLiteral("纹理采样结果可能与实时画布不同")));
        for (const auto* check : dialog.findChildren<QCheckBox*>()) QVERIFY(!check->isChecked());
        dialog.SetName(QStringLiteral("<b>source.psd</b>"));
        bool found = false;
        for (const auto* label : dialog.findChildren<QLabel*>()) if (label->text() == QStringLiteral("<b>source.psd</b>")) {
            QCOMPARE(label->textFormat(), Qt::PlainText); found = true;
        }
        QVERIFY(found);
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
