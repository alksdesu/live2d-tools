#include "ui/cpsdrecoverydialog.h"
#include "ui/theme.h"
#include <QAbstractItemModelTester>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QScreen>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>
#include <cmath>

namespace {
std::shared_ptr<CPsdSourceCatalog> catalog(const QString& source)
{
    auto result = std::make_shared<CPsdSourceCatalog>(); result->sourcePath = source;
    result->sourceVersion = QStringLiteral("5.3"); result->sourceDigest = QByteArray(32, 'x');
    CPsdSourceDocument document; document.id = "doc-a"; document.name = QStringLiteral("<b>同名素材</b>"); document.canvasSize = QSize(320, 180);
    CPsdSourceLayer group; group.kind = CPsdSourceLayer::Kind::Group; group.id = "g"; group.name = QStringLiteral("眼睛"); group.visible = false;
    group.blendMode = "pass"; group.opacity = 180;
    CPsdSourceLayer layer; layer.id = "r"; layer.name = QStringLiteral("高光"); layer.parentIndex = 0; layer.bounds = QRect(-4, 6, 8, 3);
    layer.png = "not-decoded-by-ui"; layer.clipping = true; layer.blendMode = "mul ";
    CPsdSourceLayer empty; empty.id = "e"; empty.name = QStringLiteral("空素材"); empty.parentIndex = 0;
    document.layers = {group, layer, empty};
    document.diagnostics.append({CPsdSourceDiagnostic::Severity::Information, "info", QStringLiteral("<b>原始说明</b>"), "r"});
    result->documents.append(document); document.id = "doc-b"; document.canvasSize = QSize(14, 26); document.layers = {empty}; document.layers[0].parentIndex = -1;
    result->documents.append(document); return result;
}
QPushButton* save(CPsdRecoveryDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("psdRecoveryExport")); }
double luminance(const QColor& color)
{
    const auto linear = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
}

class PsdRecoveryDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void metadataTreePreservesIdentityAndDoesNotDecodePng()
    {
        QTemporaryDir temp; auto data = catalog(temp.filePath("source.cmo3")); CPsdRecoveryDialog dialog(data);
        auto* tree = dialog.findChild<QTreeView*>("psdRecoveryLayers"); auto* model = tree->model();
        QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model->rowCount(), 1); auto group = model->index(0, 0); QCOMPARE(model->rowCount(group), 2);
        QCOMPARE(model->data(group).toString(), QStringLiteral("眼睛"));
        QVERIFY(model->index(0, 1).data().toString().contains(QStringLiteral("隐藏")));
        QCOMPARE(model->index(1, 0, group).data(Qt::UserRole).toInt(), 2);
        QVERIFY(model->index(1, 1, group).data().toString().contains(QStringLiteral("空图层")));
        tree->setCurrentIndex(model->index(0, 0, group));
        const auto details = dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails")->toPlainText();
        QVERIFY(details.contains("-4, 6")); QVERIFY(details.contains(QStringLiteral("正片叠底")));
        QVERIFY(details.contains("<b>原始说明</b>")); QVERIFY(details.contains(QStringLiteral("Photoshop")));
        dialog.SetExportHandler([](int, const QString&, QString*) { return true; }); QVERIFY(save(dialog)->isEnabled());
    }

    void duplicateNamesSelectDistinctDocumentsAndRemainOpenAfterExport()
    {
        QTemporaryDir temp; CPsdRecoveryDialog dialog(catalog(temp.filePath("source.cmo3")));
        QVector<int> exported; dialog.SetExportHandler([&](int index, const QString&, QString*) { exported.append(index); return true; });
        QSignalSpy spy(&dialog, &CPsdRecoveryDialog::Exported); dialog.show();
        QVERIFY(dialog.ExportTo(temp.filePath("first.psd"))); QVERIFY(dialog.isVisible());
        dialog.findChild<QComboBox*>("psdRecoveryDocuments")->setCurrentIndex(1);
        QCOMPARE(dialog.SelectedDocumentIndex(), 1); QVERIFY(dialog.findChild<QLabel*>("psdRecoverySummary")->text().contains("14 × 26"));
        QVERIFY(dialog.ExportTo(temp.filePath("second.psd"))); QCOMPARE(exported, QVector<int>({0, 1})); QCOMPARE(spy.count(), 2);
    }

    void failedExportRetainsSelectionAndAllowsRetry()
    {
        QTemporaryDir temp; CPsdRecoveryDialog dialog(catalog(temp.filePath("source.cmo3"))); int calls = 0;
        dialog.SetExportHandler([&](int, const QString&, QString* error) { if (++calls == 1) { *error = QStringLiteral("<i>目标只读</i>"); return false; } return true; });
        dialog.findChild<QComboBox*>("psdRecoveryDocuments")->setCurrentIndex(1);
        QVERIFY(!dialog.ExportTo(temp.filePath("result.psd"))); QCOMPARE(dialog.SelectedDocumentIndex(), 1);
        QCOMPARE(dialog.Error(), QStringLiteral("<i>目标只读</i>")); QVERIFY(save(dialog)->isEnabled());
        QVERIFY(dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails")->toPlainText().contains(dialog.Error()));
        QVERIFY(dialog.ExportTo(temp.filePath("result.psd"))); QVERIFY(dialog.Error().isEmpty()); QCOMPARE(calls, 2);
    }

    void activeExportCannotReenterAndCloseCancelsBeforeFinishing()
    {
        QTemporaryDir temp; CPsdRecoveryDialog dialog(catalog(temp.filePath("source.cmo3"))); QSignalSpy canceled(&dialog, &CPsdRecoveryDialog::CancelRequested);
        QSignalSpy finished(&dialog, &QDialog::finished); dialog.show(); bool protectedState = false;
        dialog.SetExportHandler([&](int, const QString& path, QString* error) {
            protectedState = dialog.IsExporting() && !save(dialog)->isEnabled() && !dialog.ExportTo(path); dialog.reject();
            dialog.findChild<QComboBox*>("psdRecoveryDocuments")->setCurrentIndex(1);
            protectedState = protectedState && dialog.SelectedDocumentIndex() == 0;
            protectedState = protectedState && finished.count() == 0; *error = QStringLiteral("已取消"); return false;
        });
        QVERIFY(!dialog.ExportTo(temp.filePath("result.psd"))); QVERIFY(protectedState); QCOMPARE(canceled.count(), 1); QCOMPARE(finished.count(), 1); QVERIFY(!dialog.isVisible());
    }

    void callbackCanDestroyDialogAndOwner()
    {
        QTemporaryDir temp; auto* owner = new QWidget;
        QPointer<CPsdRecoveryDialog> dialog = new CPsdRecoveryDialog(catalog(temp.filePath("source.cmo3")), owner);
        dialog->SetExportHandler([owner](int, const QString&, QString*) { delete owner; return true; });
        QVERIFY(!dialog->ExportTo(temp.filePath("result.psd"))); QVERIFY(!dialog);
    }

    void exportPathIsOwnedBeforeCallbacks_data()
    {
        QTest::addColumn<bool>("borrowed"); QTest::addColumn<bool>("beforeHandler");
        QTest::newRow("string-handler") << false << false;
        QTest::newRow("string-text-change") << false << true;
        QTest::newRow("borrowed-handler") << true << false;
        QTest::newRow("borrowed-text-change") << true << true;
    }
    void exportPathIsOwnedBeforeCallbacks()
    {
        QFETCH(bool, borrowed); QFETCH(bool, beforeHandler);
        QTemporaryDir temp; const auto first = temp.filePath("first.psd"), other = temp.filePath("other.psd");
        QCOMPARE(first.size(), other.size()); QVector<QChar> storage(first.cbegin(), first.cend());
        QString path = borrowed ? QString::fromRawData(storage.constData(), storage.size()) : first;
        CPsdRecoveryDialog dialog(catalog(temp.filePath("source.cmo3"))); QSignalSpy exported(&dialog, &CPsdRecoveryDialog::Exported);
        bool mutated = false; QString handlerPath;
        const auto mutate = [&] {
            if (mutated) return;
            mutated = true;
            if (borrowed) std::copy(other.cbegin(), other.cend(), storage.begin());
            else path = other;
        };
        if (beforeHandler) connect(dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails"), &QPlainTextEdit::textChanged, &dialog, mutate);
        dialog.SetExportHandler([&](int, const QString& value, QString*) { handlerPath = QString(value.constData(), value.size()); if (!beforeHandler) mutate(); return true; });
        QVERIFY(dialog.ExportTo(path)); QVERIFY(mutated); QCOMPARE(path, other); QCOMPARE(handlerPath, first);
        QCOMPARE(exported.count(), 1); QCOMPARE(exported[0][1].toString(), first);
        const auto details = dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails")->toPlainText();
        QVERIFY(details.contains(QStringLiteral("已保存：%1").arg(first)));
        QVERIFY(!details.contains(QStringLiteral("已保存：%1").arg(other)));
    }

    void invalidHierarchyAndErrorsDisableOnlyAffectedDocuments()
    {
        QTemporaryDir temp; auto data = catalog(temp.filePath("source.cmo3"));
        data->documents[0].diagnostics.append({CPsdSourceDiagnostic::Severity::Error, "missing", QStringLiteral("像素资源缺失"), "r"});
        CPsdRecoveryDialog dialog(data); dialog.SetExportHandler([](int, const QString&, QString*) { return true; });
        QVERIFY(!save(dialog)->isEnabled()); dialog.findChild<QComboBox*>("psdRecoveryDocuments")->setCurrentIndex(1); QVERIFY(save(dialog)->isEnabled());
        data = catalog(temp.filePath("source.cmo3")); data->documents[0].layers[0].parentIndex = 0;
        CPsdRecoveryDialog broken(data); broken.SetExportHandler([](int, const QString&, QString*) { return true; });
        QVERIFY(!save(broken)->isEnabled()); QCOMPARE(broken.findChild<QTreeView*>()->model()->rowCount(), 0);
    }

    void sourceOverwriteAndSymlinkAliasesAreRejected()
    {
        QTemporaryDir temp; const auto source = temp.filePath("source.cmo3"), alias = temp.filePath("alias.psd");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        QVERIFY(QFile::link(source, alias)); QString error;
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, source, &error));
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, alias, &error));
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(alias, source, &error));
        QVERIFY(CPsdRecoveryDialog::ValidateOutputPath(source, temp.filePath("output.psd"), &error));
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, temp.path(), &error));
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, temp.filePath("missing/output.psd"), &error));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("original"));
    }

    void sourcePathRemainsProtectedAfterRemovalAndTargetRelinking()
    {
        QTemporaryDir temp; const auto source = temp.filePath("source.cmo3"), output = temp.filePath("output.psd");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        QString error; QVERIFY(CPsdRecoveryDialog::ValidateOutputPath(source, output, &error));
        QVERIFY(QFile::link(source, output)); QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, output, &error));
        QVERIFY(QFile::remove(source)); QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, source, &error));
        QVERIFY(!CPsdRecoveryDialog::ValidateOutputPath(source, output, &error));
    }

    void globalErrorBlocksAllDocumentsWhileInformationDoesNot()
    {
        QTemporaryDir temp; auto data = catalog(temp.filePath("source.cmo3"));
        data->diagnostics.append({CPsdSourceDiagnostic::Severity::Error, "invalid", QStringLiteral("无效来源"), {}});
        CPsdRecoveryDialog dialog(data); dialog.SetExportHandler([](int, const QString&, QString*) { return true; });
        QVERIFY(!save(dialog)->isEnabled()); dialog.findChild<QComboBox*>("psdRecoveryDocuments")->setCurrentIndex(1); QVERIFY(!save(dialog)->isEnabled());
        CPsdRecoveryDialog empty(std::make_shared<CPsdSourceCatalog>()); QVERIFY(!save(empty)->isEnabled()); QCOMPARE(empty.SelectedDocumentIndex(), -1);
    }

    void escapeClosesWithoutStartingExport()
    {
        QTemporaryDir temp; CPsdRecoveryDialog dialog(catalog(temp.filePath("source.cmo3"))); int calls = 0;
        dialog.SetExportHandler([&](int, const QString&, QString*) { ++calls; return true; });
        QSignalSpy finished(&dialog, &QDialog::finished); dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape); QCOMPARE(finished.count(), 1); QCOMPARE(calls, 0);
        QVERIFY(save(dialog)->focusPolicy() & Qt::TabFocus);
        QVERIFY(dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails")->focusPolicy() != Qt::NoFocus);
    }

    void compactThemesKeepActionsAndScrollableDetails_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void compactThemesKeepActionsAndScrollableDetails()
    {
        QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        QTemporaryDir temp; auto data = catalog(temp.filePath(QString(200, QChar(0x8def)) + ".cmo3"));
        data->documents[0].name = QString(200, QChar(0x5c42));
        CPsdRecoveryDialog dialog(data); dialog.SetExportHandler([](int, const QString&, QString*) { return true; });
        dialog.resize(560, 560); dialog.show(); QTest::qWait(30);
        QCOMPARE(dialog.width(), 560);
        const auto available = dialog.screen()->availableGeometry();
        const int frameHeight = dialog.frameGeometry().height() - dialog.height();
        if (available.height() >= 560 + frameHeight) QCOMPARE(dialog.height(), 560);
        else QVERIFY(dialog.frameGeometry().height() <= available.height());
        QVERIFY(dialog.rect().contains(save(dialog)->mapTo(&dialog, QPoint())));
        QVERIFY(dialog.rect().contains(save(dialog)->mapTo(&dialog, save(dialog)->rect().bottomRight())));
        auto* details = dialog.findChild<QPlainTextEdit*>("psdRecoveryDetails"); QVERIFY(details->isReadOnly());
        QCOMPARE(details->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        auto* tree = dialog.findChild<QTreeView*>();
        const auto item = tree->visualRect(tree->model()->index(0, 0));
        const QRect branch(item.left() - tree->indentation(), item.top(), tree->indentation(), item.height());
        const auto pixels = tree->viewport()->grab().toImage(); const qreal dpr = pixels.devicePixelRatio();
        const auto area = QRectF(branch).intersected(QRectF(tree->viewport()->rect()));
        const double background = luminance(tree->palette().color(QPalette::Base)); double contrast = 1;
        for (int y = qCeil(area.top() * dpr); y < qFloor(area.bottom() * dpr); ++y)
            for (int x = qCeil(area.left() * dpr); x < qFloor(area.right() * dpr); ++x) {
                const double value = luminance(pixels.pixelColor(x, y)); contrast = qMax(contrast, (qMax(value, background) + .05) / (qMin(value, background) + .05));
            }
        QVERIFY2(contrast >= 3, qPrintable(QStringLiteral("branch pixel contrast %1").arg(contrast)));
        const auto directory = qEnvironmentVariable("PSD_RECOVERY_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory); const auto path = directory + (dark ? "/recovery-dark-560" : "/recovery-light-560");
            QVERIFY(dialog.grab().save(path + ".png"));
            QFile geometry(path + ".json"); QVERIFY(geometry.open(QIODevice::WriteOnly));
            geometry.write(QJsonDocument(QJsonObject{{"width", dialog.width()}, {"height", dialog.height()},
                {"dpr", dialog.devicePixelRatioF()}, {"availableWidth", available.width()}, {"availableHeight", available.height()},
                {"treeWidth", dialog.findChild<QTreeView*>()->width()}, {"treeHeight", dialog.findChild<QTreeView*>()->height()},
                {"detailsHeight", details->height()}}).toJson());
        }
        qApp->setStyleSheet({});
    }
};

QTEST_MAIN(PsdRecoveryDialogTests)
#include "psd_recovery_dialog_tests.moc"
