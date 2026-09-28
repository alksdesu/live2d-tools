#include "ui/cobjectnamesimportdialog.h"
#include "ui/theme.h"
#include <QAbstractItemModelTester>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QScrollBar>
#include <stdexcept>
#include <QTableView>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QHeaderView>
#include <QtTest>

namespace {
using Dialog = CObjectNamesImportDialog;
Dialog::Context context()
{
    Dialog::Context value;
    value.sourcePath = QStringLiteral("/原工程/<b>名字.cmo3");
    value.sourceDigest = QByteArray(32, 'x');
    value.diagnostics = {QStringLiteral("结构相符不证明同源")};
    value.rows = {
        {"part", "Shared", "Shared", QStringLiteral("眼睛组"), "compatible", {}, {QStringLiteral("父部件相符")}, true},
        {"artmesh", "Shared", QStringLiteral("用户高光"), QStringLiteral("来源高光"), "compatible", {}, {QStringLiteral("拓扑相符")}, true},
        {"warp", "Warp", "Warp", QStringLiteral("变形区域"), "compatible", {}, {}, true},
        {"artmesh", "Same", QStringLiteral("相同"), QStringLiteral("相同"), "compatible", {}, {}, true},
        {"rotation", "Other", "Other", QStringLiteral("无对应"), "conflict", QStringLiteral("父级结构不同"), {}, false}
    };
    return value;
}
QTableView* table(Dialog& dialog) { return dialog.findChild<QTableView*>("objectNamesTable"); }
QPushButton* button(Dialog& dialog, const char* name) { return dialog.findChild<QPushButton*>(name); }
QModelIndex item(Dialog& dialog, int row) { return table(dialog)->model()->index(row, 0); }
bool checked(Dialog& dialog, int row) { return item(dialog, row).data(Qt::CheckStateRole).toInt() == Qt::Checked; }
class TextLayoutProbe final : public QStyledItemDelegate
{
public:
    static QString VisibleText(QTableView* view, const QModelIndex& index, bool* naturalSizeFits = nullptr) {
        TextLayoutProbe probe;
        QStyleOptionViewItem option;
        option.initFrom(view); option.widget = view;
        probe.initStyleOption(&option, index);
        option.rect = view->visualRect(index);
        const auto textRect = view->style()->subElementRect(QStyle::SE_ItemViewItemText, &option, view);
        if (naturalSizeFits) *naturalSizeFits = option.rect.width() >= view->itemDelegate()->sizeHint(option, index).width();
        return option.fontMetrics.elidedText(option.text, Qt::ElideRight, textRect.width());
    }
};
}

class ObjectNamesImportDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void defaultsFillOnlyAndTypedIdentitiesAreSeparate()
    {
        Dialog dialog(context());
        auto* model = table(dialog)->model();
        QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model->rowCount(), 5); QCOMPARE(model->columnCount(), 6);
        QVERIFY(checked(dialog, 0)); QVERIFY(!checked(dialog, 1)); QVERIFY(checked(dialog, 2));
        QVERIFY(!checked(dialog, 3)); QVERIFY(!checked(dialog, 4));
        QVERIFY(!(model->flags(item(dialog, 3)) & Qt::ItemIsUserCheckable));
        QVERIFY(!(model->flags(item(dialog, 4)) & Qt::ItemIsUserCheckable));
        QVector<Dialog::Selection> selected;
        dialog.SetSubmissionHandler([&](const auto& values, QString*) { selected = values; return true; });
        QVERIFY(button(dialog, "objectNamesApply")->isEnabled());
        QVERIFY(model->setData(item(dialog, 1), Qt::Checked, Qt::CheckStateRole));
        QVERIFY(dialog.Submit()); QCOMPARE(selected.size(), 3);
        QCOMPARE(selected[0].type, QString("part")); QCOMPARE(selected[0].id, QString("Shared"));
        QCOMPARE(selected[1].type, QString("artmesh")); QCOMPARE(selected[1].id, QString("Shared"));
        QCOMPARE(selected[1].name, QStringLiteral("来源高光")); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void filteringRetainsStableSelectionAndSummaryIncludesHiddenRows()
    {
        Dialog dialog(context());
        dialog.SetSubmissionHandler([](const auto&, QString*) { return false; });
        auto* filter = dialog.findChild<QLineEdit*>("objectNamesFilter");
        filter->setText(QStringLiteral("用户高光")); QCOMPARE(table(dialog)->model()->rowCount(), 1);
        QCOMPARE(item(dialog, 0).data(Qt::UserRole).toInt(), 1);
        QVERIFY(table(dialog)->model()->setData(item(dialog, 0), Qt::Checked, Qt::CheckStateRole));
        auto* summary = dialog.findChild<QLabel*>("objectNamesSummary");
        QVERIFY(summary->text().contains(QStringLiteral("2 项补全、1 项覆盖")));
        filter->clear(); QVERIFY(checked(dialog, 0)); QVERIFY(checked(dialog, 1)); QVERIFY(checked(dialog, 2));
        button(dialog, "objectNamesKeepAll")->click();
        QVERIFY(!button(dialog, "objectNamesApply")->isEnabled());
        button(dialog, "objectNamesFillMissing")->click();
        QVERIFY(checked(dialog, 0)); QVERIFY(!checked(dialog, 1)); QVERIFY(checked(dialog, 2));
    }
    void failureKeepsSelectionAndErrorAllowsRetry()
    {
        Dialog dialog(context()); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString* error) {
            ++calls; if (calls == 1) { *error = QStringLiteral("模型已改变\n请重新核对 <b>对象</b>"); return false; } return true;
        });
        dialog.show(); QVERIFY(!dialog.Submit()); QVERIFY(dialog.isVisible()); QVERIFY(checked(dialog, 0));
        QCOMPARE(dialog.findChild<QLabel*>("objectNamesStatus")->text(), QStringLiteral("模型已改变"));
        QVERIFY(dialog.findChild<QPlainTextEdit*>("objectNamesDetails")->toPlainText().contains("<b>对象</b>"));
        QCOMPARE(dialog.findChild<QLabel*>("objectNamesStatus")->textFormat(), Qt::PlainText);
        QVERIFY(dialog.Submit()); QCOMPARE(calls, 2); QVERIFY(!dialog.isVisible());
    }
    void canceledAndUnchangedNeverSubmit()
    {
        Dialog dialog(context()); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        button(dialog, "objectNamesKeepAll")->click(); QVERIFY(!dialog.Submit()); QCOMPARE(calls, 0);
        button(dialog, "objectNamesFillMissing")->click(); dialog.reject();
        QVERIFY(!dialog.Submit()); QCOMPARE(calls, 0); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    void spaceTogglesFocusedCheckboxWithoutSubmitting()
    {
        Dialog dialog(context()); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        dialog.show(); table(dialog)->setFocus(); table(dialog)->setCurrentIndex(item(dialog, 0));
        QVERIFY(checked(dialog, 0)); QTest::keyClick(table(dialog), Qt::Key_Space);
        QVERIFY(!checked(dialog, 0)); QCOMPARE(calls, 0);
    }
    void longErrorStaysAtStartOfDetailsWithoutGrowingWindow()
    {
        Dialog dialog(context()); const QString reason = QString(4000, QChar(0x9519)) + "\nsecond paragraph";
        dialog.SetSubmissionHandler([&](const auto&, QString* error) { *error = reason; return false; });
        dialog.setStyleSheet(Theme::globalStyleSheet()); dialog.ensurePolished(); dialog.resize(560, 460); dialog.show();
        QVERIFY(!dialog.Submit()); QTest::qWait(20);
        QVERIFY(dialog.width() <= 560); QVERIFY(dialog.height() <= 460);
        const auto* status = dialog.findChild<QLabel*>("objectNamesStatus");
        QVERIFY(status->height() <= status->fontMetrics().height() * 2);
        auto* details = dialog.findChild<QPlainTextEdit*>("objectNamesDetails");
        QVERIFY(details->toPlainText().startsWith(QStringLiteral("无法采用名称：\n") + reason));
        QCOMPARE(details->verticalScrollBar()->value(), details->verticalScrollBar()->minimum());
        QVERIFY(dialog.rect().contains(QRect(button(dialog, "objectNamesApply")->mapTo(&dialog, QPoint()), button(dialog, "objectNamesApply")->size())));
    }
    void malformedContextCannotSelectUnsafeRows_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"duplicate", "bad-name", "unknown-type", "empty-id", "bad-digest", "large-count"}) QTest::newRow(kind) << QString(kind);
    }
    void malformedContextCannotSelectUnsafeRows()
    {
        QFETCH(QString, kind); auto data = context(); data.rows.resize(1);
        if (kind == "duplicate") data.rows.append(data.rows.first());
        if (kind == "bad-name") data.rows[0].sourceName = "line\nbreak";
        if (kind == "unknown-type") data.rows[0].type = "source-layer";
        if (kind == "empty-id") data.rows[0].id.clear();
        if (kind == "bad-digest") data.sourceDigest = "invalid";
        if (kind == "large-count") data.rows.resize(100001);
        Dialog dialog(data); dialog.SetSubmissionHandler([](const auto&, QString*) { qFatal("invalid submit"); return false; });
        QVERIFY(!button(dialog, "objectNamesApply")->isEnabled()); QVERIFY(!dialog.Submit());
        for (int row = 0; row < table(dialog)->model()->rowCount(); ++row)
            QVERIFY(!(table(dialog)->model()->flags(item(dialog, row)) & Qt::ItemIsUserCheckable));
    }
    void tooltipAndDetailsKeepReasonLiteral()
    {
        auto data = context(); data.rows[0].reason = "<b>literal & text</b>\nnext";
        Dialog dialog(data);
        const auto tooltip = item(dialog, 0).data(Qt::ToolTipRole).toString();
        QVERIFY(tooltip.contains("&lt;b&gt;literal &amp; text&lt;/b&gt;")); QVERIFY(!tooltip.contains("<b>"));
        QVERIFY(dialog.findChild<QPlainTextEdit*>("objectNamesDetails")->toPlainText().contains(data.rows[0].reason));
        dialog.findChild<QLineEdit*>("objectNamesFilter")->setText("<b>");
        QCOMPARE(table(dialog)->model()->rowCount(), 1); QCOMPARE(item(dialog, 0).data(Qt::UserRole).toInt(), 0);
    }
    void borrowedStringsAndDigestAreOwned()
    {
        QString source = "source.cmo3", name = "borrowed name", evidence = "immutable evidence"; QByteArray digest(32, 'q');
        auto data = context(); data.rows.resize(1);
        data.sourcePath = QString::fromRawData(source.data(), source.size()); data.sourceDigest = QByteArray::fromRawData(digest.data(), digest.size());
        data.rows[0].sourceName = QString::fromRawData(name.data(), name.size()); data.rows[0].evidence = {QString::fromRawData(evidence.data(), evidence.size())};
        Dialog dialog(data); source.fill('x'); name.fill('x'); evidence.fill('x'); digest.fill('x'); data.rows.clear();
        const auto details = dialog.findChild<QPlainTextEdit*>("objectNamesDetails")->toPlainText();
        QVERIFY(details.contains("source.cmo3")); QVERIFY(details.contains("immutable evidence")); QVERIFY(details.contains(QString::fromLatin1(QByteArray(32, 'q').toHex())));
        QVector<Dialog::Selection> selected;
        dialog.SetSubmissionHandler([&](const auto& values, QString*) { selected = values; return true; });
        QVERIFY(dialog.Submit()); QCOMPARE(selected[0].name, QString("borrowed name"));
    }
    void synchronousReentryAndCloseDuringSubmissionDoNotRepeatCommit()
    {
        Dialog dialog(context()); int calls = 0; QSignalSpy finished(&dialog, &QDialog::finished);
        bool submitting = false, reentryRejected = false, changesRejected = false;
        dialog.SetSubmissionHandler([&](const auto&, QString*) {
            ++calls; submitting = dialog.IsSubmitting(); reentryRejected = !dialog.Submit();
            dialog.done(QDialog::Accepted); dialog.reject();
            changesRejected = !table(dialog)->model()->setData(item(dialog, 0), Qt::Unchecked, Qt::CheckStateRole);
            return true;
        });
        connect(&dialog, &QDialog::finished, &dialog, [&](int) { QVERIFY(!dialog.Submit()); });
        dialog.show(); QVERIFY(dialog.Submit()); QCOMPARE(calls, 1); QCOMPARE(finished.count(), 1); QVERIFY(!dialog.IsSubmitting());
        QVERIFY(submitting); QVERIFY(reentryRejected); QVERIFY(changesRejected);
    }
    void handlerDeletionAndOwnerDeletionAreSafe_data()
    {
        QTest::addColumn<bool>("owner"); QTest::newRow("dialog") << false; QTest::newRow("owner") << true;
    }
    void handlerDeletionAndOwnerDeletionAreSafe()
    {
        QFETCH(bool, owner); auto* parent = new QWidget; auto* dialog = new Dialog(context(), parent); QPointer<Dialog> alive(dialog);
        int calls = 0; bool before = false, after = false;
        dialog->SetSubmissionHandler([&](const auto& values, QString*) {
            ++calls; before = values[0].name == QStringLiteral("眼睛组");
            if (owner) delete parent; else delete dialog;
            after = values[0].name == QStringLiteral("眼睛组"); return true;
        });
        QVERIFY(dialog->Submit()); QVERIFY(!alive); QCOMPARE(calls, 1); QVERIFY(before); QVERIFY(after); if (!owner) delete parent;
    }
    void exceptionPreservesRetryState()
    {
        Dialog dialog(context()); dialog.SetSubmissionHandler([](const auto&, QString*) -> bool { throw std::runtime_error("error"); });
        QVERIFY(!dialog.Submit()); QVERIFY(!dialog.Error().isEmpty()); QVERIFY(!dialog.IsSubmitting()); QVERIFY(checked(dialog, 0));
        dialog.SetSubmissionHandler([](const auto&, QString*) { return true; }); QVERIFY(dialog.Submit());
    }
    void shortColumnsExposeDistinctStatuses_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {560, 1040})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void shortColumnsExposeDistinctStatuses()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        auto data = context();
        data.rows[0].status = QStringLiteral("结构匹配");
        data.rows[1].status = QStringLiteral("结构冲突");
        data.rows[2].status = QStringLiteral("结构或身份冲突");
        data.rows[3].status = QStringLiteral("模型中没有对应对象");
        data.rows[4].status = QStringLiteral("工程中没有对应对象");
        Dialog dialog(data);
        dialog.setStyleSheet(Theme::globalStyleSheet(!dark)); dialog.ensurePolished();
        dialog.resize(width, 580); dialog.show();
        dialog.setStyleSheet(Theme::globalStyleSheet(dark)); QTest::qWait(20);
        auto* view = table(dialog);
        QCOMPARE(view->horizontalScrollBar()->maximum(), 0);
        QStringList visible;
        for (int row = 0; row < data.rows.size(); ++row) {
            for (int column : {0, 1}) {
                const auto index = view->model()->index(row, column);
                bool fits = false;
                const auto text = TextLayoutProbe::VisibleText(view, index, &fits);
                QVERIFY2(fits, qPrintable(QStringLiteral("%1: delegate width exceeds cell width").arg(index.data().toString())));
                QCOMPARE(text, index.data().toString());
            }
            const auto index = view->model()->index(row, 5);
            bool fits = false;
            const auto text = TextLayoutProbe::VisibleText(view, index, &fits);
            if (row < 2) { QVERIFY(fits); QCOMPARE(text, data.rows[row].status); }
            visible.append(text);
        }
        QVERIFY(visible[0] != visible[1]); QVERIFY(visible[0] != visible[2]); QVERIFY(visible[3] != visible[4]);
        QVERIFY(view->columnWidth(3) > view->horizontalHeader()->minimumSectionSize());
        QVERIFY(view->columnWidth(4) > view->horizontalHeader()->minimumSectionSize());
        if (width == 560) QCOMPARE(dialog.width(), width);
        else QVERIFY(dialog.width() > 560);
        qInfo() << "status text" << visible << "requested width" << width << "actual size" << dialog.size() << "DPR" << dialog.devicePixelRatioF();
        const auto screenshot = qEnvironmentVariable("LIVE2D_NAME_DIALOG_SCREENSHOTS");
        if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot + QStringLiteral("-status-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        data.rows[0].status = QString(4000, QChar(0x7ed3));
        Dialog longStatus(data); longStatus.setStyleSheet(Theme::globalStyleSheet(dark)); longStatus.ensurePolished();
        longStatus.resize(width, 580); longStatus.show(); QTest::qWait(20);
        QCOMPARE(table(longStatus)->columnWidth(5), view->columnWidth(5));
        QCOMPARE(table(longStatus)->horizontalScrollBar()->maximum(), 0);
        QVERIFY(longStatus.findChild<QPlainTextEdit*>("objectNamesDetails")->toPlainText().contains(data.rows[0].status));
    }
    void narrowWindowAndLongPlainTextRemainUsable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void narrowWindowAndLongPlainTextRemainUsable()
    {
        QFETCH(bool, dark); auto data = context(); data.sourcePath = QString(2000, 'p') + "<b>plain</b>";
        data.rows[0].sourceName = QString(500, QChar(0x540d)); data.diagnostics.append(QString(5000, QChar(0x8bf4)));
        Dialog dialog(data); dialog.setStyleSheet(Theme::globalStyleSheet(dark)); dialog.ensurePolished();
        dialog.SetSubmissionHandler([](const auto&, QString*) { return false; }); dialog.resize(560, 460); dialog.show(); QTest::qWait(20);
        QVERIFY(dialog.width() <= 560); QVERIFY(dialog.height() <= 460);
        QCOMPARE(table(dialog)->horizontalScrollBar()->maximum(), 0);
        for (const char* name : {"objectNamesApply", "objectNamesCancel", "objectNamesFillMissing", "objectNamesKeepAll"}) {
            auto* control = button(dialog, name); QVERIFY(control->isVisible()); QVERIFY(dialog.rect().contains(QRect(control->mapTo(&dialog, QPoint()), control->size())));
        }
        auto* details = dialog.findChild<QPlainTextEdit*>("objectNamesDetails"); QVERIFY(details->isReadOnly());
        QVERIFY(details->toPlainText().contains("<b>plain</b>")); QVERIFY(details->verticalScrollBar()->maximum() > 0);
        QVERIFY(dialog.findChild<QLineEdit*>("objectNamesFilter")->isEnabled());
        qInfo() << "dialog geometry" << dialog.size() << "DPR" << dialog.devicePixelRatioF() << "platform" << QGuiApplication::platformName();
        const auto screenshot = qEnvironmentVariable("LIVE2D_NAME_DIALOG_SCREENSHOTS");
        if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot + (dark ? "-dark.png" : "-light.png")));
    }
};
QTEST_MAIN(ObjectNamesImportDialogTests)
#include "object_names_import_dialog_tests.moc"
