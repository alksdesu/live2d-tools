#include "ui/calphameshdialog.h"
#include "ui/theme.h"
#include <QtTest>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleFactory>
#include <QStyle>
#include <QDir>
#include <QToolButton>
#include <QWheelEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QScreen>
#include <QMainWindow>

using Planner = live2dstruct::CAlphaMeshPlanner;
namespace {
CAlphaMeshDialog::Source source(int size = 96)
{
    CAlphaMeshDialog::Source result;
    auto& input = result.input;
    input.artMeshId = "ArtMesh-Hair"; input.sourceToken = "frozen-source";
    input.image = QImage(size, size, QImage::Format_RGBA8888); input.image.fill(Qt::transparent);
    for (int y = 8; y < size - 8; ++y) for (int x = 8; x < size - 8; ++x)
        input.image.setPixelColor(x, y, QColor(210, 76, 118, 220));
    input.pixelCoordinates = {{0, 0}, {double(size), 0}, {double(size), double(size)}, {0, double(size)}};
    input.uvs = {0, 0, 1, 0, 1, 1, 0, 1}; input.indices = {0, 1, 2, 0, 2, 3}; input.referenceSize = {1024, 1024};
    result.label = QStringLiteral("当前素材图集"); result.detail = QStringLiteral("仅处理当前 UV 覆盖范围，重叠素材不能自动分离。");
    return result;
}
QRectF rasterBounds(QWidget* canvas)
{
    const auto image = canvas->grab().toImage(); QRect bounds;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto pixel = image.pixelColor(x, y);
        if (pixel == QColor(234, 49, 82)) bounds |= QRect(x, y, 1, 1);
    }
    const auto dpr = image.devicePixelRatio();
    return QRectF(bounds.x() / dpr, bounds.y() / dpr, bounds.width() / dpr, bounds.height() / dpr);
}
CAlphaMeshDialog::Source solidRasterSource()
{
    auto frozen = source(); auto& input = frozen.input;
    input.image = QImage(64, 32, QImage::Format_RGBA8888); input.image.fill(QColor(234, 49, 82));
    input.pixelCoordinates = {{0, 0}, {64, 0}, {64, 32}, {0, 32}}; return frozen;
}
template<class T> T* control(CAlphaMeshDialog& dialog, const char* name) { return dialog.findChild<T*>(QString::fromLatin1(name)); }
QPushButton* apply(CAlphaMeshDialog& dialog) { return control<QPushButton>(dialog, "alphaMeshApply"); }
void show(CAlphaMeshDialog& dialog) { dialog.show(); QApplication::processEvents(); }
}
class AlphaMeshDialogTests : public QObject
{
    Q_OBJECT
    QString m_nativeStyle;
private slots:
    void initTestCase() { m_nativeStyle = qApp->style()->objectName(); }
    void init()
    {
        qApp->setStyleSheet({}); qApp->setStyle(QStyleFactory::create("Fusion"));
        qApp->setPalette(qApp->style()->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(false));
    }
    void initialPreviewDoesNotSubmitAndOneApplyUsesValidatedFrozenPlan()
    {
        auto frozen = source(); CAlphaMeshDialog dialog(frozen); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto& plan, QString* error) { ++calls; return Planner::Validate(frozen.input, plan, error); });
        QVERIFY(dialog.IsValid()); QVERIFY(!apply(dialog)->isEnabled()); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QCOMPARE(calls, 0); QVERIFY(apply(dialog)->isEnabled());
        QVERIFY(dialog.Result().statistics.newVertices > 4);
        QSignalSpy finished(&dialog, &QDialog::finished); dialog.accept(); QCOMPARE(calls, 1); QCOMPARE(finished.count(), 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
        dialog.accept(); QCOMPARE(calls, 1);
    }
    void presetsChangeDensityPreserveAdvancedOptionsAndLatestRequestWins()
    {
        CAlphaMeshDialog dialog(source()); show(dialog); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        QVERIFY(control<QPushButton>(dialog, "alphaMeshDensity1")->text().startsWith(QStringLiteral("✓ ")));
        QCOMPARE(control<QPushButton>(dialog, "alphaMeshDensity1")->accessibleName(), QStringLiteral("标准"));
        auto* margin = control<QDoubleSpinBox>(dialog, "alphaMeshMargin"); auto* alpha = control<QSpinBox>(dialog, "alphaMeshThreshold");
        margin->setValue(0); alpha->setValue(120);
        control<QPushButton>(dialog, "alphaMeshDensity0")->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); const int coarse = dialog.Result().statistics.newVertices;
        control<QPushButton>(dialog, "alphaMeshDensity2")->click(); control<QPushButton>(dialog, "alphaMeshDensity0")->click(); control<QPushButton>(dialog, "alphaMeshDensity2")->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        QVERIFY(control<QPushButton>(dialog, "alphaMeshDensity2")->text().startsWith(QStringLiteral("✓ ")));
        QCOMPARE(margin->value(), 0.0); QCOMPARE(alpha->value(), 120); QVERIFY(dialog.Result().statistics.newVertices > coarse);
        QCOMPARE(dialog.Result().statistics.boundarySpacingPixels, Planner::Preset(Planner::Density::Fine).boundarySpacing);
        control<QDoubleSpinBox>(dialog, "alphaMeshBoundarySpacing")->setValue(13);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        for (int i = 0; i < 3; ++i) {
            auto* preset = control<QPushButton>(dialog, qPrintable(QString("alphaMeshDensity%1").arg(i)));
            QVERIFY(!preset->isChecked()); QVERIFY(!preset->text().startsWith(QStringLiteral("✓ "))); QCOMPARE(preset->text(), preset->accessibleName());
        }
    }
    void failedCandidateKeepsLastPreviewButCannotSubmitStaleSettings()
    {
        CAlphaMeshDialog dialog(source()); int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; }); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); const auto previous = dialog.Result().pixelCoordinates;
        control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255); QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000);
        QVERIFY(!dialog.IsReady()); QVERIFY(!dialog.Error().isEmpty()); QCOMPARE(dialog.Result().pixelCoordinates, previous); QVERIFY(!apply(dialog)->isEnabled());
        dialog.accept(); QCOMPARE(calls, 0);
        control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(0); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QCOMPARE(dialog.Result().pixelCoordinates, previous);
    }
    void failedSubmissionKeepsDialogAndAllowsRetry()
    {
        CAlphaMeshDialog dialog(source()); int calls = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString* error) { if (++calls == 1) { *error = QStringLiteral("模型已改变，请重新检查"); return false; } return true; });
        show(dialog); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.accept(); QCOMPARE(calls, 1); QCOMPARE(finished.count(), 0); QVERIFY(dialog.isVisible()); QVERIFY(dialog.IsReady()); QVERIFY(dialog.Error().contains(QStringLiteral("模型已改变")));
        dialog.accept(); QCOMPARE(calls, 2); QCOMPARE(finished.count(), 1);
    }
    void invalidTextCannotSubmitPreviousNumericValue()
    {
        CAlphaMeshDialog dialog(source()); int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        show(dialog); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        auto* spin = control<QDoubleSpinBox>(dialog, "alphaMeshBoundarySpacing"); auto* edit = spin->findChild<QLineEdit*>(); QVERIFY(edit);
        edit->setText(QStringLiteral("-")); QVERIFY(!spin->hasAcceptableInput()); dialog.accept(); QCOMPARE(calls, 0); QVERIFY(!dialog.IsReady());
        spin->setValue(12); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        edit->setText(QStringLiteral("18")); QVERIFY(spin->hasAcceptableInput()); QVERIFY(!dialog.IsReady()); dialog.accept(); QCOMPARE(calls, 0);
        spin->interpretText(); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QCOMPARE(dialog.Result().statistics.boundarySpacingPixels, 18.0);
    }
    void rejectBeforeStartAndDuringCalculationNeverSubmits()
    {
        int calls = 0;
        { CAlphaMeshDialog dialog(source()); dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; }); dialog.reject(); QApplication::processEvents(); QVERIFY(!dialog.IsReady()); }
        { CAlphaMeshDialog dialog(source(512)); dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; }); show(dialog); QVERIFY(dialog.IsBusy()); dialog.reject(); QVERIFY(!dialog.isVisible()); }
        QCOMPARE(calls, 0);
    }
    void parentDestructionCancelsWorkerAndSubmissionMayDestroyOwner()
    {
        auto* owner = new QWidget; auto* dialog = new CAlphaMeshDialog(source(512), owner); QPointer<CAlphaMeshDialog> guarded(dialog); show(*dialog); delete owner; QVERIFY(!guarded);
        owner = new QWidget; dialog = new CAlphaMeshDialog(source(), owner); guarded = dialog;
        dialog->SetSubmissionHandler([&](const auto&, QString*) { delete owner; owner = nullptr; return true; }); show(*dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->IsReady(), 10000); dialog->accept(); QVERIFY(!guarded);
    }
    void emptyAndLiteralSourceAreSafe()
    {
        auto frozen = source(); frozen.label = QStringLiteral("<b>来源 & PSD</b>"); CAlphaMeshDialog dialog(frozen);
        auto* label = control<QLabel>(dialog, "alphaMeshSource"); QCOMPARE(label->textFormat(), Qt::PlainText); QVERIFY(label->text().contains(frozen.label));
        frozen.input.image = {}; CAlphaMeshDialog invalid(frozen); QVERIFY(!invalid.IsValid()); QVERIFY(!invalid.IsBusy()); QVERIFY(!apply(invalid)->isEnabled()); QVERIFY(!invalid.Error().isEmpty());
    }
    void invalidPreviewGeometryNeverBecomesReadyOrSubmits()
    {
        auto frozen = source(); frozen.input.indices = {0, 1, 65535}; CAlphaMeshDialog dialog(frozen); int submissions = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString*) { ++submissions; return true; }); QSignalSpy ready(&dialog, &CAlphaMeshDialog::Ready); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000); QVERIFY(!dialog.IsReady()); QVERIFY(!dialog.Error().isEmpty()); QCOMPARE(ready.count(), 0);
        QVERIFY(dialog.Result().pixelCoordinates.isEmpty()); QVERIFY(!apply(dialog)->isEnabled()); dialog.accept(); QCOMPARE(submissions, 0);
    }
    void inFlightResultCannotReplaceNewerFailureOrEmitReadyAfterClose()
    {
        CAlphaMeshDialog dialog(source(512)); QSignalSpy ready(&dialog, &CAlphaMeshDialog::Ready); show(dialog); QVERIFY(dialog.IsBusy());
        control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000); QVERIFY(!dialog.IsReady()); QCOMPARE(ready.count(), 0); QVERIFY(!dialog.Error().isEmpty());
        control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(0); QApplication::processEvents(); QVERIFY(dialog.IsBusy()); dialog.reject();
        QTest::qWait(80); QCOMPARE(ready.count(), 0); QVERIFY(!dialog.IsReady());
    }
    void submissionCannotCloseItselfUntilCallbackFinishesAndReadyMayDestroyOwner()
    {
        CAlphaMeshDialog dialog(source()); bool guarded = false;
        dialog.SetSubmissionHandler([&](const auto&, QString* error) {
            guarded = dialog.IsSubmitting(); dialog.reject(); dialog.done(QDialog::Rejected); dialog.accept();
            *error = QStringLiteral("可重试的发布失败"); return false;
        });
        show(dialog); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.accept(); QVERIFY(guarded); QVERIFY(!dialog.IsSubmitting()); QVERIFY(dialog.isVisible()); QCOMPARE(finished.count(), 0);
        dialog.reject(); QCOMPARE(finished.count(), 1);
        auto* owner = new QWidget; auto* owned = new CAlphaMeshDialog(source(), owner); QPointer<CAlphaMeshDialog> pointer(owned);
        connect(owned, &CAlphaMeshDialog::Ready, this, [&] { delete owner; owner = nullptr; }); owned->show();
        QTRY_VERIFY_WITH_TIMEOUT(!pointer, 10000);
    }
    void sourcePixelsFitRepeatAndZoomKeepTheirCoordinateMeaning_data()
    {
        QTest::addColumn<double>("rasterDpr");
        QTest::newRow("raster-dpr-1") << 1.0; QTest::newRow("raster-dpr-2") << 2.0;
    }
    void sourcePixelsFitRepeatAndZoomKeepTheirCoordinateMeaning()
    {
        QFETCH(double, rasterDpr); auto frozen = source(); auto& input = frozen.input;
        input.image = QImage(16, 8, QImage::Format_RGBA8888); input.image.setDevicePixelRatio(rasterDpr);
        const QColor first(234, 49, 82), second(40, 192, 124);
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 16; ++x) input.image.setPixelColor(x, y, x < 8 ? first : second);
        input.pixelCoordinates = {{-16, -8}, {16, -8}, {16, 8}, {-16, 8}}; input.uvs = {-1, -1, 1, -1, 1, 1, -1, 1}; input.repeat = true;
        CAlphaMeshDialog dialog(frozen); control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255); dialog.resize(1000, 680); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000); QVERIFY(!dialog.IsReady()); QApplication::processEvents();
        auto* canvas = control<QWidget>(dialog, "alphaMeshCanvas"); QVERIFY(canvas);
        const int margin = canvas->fontMetrics().height();
        const double scale = std::min((canvas->width() - 2.0 * margin) / 32, (canvas->height() - 2.0 * margin) / 16);
        const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
        const auto sample = [&](QPointF viewport) { auto image = canvas->grab().toImage(); const auto dpr = image.devicePixelRatio(); return image.pixelColor(qRound(viewport.x() * dpr), qRound(viewport.y() * dpr)); };
        for (const auto point : {QPointF(-12.5, -3.5), QPointF(3.5, 4.5)}) QCOMPARE(sample(center + point * scale), first);
        for (const auto point : {QPointF(-3.5, -3.5), QPointF(12.5, 4.5)}) QCOMPARE(sample(center + point * scale), second);
        const auto anchor = center + QPointF(-12.5, -3.5) * scale; const auto before = sample(anchor);
        QWheelEvent wheel(anchor, canvas->mapToGlobal(anchor.toPoint()), {}, {0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel); QCOMPARE(sample(anchor), before);
        const auto afterZoom = canvas->grab().toImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, center.toPoint()); QTest::mouseMove(canvas, center.toPoint() + QPoint(13, 7)); QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, center.toPoint() + QPoint(13, 7));
        QCOMPARE(sample(anchor + QPointF(13, 7)), before); QVERIFY(canvas->grab().toImage() != afterZoom);
    }
    void transparentRasterAndOriginalUvClipRemainVisibleOnFailure()
    {
        auto frozen = source(); auto& input = frozen.input; input.image = QImage(16, 8, QImage::Format_RGBA8888); input.image.fill(QColor(230, 61, 90, 128));
        input.pixelCoordinates = {{0, 0}, {16, 0}, {0, 8}}; input.indices = {0, 1, 2}; input.uvs = {0, 0, 1, 0, 0, 1};
        CAlphaMeshDialog dialog(frozen); control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255); dialog.resize(1000, 680); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000); QApplication::processEvents(); auto* canvas = control<QWidget>(dialog, "alphaMeshCanvas");
        const double scale = std::min((canvas->width() - 2.0 * canvas->fontMetrics().height()) / 16, (canvas->height() - 2.0 * canvas->fontMetrics().height()) / 8);
        const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0); auto image = canvas->grab().toImage(); const double dpr = image.devicePixelRatio();
        const auto at = [&](QPointF p) { const auto position = center + (p - QPointF(8, 4)) * scale; return image.pixelColor(qRound(position.x() * dpr), qRound(position.y() * dpr)); };
        const auto tokens = Theme::tokens(false); const QColor base(tokens.value("@surface@")), alternate(tokens.value("@surfaceHover@"));
        QVERIFY(at({14, 6}) == base || at({14, 6}) == alternate);
        const auto inside = at({2, 2}); QVERIFY(inside != base && inside != alternate); QVERIFY(inside.red() > inside.green()); QVERIFY(inside.green() > 61); QCOMPARE(inside.alpha(), 255);
    }
    void stopThenRetryAndExternalDoneKeepSubmissionExplicit()
    {
        CAlphaMeshDialog dialog(source()); int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; }); show(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); const auto previous = dialog.Result().pixelCoordinates;
        control<QDoubleSpinBox>(dialog, "alphaMeshInteriorSpacing")->setValue(15); control<QPushButton>(dialog, "alphaMeshStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000); QVERIFY(!dialog.IsReady()); QCOMPARE(dialog.Result().pixelCoordinates, previous); QVERIFY(!apply(dialog)->isEnabled());
        control<QPushButton>(dialog, "alphaMeshGenerate")->click(); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        dialog.done(QDialog::Rejected); QCOMPARE(calls, 0); QVERIFY(!dialog.IsReady()); QVERIFY(!dialog.isVisible());
    }
    void narrowFailureHasVisiblePlainTextStatusAndCompleteCopyableDetail()
    {
        CAlphaMeshDialog dialog(source()); const QString error = QStringLiteral("<b>模型已改变 & 未提交</b> ") + QString(300, QChar(0x7f51));
        dialog.SetSubmissionHandler([&](const auto&, QString* output) { *output = error; return false; }); dialog.resize(560, 640); show(dialog);
        control<QToolButton>(dialog, "alphaMeshAdvanced")->click(); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); dialog.accept(); QApplication::processEvents();
        auto* status = control<QLabel>(dialog, "alphaMeshStatus"); QCOMPARE(status->textFormat(), Qt::PlainText);
        QVERIFY(status->text().startsWith(QStringLiteral("未能完成操作"))); QVERIFY(status->text().endsWith(QChar(0x2026))); QVERIFY(status->isVisibleTo(&dialog));
        QVERIFY(status->accessibleDescription().contains(error)); QVERIFY(control<QPlainTextEdit>(dialog, "alphaMeshFeedback")->toPlainText().contains(error));
        QVERIFY(dialog.width() <= 560); QVERIFY(dialog.height() <= 640); QVERIFY(status->height() <= status->fontMetrics().height() * 2);
        QVERIFY(dialog.IsReady()); QVERIFY(apply(dialog)->isEnabled());
        const QString output = qEnvironmentVariable("ALPHA_MESH_SCREENSHOTS");
        if (!output.isEmpty()) { QDir().mkpath(output); QVERIFY(dialog.grab().save(output + "/alpha-failed-560.png")); }
    }
    void untouchedPreviewFollowsResizeWithoutManualFit()
    {
        CAlphaMeshDialog dialog(solidRasterSource()); control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255);
        dialog.resize(1000, 760); show(dialog); QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000);
        auto* canvas = control<QWidget>(dialog, "alphaMeshCanvas");
        const auto fits = [&] {
            const double scale = std::min((canvas->width() - 2.0 * canvas->fontMetrics().height()) / 64,
                                          (canvas->height() - 2.0 * canvas->fontMetrics().height()) / 32);
            const auto bounds = rasterBounds(canvas); const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
            return QLineF(bounds.center(), center).length() <= 1 && std::abs(bounds.width() - 64 * scale) <= 1 && std::abs(bounds.height() - 32 * scale) <= 1;
        };
        QTRY_VERIFY_WITH_TIMEOUT(fits(), 2000);
        dialog.resize(560, 640); QTRY_VERIFY_WITH_TIMEOUT(fits(), 2000);
        dialog.resize(1000, 760); QTRY_VERIFY_WITH_TIMEOUT(fits(), 2000);
    }
    void manualViewAndActivePanKeepScaleAndCenterDuringResize()
    {
        CAlphaMeshDialog dialog(solidRasterSource()); control<QSpinBox>(dialog, "alphaMeshThreshold")->setValue(255);
        dialog.resize(560, 640); show(dialog); QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 10000);
        auto* canvas = control<QWidget>(dialog, "alphaMeshCanvas");
        QTRY_VERIFY_WITH_TIMEOUT(QLineF(rasterBounds(canvas).center(), QPointF(canvas->width() / 2.0, canvas->height() / 2.0)).length() <= 1, 2000);
        const QPointF anchor(canvas->width() / 2.0, canvas->height() / 2.0);
        QWheelEvent wheel(anchor, canvas->mapToGlobal(anchor), {}, {0, -120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false); QApplication::sendEvent(canvas, &wheel);
        const auto before = rasterBounds(canvas); QVERIFY(before.width() > 30); const auto oldSize = canvas->size();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, anchor.toPoint());
        const QPointF pointer = canvas->mapToGlobal(anchor);
        dialog.resize(1000, 760); QApplication::processEvents();
        const auto expectedCenter = before.center() + QPointF((canvas->width() - oldSize.width()) / 2.0, (canvas->height() - oldSize.height()) / 2.0);
        const auto resized = rasterBounds(canvas);
        QVERIFY(std::abs(resized.width() - before.width()) <= 1); QVERIFY(QLineF(resized.center(), expectedCenter).length() <= 1);
        QTest::mouseMove(canvas, canvas->mapFromGlobal(pointer).toPoint());
        const auto stationary = rasterBounds(canvas); QVERIFY(QLineF(stationary.center(), expectedCenter).length() <= 1);
        QTest::mouseMove(canvas, canvas->mapFromGlobal(pointer).toPoint() + QPoint(7, 5));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, canvas->mapFromGlobal(pointer).toPoint() + QPoint(7, 5));
        QVERIFY(QLineF(rasterBounds(canvas).center(), expectedCenter + QPointF(7, 5)).length() <= 1);
    }
    void nativeStyleWideToCompactKeepsAllPreviewToolsReachable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<bool>("longSource");
        for (bool dark : {false, true}) for (bool longSource : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(longSource ? "long" : "short"))) << dark << longSource;
    }
    void nativeStyleWideToCompactKeepsAllPreviewToolsReachable()
    {
        QFETCH(bool, dark); QFETCH(bool, longSource); qApp->setStyleSheet({}); auto* style = QStyleFactory::create(m_nativeStyle); QVERIFY(style);
        qApp->setStyle(style); qApp->setPalette(style->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        auto frozen = source(); frozen.input.artMeshId = "BrowR";
        if (longSource) {
            frozen.label = QStringLiteral("当前图集采样：texture_00.png");
            frozen.detail = QStringLiteral("未使用独立来源图层；只处理现有 UV 三角覆盖内的图集透明度，不能分离覆盖范围内混入的其它图案。超界 UV 按重复纹理处理。");
        }
        CAlphaMeshDialog dialog(frozen); dialog.SetSubmissionHandler([](const auto&, QString*) { return false; }); dialog.resize(1000, 760); show(dialog); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000);
        const auto candidate = dialog.Result().pixelCoordinates;
        const auto verifyControls = [&] {
            for (const auto* name : {"alphaMeshFit", "alphaMeshShowOld", "alphaMeshShowContours", "alphaMeshApply"}) {
                auto* widget = control<QWidget>(dialog, name); QVERIFY(widget); QVERIFY(widget->isVisibleTo(&dialog));
                const QRect visible(widget->mapTo(&dialog, QPoint()), widget->size()); QVERIFY2(dialog.rect().contains(visible), name);
                const QRect withinParent(widget->mapTo(widget->parentWidget(), QPoint()), widget->size()); QVERIFY2(widget->parentWidget()->rect().contains(withinParent), name);
            }
            QCOMPARE(control<QScrollArea>(dialog, "alphaMeshControlsScroll")->horizontalScrollBar()->maximum(), 0);
        };
        const auto verifySize = [&](int width, int height) {
            QCOMPARE(dialog.width(), width);
            if (dialog.screen()->availableGeometry().height() >= height + dialog.style()->pixelMetric(QStyle::PM_TitleBarHeight)) QCOMPARE(dialog.height(), height);
            else { QVERIFY(dialog.height() <= height); QVERIFY(dialog.height() >= dialog.minimumHeight()); }
        };
        dialog.resize(560, 640); QTest::qWait(30); verifySize(560, 640); verifyControls();
        control<QToolButton>(dialog, "alphaMeshAdvanced")->click(); QTest::qWait(30); verifySize(560, 640); verifyControls();
        const QString output = qEnvironmentVariable("ALPHA_MESH_SCREENSHOTS");
        if (!output.isEmpty()) {
            QDir().mkpath(output); const auto path = output + QString("/native-%1-%2-560").arg(dark ? "dark" : "light").arg(longSource ? "long" : "short");
            QVERIFY(dialog.grab().save(path + ".png")); QFile record(path + ".json"); QVERIFY(record.open(QIODevice::WriteOnly));
            const QJsonObject geometry{{"style", m_nativeStyle}, {"requestedWidth", 560}, {"requestedHeight", 640}, {"actualWidth", dialog.width()}, {"actualHeight", dialog.height()},
                {"minimumWidth", dialog.minimumWidth()}, {"minimumHeight", dialog.minimumHeight()}, {"screenAvailableHeight", dialog.screen()->availableGeometry().height()}, {"dpr", dialog.devicePixelRatioF()}};
            QVERIFY(record.write(QJsonDocument(geometry).toJson()) > 0);
        }
        dialog.resize(1000, 760); QTest::qWait(30); verifySize(1000, 760); verifyControls();
        QVERIFY(dialog.IsReady()); QCOMPARE(dialog.Result().pixelCoordinates, candidate);
    }
    void asynchronousApplicationModalPreviewUnderLargeFramelessParentRemainsVisible()
    {
        qApp->setStyleSheet({}); auto* style = QStyleFactory::create(m_nativeStyle); QVERIFY(style); qApp->setStyle(style);
        qApp->setPalette(style->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(false));
        auto parent = std::make_unique<QMainWindow>(); parent->setWindowFlag(Qt::FramelessWindowHint); parent->resize(1380, 980); parent->show(); QTest::qWait(20);
        for (int cycle = 0; cycle < 2; ++cycle) {
            auto* dialog = new CAlphaMeshDialog(solidRasterSource(), parent.get()); QPointer<CAlphaMeshDialog> guarded(dialog);
            control<QSpinBox>(*dialog, "alphaMeshThreshold")->setValue(255); QSignalSpy finished(dialog, &QDialog::finished); dialog->show();
            QTRY_VERIFY_WITH_TIMEOUT(!dialog->IsBusy(), 10000);
            QCOMPARE(dialog->windowModality(), Qt::ApplicationModal); QVERIFY(dialog->windowType() != Qt::Sheet);
            auto* canvas = control<QWidget>(*dialog, "alphaMeshCanvas");
            const auto rasterFits = [&] {
                const auto bounds = rasterBounds(canvas); const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
                return bounds.width() > 20 && bounds.height() > 20 && QLineF(bounds.center(), center).length() <= 1;
            };
            for (const auto size : {QSize(1000, 760), QSize(560, 640)}) {
                dialog->resize(size); QTRY_VERIFY_WITH_TIMEOUT(rasterFits(), 2000);
                QCOMPARE(dialog->width(), size.width()); QVERIFY(dialog->height() <= size.height());
                QVERIFY(canvas->height() > canvas->fontMetrics().height() * 2);
                QCOMPARE(control<QScrollArea>(*dialog, "alphaMeshControlsScroll")->horizontalScrollBar()->maximum(), 0);
                for (const auto* name : {"alphaMeshFit", "alphaMeshShowOld", "alphaMeshShowContours", "alphaMeshApply"}) {
                    auto* widget = control<QWidget>(*dialog, name); QVERIFY(widget->isVisibleTo(dialog));
                    QVERIFY(dialog->rect().contains(QRect(widget->mapTo(dialog, QPoint()), widget->size())));
                }
            }
            const QString output = qEnvironmentVariable("ALPHA_MESH_SCREENSHOTS");
            if (!output.isEmpty()) {
                QDir().mkpath(output); const auto path = output + QString("/parent-application-modal-%1").arg(cycle);
                QVERIFY(dialog->grab().save(path + ".png")); QFile record(path + ".json"); QVERIFY(record.open(QIODevice::WriteOnly));
                const QJsonObject geometry{{"style", m_nativeStyle}, {"parentWidth", parent->width()}, {"parentHeight", parent->height()},
                    {"actualWidth", dialog->width()}, {"actualHeight", dialog->height()}, {"canvasWidth", canvas->width()}, {"canvasHeight", canvas->height()},
                    {"windowType", int(dialog->windowType())}, {"windowModality", int(dialog->windowModality())},
                    {"topContentsMargin", dialog->contentsMargins().top()}, {"dpr", dialog->devicePixelRatioF()}};
                QVERIFY(record.write(QJsonDocument(geometry).toJson()) > 0);
            }
            dialog->reject(); QCOMPARE(finished.count(), 1); dialog->deleteLater(); QTRY_VERIFY(!guarded); QVERIFY(parent->isVisible());
        }
        auto* pending = new CAlphaMeshDialog(source(512), parent.get()); QPointer<CAlphaMeshDialog> guarded(pending); pending->show(); QApplication::processEvents();
        parent.reset(); QVERIFY(!guarded);
    }
    void themeAndNarrowLayout_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {1000, 640, 560}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void themeAndNarrowLayout()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); CAlphaMeshDialog dialog(source());
        dialog.SetSubmissionHandler([](const auto&, QString*) { return false; }); const int height = width == 560 ? 640 : 680; dialog.resize(width, height); show(dialog);
        control<QToolButton>(dialog, "alphaMeshAdvanced")->click(); QTRY_VERIFY_WITH_TIMEOUT(dialog.IsReady(), 10000); QApplication::processEvents();
        auto* scroll = control<QScrollArea>(dialog, "alphaMeshControlsScroll"); QVERIFY(scroll); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(apply(dialog)->isVisibleTo(&dialog)); QVERIFY(control<QWidget>(dialog, "alphaMeshCanvas")->height() >= 100);
        QVERIFY(dialog.width() <= width); QVERIFY(dialog.height() <= height);
        const QString output = qEnvironmentVariable("ALPHA_MESH_SCREENSHOTS");
        if (!output.isEmpty()) {
            QDir().mkpath(output); const QString path = output + QString("/alpha-%1-%2").arg(dark ? "dark" : "light").arg(width);
            const auto screenshot = dialog.grab(); QVERIFY(screenshot.save(path + ".png"));
            const QJsonObject geometry{{"requestedWidth", width}, {"requestedHeight", height}, {"actualWidth", dialog.width()}, {"actualHeight", dialog.height()},
                {"dpr", screenshot.devicePixelRatio()}, {"pixelWidth", screenshot.width()}, {"pixelHeight", screenshot.height()},
                {"screenAvailableHeight", dialog.screen()->availableGeometry().height()}, {"horizontalScrollMaximum", scroll->horizontalScrollBar()->maximum()},
                {"verticalScrollMaximum", scroll->verticalScrollBar()->maximum()}, {"applyVisible", apply(dialog)->isVisibleTo(&dialog)}};
            QFile record(path + ".json"); QVERIFY(record.open(QIODevice::WriteOnly)); QVERIFY(record.write(QJsonDocument(geometry).toJson()) > 0);
        }
    }
};
QTEST_MAIN(AlphaMeshDialogTests)
#include "alpha_mesh_dialog_tests.moc"
