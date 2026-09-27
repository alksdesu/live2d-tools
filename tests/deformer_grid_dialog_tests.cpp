#include "ui/cdeformergriddialog.h"
#include "ui/theme.h"
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QStyle>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSpinBox>
#include <QTabWidget>
#include <limits>
#include <cmath>
#include <stdexcept>

using Dialog = CDeformerGridDialog;
namespace {
Dialog::Context context()
{
    return {"WarpId", QStringLiteral("头部转换网格 🌙"), 1, 2, 3, 2, 1, true};
}
QVector<QPointF> grid(int rows, int columns, QPointF offset = {})
{
    QVector<QPointF> result;
    for (int row = 0; row <= rows; ++row) for (int column = 0; column <= columns; ++column)
        result.append(QPointF(-2 + 4.0 * column / columns, -1 + 2.0 * row / rows) + offset);
    return result;
}
Dialog::Prepare prepare(const Dialog::Context& source)
{
    return [source](int rows, int columns, int keyform, Dialog::Preview& result, QString*) {
        if (keyform >= 0) { result.beforeGrid = grid(source.rows, source.columns); result.afterGrid = grid(rows, columns, {.15, .1}); }
        result.beforeContours = {{QPointF(1000000, -7000000), QPointF(1000060, -7000000)},
            {QPointF(1000060, -7000000), QPointF(1000030, -6999960)}, {QPointF(1000030, -6999960), QPointF(1000000, -7000000)}};
        result.afterContours = result.beforeContours;
        for (auto& line : result.afterContours) line.translate({2, 1});
        result.maximumDisplacementPixels = 2.2360679;
        result.summary = QStringLiteral("候选已完成：保留全部普通与融合关键形。");
        result.limitations = QStringLiteral("这里只比较当前冻结参数姿态，不代表全部姿态。<限制> & 数据");
        return true;
    };
}
QSpinBox* rows(Dialog& dialog) { return dialog.findChild<QSpinBox*>("deformerGridRows"); }
QSpinBox* columns(Dialog& dialog) { return dialog.findChild<QSpinBox*>("deformerGridColumns"); }
QPushButton* apply(Dialog& dialog) { return dialog.findChild<QPushButton*>("primaryBtn"); }
QPushButton* update(Dialog& dialog) { return dialog.findChild<QPushButton*>("deformerGridUpdate"); }
void change(Dialog& dialog) { rows(dialog)->setValue(3); columns(dialog)->setValue(4); }
QRect ink(const QImage& source, QColor background)
{
    const auto image = source.convertToFormat(QImage::Format_RGB32);
    int left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int difference = std::max({std::abs(qRed(line[x]) - background.red()), std::abs(qGreen(line[x]) - background.green()), std::abs(qBlue(line[x]) - background.blue())});
            if (difference < 40) continue;
            left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y);
        }
    }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
int contrastingPixels(const QImage& source, QColor target, QColor background)
{
    const auto luminance = [](QColor color) {
        const auto channel = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
        return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) + .0722 * channel(color.blueF());
    };
    const auto image = source.convertToFormat(QImage::Format_RGB32); const double base = luminance(background); int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (std::max({std::abs(qRed(line[x]) - target.red()), std::abs(qGreen(line[x]) - target.green()), std::abs(qBlue(line[x]) - target.blue())}) > 16) continue;
            const double value = luminance(QColor::fromRgb(line[x]));
            if ((std::max(base, value) + .05) / (std::min(base, value) + .05) >= 3) ++count;
        }
    }
    return count;
}
bool fitted(QWidget* canvas, QColor background, double aspect)
{
    const auto image = canvas->grab().toImage(); const auto pixels = ink(image, background);
    if (pixels.isEmpty() || !image.rect().contains(pixels) ||
        std::max(double(pixels.width()) / image.width(), double(pixels.height()) / image.height()) < .5 ||
        std::abs(double(pixels.width()) / pixels.height() - aspect) > aspect * .06) return false;
    return std::abs(pixels.center().x() - image.width() / 2) < image.width() / 20 &&
        std::abs(pixels.center().y() - image.height() / 2) < image.height() / 20;
}
}

class DeformerGridDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void explicitPreviewAndOneApplyUseCurrentInputs()
    {
        const auto source = context(); int prepares = 0, applies = 0, receivedForm = -1;
        Dialog dialog(source, [&](int r, int c, int keyform, auto& result, auto* error) {
            ++prepares; receivedForm = keyform; return prepare(source)(r, c, keyform, result, error);
        }, [&](int r, int c, QString*) { ++applies; return r == 3 && c == 4; });
        dialog.show(); QCoreApplication::processEvents(); QCOMPARE(prepares, 0); QVERIFY(!apply(dialog)->isEnabled());
        change(dialog); QCOMPARE(prepares, 0); dialog.UpdatePreview(); QCOMPARE(prepares, 1); QCOMPARE(receivedForm, 1); QVERIFY(dialog.IsReady());
        QVERIFY(apply(dialog)->isEnabled()); dialog.accept(); QCOMPARE(applies, 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
        dialog.accept(); QCOMPARE(applies, 1);
    }
    void pendingTextAndKeyformInvalidateWithoutAutomaticCalculation()
    {
        auto source = context(); int count = 0;
        Dialog dialog(source, [&](int r, int c, int f, auto& result, auto* error) { ++count; return prepare(source)(r,c,f,result,error); }, [](int,int,QString*) { return true; });
        change(dialog); dialog.UpdatePreview(); QVERIFY(dialog.IsReady());
        auto* input = rows(dialog)->findChild<QLineEdit*>(); input->clear(); QVERIFY(!dialog.IsReady()); QVERIFY(!apply(dialog)->isEnabled());
        dialog.UpdatePreview(); QCOMPARE(count, 1); QVERIFY(!dialog.Error().isEmpty());
        input->setText("4"); dialog.UpdatePreview(); QCOMPARE(count, 2); QCOMPARE(rows(dialog)->value(), 4); QVERIFY(dialog.IsReady());
        dialog.findChild<QComboBox*>("deformerGridKeyform")->setCurrentIndex(2); QVERIFY(!dialog.IsReady()); QCOMPARE(count, 2);
        dialog.UpdatePreview(); QVERIFY(dialog.IsReady()); QCOMPARE(count, 3);
    }
    void unchangedDimensionsAndCancelNeverSubmit()
    {
        int applies = 0; auto source = context();
        Dialog dialog(source, prepare(source), [&](int,int,QString*) { ++applies; return true; });
        dialog.show(); dialog.UpdatePreview(); QVERIFY(dialog.IsReady()); QVERIFY(!apply(dialog)->isEnabled());
        dialog.accept(); QCOMPARE(applies, 0); QVERIFY(dialog.isVisible());
        change(dialog); dialog.UpdatePreview(); dialog.reject(); QCOMPARE(applies, 0); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    void failedApplyRetainsPreviewAndCanRetry()
    {
        int applies = 0; auto source = context();
        Dialog dialog(source, prepare(source), [&](int,int,QString* error) { ++applies; *error = "A & B <stale>"; return applies > 1; });
        dialog.show(); change(dialog); dialog.UpdatePreview(); dialog.accept(); QVERIFY(dialog.isVisible()); QVERIFY(dialog.IsReady());
        QCOMPARE(dialog.Error(), QString("A & B <stale>")); QVERIFY(apply(dialog)->isEnabled());
        QVERIFY(dialog.findChild<QPlainTextEdit*>("deformerGridDetails")->toPlainText().contains(dialog.Error()));
        dialog.accept(); QCOMPARE(applies, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void invalidPreviewNeverReplacesLastAcceptedGeometry_data()
    {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 6; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidPreviewNeverReplacesLastAcceptedGeometry()
    {
        QFETCH(int, fault); auto source = context(); bool invalid = false;
        Dialog dialog(source, [&](int r,int c,int f,auto& result,auto* error) {
            prepare(source)(r,c,f,result,error); if (!invalid) return true;
            if (fault == 0) result.afterGrid.removeLast();
            if (fault == 1) result.afterGrid[0].setX(std::numeric_limits<double>::quiet_NaN());
            if (fault == 2) { result.afterGrid[0].setX(999); result.afterContours[0].setP1({std::numeric_limits<double>::infinity(), 0}); }
            if (fault == 3) result.maximumDisplacementPixels = -1;
            if (fault == 4) throw std::runtime_error("prepare failure");
            if (fault == 5) { *error = "cannot prepare"; return false; }
            return true;
        }, [](int,int,QString*) { return true; });
        auto* canvas = dialog.findChild<QWidget*>("deformerGridCanvas"); canvas->setFixedSize(360, 220);
        dialog.show(); change(dialog); dialog.UpdatePreview(); QTest::qWait(30); QVERIFY(dialog.IsReady());
        const auto before = canvas->grab().toImage(); invalid = true; dialog.UpdatePreview();
        QVERIFY(!dialog.IsReady()); QVERIFY(!apply(dialog)->isEnabled()); QVERIFY(!dialog.Error().isEmpty());
        QCOMPARE(canvas->grab().toImage(), before);
        invalid = false; dialog.UpdatePreview(); QVERIFY(dialog.IsReady()); QVERIFY(dialog.Error().isEmpty());
    }
    void changingInputsDuringPreparationDiscardsStaleResult()
    {
        auto source = context(); Dialog* current = nullptr; bool changeDuringPrepare = true; int calls = 0;
        Dialog dialog(source, [&](int r,int c,int f,auto& result,auto* error) {
            ++calls; current->UpdatePreview(); current->accept();
            if (changeDuringPrepare) rows(*current)->setValue(r + 1);
            return prepare(source)(r,c,f,result,error);
        }, [](int,int,QString*) { return true; }); current = &dialog;
        change(dialog); dialog.UpdatePreview(); QCOMPARE(calls, 1); QVERIFY(!dialog.IsReady()); QVERIFY(!dialog.IsPreparing());
        changeDuringPrepare = false; dialog.UpdatePreview(); QVERIFY(dialog.IsReady()); QCOMPARE(calls, 2);
    }
    void callbacksMayDestroyOwner_data()
    {
        QTest::addColumn<bool>("duringApply"); QTest::newRow("prepare") << false; QTest::newRow("apply") << true;
    }
    void callbacksMayDestroyOwner()
    {
        QFETCH(bool, duringApply); auto source = context(); auto* owner = new QWidget;
        QPointer<Dialog> dialog = new Dialog(source, [&](int r,int c,int f,auto& result,auto* error) {
            if (!duringApply) { delete owner; return false; } return prepare(source)(r,c,f,result,error);
        }, [&](int,int,QString*) { delete owner; return true; }, owner);
        change(*dialog); dialog->UpdatePreview(); if (duringApply) dialog->accept(); QVERIFY(!dialog); QCoreApplication::processEvents();
    }
    void cancelDuringPrepareAndSubmitReentryAreSafe()
    {
        auto source = context(); Dialog* current = nullptr; int applies = 0;
        Dialog canceled(source, [&](int,int,int,auto&,QString*) { current->reject(); return true; }, [&](int,int,QString*) { ++applies; return true; });
        current = &canceled; canceled.show(); change(canceled); canceled.UpdatePreview(); QVERIFY(!canceled.isVisible()); QVERIFY(!canceled.IsReady()); QCOMPARE(applies, 0);
        Dialog committed(source, prepare(source), [&](int,int,QString*) { ++applies; current->accept(); current->reject(); return true; });
        current = &committed; change(committed); committed.UpdatePreview(); committed.accept(); QCOMPARE(applies, 1); QCOMPARE(committed.result(), int(QDialog::Accepted));
    }
    void noOrdinaryFormAndLargeVirtualListDoNotInventGrid()
    {
        auto source = context(); source.ordinaryKeyforms = 0; int form = 99;
        Dialog empty(source, [&](int r,int c,int f,auto& result,auto* error) { form = f; return prepare(source)(r,c,f,result,error); }, [](int,int,QString*) { return true; });
        change(empty); empty.UpdatePreview(); QCOMPARE(form, -1); QVERIFY(empty.IsReady());
        source.ordinaryKeyforms = 1000000; source.initialKeyform = 999999;
        Dialog many(source, {}, {}); auto* selection = many.findChild<QComboBox*>("deformerGridKeyform");
        QCOMPARE(selection->count(), 1000000); QCOMPARE(selection->currentData().toInt(), 999999);
        QVERIFY(!update(many)->isEnabled());
    }
    void extremeCoordinatesUseIndependentCenteredViews()
    {
        qApp->setStyleSheet(Theme::globalStyleSheet(false)); auto source = context();
        Dialog dialog(source, [&](int r,int c,int f,auto& result,auto* error) {
            prepare(source)(r,c,f,result,error);
            for (auto* points : {&result.beforeGrid, &result.afterGrid}) for (auto& point : *points)
                point = QPointF(point.x() * 1e30 + 1e38, point.y() * 1e30 - 1e38);
            for (auto* lines : {&result.beforeContours, &result.afterContours}) for (auto& line : *lines)
                line = QLineF(line.p1() * 1e-25, line.p2() * 1e-25);
            return true;
        }, [](int,int,QString*) { return true; });
        dialog.resize(800, 700); dialog.show(); change(dialog); dialog.UpdatePreview(); QVERIFY(dialog.IsReady());
        auto* tabs = dialog.findChild<QTabWidget*>("deformerGridPreviewTabs");
        const QColor background(Theme::tokens(false).value("@surface@"));
        QTRY_VERIFY(fitted(dialog.findChild<QWidget*>("deformerGridCanvas"), background, 4.15 / 2.1));
        tabs->setCurrentIndex(1); QTRY_VERIFY(fitted(dialog.findChild<QWidget*>("deformerGridContours"), background, 62.0 / 41.0));
    }
    void themesResizeAndIndependentCoordinateSpaces_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void themesResizeAndIndependentCoordinateSpaces()
    {
        QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        QMainWindow owner; owner.setWindowFlag(Qt::FramelessWindowHint); owner.resize(1380, 980); owner.show();
        auto source = context(); source.id = QString(63, 'W'); source.name = QStringLiteral("名称") + QString(100, QChar(0x4e2d));
        Dialog dialog(source, prepare(source), [](int,int,QString*) { return true; }, &owner);
        dialog.resize(1000, 760); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog)); QTest::qWait(30);
        change(dialog); dialog.UpdatePreview();
        QCOMPARE(dialog.windowModality(), Qt::ApplicationModal);
        auto* tabs = dialog.findChild<QTabWidget*>("deformerGridPreviewTabs");
        auto* gridCanvas = dialog.findChild<QWidget*>("deformerGridCanvas"); auto* contourCanvas = dialog.findChild<QWidget*>("deformerGridContours");
        const QColor background(Theme::tokens(dark).value("@surface@"));
        const auto directory = qEnvironmentVariable("DEFORMER_GRID_IMAGES");
        for (const auto size : {QSize(1000,760), QSize(560,640)}) {
            dialog.resize(size); QTest::qWait(30); tabs->setCurrentIndex(0);
            QTRY_VERIFY(fitted(gridCanvas, background, 4.15 / 2.1));
            const auto picture = gridCanvas->grab().toImage();
            QVERIFY(contrastingPixels(picture, QColor(Theme::tokens(dark).value("@accentInk@")), background) > 20);
            QVERIFY(contrastingPixels(picture, QColor(Theme::tokens(dark).value("@textMuted@")), background) > 20);
            QCOMPARE(dialog.width(), size.width());
            if (dialog.screen()->availableGeometry().height() >= size.height() + dialog.style()->pixelMetric(QStyle::PM_TitleBarHeight)) QCOMPARE(dialog.height(), size.height());
            else { QVERIFY(dialog.height() <= size.height()); QVERIFY(dialog.height() >= dialog.minimumHeight()); }
            auto* scroll = dialog.findChild<QScrollArea*>("deformerGridControls"); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            QVERIFY(apply(dialog)->isVisible()); QVERIFY(update(dialog)->isVisible());
            QCOMPARE(apply(dialog)->palette().color(QPalette::Button), QColor(Theme::tokens(dark).value("@accentFill@")));
            QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
            for (auto* input : {rows(dialog), columns(dialog)}) {
                const QRect bounds(input->mapTo(scroll->viewport(), QPoint()), input->size());
                QVERIFY(scroll->viewport()->rect().contains(bounds));
            }
            if (!directory.isEmpty()) {
                QFile record(directory + QString("/geometry-%1-%2.json").arg(dark ? "dark" : "light").arg(size.width()));
                QVERIFY(record.open(QIODevice::WriteOnly));
                const QJsonObject geometry{{"requestedWidth", size.width()}, {"requestedHeight", size.height()}, {"actualWidth", dialog.width()}, {"actualHeight", dialog.height()},
                    {"screenAvailableHeight", dialog.screen()->availableGeometry().height()}, {"dpr", dialog.devicePixelRatioF()},
                    {"controlsWidth", scroll->viewport()->width()}, {"controlsHeight", scroll->viewport()->height()}, {"canvasWidth", gridCanvas->width()}, {"canvasHeight", gridCanvas->height()}};
                QVERIFY(record.write(QJsonDocument(geometry).toJson()) > 0);
            }
            if (!directory.isEmpty()) QVERIFY(dialog.grab().save(directory + QString("/grid-%1-%2.png").arg(dark ? "dark" : "light").arg(size.width())));
            tabs->setCurrentIndex(1); QTRY_VERIFY(fitted(contourCanvas, background, 62.0 / 41.0));
            if (!directory.isEmpty()) QVERIFY(dialog.grab().save(directory + QString("/contours-%1-%2.png").arg(dark ? "dark" : "light").arg(size.width())));
        }
        auto* cancel = dialog.findChild<QDialogButtonBox*>("deformerGridButtons")->button(QDialogButtonBox::Cancel);
        QVERIFY(cancel->isDefault()); QVERIFY(!apply(dialog)->autoDefault());
        QVERIFY(dialog.findChild<QLineEdit*>("deformerGridId")->isReadOnly());
        QCOMPARE(dialog.findChild<QLineEdit*>("deformerGridName")->text(), source.name);
    }
};
QTEST_MAIN(DeformerGridDialogTests)
#include "deformer_grid_dialog_tests.moc"
