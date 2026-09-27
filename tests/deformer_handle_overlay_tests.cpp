#include "opengl/cdeformerhandleoverlay.h"
#include "ui/theme.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QStyleFactory>
#include <QtTest>
#include <cmath>
#include <limits>

namespace {
class Canvas : public QWidget
{
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window));
        QColor grid = palette().color(QPalette::WindowText); grid.setAlphaF(.07);
        painter.setPen(grid);
        const int step = qMax(1, fontMetrics().height() * 2);
        for (int x = 0; x < width(); x += step) painter.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += step) painter.drawLine(0, y, width(), y);
    }
};

QImage render(QWidget& widget, int scale)
{
    QImage image(widget.size() * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

QColor at(const QImage& image, const QPointF& pixel)
{
    return image.pixelColor(qRound(pixel.x() * image.devicePixelRatio()), qRound(pixel.y() * image.devicePixelRatio()));
}

double luminance(const QColor& color)
{
    const auto linear = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
double contrast(const QColor& first, const QColor& second)
{
    const double a = luminance(first), b = luminance(second); return (std::max(a,b) + .05) / (std::min(a,b) + .05);
}
qreal coloredDiameter(const QImage& image, const QPointF& center, const QColor& color)
{
    const qreal scale = image.devicePixelRatio();
    const int x = qRound(center.x() * scale), y = qRound(center.y() * scale);
    int start = x, end = x;
    const auto matches = [&](int at) {
        const auto pixel = image.pixelColor(at, y);
        return qAbs(pixel.red() - color.red()) < 3 && qAbs(pixel.green() - color.green()) < 3 && qAbs(pixel.blue() - color.blue()) < 3;
    };
    while (start > 0 && matches(start - 1)) --start;
    while (end + 1 < image.width() && matches(end + 1)) ++end;
    return (end - start + 1) / scale;
}
}

class DeformerHandleOverlayTests : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { qApp->setStyleSheet({}); }

    void warpProjectionSelectionAndRectangleKeepPointIndices()
    {
        CDeformerHandleOverlay overlay;
        const QVector<float> points{-1, -1, -1, 1, 1, -1, 1, 1, 0, 0};
        QVERIFY(overlay.SetWarp(points, {}, {400, 300}));
        QCOMPARE(overlay.PointPixels(), (QVector<QPointF>{{0, 300}, {0, 0}, {400, 300}, {400, 0}, {200, 150}}));
        for (int index = 0; index < 5; ++index) {
            const auto hit = overlay.HitTest(overlay.PointPixels()[index]);
            QCOMPARE(hit.kind, CDeformerHandleOverlay::WarpPoint); QCOMPARE(hit.index, index);
        }
        overlay.SetSelected({-1, 4, 1, 4, 99}); QCOMPARE(overlay.Selected(), QVector<int>({1, 4}));
        QCOMPARE(overlay.PointsInRect(QRectF(400, 300, -400, -300)), QVector<int>({0, 1, 2, 3, 4}));
        QCOMPARE(overlay.PointsInRect(QRectF(200, 150, 0, 0)), QVector<int>({4}));
        QCOMPARE(overlay.PointsInRect(QRectF(0, 0, 400, 1)), QVector<int>({1, 3}));
        QVERIFY(overlay.PointsInRect({10, 10, 10, 10}).isEmpty());
        QVERIFY(overlay.SetWarp({0, 0, .5f, .5f}, {}, {400, 300}));
        QCOMPARE(overlay.Selected(), QVector<int>{1});
        overlay.Clear(); QVERIFY(overlay.PointPixels().isEmpty()); QVERIFY(overlay.Selected().isEmpty());
        QCOMPARE(overlay.HitTest({200, 150}).kind, CDeformerHandleOverlay::None);
        QVERIFY(overlay.PointsInRect({0, 0, 400, 300}).isEmpty());
    }

    void warpHitsChooseNearestAndUseScreenDistances()
    {
        CDeformerHandleOverlay overlay;
        QVERIFY(overlay.SetWarp({0, 0, .03125f, 0, 0, 0}, {}, {400, 300}));
        overlay.SetSelected({2});
        QCOMPARE(overlay.HitTest({200, 150}).index, 0);
        QCOMPARE(overlay.HitTest({203.5, 150}).index, 1);
        QCOMPARE(overlay.HitTest({203.125, 150}).index, 0);
        QVERIFY(overlay.SetWarp({0, 0}, {}, {400, 300}));
        int boundary = 0;
        while (boundary < 100 && overlay.HitTest({200.0 + boundary, 150}).kind != CDeformerHandleOverlay::None) ++boundary;
        QVERIFY(boundary > 2 && boundary < 100);
        QMatrix4x4 zoom; zoom.scale(50);
        QVERIFY(overlay.SetWarp({0, 0}, zoom, {400, 300}));
        QCOMPARE(overlay.HitTest({200.0 + boundary - 1, 150}).kind, CDeformerHandleOverlay::WarpPoint);
        QCOMPARE(overlay.HitTest({200.0 + boundary, 150}).kind, CDeformerHandleOverlay::None);
        QCOMPARE(overlay.HitTest({-1, 150}).kind, CDeformerHandleOverlay::None);
        QCOMPARE(overlay.HitTest({std::numeric_limits<double>::quiet_NaN(), 150}).kind, CDeformerHandleOverlay::None);
    }

    void invalidWarpFramesAreAtomic_data()
    {
        QTest::addColumn<int>("failure");
        const QStringList names{"empty", "odd", "nan", "infinite", "singular", "matrix-nan", "zero-viewport", "negative-viewport", "horizon", "budget"};
        for (int failure = 0; failure < names.size(); ++failure) QTest::newRow(qPrintable(names[failure])) << failure;
    }

    void invalidWarpFramesAreAtomic()
    {
        QFETCH(int, failure);
        CDeformerHandleOverlay overlay;
        QVERIFY(overlay.SetWarp({0, 0, .5f, .5f}, {}, {400, 300})); overlay.SetSelected({1});
        const auto before = overlay.PointPixels();
        QVector<float> points{0, 0, .5f, .5f}; QMatrix4x4 matrix; QSize size(400, 300);
        if (failure == 0) points.clear();
        else if (failure == 1) points.append(0);
        else if (failure == 2) points[2] = std::numeric_limits<float>::quiet_NaN();
        else if (failure == 3) points[3] = std::numeric_limits<float>::infinity();
        else if (failure == 4) matrix.scale(0);
        else if (failure == 5) matrix(0, 0) = std::numeric_limits<float>::quiet_NaN();
        else if (failure == 6) size = {0, 300};
        else if (failure == 7) size = {-1, 300};
        else if (failure == 8) { matrix(3, 0) = 1; points[2] = -1; }
        else if (failure == 9) points.fill(0, 2000002);
        QVERIFY(!overlay.SetWarp(points, matrix, size));
        QCOMPARE(overlay.PointPixels(), before); QCOMPARE(overlay.Selected(), QVector<int>{1}); QCOMPARE(overlay.size(), QSize(400, 300));
        QCOMPARE(overlay.HitTest(before[1]).index, 1);
    }

    void rotationDirectionAndLengthAreIndependentOfScale()
    {
        CDeformerHandleOverlay overlay;
        QVERIFY(overlay.SetRotation({0, 0}, {1, 1}, {}, {400, 300}));
        const auto delta = overlay.AnglePixel() - overlay.OriginPixel();
        const double length = std::hypot(delta.x(), delta.y());
        QVERIFY(length > 20 && length < 200);
        QVERIFY(std::abs(delta.y() / delta.x() + .75) < 1e-8);
        QCOMPARE(overlay.HitTest(overlay.OriginPixel()).kind, CDeformerHandleOverlay::RotationOrigin);
        QCOMPARE(overlay.HitTest(overlay.AnglePixel()).kind, CDeformerHandleOverlay::RotationAngle);
        QCOMPARE(overlay.HitTest(overlay.AnglePixel()).index, -1);
        QCOMPARE(overlay.HitTest(overlay.OriginPixel() + delta * .5).kind, CDeformerHandleOverlay::None);
        QMatrix4x4 zoom; zoom.scale(5);
        QVERIFY(overlay.SetRotation({0, 0}, {50, 50}, zoom, {400, 300}));
        QVERIFY(std::abs(QLineF(overlay.OriginPixel(), overlay.AnglePixel()).length() - length) < 1e-8);
        QMatrix4x4 rotated; rotated.rotate(90, 0, 0, 1);
        QVERIFY(overlay.SetRotation({0, 0}, {1, 0}, rotated, {400, 300}));
        QVERIFY(std::abs(overlay.AnglePixel().x() - overlay.OriginPixel().x()) < 1e-4);
        QVERIFY(overlay.AnglePixel().y() < overlay.OriginPixel().y());
        overlay.SetSelected({0, 1}); QVERIFY(overlay.Selected().isEmpty()); QVERIFY(overlay.PointPixels().isEmpty());
        QVERIFY(overlay.PointsInRect({0, 0, 400, 300}).isEmpty());
        auto font = overlay.font(); font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() * 2 : 24);
        overlay.setFont(font);
        QVERIFY(QLineF(overlay.OriginPixel(), overlay.AnglePixel()).length() > length);
    }

    void invalidRotationFramesAreAtomic_data()
    {
        QTest::addColumn<int>("failure");
        const QStringList names{"zero-direction", "nan-origin", "infinite-direction", "singular", "zero-viewport", "overflow", "horizon"};
        for (int failure = 0; failure < names.size(); ++failure) QTest::newRow(qPrintable(names[failure])) << failure;
    }

    void invalidRotationFramesAreAtomic()
    {
        QFETCH(int, failure);
        CDeformerHandleOverlay overlay;
        QVERIFY(overlay.SetRotation({.2, -.1}, {1, 0}, {}, {400, 300}));
        const auto center = overlay.OriginPixel(), angle = overlay.AnglePixel();
        QPointF origin, direction(1, 0); QMatrix4x4 matrix; QSize size(400, 300);
        if (failure == 0) direction = {};
        else if (failure == 1) origin.setX(std::numeric_limits<double>::quiet_NaN());
        else if (failure == 2) direction.setY(std::numeric_limits<double>::infinity());
        else if (failure == 3) matrix.scale(0);
        else if (failure == 4) size = {400, 0};
        else if (failure == 5) { origin.setX(std::numeric_limits<double>::max()); direction.setX(origin.x()); }
        else if (failure == 6) { matrix(3, 0) = 1; direction = {-1, 0}; }
        QVERIFY(!overlay.SetRotation(origin, direction, matrix, size));
        QCOMPARE(overlay.OriginPixel(), center); QCOMPARE(overlay.AnglePixel(), angle); QCOMPARE(overlay.size(), QSize(400, 300));
        QCOMPARE(overlay.HitTest(center).kind, CDeformerHandleOverlay::RotationOrigin);
    }

    void framesOwnTheirGeometryAndDoNotCreatePerPointWidgets()
    {
        CDeformerHandleOverlay overlay;
        QVector<float> points;
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) points.append({x / 100.0f, y / 100.0f});
        QVERIFY(overlay.SetWarp(points, {}, {400, 300}));
        QCOMPARE(overlay.PointPixels().size(), 10000);
        QCOMPARE(overlay.findChildren<QWidget*>().size(), 0);
        QVERIFY(overlay.testAttribute(Qt::WA_TransparentForMouseEvents)); QVERIFY(!overlay.autoFillBackground());
        QCOMPARE(overlay.focusPolicy(), Qt::NoFocus);
        const auto pixels = overlay.PointPixels(); points.fill(0);
        QCOMPARE(overlay.PointPixels(), pixels);
        overlay.resize(500, 300);
        QCOMPARE(overlay.HitTest(pixels[0]).kind, CDeformerHandleOverlay::None);
        QVERIFY(overlay.PointsInRect({0, 0, 500, 300}).isEmpty());
        QVERIFY(overlay.SetWarp(points, {}, {500, 300}));
        QCOMPARE(overlay.HitTest({250, 150}).index, 0);
    }

    void rotationArrowContrastsWithFilledHandle_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void rotationArrowContrastsWithFilledHandle()
    {
        QFETCH(bool,dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); Canvas canvas; canvas.resize(400,300); CDeformerHandleOverlay overlay(&canvas);
        QVERIFY(overlay.SetWarp({0,0}, {}, canvas.size())); overlay.SetSelected({0}); canvas.show(); overlay.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto palette = overlay.palette(); palette.setColor(QPalette::HighlightedText, Qt::white); overlay.setPalette(palette);
        const QColor accent = at(render(canvas,4),overlay.PointPixels()[0]); QVERIFY(contrast(accent,overlay.palette().color(QPalette::Window)) >= 3);
        QVERIFY(overlay.SetRotation({0,0},{1,0},{},canvas.size())); const auto image = render(canvas,4); const QPointF center = overlay.AnglePixel(); double maximum = 1;
        for (int y = qRound((center.y()-2) * 4); y <= qRound((center.y()+1) * 4); ++y)
            for (int x = qRound((center.x()-1) * 4); x <= qRound((center.x()+1) * 4); ++x) maximum = std::max(maximum,contrast(image.pixelColor(x,y),accent));
        QVERIFY2(maximum >= 3,qPrintable(QString("rotation arrow contrast %1").arg(maximum)));
    }
    void themesTransparencyAndHighDpi_data()
    {
        QTest::addColumn<QString>("styleName"); QTest::addColumn<bool>("dark");
        for (const auto& style : QStyleFactory::keys()) if (style == "Fusion" ||
            (QGuiApplication::platformName() == "cocoa" && style.compare("macOS", Qt::CaseInsensitive) == 0))
            for (bool dark : {false, true}) QTest::newRow(qPrintable(style + (dark ? "-dark" : "-light"))) << style << dark;
    }

    void themesTransparencyAndHighDpi()
    {
        QFETCH(QString, styleName); QFETCH(bool, dark);
        auto* style = QStyleFactory::create(styleName); QVERIFY(style);
        qApp->setStyle(style); qApp->setPalette(style->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        Canvas canvas; canvas.resize(520, 340);
        CDeformerHandleOverlay overlay(&canvas);
        QVERIFY(overlay.SetWarp({-.5f, -.3f, 0, .3f, .5f, -.3f}, {}, canvas.size()));
        overlay.SetSelected({0, 2}); overlay.SetSelectionRect(QRectF(40, 45, 120, 70)); overlay.hide();
        if (const auto* screen = QGuiApplication::primaryScreen()) canvas.move(screen->availableGeometry().center() - canvas.rect().center());
        canvas.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto background = render(canvas, 1);
        overlay.show(); overlay.raise();
        const auto image = render(canvas, 1), highDpi = render(canvas, 2);
        QCOMPARE(at(image, {14, 37}), at(background, {14, 37}));
        const auto selected = overlay.PointPixels()[0];
        const auto accent = at(image, selected);
        QVERIFY(contrast(accent, overlay.palette().color(QPalette::Window)) >= 3); QCOMPARE(at(highDpi, selected).rgba(), accent.rgba());
        QVERIFY(at(image, selected) != at(background, selected));
        const auto normalSize = coloredDiameter(image, selected, accent), scaledSize = coloredDiameter(highDpi, selected, accent);
        QVERIFY(normalSize > 2); QVERIFY(std::abs(normalSize - scaledSize) < 2);
        const auto nativeImage = canvas.grab().toImage(); const qreal nativeScale = nativeImage.devicePixelRatio(); double rectangleContrast = 1;
        for (int y = qRound(43 * nativeScale); y <= qRound(47 * nativeScale); ++y) for (int x = qRound(65 * nativeScale); x <= qRound(115 * nativeScale); ++x)
            rectangleContrast = std::max(rectangleContrast, contrast(nativeImage.pixelColor(x,y), overlay.palette().color(QPalette::Window)));
        QVERIFY(rectangleContrast >= 3);
        const auto framed = image;
        overlay.SetSelectionRect(QRectF(std::numeric_limits<double>::quiet_NaN(), 0, 10, 10));
        QCOMPARE(render(canvas, 1), framed);
        overlay.SetSelectionRect(std::nullopt);
        const auto unframed = render(canvas, 1);
        QVERIFY(unframed != framed);
        overlay.SetSelectionRect(QRectF(800, 800, 40, 40));
        QCOMPARE(render(canvas, 1), unframed);
        const QString directory = qEnvironmentVariable("DEFORMER_HANDLE_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            const auto stem = directory + '/' + styleName + (dark ? "-dark" : "-light");
            QVERIFY(framed.save(stem + "-warp.png")); QVERIFY(highDpi.save(stem + "-warp@2x.png"));
            QVERIFY(nativeImage.save(stem + "-warp-native.png"));
            QVERIFY(overlay.SetRotation({0, 0}, {1, 1}, {}, canvas.size()));
            QVERIFY(render(canvas, 1).save(stem + "-rotation.png"));
            QVERIFY(canvas.grab().save(stem + "-rotation-native.png"));
        }
    }
};

QTEST_MAIN(DeformerHandleOverlayTests)
#include "deformer_handle_overlay_tests.moc"
