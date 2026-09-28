#include "ui/cexportresultspanel.h"
#include "ui/theme.h"
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QToolBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSplitter>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>

namespace {
PsdBatchFileResult result(int index, PsdBatchFileState state, const QString& source,
                          const QString& output, bool committed = false)
{
    PsdBatchFileResult value;
    value.index = index;
    value.sourcePath = source;
    value.outputPath = output;
    value.state = state;
    value.stage = state == PsdBatchFileState::Succeeded ? PsdExportStage::Done :
                  state == PsdBatchFileState::Pending || state == PsdBatchFileState::NotRun ? PsdExportStage::Queued :
                  PsdExportStage::Exporting;
    value.outputCommitted = committed;
    return value;
}

QPushButton* button(CExportResultsPanel& panel, const char* name)
{
    return panel.findChild<QPushButton*>(QLatin1String(name));
}

bool showPanel(CExportResultsPanel& panel)
{
    panel.resize(1100, 550);
    panel.show();
    return QTest::qWaitForWindowExposed(&panel);
}
}

class ExportResultsPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void coverageVersionFourKeepsOriginalReportFieldsAndPlainIdentity()
    {
        using Coverage = CPsdExportCoverage;
        Coverage::Context input; input.sourceMocDigest = QByteArray(32, 's'); input.runtimeMocDigest = QByteArray(32, 'r');
        input.canvas = QSize(32, 48); input.runtimeCount = 3;
        QVector<Coverage::Record> rows{{0, "Mesh0", "同名", "Part", 1, Coverage::State::Raster, Coverage::Reason::None, QRect(0, 0, 2, 3)},
            {1, "Mesh1", "<b>当前无像素</b>", "Part", 2, Coverage::State::Empty, Coverage::Reason::OutsideCanvas, {}},
            {2, "Mesh2", "隐藏", "", 3, Coverage::State::Excluded, Coverage::Reason::EditorHidden, {}}};
        auto output = result(0, PsdBatchFileState::Succeeded, "/model.json", "/model.psd", true);
        QVERIFY(Coverage::Capture(input, rows, true, output.coverage));
        CExportResultsPanel panel; QVERIFY(panel.SetResults({output}));
        const auto report = panel.ReportJson(); QCOMPARE(report["version"].toInt(), 4);
        const auto file = report["files"].toArray()[0].toObject();
        QCOMPARE(file["source"].toString(), output.sourcePath); QVERIFY(file["outputCommitted"].toBool());
        QVERIFY(file.contains("diagnostics")); QVERIFY(!file.contains("sourceRecovery"));
        const auto coverage = file["objectCoverage"].toObject(); QCOMPARE(coverage["version"].toInt(), 1);
        QCOMPARE(coverage["records"].toArray().size(), 3);
        QCOMPARE(coverage["records"].toArray()[1].toObject()["psdLayerId"].toInt(), 2);
        const auto details = panel.findChild<QPlainTextEdit*>("exportResultDetails");
        QVERIFY(details); QVERIFY(details->toPlainText().contains(QStringLiteral("<b>当前无像素</b>")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("不代表原始素材已恢复")));
        auto invalid = output; invalid.outputCommitted = false; QVERIFY(!panel.SetResults({invalid}));
        invalid = output; invalid.state = PsdBatchFileState::Canceled; QVERIFY(!panel.SetResults({invalid}));
        QCOMPARE(panel.ReportJson(), report);
        QVERIFY(panel.SetResults({result(0, PsdBatchFileState::Succeeded, "/legacy.json", "/legacy.psd", true)}));
        QCOMPARE(panel.ReportJson()["version"].toInt(), 2);
    }

    void canceledCoverageShowsPreparedLayersAndCompleteReport()
    {
        using Coverage = CPsdExportCoverage;
        Coverage::Context input; input.sourceMocDigest = input.runtimeMocDigest = QByteArray(32, 's');
        input.canvas = QSize(32, 32); input.runtimeCount = 105;
        QVector<Coverage::Record> rows;
        for (int index = 0; index < 105; ++index) rows.append({index, QString::number(index), QStringLiteral("长名称"), {}, quint32(index + 1)});
        rows[0].state = Coverage::State::Raster; rows[0].bounds = QRect(0, 0, 1, 1);
        auto output = result(0, PsdBatchFileState::Canceled, "/model.json", "/model.psd");
        output.error = QStringLiteral("用户已取消");
        QVERIFY(Coverage::Capture(input, rows, false, output.coverage));
        CExportResultsPanel panel; QVERIFY(panel.SetResults({output}));
        const auto text = panel.findChild<QPlainTextEdit*>("exportResultDetails")->toPlainText();
        QVERIFY(text.startsWith(output.error + QLatin1Char('\n') + QStringLiteral("未提交新的输出文件。")));
        QVERIFY(text.contains(QStringLiteral("尚未提交")));
        QVERIFY(text.contains(QStringLiteral("另有 4 项")));
        QCOMPARE(panel.ReportJson()["files"].toArray()[0].toObject()["objectCoverage"].toObject()["records"].toArray().size(), 105);
        QVERIFY(!button(panel, "exportRetryFailed")->isEnabled());
    }

    void coverageDetailsRemainReadableInNarrowThemes_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        QTest::newRow("light-560") << false << 560;
        QTest::newRow("dark-560") << true << 560;
        QTest::newRow("light-960") << false << 960;
        QTest::newRow("dark-960") << true << 960;
    }
    void coverageDetailsRemainReadableInNarrowThemes()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        const QString original = qApp->styleSheet();
        const auto restore = qScopeGuard([&] { qApp->setStyleSheet(original); });
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CPsdExportCoverage::Context input;
        input.sourceMocDigest = input.runtimeMocDigest = QByteArray(32, 's'); input.canvas = QSize(32, 32); input.runtimeCount = 1;
        auto output = result(0, PsdBatchFileState::Failed, QString(300, 'S'), QString(300, 'O'));
        output.error = QStringLiteral("写入失败：磁盘空间不足");
        QVERIFY(CPsdExportCoverage::Capture(input, {{0, "Mesh", QStringLiteral("<b>很长的对象名称</b>"), "Part", 1}}, false, output.coverage));
        CExportResultsPanel panel; QVERIFY(panel.SetResults({output})); panel.resize(width, 520); panel.show(); panel.raise(); panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel)); QTest::qWait(50);
        auto* details = panel.findChild<QPlainTextEdit*>("exportResultDetails"); QVERIFY(details);
        QVERIFY(details->isVisible()); QVERIFY(details->viewport()->height() >= details->fontMetrics().height() * 3);
        QVERIFY(details->document()->firstBlock().text().startsWith(output.error));
        QCOMPARE(details->verticalScrollBar()->value(), details->verticalScrollBar()->minimum());
        const QRect first = details->cursorRect(QTextCursor(details->document()->firstBlock()));
        QVERIFY(details->viewport()->rect().intersects(first));
        QVERIFY(details->toPlainText().contains(QStringLiteral("<b>很长的对象名称</b>")));
        const QString directory = qEnvironmentVariable("PSD_COVERAGE_IMAGE_DIR");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            QVERIFY(panel.grab().save(directory + QStringLiteral("/coverage-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
            qInfo() << "actual" << panel.size() << "dpr" << panel.devicePixelRatioF() << "details" << details->viewport()->size();
        }
    }

    void dockedReportKeepsOneCompleteRowAndSelectedCounts_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void dockedReportKeepsOneCompleteRowAndSelectedCounts()
    {
        QFETCH(bool, dark);
        const QString original = qApp->styleSheet();
        const auto restore = qScopeGuard([&] { qApp->setStyleSheet(original); });
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        QMainWindow window; window.setWindowFlag(Qt::FramelessWindowHint);
        auto* canvas = new QWidget(&window); canvas->setMinimumSize(270, canvas->fontMetrics().lineSpacing() * 10);
        window.setCentralWidget(canvas); window.addToolBar(QStringLiteral("工具"))->addAction(QStringLiteral("导出 PSD"));
        window.statusBar()->showMessage(QStringLiteral("查看导出结果"));
        auto* panel = new CExportResultsPanel(&window); window.addDockWidget(Qt::BottomDockWidgetArea, panel);
        auto value = result(0, PsdBatchFileState::Canceled, "/model.json", "/model.psd"); value.error = QStringLiteral("用户取消");
        CPsdExportCoverage::Context context; context.sourceMocDigest = context.runtimeMocDigest = QByteArray(32, 's');
        context.canvas = QSize(32, 32); context.runtimeCount = 1;
        QVERIFY(CPsdExportCoverage::Capture(context, {{0, "Mesh", QStringLiteral("对象"), {}, 1}}, false, value.coverage));
        QVERIFY(panel->SetResults({value})); window.resize(1000, 760); window.show(); window.raise(); window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* table = panel->findChild<QTableWidget*>("exportResultsTable");
        auto* details = panel->findChild<QPlainTextEdit*>("exportResultDetails");
        auto* summary = panel->findChild<QLabel*>("exportResultsSummary");
        QVERIFY(table && details && summary);
        for (int fontChange = 0; fontChange < 2; ++fontChange) {
            if (fontChange) {
                const int before = details->fontMetrics().lineSpacing();
                panel->setStyleSheet(QStringLiteral("QTableWidget, QHeaderView, QPlainTextEdit { font-size: %1px; }").arg(details->fontMetrics().height() + 2));
                QVERIFY(details->fontMetrics().lineSpacing() > before);
            }
            for (const QSize size : {QSize(900, 720), QSize(1000, 760)}) {
                window.resize(size); window.resizeDocks({panel}, {1}, Qt::Vertical); QTest::qWait(80);
                QVERIFY(window.width() <= size.width() && window.height() <= size.height());
                const QRect row = table->visualItemRect(table->item(0, 0));
                QVERIFY2(row.height() > 0 && row.top() >= 0 && row.bottom() < table->viewport()->height(),
                    qPrintable(QStringLiteral("row %1..%2; viewport %3").arg(row.top()).arg(row.bottom()).arg(table->viewport()->height())));
                qInfo() << "detail metrics" << details->size() << details->minimumSizeHint() << details->frameWidth() << details->contentsMargins() << details->viewport()->geometry() << details->fontMetrics().lineSpacing() << details->document()->documentMargin();
                QVERIFY(details->viewport()->height() >= details->fontMetrics().lineSpacing() * 2);
                QVERIFY(summary->text().contains(value.coverage.Summary()));
                QVERIFY(summary->text().contains(QStringLiteral("未提交")));
                QVERIFY(window.rect().contains(QRect(panel->mapTo(&window, QPoint()), panel->size())));
                QVERIFY(canvas->height() >= canvas->minimumHeight());
                const QString directory = qEnvironmentVariable("PSD_COVERAGE_IMAGE_DIR");
                if (!directory.isEmpty()) {
                    QDir().mkpath(directory);
                    QVERIFY(window.grab().save(directory + QStringLiteral("/docked-%1-%2-font%3.png").arg(dark ? "dark" : "light").arg(size.width()).arg(fontChange)));
                    qInfo() << "docked" << window.size() << "dpr" << window.devicePixelRatioF() << "row" << row << "viewport" << table->viewport()->size() << "details" << details->viewport()->size();
                }
            }
        }
        auto* splitter = panel->findChild<QSplitter*>(); QVERIFY(splitter);
        splitter->setSizes({0, splitter->height()}); QTest::qWait(20);
        QCOMPARE(splitter->sizes().first(), 0);
        auto second = value; second.index = 1; second.outputPath = "/other.psd"; second.coverage = {};
        QVERIFY(panel->SetResults({value, second})); table->setCurrentCell(1, 0);
        QVERIFY(!summary->text().contains(value.coverage.Summary()));
        table->setCurrentCell(0, 0); QVERIFY(summary->text().contains(value.coverage.Summary()));
    }

    void archivedPsdReportKeepsVersionAndExactContentEvidence()
    {
        CExportResultsPanel panel;
        auto output = result(0, PsdBatchFileState::Succeeded, "/missing/original.psd", "/restored.psd", true);
        CExportSourceInfo::Details source;
        source.method = CExportSourceInfo::Method::OriginalPsd;
        source.documentId = source.revisionId = "pinned-revision";
        source.documentName = QStringLiteral("<b>眼睛</b>"); source.familyId = "source-family";
        source.inputDigest = source.outputDigest = QByteArray(32, 'd');
        output.sourceInfo = CExportSourceInfo::Capture(source);
        QVERIFY(panel.SetResults({output}));
        const auto report = panel.ReportJson();
        QCOMPARE(report["version"].toInt(), 3);
        const auto info = report["files"].toArray().first().toObject()["sourceRecovery"].toObject();
        QCOMPARE(info["method"].toString(), QString("original_psd"));
        QCOMPARE(info["revisionId"].toString(), QString("pinned-revision"));
        QCOMPARE(info["inputDigest"], info["outputDigest"]);
        const auto details = panel.findChild<QPlainTextEdit*>("exportResultDetails")->toPlainText();
        QVERIFY(details.contains(source.documentName));
        QVERIFY(details.contains(QStringLiteral("原始 PSD 已完整另存")));
        auto invalid = output;
        source.outputDigest = QByteArray(32, 'x');
        invalid.sourceInfo = CExportSourceInfo::Capture(source);
        QVERIFY(!IsPsdBatchResultValid(invalid));
        QVERIFY(!panel.SetResults({invalid}));
        source.outputDigest.clear(); invalid.sourceInfo = CExportSourceInfo::Capture(source);
        QVERIFY(!IsPsdBatchResultValid(invalid));
        invalid.state = PsdBatchFileState::Failed; invalid.outputCommitted = false;
        QVERIFY(panel.SetResults({invalid}));
        panel.SetRetryAvailable(true);
        QVERIFY(!button(panel, "exportRetryFailed")->isEnabled());
        source.method = CExportSourceInfo::Method::ReconstructedLayers;
        output.sourceInfo = CExportSourceInfo::Capture(source);
        QVERIFY(IsPsdBatchResultValid(output));
        QVERIFY(panel.SetResults({output}));
        QCOMPARE(panel.ReportJson()["files"].toArray().first().toObject()["sourceRecovery"].toObject()["method"].toString(), QString("reconstructed_layers"));
    }

    void sourceReportOwnsBorrowedMetadataAndParticipatesInIdentity()
    {
        QString name = "original-name";
        QByteArray digest(32, 'q');
        CExportSourceInfo::Details details;
        details.documentId = "document";
        details.documentName = QString::fromRawData(name.constData(), name.size());
        details.inputDigest = QByteArray::fromRawData(digest.constData(), digest.size());
        auto before = result(0, PsdBatchFileState::Failed, "/source.cmo3", "/out.psd");
        before.sourceInfo = CExportSourceInfo::Capture(details);
        auto after = before;
        name.fill('x'); digest.fill('z');
        QCOMPARE(before.sourceInfo.Get().documentName, QString("original-name"));
        QCOMPARE(before.sourceInfo.Get().inputDigest, QByteArray(32, 'q'));
        QVERIFY(before == after);
        details.documentName = "changed-name";
        after.sourceInfo = CExportSourceInfo::Capture(details);
        QVERIFY(before != after);
        details.documentName = QString(32769, 'a');
        QVERIFY(!CExportSourceInfo::Capture(details).IsValid());
    }

    void successfulOutputKeepsStructuredWarnings()
    {
        CExportResultsPanel panel;
        auto output = result(0, PsdBatchFileState::Succeeded, "/model.json", "/output.psd", true);
        output.diagnostics = CExportDiagnostics::Capture({{live2dstruct::CModelDiagnostic::Warning,
            "mask_source_duplicate", QStringLiteral("<b>保留重复蒙版</b> & 原始次数"), 42, "artmesh", "Eye"}},
            "/source.moc3", QByteArray(32, 'a'), 9);
        QVERIFY(panel.SetResults({output}));
        auto* table = panel.findChild<QTableWidget*>("exportResultsTable");
        auto* details = panel.findChild<QPlainTextEdit*>("exportResultDetails");
        auto* summary = panel.findChild<QLabel*>("exportResultsSummary");
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("成功（有提示）"));
        QVERIFY(details->toPlainText().contains(QStringLiteral("<b>保留重复蒙版</b>")));
        QVERIFY(summary->text().contains(QStringLiteral("1 项包含检查提示")));
        const auto json = panel.ReportJson();
        QCOMPARE(json["version"].toInt(), 2);
        const auto file = json["files"].toArray().first().toObject();
        QCOMPARE(file["state"].toString(), QStringLiteral("succeeded"));
        QVERIFY(file["error"].toString().isEmpty());
        QCOMPARE(file["diagnostics"].toObject()["entries"].toArray().first().toObject()["objectId"].toString(), QStringLiteral("Eye"));
        QCOMPARE(file["diagnostics"].toObject()["documentRevision"].toString(), QStringLiteral("9"));
        QVERIFY(!button(panel, "exportRetryFailed")->isEnabled());
    }

    void canceledFileIsVisibleTerminalAndNotRetryable()
    {
        CExportResultsPanel panel;
        auto canceled = result(0, PsdBatchFileState::Canceled, QStringLiteral("/canceled.json"), QStringLiteral("/canceled.psd"));
        canceled.stage = PsdExportStage::Writing;
        canceled.error = QStringLiteral("用户取消了当前文件 <literal>");
        canceled.elapsedMs = 1240;
        auto notRun = result(1, PsdBatchFileState::NotRun, QStringLiteral("/not-run.json"), QStringLiteral("/not-run.psd"));
        auto failed = result(2, PsdBatchFileState::Failed, QStringLiteral("/failed.json"), QStringLiteral("/failed.psd"));
        failed.stage = PsdExportStage::Encoding;
        QVERIFY(panel.SetResults({canceled, notRun, failed}));
        QVERIFY(showPanel(panel));
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        auto* details = panel.findChild<QPlainTextEdit*>(QStringLiteral("exportResultDetails"));
        auto* summary = panel.findChild<QLabel*>(QStringLiteral("exportResultsSummary"));
        auto* retry = button(panel, "exportRetryFailed");
        QVERIFY(table && details && summary && retry);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("已取消"));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("写入文件"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("1.24 s"));
        QVERIFY(details->toPlainText().startsWith(canceled.error));
        QVERIFY(details->toPlainText().contains(QStringLiteral("未提交新的输出文件。")));
        QVERIFY(summary->text().contains(QStringLiteral("已取消 1")));
        QVERIFY(summary->text().contains(QStringLiteral("未执行 1")));
        QVERIFY(summary->text().contains(QStringLiteral("失败 1")));
        QSignalSpy retries(&panel, &CExportResultsPanel::RetryRequested);
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(retries.size(), 1);
        QCOMPARE(retries[0][0].toStringList(), QStringList{failed.sourcePath});
        auto stale = canceled;
        stale.state = PsdBatchFileState::Running;
        QVERIFY(!panel.UpdateResult(0, stale));
        const auto files = panel.ReportJson()[QStringLiteral("files")].toArray();
        QCOMPARE(files[0].toObject()[QStringLiteral("state")].toString(), QStringLiteral("canceled"));
        QCOMPARE(files[0].toObject()[QStringLiteral("stage")].toString(), QStringLiteral("writing"));
        QCOMPARE(files[1].toObject()[QStringLiteral("state")].toString(), QStringLiteral("not_run"));
        QCOMPARE(files[2].toObject()[QStringLiteral("stage")].toString(), QStringLiteral("encoding"));
        QCOMPARE(static_cast<int>(PsdBatchFileState::NotRun), 4);
        QCOMPARE(static_cast<int>(PsdBatchFileState::Canceled), 5);
        QVERIFY(panel.SetResults({canceled, notRun}));
        QVERIFY(!retry->isEnabled());
    }

    void stageDisplayTracksLayerEncodingButDoesNotLeaveFinalWriting()
    {
        CExportResultsPanel panel;
        auto running = result(0, PsdBatchFileState::Running, QStringLiteral("/model.json"), QStringLiteral("/model.psd"));
        QVERIFY(panel.SetResults({running}));
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        QVERIFY(table);
        for (auto stage : {PsdExportStage::Encoding, PsdExportStage::Exporting, PsdExportStage::Encoding, PsdExportStage::Writing}) {
            running.stage = stage;
            ++running.elapsedMs;
            QVERIFY(panel.UpdateResult(0, running));
            QCOMPARE(table->item(0, 3)->text(), PsdExportStageText(stage));
        }
        auto stale = running;
        stale.stage = PsdExportStage::Encoding;
        QVERIFY(!panel.UpdateResult(0, stale));
        stale.stage = PsdExportStage::Exporting;
        QVERIFY(!panel.UpdateResult(0, stale));
        running.stage = PsdExportStage::Restoring;
        QVERIFY(panel.UpdateResult(0, running));
        stale.stage = PsdExportStage::Writing;
        QVERIFY(!panel.UpdateResult(0, stale));
        running.state = PsdBatchFileState::Canceled;
        running.stage = PsdExportStage::Encoding;
        QVERIFY(panel.UpdateResult(0, running));
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("已取消"));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("压缩图层"));
    }

    void displaysPlainPathsErrorsAndCancelableStates()
    {
        CExportResultsPanel panel;
        panel.resize(1100, 550);
        const QString source = QStringLiteral("/模型/<b>source & 文本</b>.model3.json");
        const QString output = QStringLiteral("/输出/<img src=missing>.psd");
        const QString error = QStringLiteral("<b>磁盘写入失败</b> & 原因\n第二行：无权限");
        auto failed = result(0, PsdBatchFileState::Failed, source, output);
        failed.error = error;
        failed.elapsedMs = 1234;
        auto canceled = result(1, PsdBatchFileState::NotRun, QStringLiteral("/b.json"), QStringLiteral("/b.psd"));
        canceled.error = QStringLiteral("已取消，未开始导出");
        QSignalSpy opened(&panel, &CExportResultsPanel::OpenOutputRequested);
        QSignalSpy retried(&panel, &CExportResultsPanel::RetryRequested);
        QVERIFY(panel.SetResults({failed, canceled}));
        panel.show();
        QCoreApplication::processEvents();
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        auto* details = panel.findChild<QPlainTextEdit*>(QStringLiteral("exportResultDetails"));
        auto* summary = panel.findChild<QLabel*>(QStringLiteral("exportResultsSummary"));
        QVERIFY(table && details && summary);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->columnCount(), 6);
        QCOMPARE(table->item(0, 0)->text(), source);
        QCOMPARE(table->item(0, 1)->text(), output);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("失败"));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("导出图层"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("1.23 s"));
        QCOMPARE(table->item(0, 5)->text(), error);
        QVERIFY(details->toPlainText().contains(source));
        QVERIFY(details->toPlainText().contains(output));
        QVERIFY(details->toPlainText().startsWith(error));
        QVERIFY(details->isReadOnly());
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QVERIFY(summary->text().contains(QStringLiteral("失败 1")));
        QVERIFY(summary->text().contains(QStringLiteral("未执行 1")));
        table->setCurrentCell(1, 0);
        QCOMPARE(table->item(1, 2)->text(), QStringLiteral("未执行"));
        QCOMPARE(table->item(1, 4)->text(), QStringLiteral("—"));
        QVERIFY(details->toPlainText().startsWith(canceled.error));
        QCOMPARE(opened.size(), 0);
        QCOMPARE(retried.size(), 0);
        QVERIFY(!panel.grab().isNull());
    }

    void narrowWindowShowsErrorBeforeLongPathsAndKeepsScrollableDetails()
    {
        CExportResultsPanel panel;
        panel.resize(460, 520);
        const QString longPath = QStringLiteral("/很长的项目目录/重复的中文与Unicode-é/").repeated(15);
        auto failed = result(0, PsdBatchFileState::Failed, longPath + QStringLiteral("模型.json"),
                             longPath + QStringLiteral("输出.psd"), true);
        failed.stage = PsdExportStage::Restoring;
        failed.error = QStringLiteral("错误首行：<b>恢复预览失败</b>\n错误第二行：测试字面文本");
        QVERIFY(panel.SetResults({failed}));
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* details = panel.findChild<QPlainTextEdit*>(QStringLiteral("exportResultDetails"));
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        auto* splitter = panel.findChild<QSplitter*>();
        QVERIFY(details && table && splitter);
        QCOMPARE(details->verticalScrollBar()->value(), 0);
        QVERIFY(details->height() >= table->height() * 3 / 4);
        QVERIFY(details->toPlainText().startsWith(failed.error));
        QVERIFY(details->toPlainText().indexOf(QStringLiteral("文件已确认写出。")) <
                details->toPlainText().indexOf(QStringLiteral("源模型：")));
        const QTextBlock first = details->document()->firstBlock();
        QVERIFY(first.isValid());
        QCOMPARE(first.text(), QStringLiteral("错误首行：<b>恢复预览失败</b>"));
        const QRect visibleLine = details->cursorRect(QTextCursor(first));
        QVERIFY(details->viewport()->rect().intersects(visibleLine));
        QVERIFY(details->verticalScrollBar()->maximum() > 0);
        QVERIFY(table->horizontalScrollBar()->maximum() > 0);
        details->verticalScrollBar()->setValue(details->verticalScrollBar()->maximum());
        QTextCursor finalLine(details->document());
        finalLine.movePosition(QTextCursor::End);
        QVERIFY(details->viewport()->rect().intersects(details->cursorRect(finalLine)));
        const int initialHeight = details->height();
        splitter->setSizes({initialHeight / 2, initialHeight * 3 / 2});
        QVERIFY(details->height() > initialHeight);
        QVERIFY(details->toPlainText().contains(failed.sourcePath));
        QVERIFY(details->toPlainText().contains(failed.outputPath));
    }

    void updatesSnapshotsWithoutAcceptingStaleOrInvalidRows()
    {
        CExportResultsPanel panel;
        const auto pending = result(0, PsdBatchFileState::Pending, QStringLiteral("/a.json"), QStringLiteral("/a.psd"));
        QVERIFY(panel.SetResults({pending}));
        const auto snapshot = panel.Results();
        auto running = pending;
        running.state = PsdBatchFileState::Running;
        running.stage = PsdExportStage::Preparing;
        running.elapsedMs = 12;
        QVERIFY(panel.UpdateResult(0, running));
        QCOMPARE(snapshot[0].state, PsdBatchFileState::Pending);
        auto bad = running;
        bad.stage = PsdExportStage::Preflight;
        QVERIFY(!panel.UpdateResult(0, bad));
        bad = running;
        bad.elapsedMs = 1;
        QVERIFY(!panel.UpdateResult(0, bad));
        QVERIFY(!panel.UpdateResult(0, pending));
        bad = running;
        bad.sourcePath = QStringLiteral("/other.json");
        QVERIFY(!panel.UpdateResult(0, bad));
        bad = running;
        bad.outputPath = QStringLiteral("/other.psd");
        QVERIFY(!panel.UpdateResult(0, bad));
        QVERIFY(!panel.UpdateResult(1, running));
        bad = running;
        bad.index = 1;
        QVERIFY(!panel.UpdateResult(0, bad));
        QVERIFY(!panel.SetResults({bad}));
        bad = running;
        bad.state = PsdBatchFileState(99);
        QVERIFY(!panel.SetResults({bad}));
        QVERIFY(!panel.UpdateResult(0, bad));
        bad = running;
        bad.stage = PsdExportStage(-1);
        QVERIFY(!panel.SetResults({bad}));
        bad = running;
        bad.elapsedMs = -1;
        QVERIFY(!panel.SetResults({bad}));
        QVERIFY(panel.Results() == PsdBatchResults{running});
        auto succeeded = running;
        succeeded.state = PsdBatchFileState::Succeeded;
        succeeded.stage = PsdExportStage::Done;
        succeeded.outputCommitted = true;
        succeeded.elapsedMs = 25;
        QVERIFY(panel.UpdateResult(0, succeeded));
        QVERIFY(panel.UpdateResult(0, succeeded));
        QVERIFY(!panel.UpdateResult(0, running));
        QVERIFY(panel.Results() == PsdBatchResults{succeeded});
        QVERIFY(panel.SetResults({pending}));
        auto canceled = pending;
        canceled.state = PsdBatchFileState::NotRun;
        QVERIFY(panel.UpdateResult(0, canceled));
        QVERIFY(!panel.UpdateResult(0, running));
        QVERIFY(panel.SetResults({}));
        QVERIFY(panel.Results().isEmpty());
    }

    void onlyExplicitCommittedExistingFileCanOpen()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString output = temp.filePath(QStringLiteral("written.psd"));
        QFile file(output);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("PSD"), 3);
        file.close();
        CExportResultsPanel panel;
        auto* open = button(panel, "exportOpenOutput");
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        QVERIFY(open && table);
        PsdBatchResults records{
            result(0, PsdBatchFileState::Succeeded, QStringLiteral("/ok.json"), output, true),
            result(1, PsdBatchFileState::Failed, QStringLiteral("/restore.json"), output, true),
            result(2, PsdBatchFileState::Failed, QStringLiteral("/unconfirmed.json"), output),
            result(3, PsdBatchFileState::Succeeded, QStringLiteral("/missing.json"), temp.filePath(QStringLiteral("missing.psd")), true),
            result(4, PsdBatchFileState::Succeeded, QStringLiteral("/directory.json"), temp.path(), true)};
        records[1].stage = PsdExportStage::Restoring;
        records[1].error = QStringLiteral("恢复预览失败");
        QSignalSpy opened(&panel, &CExportResultsPanel::OpenOutputRequested);
        QVERIFY(panel.SetResults(records));
        QVERIFY(showPanel(panel));
        QCOMPARE(opened.size(), 0);
        QVERIFY(open->isEnabled());
        QVERIFY(open->isVisible());
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), output);
        table->setCurrentCell(1, 0);
        QVERIFY(open->isEnabled());
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opened.size(), 2);
        for (int index = 2; index < records.size(); ++index) {
            table->setCurrentCell(index, 0);
            QVERIFY(!open->isEnabled());
            QTest::mouseClick(open, Qt::LeftButton);
            QCOMPARE(opened.size(), 2);
        }
        table->setCurrentCell(0, 0);
        QVERIFY(open->isEnabled());
        QVERIFY(QFile::remove(output));
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opened.size(), 2);
        QVERIFY(!open->isEnabled());
    }

    void retriesOnlyUncommittedFailuresInOriginalOrder()
    {
        CExportResultsPanel panel;
        const QString duplicate = QStringLiteral("/same.model3.json");
        PsdBatchResults records{
            result(0, PsdBatchFileState::Failed, duplicate, QStringLiteral("/a.psd")),
            result(1, PsdBatchFileState::NotRun, QStringLiteral("/canceled.json"), QStringLiteral("/b.psd")),
            result(2, PsdBatchFileState::Failed, QStringLiteral("/restore.json"), QStringLiteral("/c.psd"), true),
            result(3, PsdBatchFileState::Succeeded, QStringLiteral("/ok.json"), QStringLiteral("/d.psd"), true),
            result(4, PsdBatchFileState::Failed, duplicate, QStringLiteral("/e.psd")),
            result(5, PsdBatchFileState::Running, QStringLiteral("/running.json"), QStringLiteral("/f.psd")),
            result(6, PsdBatchFileState::Pending, QStringLiteral("/pending.json"), QStringLiteral("/g.psd")),
            result(7, PsdBatchFileState::Failed, QStringLiteral("/last.json"), QStringLiteral("/h.psd"))};
        QVERIFY(panel.SetResults(records));
        QVERIFY(showPanel(panel));
        auto* retry = button(panel, "exportRetryFailed");
        auto* copy = button(panel, "exportCopyReport");
        QVERIFY(retry && copy);
        QSignalSpy retried(&panel, &CExportResultsPanel::RetryRequested);
        QVERIFY(retry->isEnabled());
        QVERIFY(retry->isVisible());
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(retried.size(), 1);
        QCOMPARE(retried[0][0].toStringList(), (QStringList{duplicate, duplicate, QStringLiteral("/last.json")}));
        panel.SetBusy(true);
        QVERIFY(!retry->isEnabled());
        QVERIFY(copy->isEnabled());
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(retried.size(), 1);
        panel.SetBusy(false);
        QVERIFY(retry->isEnabled());
        QVERIFY(panel.Results() == records);
        QVERIFY(panel.SetResults({result(0, PsdBatchFileState::NotRun, duplicate, QStringLiteral("/a.psd"))}));
        QVERIFY(!retry->isEnabled());
        QVERIFY(copy->isEnabled());
        QVERIFY(panel.SetResults({}));
        QVERIFY(!copy->isEnabled());
    }

    void busyPreventsOpeningWithoutChangingSelectionOrResults()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("ok.psd"));
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.close();
        CExportResultsPanel panel;
        const auto record = result(0, PsdBatchFileState::Succeeded, QStringLiteral("/a.json"), path, true);
        QVERIFY(panel.SetResults({record}));
        QVERIFY(showPanel(panel));
        auto* open = button(panel, "exportOpenOutput");
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        QVERIFY(open && table);
        QSignalSpy opened(&panel, &CExportResultsPanel::OpenOutputRequested);
        panel.SetBusy(true);
        QVERIFY(!open->isEnabled());
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opened.size(), 0);
        panel.SetBusy(false);
        QVERIFY(open->isEnabled());
        QCOMPARE(table->currentRow(), 0);
        QVERIFY(panel.Results() == PsdBatchResults{record});
    }

    void recoveryDisablesRetryWhileConfirmedOutputRemainsOpenable()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString output = temp.filePath(QStringLiteral("written.psd"));
        QFile file(output);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        CExportResultsPanel panel;
        const QString duplicate = QStringLiteral("/重复.model3.json");
        const PsdBatchResults records{
            result(0, PsdBatchFileState::Failed, duplicate, QStringLiteral("/a.psd")),
            result(1, PsdBatchFileState::Failed, duplicate, QStringLiteral("/b.psd")),
            result(2, PsdBatchFileState::Failed, QStringLiteral("/last.json"), QStringLiteral("/c.psd")),
            result(3, PsdBatchFileState::Failed, QStringLiteral("/restore.json"), output, true)};
        QVERIFY(panel.SetResults(records));
        panel.SetBusy(false);
        const QString reason = QStringLiteral("预览 <model> & 状态尚未恢复");
        panel.SetRetryAvailable(false, reason);
        QVERIFY(showPanel(panel));
        auto* open = button(panel, "exportOpenOutput");
        auto* retry = button(panel, "exportRetryFailed");
        auto* table = panel.findChild<QTableWidget*>(QStringLiteral("exportResultsTable"));
        QVERIFY(open && retry && table);
        QSignalSpy opened(&panel, &CExportResultsPanel::OpenOutputRequested);
        QSignalSpy retried(&panel, &CExportResultsPanel::RetryRequested);
        table->setCurrentCell(3, 0);
        QVERIFY(open->isEnabled());
        QVERIFY(!retry->isEnabled());
        QCOMPARE(retry->toolTip(), QStringLiteral("<qt>预览 &lt;model&gt; &amp; 状态尚未恢复</qt>"));
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), output);
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(retried.size(), 0);
        panel.SetBusy(true);
        QVERIFY(!open->isEnabled());
        panel.SetRetryAvailable(true);
        QVERIFY(!retry->isEnabled());
        panel.SetBusy(false);
        QVERIFY(open->isEnabled());
        QVERIFY(retry->isEnabled());
        QVERIFY(retry->toolTip().isEmpty());
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(retried.size(), 1);
        QCOMPARE(retried[0][0].toStringList(), (QStringList{duplicate, duplicate, QStringLiteral("/last.json")}));
        QVERIFY(panel.Results() == records);
    }

    void reportRoundTripsExactDetailsAndCopiesOnlyOnUserAction()
    {
        CExportResultsPanel panel;
        auto failed = result(0, PsdBatchFileState::Failed, QStringLiteral("/模型/\"name\".json"), QStringLiteral("/输出/a.psd"), true);
        failed.stage = PsdExportStage::Restoring;
        failed.error = QStringLiteral("<script>literal</script>\n\t错误：\\disk");
        failed.elapsedMs = 9876543210LL;
        const auto canceled = result(1, PsdBatchFileState::NotRun, QStringLiteral("/b.json"), QStringLiteral("/b.psd"));
        auto* clipboard = QApplication::clipboard();
        const QString original = clipboard->text();
        const auto restoreClipboard = qScopeGuard([&] { clipboard->setText(original); });
        clipboard->setText(QStringLiteral("unrelated clipboard"));
        QVERIFY(panel.SetResults({failed, canceled}));
        panel.SetBusy(true);
        QVERIFY(showPanel(panel));
        QCOMPARE(clipboard->text(), QStringLiteral("unrelated clipboard"));
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(panel.ReportText().toUtf8(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QCOMPARE(json.object(), panel.ReportJson());
        QCOMPARE(json[QStringLiteral("version")].toInt(), 2);
        QVERIFY(json[QStringLiteral("busy")].toBool());
        const auto files = json[QStringLiteral("files")].toArray();
        QCOMPARE(files.size(), 2);
        const auto first = files[0].toObject();
        QCOMPARE(first[QStringLiteral("source")].toString(), failed.sourcePath);
        QCOMPARE(first[QStringLiteral("output")].toString(), failed.outputPath);
        QCOMPARE(first[QStringLiteral("error")].toString(), failed.error);
        QCOMPARE(first[QStringLiteral("state")].toString(), QStringLiteral("failed"));
        QCOMPARE(first[QStringLiteral("stage")].toString(), QStringLiteral("restoring"));
        QCOMPARE(first[QStringLiteral("elapsedMs")].toInteger(), failed.elapsedMs);
        QVERIFY(first[QStringLiteral("outputCommitted")].toBool());
        QCOMPARE(files[1].toObject()[QStringLiteral("state")].toString(), QStringLiteral("not_run"));
        auto* copy = button(panel, "exportCopyReport");
        QVERIFY(copy && copy->isEnabled());
        QVERIFY(copy->isVisible());
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(clipboard->text(), panel.ReportText());
    }
};

QTEST_MAIN(ExportResultsPanelTests)
#include "export_results_panel_tests.moc"
