#include "opengl/cvertexselectionoverlay.h"
#include "ui/theme.h"
#include <QtTest>
#include <QDir>
#include <QPainter>
#include <QStyleFactory>
#include <QStyle>
#include <limits>
#include <cmath>

namespace {
using Overlay = CVertexSelectionOverlay;
using Selection = live2dstruct::CVertexSelection;
Overlay::Frame frame()
{
    Overlay::Frame f; f.viewport = QSize(360, 240); f.points = {{40, 50}, {95, 50}, {150, 50}, {205, 50}, {260, 50}}; f.weights = {1, .25, .75, 1, 0}; f.hard = {0}; return f;
}
class Host : public QWidget {
public:
    int presses = 0;
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); p.fillRect(rect(), palette().color(QPalette::Window)); }
    void mousePressEvent(QMouseEvent*) override { ++presses; }
};
class CountOverlay : public Overlay {
public:
    using Overlay::Overlay; int paints = 0;
protected:
    void paintEvent(QPaintEvent* event) override { ++paints; Overlay::paintEvent(event); }
};
void settle() { for (int i = 0; i < 3; ++i) { QApplication::processEvents(); QTest::qWait(10); } }
QImage render(QWidget& widget) { QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); widget.render(&image); return image; }
double luminance(const QColor& color)
{
    const auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
double contrast(const QColor& a, const QColor& b)
{
    const double x = luminance(a), y = luminance(b); return (std::max(x,y) + .05) / (std::min(x,y) + .05);
}
double maximumContrast(const QImage& image, const QRect& logical, const QColor& background)
{
    const qreal scale = image.devicePixelRatio(); double result = 1;
    for (int y = qRound(logical.top() * scale); y < qRound((logical.bottom() + 1) * scale); ++y)
        for (int x = qRound(logical.left() * scale); x < qRound((logical.right() + 1) * scale); ++x) result = std::max(result, contrast(image.pixelColor(x,y), background));
    return result;
}
QRect differenceBounds(const QImage& a, const QImage& b)
{
    QRect result;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) if (a.pixel(x,y) != b.pixel(x,y)) result |= QRect(x,y,1,1);
    return result;
}
}
class VertexSelectionOverlayTests : public QObject
{
    Q_OBJECT
    QString m_nativeStyle;
private slots:
    void initTestCase() { m_nativeStyle = qApp->style()->objectName(); }
    void init() { qApp->setStyleSheet({}); qApp->setStyle(QStyleFactory::create(m_nativeStyle)); qApp->setPalette(qApp->style()->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(false)); }
    void unorderedHardSelectionIsAcceptedWithoutChangingCaller()
    {
        Overlay overlay; auto f = frame(); f.hard = {3, 0}; QVERIFY(overlay.SetFrame(f)); QCOMPARE(f.hard, QVector<int>({3,0})); QCOMPARE(overlay.CurrentFrame().hard, f.hard);
        auto ordered = f; ordered.hard = {0,3}; Overlay reference; QVERIFY(reference.SetFrame(ordered)); QCOMPARE(render(overlay), render(reference));
    }
    void acceptedFrameOwnsExistingRawAliases()
    {
        Overlay overlay; auto f = frame(); f.lasso = {{20,100},{70,120},{35,150}};
        auto* points = f.points.data(); auto* weights = f.weights.data(); auto* hard = f.hard.data(); auto* lasso = f.lasso.data();
        QVERIFY(overlay.SetFrame(f)); const auto before = render(overlay); points[0] = {300,220}; weights[1] = 1; hard[0] = 3; lasso[0] = {300,200};
        QCOMPARE(overlay.CurrentFrame().points[0], QPointF(40,50)); QCOMPARE(overlay.CurrentFrame().weights[1], .25); QCOMPARE(overlay.CurrentFrame().hard[0], 0); QCOMPARE(overlay.CurrentFrame().lasso[0], QPointF(20,100)); QCOMPARE(render(overlay), before);
    }
    void invalidFramesAreAtomic_data()
    {
        QTest::addColumn<int>("kind"); for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidFramesAreAtomic()
    {
        QFETCH(int, kind); Overlay overlay; QVERIFY(overlay.SetFrame(frame())); const auto before = render(overlay); auto f = frame();
        switch (kind) {
        case 0: f.weights[1] = std::numeric_limits<double>::quiet_NaN(); break;
        case 1: f.weights[1] = -1; break;
        case 2: f.weights.removeLast(); break;
        case 3: f.hard = {0,0}; break;
        case 4: f.hard = {8}; break;
        case 5: f.brushRadius = -1; break;
        case 6: f.viewport = {}; break;
        case 7: f.points[0].setX(std::numeric_limits<double>::infinity()); break;
        case 8: f.lasso.fill(QPointF(1,1), 4097); break;
        case 9: f.weights[0] = .9; break;
        }
        QString error; QVERIFY(!overlay.SetFrame(f, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(render(overlay), before);
    }
    void sameFrameDoesNotScheduleAnotherPaint()
    {
        Host host; host.resize(360,240); CountOverlay overlay(&host); auto f = frame(); f.hard = {3,0}; QVERIFY(overlay.SetFrame(f)); host.show(); overlay.show(); settle();
        const int before = overlay.paints; for (int i = 0; i < 20; ++i) QVERIFY(overlay.SetFrame(f)); settle(); QCOMPARE(overlay.paints, before);
        f.weights[1] = .6; QVERIFY(overlay.SetFrame(f)); settle(); QVERIFY(overlay.paints > before);
    }
    void pointWeightsAndHardMembershipHaveDistinctPixels_data() { QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true; }
    void pointWeightsAndHardMembershipHaveDistinctPixels()
    {
        QFETCH(bool,dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); Overlay overlay; QVERIFY(overlay.SetFrame(frame())); const auto image = render(overlay);
        const auto patch = [&image](int x) { return image.copy(x-13,37,26,26); };
        QVERIFY(patch(40) != patch(205)); QVERIFY(patch(95) != patch(150)); QVERIFY(patch(95) != patch(260));
        const auto original = image; auto f = frame(); f.drawPoints = false; QVERIFY(overlay.SetFrame(f)); QVERIFY(!differenceBounds(original, render(overlay)).isEmpty());
        const QString directory = qEnvironmentVariable("VERTEX_UI_SCREENSHOTS"); if (!directory.isEmpty()) { QDir().mkpath(directory); original.save(directory+QString("/overlay-%1.png").arg(dark ? "dark" : "light")); }
    }
    void lassoAndRectangleStayClippedAndClearCompletely()
    {
        Overlay overlay; auto f = frame(); f.drawPoints = false; QVERIFY(overlay.SetFrame(f)); const auto empty = render(overlay);
        f.lasso = {{-500,-500},{180,190},{-500,190},{180,-500}}; QVERIFY(overlay.SetFrame(f)); const auto lasso = render(overlay); QVERIFY(!differenceBounds(empty,lasso).isEmpty());
        f.lasso.clear(); f.rectangle = QRectF(-20,80,250,120); QVERIFY(overlay.SetFrame(f)); QVERIFY(render(overlay) != lasso);
        overlay.Clear(); QCOMPARE(render(overlay), empty);
    }
    void brushRadiusUsesLogicalPixelsAndSubtractHasDistinctCursor()
    {
        Overlay overlay; auto f = frame(); f.drawPoints = false; QVERIFY(overlay.SetFrame(f)); const auto empty = render(overlay); f.brushCenter = QPointF(180,120); f.brushRadius = 40; f.brushHardness = .5; QVERIFY(overlay.SetFrame(f)); const auto plus = render(overlay);
        const auto bounds = differenceBounds(empty,plus); QVERIFY(bounds.width() >= 80); QVERIFY(bounds.width() <= 86); QVERIFY(bounds.height() >= 80); QVERIFY(bounds.height() <= 86); QCOMPARE(bounds.center(), QPoint(179,119));
        f.brushOperation = Selection::Combine::Subtract; QVERIFY(overlay.SetFrame(f)); QVERIFY(render(overlay) != plus);
    }
    void nativeBrushRadiusRemainsLogicalAtDisplayDpr()
    {
        Host host; host.resize(360,240); Overlay overlay(&host); auto f = frame(); f.drawPoints = false; QVERIFY(overlay.SetFrame(f)); host.show(); overlay.show(); settle();
        const auto empty = host.grab().toImage(); const qreal dpr = empty.devicePixelRatio();
        f.brushCenter = QPointF(180,120); f.brushRadius = 40; f.brushHardness = 0; QVERIFY(overlay.SetFrame(f)); settle(); const auto image = host.grab().toImage();
        const QRect bounds = differenceBounds(empty,image); QVERIFY(bounds.width() / dpr >= 80); QVERIFY(bounds.width() / dpr <= 86); QVERIFY(bounds.height() / dpr >= 80); QVERIFY(bounds.height() / dpr <= 86);
        QVERIFY(std::abs(bounds.center().x() / dpr - 180) <= 1); QVERIFY(std::abs(bounds.center().y() / dpr - 120) <= 1);
        const QString directory = qEnvironmentVariable("VERTEX_UI_SCREENSHOTS");
        if (!directory.isEmpty()) {
            auto displayed = frame(); displayed.brushCenter = QPointF(270,165); displayed.brushRadius = 38; displayed.lasso = {{30,110},{160,130},{120,210},{50,180}};
            QVERIFY(overlay.SetFrame(displayed)); settle(); host.grab().save(directory + "/overlay-native-light.png");
            qApp->setStyleSheet(Theme::globalStyleSheet(true)); settle(); host.grab().save(directory + "/overlay-native-dark.png");
        }
    }
    void outlinesRemainVisibleWithLowContrastSystemHighlight_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void outlinesRemainVisibleWithLowContrastSystemHighlight()
    {
        QFETCH(bool,dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); Host host; host.resize(360,240); Overlay overlay(&host);
        auto f = frame(); f.points = {{50,210},{100,210}}; f.hard = {0}; f.weights = {1,1};
        f.rectangle = QRectF(25,60,95,35); f.lasso = {{25,130},{125,130},{100,180}}; f.brushCenter = QPointF(265,145); f.brushRadius = 35; f.brushHardness = .5;
        QVERIFY(overlay.SetFrame(f)); host.show(); overlay.show(); settle();
        const QColor background = host.grab().toImage().pixelColor(0,0); auto palette = overlay.palette(); palette.setColor(QPalette::Highlight, background); overlay.setPalette(palette); settle(); const auto image = host.grab().toImage();
        for (const auto& region : {QRect(40,58,40,5), QRect(40,128,40,5), QRect(258,108,14,5), QRect(48,208,4,4), QRect(98,208,4,4)})
            QVERIFY2(maximumContrast(image,region,background) >= 3, qPrintable(QString("low contrast at (%1,%2): %3").arg(region.x()).arg(region.y()).arg(maximumContrast(image,region,background))));
    }
    void staleSizeAndTransparentInputDoNotIntercept()
    {
        Host host; host.resize(360,240); Overlay overlay(&host); QVERIFY(overlay.SetFrame(frame())); host.show(); overlay.show(); settle(); QVERIFY(overlay.testAttribute(Qt::WA_TransparentForMouseEvents)); QCOMPARE(overlay.focusPolicy(), Qt::NoFocus);
        QVERIFY(host.childAt(40,50) != &overlay); QTest::mouseClick(&host, Qt::LeftButton, {}, QPoint(40,50)); QCOMPARE(host.presses, 1);
        overlay.resize(400,240); const auto stale = render(overlay); overlay.Clear(); QCOMPARE(render(overlay), stale);
    }
    void subpixelValuesAreNotSilentlyMerged_data()
    {
        QTest::addColumn<int>("field");
        const QStringList fields{"point-x", "point-y", "brush-x", "brush-y", "lasso-x", "lasso-y", "rectangle-x", "rectangle-y", "rectangle-width", "rectangle-height"};
        for (int field = 0; field < fields.size(); ++field) QTest::newRow(qPrintable(fields[field])) << field;
    }
    void subpixelValuesAreNotSilentlyMerged()
    {
        QFETCH(int, field);
        Overlay overlay; auto f = frame(); f.rectangle = QRectF(20, 25, 33, 41); f.brushCenter = QPointF(50, 70); f.lasso = {{30, 32}, {47, 62}};
        QVERIFY(overlay.SetFrame(f)); render(overlay);
        const auto next = [](qreal value) { return std::nextafter(value, 100.); };
        if (field == 0) f.points[0].setX(next(f.points[0].x()));
        if (field == 1) f.points[0].setY(next(f.points[0].y()));
        if (field == 2) f.brushCenter->setX(next(f.brushCenter->x()));
        if (field == 3) f.brushCenter->setY(next(f.brushCenter->y()));
        if (field == 4) f.lasso[0].setX(next(f.lasso[0].x()));
        if (field == 5) f.lasso[0].setY(next(f.lasso[0].y()));
        if (field == 6) f.rectangle->moveLeft(next(f.rectangle->x()));
        if (field == 7) f.rectangle->moveTop(next(f.rectangle->y()));
        if (field == 8) f.rectangle->setWidth(next(f.rectangle->width()));
        if (field == 9) f.rectangle->setHeight(next(f.rectangle->height()));
        QVERIFY(overlay.SetFrame(f)); const auto& current = overlay.CurrentFrame();
        QVERIFY(current.points[0].x() == f.points[0].x()); QVERIFY(current.points[0].y() == f.points[0].y());
        QVERIFY(current.brushCenter->x() == f.brushCenter->x()); QVERIFY(current.brushCenter->y() == f.brushCenter->y());
        QVERIFY(current.lasso[0].x() == f.lasso[0].x()); QVERIFY(current.lasso[0].y() == f.lasso[0].y());
        QVERIFY(current.rectangle->x() == f.rectangle->x()); QVERIFY(current.rectangle->y() == f.rectangle->y());
        QVERIFY(current.rectangle->width() == f.rectangle->width()); QVERIFY(current.rectangle->height() == f.rectangle->height());
    }
    void parentDestructionIsSafe() { auto* parent = new Host; QPointer<Overlay> overlay = new Overlay(parent); QVERIFY(overlay->SetFrame(frame())); delete parent; QVERIFY(!overlay); }
};
QTEST_MAIN(VertexSelectionOverlayTests)
#include "vertex_selection_overlay_tests.moc"
