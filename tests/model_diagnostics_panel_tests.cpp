#include "ui/cmodeldiagnosticspanel.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QtEndian>

using namespace live2dstruct;
namespace {
QByteArray source(const QVector<int>& masks, const QString& id = {})
{
    QFile file(QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3");
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto bytes = file.readAll();
    const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
    const int pool = (bytes.size() + 63) & ~63;
    bytes.resize(pool + masks.size() * 4);
    for (int index = 0; index < masks.size(); ++index) qToLittleEndian<qint32>(masks[index], bytes.data() + pool + index * 4);
    qToLittleEndian<qint32>(pool, bytes.data() + 0x180);
    qToLittleEndian<qint32>(masks.size(), bytes.data() + word(0x40) + 17 * 4);
    qToLittleEndian<qint32>(0, bytes.data() + word(0xfc));
    qToLittleEndian<qint32>(masks.size(), bytes.data() + word(0x100));
    if (!id.isEmpty()) {
        const auto encoded = id.toUtf8();
        if (encoded.size() >= 64) return {};
        const int at = word(0xc4);
        bytes.replace(at, 64, encoded + QByteArray(64 - encoded.size(), '\0'));
    }
    bytes.append(QByteArray(((bytes.size() + 63) & ~qsizetype(63)) - bytes.size(), '\0'));
    return bytes;
}
QListWidget* list(CModelDiagnosticsPanel& panel) { return panel.findChild<QListWidget*>("diagnosticsList"); }
QPlainTextEdit* details(CModelDiagnosticsPanel& panel) { return panel.findChild<QPlainTextEdit*>("diagnosticsDetails"); }
QPushButton* locate(CModelDiagnosticsPanel& panel) { return panel.findChild<QPushButton*>("diagnosticsLocate"); }
int row(CModelDiagnosticsPanel& panel, const QString& code)
{
    const auto diagnostics = panel.ReportJson()["diagnostics"].toArray();
    for (int index = 0; index < diagnostics.size(); ++index) if (diagnostics[index].toObject()["code"] == code) return index;
    return -1;
}
}
class ModelDiagnosticsPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void warningHasStableIdentityAndPlainCompleteDetails()
    {
        CModelDocument document;
        const QString id = QStringLiteral("<b>眼睛 & 高光 😀</b>");
        QString error;
        QVERIFY2(document.LoadBytes(source({1, 2, 1}, id), "memory.moc3", &error), qPrintable(error));
        CModelDiagnosticsPanel panel; panel.SetDocument(&document);
        const int at = row(panel, "mask_source_duplicate"); QVERIFY(at >= 0);
        list(panel)->setCurrentRow(at);
        QVERIFY(details(panel)->isReadOnly());
        QVERIFY(details(panel)->toPlainText().contains(id));
        QVERIFY(details(panel)->toPlainText().contains(document.Validate()[at].message));
        QVERIFY(list(panel)->item(at)->text().startsWith(QStringLiteral("警告")));
        QVERIFY(list(panel)->item(at)->text().contains(id));
        QVERIFY(locate(panel)->isEnabled());
        QSignalSpy requested(&panel, &CModelDiagnosticsPanel::ObjectRequested);
        locate(panel)->click(); QCOMPARE(requested.size(), 1);
        QCOMPARE(requested[0][0].toString(), id); QCOMPARE(requested[0][1].toString(), QStringLiteral("artmesh"));
        details(panel)->selectAll();
        panel.Refresh();
        QCOMPARE(details(panel)->textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n'), details(panel)->toPlainText());
    }
    void reportVersionAndFrozenIdentityAreUnambiguous()
    {
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(source({1, 1, 99}), "<source&>.moc3", &error), qPrintable(error));
        CModelDiagnosticsPanel panel; panel.SetDocument(&document);
        const auto report = panel.ReportJson();
        QCOMPARE(report["format"].toString(), QStringLiteral("live2d2psd-diagnostics-v2"));
        QCOMPARE(report["sourceMoc"].toString(), document.SourcePath());
        QCOMPARE(report["sourceDigest"].toString(), QString::fromLatin1(document.SourceDigest().toHex()));
        QCOMPARE(report["documentRevision"].toString(), QString::number(document.Revision()));
        bool warning = false, failure = false;
        for (const auto value : report["diagnostics"].toArray()) {
            const auto entry = value.toObject();
            if (entry["code"] == "mask_source_duplicate") warning = entry["severity"] == "warning";
            if (entry["code"] == "mask_range") failure = entry["severity"] == "error";
            if (entry["code"].toString().startsWith("mask_")) {
                QCOMPARE(entry["objectType"].toString(), QStringLiteral("artmesh"));
                QCOMPARE(entry["objectId"].toString(), document.Drawables().front().id);
                QCOMPARE(entry["index"].toInt(), 0);
            }
        }
        QVERIFY(warning); QVERIFY(failure);
        QCOMPARE(QJsonDocument::fromJson(QJsonDocument(report).toJson()).object(), report);
    }
    void refreshKeepsSelectedObjectAndNeverLocatesStaleRows()
    {
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(source({1, 1, 99}), "memory.moc3", &error), qPrintable(error));
        CModelDiagnosticsPanel panel; panel.SetDocument(&document);
        list(panel)->setCurrentRow(row(panel, "mask_range"));
        panel.Refresh(); QVERIFY(details(panel)->toPlainText().contains("mask_range"));
        QSignalSpy requested(&panel, &CModelDiagnosticsPanel::ObjectRequested);
        const QString id = document.Drawables().front().id;
        QVERIFY(document.SetDrawableMaskIndices(id, {}));
        locate(panel)->click(); QCOMPARE(requested.size(), 0);
        QVERIFY(!locate(panel)->isEnabled()); QVERIFY(details(panel)->toPlainText().isEmpty());
        panel.SetDocument(nullptr); QVERIFY(panel.ReportJson().isEmpty()); QCOMPARE(list(panel)->count(), 0);
    }
    void locationRequiresIdentityUniqueWithinItsDirectory_data()
    {
        QTest::addColumn<int>("collision");
        QTest::newRow("same-artmesh-id") << 0;
        QTest::newRow("part-may-share-artmesh-id") << 1;
        QTest::newRow("parameter-may-share-artmesh-id") << 2;
        QTest::newRow("part-and-parameter-may-share-artmesh-id") << 3;
    }
    void locationRequiresIdentityUniqueWithinItsDirectory()
    {
        QFETCH(int, collision);
        auto bytes = source({99});
        CModelDocument baseline; QVERIFY(baseline.LoadBytes(bytes, "memory.moc3"));
        QVERIFY(baseline.Drawables().size() > 1 && !baseline.Parts().isEmpty() && !baseline.Parameters().isEmpty());
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        const int meshIds = word(0xc4); const auto name = bytes.mid(meshIds, 64);
        if (collision == 0) bytes.replace(meshIds + 64, 64, name);
        if (collision == 1 || collision == 3) bytes.replace(word(0x4c), 64, name);
        if (collision == 2 || collision == 3) bytes.replace(word(0x108), 64, name);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(bytes, "memory.moc3", &error), qPrintable(error));
        CModelDiagnosticsPanel panel; panel.SetDocument(&document);
        const int at = row(panel, "mask_range"); QVERIFY(at >= 0); list(panel)->setCurrentRow(at);
        QSignalSpy requested(&panel, &CModelDiagnosticsPanel::ObjectRequested);
        QCOMPARE(locate(panel)->isEnabled(), collision != 0);
        locate(panel)->click();
        QCOMPARE(requested.size(), collision == 0 ? 0 : 1);
        list(panel)->itemActivated(list(panel)->currentItem());
        QCOMPARE(requested.size(), collision == 0 ? 0 : 2);
        if (collision) {
            QCOMPARE(requested.last()[0].toString(), document.Drawables().front().id);
            QCOMPARE(requested.last()[1].toString(), QStringLiteral("artmesh"));
        }
    }

    void locateReceiverMayDestroyThePanel()
    {
        CModelDocument document; QVERIFY(document.LoadBytes(source({1, 1}), "memory.moc3"));
        QPointer<CModelDiagnosticsPanel> panel = new CModelDiagnosticsPanel;
        panel->SetDocument(&document);
        connect(panel, &CModelDiagnosticsPanel::ObjectRequested, panel, [panel] { delete panel.data(); });
        locate(*panel)->click(); QVERIFY(!panel);
    }
    void themesAndNarrowPanelsKeepDetailsReadable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {360, 720})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void themesAndNarrowPanelsKeepDetailsReadable()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        const auto stylesheet = qApp->styleSheet(); const auto restore = qScopeGuard([&] { qApp->setStyleSheet(stylesheet); });
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CModelDocument document; QVERIFY(document.LoadBytes(source({1, 1, 99}, QStringLiteral("<眼睛与高光-可复制>")), "memory.moc3"));
        CModelDiagnosticsPanel panel; panel.SetDocument(&document); panel.resize(width, 460); panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel)); QTest::qWait(30);
        QCOMPARE(panel.width(), width);
        QVERIFY(details(panel)->viewport()->width() > 0);
        QCOMPARE(details(panel)->horizontalScrollBar()->maximum(), 0);
        for (const auto* name : {"diagnosticsLocate", "diagnosticsRefresh", "diagnosticsExport"}) {
            const auto* button = panel.findChild<QPushButton*>(QLatin1String(name)); QVERIFY(button);
            QVERIFY(panel.rect().contains(QRect(button->mapTo(&panel, QPoint()), button->size())));
        }
        auto* summary = panel.findChild<QLabel*>("diagnosticsSummary"); QVERIFY(summary);
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        const auto directory = qEnvironmentVariable("LIVE2D_DIAGNOSTICS_SCREENSHOTS");
        if (!directory.isEmpty()) { QDir().mkpath(directory); QVERIFY(panel.grab().save(directory + QString("/diagnostics-%1-%2.png").arg(dark ? "dark" : "light").arg(width))); }
        const auto foreground = list(panel)->item(row(panel, "mask_source_duplicate"))->foreground().color();
        QCOMPARE(foreground, QColor(Theme::tokens(dark).value("@text@")));
        qApp->setStyleSheet(Theme::globalStyleSheet(!dark)); QCoreApplication::processEvents();
        QCOMPARE(list(panel)->item(row(panel, "mask_source_duplicate"))->foreground().color(), QColor(Theme::tokens(!dark).value("@text@")));
    }
};
QTEST_MAIN(ModelDiagnosticsPanelTests)
#include "model_diagnostics_panel_tests.moc"
