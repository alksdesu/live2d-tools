#include "ui/cdeformerrenamedialog.h"
#include "ui/theme.h"
#include <QtTest>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextDocument>
#include <stdexcept>

class DeformerRenameDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void savesExactTextAndKeepsIdReadOnly_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("label");
        QTest::newRow("warp") << QString("warp") << QStringLiteral("弯曲变形器（Warp）");
        QTest::newRow("rotation") << QString("rotation") << QStringLiteral("旋转变形器（Rotation）");
        QTest::newRow("mesh") << QString("artmesh") << QStringLiteral("网格（ArtMesh）");
        QTest::newRow("part") << QString("part") << QStringLiteral("部件（Part）");
    }
    void savesExactTextAndKeepsIdReadOnly()
    {
        QFETCH(QString, type); QFETCH(QString, label);
        CDeformerRenameDialog dialog("stable-id", type, QStringLiteral("原始名称"));
        QCOMPARE(dialog.findChild<QLabel*>("deformerRenameType")->text(), label);
        QString submitted;
        dialog.SetSubmissionHandler([&](const QString& name, QString*) { submitted = name; return true; });
        auto* input = dialog.findChild<QLineEdit*>("deformerRenameName"); QVERIFY(input);
        input->setText(QStringLiteral("左眼 🌙 <blink>"));
        QSignalSpy accepted(&dialog, &QDialog::accepted); dialog.accept();
        QCOMPARE(submitted, input->text()); QCOMPARE(accepted.count(), 1);
        auto* id = dialog.findChild<QLineEdit*>("deformerRenameId"); QVERIFY(id);
        QCOMPARE(id->text(), QString("stable-id")); QVERIFY(id->isReadOnly());
    }
    void failedSubmissionKeepsDraftAndCanRetry()
    {
        CDeformerRenameDialog dialog("R", "rotation", "before");
        int calls = 0;
        dialog.SetSubmissionHandler([&](const QString&, QString* error) { ++calls; *error = "A & B <failed>"; return calls > 1; });
        dialog.show(); dialog.findChild<QLineEdit*>("deformerRenameName")->setText("after");
        dialog.accept(); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Name(), QString("after"));
        QCOMPARE(dialog.Error(), QString("A & B <failed>")); QVERIFY(!dialog.IsSubmitting());
        auto* error = dialog.findChild<QLabel*>("deformerRenameError"); QCOMPARE(error->textFormat(), Qt::PlainText);
        QTextDocument tooltip; tooltip.setHtml(error->toolTip()); QCOMPARE(tooltip.toPlainText(), dialog.Error());
        QVERIFY(error->isVisible()); dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(dialog.Error().isEmpty());
    }
    void callbackReentryAndExceptionsStayAtomic()
    {
        CDeformerRenameDialog dialog("R", "rotation", "before");
        int calls = 0;
        dialog.SetSubmissionHandler([&](const QString&, QString*) -> bool {
            ++calls; if (!dialog.IsSubmitting()) throw std::runtime_error("missing guard"); dialog.reject(); dialog.accept();
            throw std::runtime_error("failure");
        });
        dialog.show(); dialog.accept(); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.Error().isEmpty()); QVERIFY(!dialog.IsSubmitting()); dialog.reject(); QVERIFY(!dialog.isVisible());
    }
    void callbackMayDestroyDialog()
    {
        QPointer<CDeformerRenameDialog> dialog = new CDeformerRenameDialog("R", "rotation", "before");
        dialog->SetSubmissionHandler([&](const QString&, QString*) { delete dialog.data(); return true; });
        dialog->accept(); QVERIFY(!dialog);
    }
    void invalidNamesDoNotReachSubmission_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("empty") << QString(); QTest::newRow("spaces") << QString("   ");
        QTest::newRow("control") << (QString("bad") + QChar(0) + "name");
        QTest::newRow("unicode") << QString(QChar(0xd800));
    }
    void invalidNamesDoNotReachSubmission()
    {
        QFETCH(QString, name); CDeformerRenameDialog dialog("W", "warp", "valid");
        int calls = 0; dialog.SetSubmissionHandler([&](const QString&, QString*) { ++calls; return true; });
        dialog.findChild<QLineEdit*>("deformerRenameName")->setText(name);
        dialog.accept(); QCOMPARE(calls, 0); QVERIFY(!dialog.Error().isEmpty());
        QVERIFY(!dialog.findChild<QPushButton*>("primaryBtn")->isEnabled());
    }
    void cancelDoesNotSubmit()
    {
        CDeformerRenameDialog dialog("W", "warp", "before");
        int calls = 0; dialog.SetSubmissionHandler([&](const QString&, QString*) { ++calls; return true; });
        dialog.findChild<QLineEdit*>("deformerRenameName")->setText("changed"); dialog.reject(); QCOMPARE(calls, 0);
    }
    void themesAndNarrowLayout_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void themesAndNarrowLayout()
    {
        QFETCH(bool, dark);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CDeformerRenameDialog dialog(QString(63, 'W'), "rotation", QStringLiteral("名称") + QString(200, QChar(0x4e2d)));
        dialog.SetSubmissionHandler([](const QString&, QString*) { return true; });
        dialog.resize(380, 330); dialog.show(); QTest::qWait(50);
        QCOMPARE(dialog.width(), 380);
        auto* scroll = dialog.findChild<QScrollArea*>(); QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(dialog.findChild<QPushButton*>("primaryBtn")->isVisible());
        QCOMPARE(dialog.findChild<QPushButton*>("primaryBtn")->palette().color(QPalette::Button), QColor(Theme::tokens(dark).value("@accentFill@")));
        QVERIFY(dialog.findChild<QLineEdit*>("deformerRenameName")->isVisible());
        auto* identifier = dialog.findChild<QLineEdit*>("deformerRenameId");
        QVERIFY(identifier->isReadOnly()); QCOMPARE(identifier->text(), QString(63, 'W'));
        identifier->setFocus(); QTest::keySequence(identifier, QKeySequence(QKeySequence::MoveToEndOfLine));
        QCOMPARE(identifier->cursorPosition(), identifier->text().size());
        QTest::keyClick(identifier, Qt::Key_X); QCOMPARE(identifier->text(), QString(63, 'W'));
        const auto directory = qEnvironmentVariable("DEFORMER_RENAME_IMAGES");
        if (!directory.isEmpty()) QVERIFY(dialog.grab().save(directory + (dark ? "/rename-dark.png" : "/rename-light.png")));
    }
};
QTEST_MAIN(DeformerRenameDialogTests)
#include "deformer_rename_dialog_tests.moc"
