#include "ui/ccmosourceimportdialog.h"
#include "ui/theme.h"
#include "cmo_artwork_source_fixture.h"
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QtTest>

namespace {
cmo3::CCmo3Project project()
{
    cmo3::CCmo3Project result; QString error;
    if (!cmo3::CCmo3Project::OpenBytes(cmo_artwork_source_fixture::archive(), "/offline/source.cmo3", result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
cmo3::CCmo3Project partiallyUnderstoodProject(int documents = 1)
{
    cmo3::CCaffArchive original, edited; QString error;
    if (!original.LoadBytes(cmo_artwork_source_fixture::archive(documents), &error)) qFatal("%s", qPrintable(error));
    for (const auto& entry : original.Entries()) {
        auto bytes = entry.content;
        if (entry.tag == "main_xml") {
            bytes.replace("uuid=\"d0-root-guid\"/>", "uuid=\"d0-root-guid\"/><i xs.n=\"parentEffect\">1</i>");
            bytes.replace("uuid=\"d0-empty-guid\"/>", "uuid=\"d0-empty-guid\"/><i xs.n=\"siblingEffect\">1</i>");
            bytes.replace("uuid=\"d0-guid\"/>", "uuid=\"d0-guid\"/><i xs.n=\"documentEffect\">1</i>");
        }
        edited.AddEntry(entry.path, bytes, entry.tag, entry.compress);
    }
    QTemporaryDir directory; const auto path = directory.filePath("effects.cmo3"); if (!edited.Save(path, &error)) qFatal("%s", qPrintable(error));
    cmo3::CCmo3Project source; if (!cmo3::CCmo3Project::OpenFile(path, source, &error)) qFatal("%s", qPrintable(error));
    return source;
}
}

class CmoSourceImportDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void sameNamesSelectNativeDocumentsAndKeepRealParents()
    {
        auto source = project(); const auto original = source.Bytes(); CCmoSourceImportDialog dialog(source);
        source = {}; QCOMPARE(dialog.SelectedDocumentIds(), QStringList({"d0-guid", "d1-guid"}));
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments"); QVERIFY(tree); QCOMPARE(tree->topLevelItemCount(), 2);
        auto* first = tree->topLevelItem(0); auto* second = tree->topLevelItem(1); QCOMPARE(first->text(0), second->text(0));
        QCOMPARE(first->childCount(), 1); QCOMPARE(first->child(0)->childCount(), 2);
        QCOMPARE(first->child(0)->child(0)->text(0), QString("d0-pixels")); QCOMPARE(first->child(0)->child(1)->text(1), QString("空图层"));
        QVERIFY(!(first->child(0)->flags() & Qt::ItemIsUserCheckable));
        first->setCheckState(0, Qt::Unchecked); QCOMPARE(dialog.SelectedDocumentIds(), QStringList{"d1-guid"});
        tree->setCurrentItem(second->child(0)->child(0));
        const auto details = dialog.findChild<QPlainTextEdit*>("cmoSourceDetails")->toPlainText();
        QVERIFY(details.contains("d1-pixels-guid")); QVERIFY(details.contains("127 / 255")); QVERIFY(details.contains("隐藏"));
        QVERIFY(details.contains("原生素材关系暂不能读取")); QVERIFY(!original.isEmpty());
    }
    void selectionAndColorAreDraftOnlyAndEmptySelectionCannotAccept()
    {
        const auto source = project(); const auto raw = source.Bytes(); CCmoSourceImportDialog dialog(source);
        auto* color = dialog.findChild<QComboBox*>("cmoSourceColorPolicy"); QVERIFY(color);
        QCOMPARE(dialog.ColorPolicy(), live2dstruct::CModelArtwork::ColorPolicy::PreserveValues);
        color->setCurrentIndex(color->findData(int(live2dstruct::CModelArtwork::ColorPolicy::ConvertToSRgb)));
        QCOMPARE(dialog.ColorPolicy(), live2dstruct::CModelArtwork::ColorPolicy::ConvertToSRgb);
        dialog.findChild<QPushButton*>("cmoSourceSelectNone")->click(); QVERIFY(dialog.SelectedDocumentIds().isEmpty());
        auto* save = dialog.findChild<QPushButton*>("cmoSourceSave"); QVERIFY(!save->isEnabled());
        QSignalSpy accepted(&dialog, &QDialog::accepted); dialog.accept(); QCOMPARE(accepted.count(), 0);
        dialog.findChild<QPushButton*>("cmoSourceSelectAll")->click(); QCOMPARE(dialog.SelectedDocumentIds().size(), 2); QVERIFY(save->isEnabled());
        dialog.accept(); QCOMPARE(accepted.count(), 1); QCOMPARE(source.Bytes(), raw);
    }
    void invalidProjectKeepsImportDisabled()
    {
        CCmoSourceImportDialog dialog(cmo3::CCmo3Project{});
        QVERIFY(dialog.SelectedDocumentIds().isEmpty()); QVERIFY(!dialog.findChild<QPushButton*>("cmoSourceSave")->isEnabled());
        QVERIFY(!dialog.findChild<QPlainTextEdit*>("cmoSourceDetails")->toPlainText().isEmpty());
        QSignalSpy accepted(&dialog, &QDialog::accepted); dialog.accept(); QCOMPARE(accepted.count(), 0);
    }
    void selectedLayerKeepsParentAndDocumentRecoveryDiagnostics()
    {
        const auto source = partiallyUnderstoodProject();
        CCmoSourceImportDialog dialog(source); QCOMPARE(dialog.SelectedDocumentIds(), QStringList{"d0-guid"});
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments"); tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
        const auto details = dialog.findChild<QPlainTextEdit*>("cmoSourceDetails")->toPlainText();
        QVERIFY(details.contains("parentEffect")); QVERIFY(details.contains("siblingEffect")); QVERIFY(details.contains("documentEffect"));
        QVERIFY(dialog.findChild<QPushButton*>("cmoSourceSave")->isEnabled());
        QVERIFY(details.contains("不能保证完整 PSD 恢复或自动关联"));
    }
    void mixedDocumentsPreserveRawValuesButRejectIncompleteColorConversion()
    {
        const auto source = partiallyUnderstoodProject(2); const auto bytes = source.Bytes(); CCmoSourceImportDialog dialog(source);
        dialog.setStyleSheet(Theme::globalStyleSheet(true)); dialog.resize(600, 600); dialog.show(); QTRY_VERIFY(dialog.isVisible());
        QCOMPARE(dialog.size(), QSize(600, 600));
        QCOMPARE(dialog.SelectedDocumentIds(), QStringList({"d0-guid", "d1-guid"}));
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments");
        auto* save = dialog.findChild<QPushButton*>("cmoSourceSave"); auto* color = dialog.findChild<QComboBox*>("cmoSourceColorPolicy");
        auto* status = dialog.findChild<QLabel*>("cmoSourceStatus");
        QVERIFY(tree->topLevelItem(0)->flags() & Qt::ItemIsUserCheckable); QVERIFY(save->isEnabled());
        QVERIFY(status->text().contains("其中 1 份")); QVERIFY(status->text().contains("不能保证完整 PSD 恢复或自动关联"));
        QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())));
        const auto screenshots = qEnvironmentVariable("LIVE2D_TEST_CMO_UI_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(dialog.grab().save(QDir(screenshots).filePath("partial-preserve-narrow.png")));
        }
        const int convert = color->findData(int(live2dstruct::CModelArtwork::ColorPolicy::ConvertToSRgb));
        const int preserve = color->findData(int(live2dstruct::CModelArtwork::ColorPolicy::PreserveValues));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        color->setCurrentIndex(convert); QVERIFY(!save->isEnabled()); QVERIFY(status->text().contains("不能完整转换颜色"));
        QCoreApplication::processEvents(); QCOMPARE(dialog.size(), QSize(600, 600));
        QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())));
        if (!screenshots.isEmpty()) QVERIFY(dialog.grab().save(QDir(screenshots).filePath("partial-convert-blocked-narrow.png")));
        dialog.accept(); QCOMPARE(accepted.count(), 0);
        tree->topLevelItem(0)->setCheckState(0, Qt::Unchecked); QCOMPARE(dialog.SelectedDocumentIds(), QStringList{"d1-guid"});
        QVERIFY(save->isEnabled()); QVERIFY(!status->text().contains("不能完整转换颜色"));
        tree->topLevelItem(0)->setCheckState(0, Qt::Checked); QVERIFY(!save->isEnabled());
        color->setCurrentIndex(preserve); QVERIFY(save->isEnabled());
        dialog.findChild<QPushButton*>("cmoSourceSelectNone")->click(); QVERIFY(!save->isEnabled());
        color->setCurrentIndex(convert); QVERIFY(!save->isEnabled()); dialog.accept(); QCOMPARE(accepted.count(), 0);
        dialog.findChild<QPushButton*>("cmoSourceSelectAll")->click(); QVERIFY(!save->isEnabled());
        color->setCurrentIndex(preserve); QVERIFY(save->isEnabled()); dialog.accept(); QCOMPARE(accepted.count(), 1);
        QCOMPARE(source.Bytes(), bytes);
    }
    void nativeProjectKeepsUnmeshedLayersSelectable()
    {
        const auto path = qEnvironmentVariable("LIVE2D_TEST_NATIVE_CMO");
        if (path.isEmpty()) QSKIP("LIVE2D_TEST_NATIVE_CMO is not configured");
        cmo3::CCmo3Project source; QString error;
        QVERIFY2(cmo3::CCmo3Project::OpenFile(path, source, &error), qPrintable(error));
        QVERIFY(source.Artwork()); QVERIFY(source.Materials()); CCmoSourceImportDialog dialog(source);
        QCOMPARE(dialog.SelectedDocumentIds().size(), source.Artwork()->documents.size());
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments");
        int layers = 0, unmeshed = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if (!(*it)->parent()) continue;
            ++layers;
            if ((*it)->text(0) != QStringLiteral("ハイライト") && (*it)->text(0) != QStringLiteral("唇")) continue;
            tree->setCurrentItem(*it);
            const auto details = dialog.findChild<QPlainTextEdit*>("cmoSourceDetails")->toPlainText();
            QVERIFY(details.contains(QStringLiteral("没有活动 ArtMesh")));
            QCOMPARE(details.count(QStringLiteral("保留旧混合类型")), 1); ++unmeshed;
        }
        QCOMPARE(layers, 73); QCOMPARE(unmeshed, 2);
        const auto screenshots = qEnvironmentVariable("LIVE2D_TEST_CMO_UI_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            dialog.setStyleSheet(Theme::globalStyleSheet(false)); dialog.resize(1100, 720); dialog.show();
            tree->expandAll(); tree->scrollToItem(tree->currentItem()); QTRY_VERIFY(dialog.isVisible());
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(dialog.grab().save(QDir(screenshots).filePath("native-unmeshed-layer.png")));
        }
    }
    void themedLayoutsKeepActionsInsideRequestedWindow_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<QSize>("size");
        QTest::newRow("light-wide") << false << QSize(1100, 720);
        QTest::newRow("dark-wide") << true << QSize(1100, 720);
        QTest::newRow("light-narrow") << false << QSize(600, 600);
        QTest::newRow("dark-narrow") << true << QSize(600, 600);
    }
    void themedLayoutsKeepActionsInsideRequestedWindow()
    {
        QFETCH(bool, dark); QFETCH(QSize, size);
        CCmoSourceImportDialog dialog(project()); dialog.setStyleSheet(Theme::globalStyleSheet(dark));
        dialog.resize(size); dialog.show(); QTRY_VERIFY(dialog.isVisible()); QCOMPARE(dialog.size(), size);
        auto* save = dialog.findChild<QPushButton*>("cmoSourceSave");
        QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())));
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments"); tree->setCurrentItem(tree->topLevelItem(1));
        tree->setFocus(); QTest::keyClick(tree, Qt::Key_Space); QCOMPARE(dialog.SelectedDocumentIds(), QStringList{"d0-guid"});
        const auto path = qEnvironmentVariable("LIVE2D_TEST_CMO_UI_SCREENSHOTS");
        if (!path.isEmpty()) {
            QVERIFY(QDir().mkpath(path));
            QVERIFY(dialog.grab().save(QDir(path).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".png")));
        }
    }
    void narrowWindowKeepsTreeDetailsAndActionsReachable()
    {
        CCmoSourceImportDialog dialog(project()); dialog.resize(dialog.fontMetrics().averageCharWidth() * 75, dialog.fontMetrics().height() * 38); dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        auto* split = dialog.findChild<QSplitter*>("cmoSourceSplitter"); QCOMPARE(split->orientation(), Qt::Vertical);
        auto* tree = dialog.findChild<QTreeWidget*>("cmoSourceDocuments"); auto* details = dialog.findChild<QPlainTextEdit*>("cmoSourceDetails");
        QVERIFY(tree->height() >= dialog.fontMetrics().height() * 6); QVERIFY(details->height() >= dialog.fontMetrics().height() * 6);
        auto* save = dialog.findChild<QPushButton*>("cmoSourceSave"); QVERIFY(save->isVisible());
        QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())));
        tree->setFocus(); tree->setCurrentItem(tree->topLevelItem(0)); QTest::keyClick(tree, Qt::Key_Space);
        QCOMPARE(dialog.SelectedDocumentIds(), QStringList{"d1-guid"});
    }
};
QTEST_MAIN(CmoSourceImportDialogTests)
#include "cmo_source_import_dialog_tests.moc"
