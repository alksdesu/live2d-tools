#include "ui/cartworksourceview.h"
#include "ui/theme.h"
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
QImage image(QSize size, QColor color)
{
    QImage result(size, QImage::Format_RGBA8888); result.fill(color); return result;
}
void initialize(CArtworkSourceView& view, QTransform mapping = QTransform::fromTranslate(-240, -115))
{
    QString error;
    QVERIFY2(view.SetSource(image({320, 120}, {240, 150, 65, 170}), {-80, -30, 320, 120}, {640, 480}, &error), qPrintable(error));
    QVERIFY2(view.SetReference(image({80, 40}, {20, 115, 220, 130}), {200, 100, 80, 40}, mapping, &error), qPrintable(error));
    QVERIFY(view.SetMeshOverlay({{200, 100}, {280, 100}, {280, 140}, {200, 140}}, {0, 1, 2, 0, 2, 3}));
}
void stable(CArtworkSourceView& view, QRectF expected = {-80, -30, 720, 510})
{
    const auto fits = [&] {
        const int margin = view.fontMetrics().height();
        const double wanted = std::min((view.width() - 2.0 * margin) / expected.width(), (view.height() - 2.0 * margin) / expected.height());
        const auto unit = view.DocumentToViewport({1, 0}) - view.DocumentToViewport({0, 0});
        return view.isVisible() && view.width() > margin * 2 && view.height() > margin * 2 && std::abs(unit.x() - wanted) < 1e-10;
    };
    QTRY_VERIFY(fits()); const auto before = view.DocumentToViewport({0, 0}); QTest::qWait(20); QVERIFY(fits()); QCOMPARE(view.DocumentToViewport({0, 0}), before);
}
void show(CArtworkSourceView& view)
{
    view.resize(900, 650); view.show(); stable(view);
}
void mouse(CArtworkSourceView* view, QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {})
{
    QMouseEvent event(type, point, view->mapToGlobal(point.toPoint()), button, buttons, modifiers); QApplication::sendEvent(view, &event);
}
QPointF referenceCenter(CArtworkSourceView& view) { return view.DocumentToViewport(view.Mapping().map(QPointF(240, 120))); }
void start(CArtworkSourceView& view, QPointF delta = {24, 12}, Qt::KeyboardModifiers modifiers = {})
{
    const auto press = referenceCenter(view); const auto document = view.ViewportToDocument(press);
    mouse(&view, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    mouse(&view, QEvent::MouseMove, view.DocumentToViewport(document + delta), Qt::NoButton, Qt::LeftButton, modifiers);
}
void closePoint(QPointF actual, QPointF expected, double tolerance = 1e-9)
{
    QVERIFY2(std::abs(actual.x() - expected.x()) <= tolerance && std::abs(actual.y() - expected.y()) <= tolerance,
             qPrintable(QString("actual %1,%2 expected %3,%4").arg(actual.x(), 0, 'g', 17).arg(actual.y(), 0, 'g', 17).arg(expected.x(), 0, 'g', 17).arg(expected.y(), 0, 'g', 17)));
}
QColor rendered(CArtworkSourceView& view, QPointF document)
{
    const auto frame = view.grab().toImage(); const auto at = view.DocumentToViewport(document) * frame.devicePixelRatio();
    return frame.pixelColor(int(std::floor(at.x())), int(std::floor(at.y())));
}
QColor over(QColor background, QColor foreground, double opacity)
{
    const double alpha = foreground.alphaF() * opacity;
    return QColor(qRound(foreground.red() * alpha + background.red() * (1 - alpha)),
                  qRound(foreground.green() * alpha + background.green() * (1 - alpha)),
                  qRound(foreground.blue() * alpha + background.blue() * (1 - alpha)));
}
void closeColor(QColor actual, QColor expected)
{
    QVERIFY2(std::abs(actual.red() - expected.red()) <= 3 && std::abs(actual.green() - expected.green()) <= 3 && std::abs(actual.blue() - expected.blue()) <= 3,
             qPrintable(QString("actual %1 expected %2").arg(actual.name(), expected.name())));
}
}

class ArtworkSourceViewTests : public QObject
{
    Q_OBJECT
private slots:
    void contentReplacementValidatesEveryFieldAtomically()
    {
        CArtworkSourceView view; QImage source(3, 2, QImage::Format_RGBA8888); source.fill(QColor(120, 20, 80, 100));
        QImage reference(4, 5, QImage::Format_RGBA8888); reference.fill(QColor(10, 150, 60, 90));
        CArtworkSourceView::Content initial{source, reference, {-3, -5, 3, 2}, {1, 2, 4, 5}, {20, 10}, QTransform::fromTranslate(4, 8), {{1, 2}, {5, 2}, {5, 7}}, {0, 1, 2}};
        QVERIFY(view.SetContent(initial)); const auto mapping = view.Mapping();
        auto invalid = initial; invalid.atlasToDocument.translate(10, 20); invalid.sourceBounds.translate(99, 99); invalid.triangles = {0, 1, 300};
        QString error; QVERIFY(!view.SetContent(invalid, &error)); QCOMPARE(view.Mapping(), mapping);
        const auto coordinate = view.CoordinatesAt(view.DocumentToViewport({-2, -4})); QCOMPARE(coordinate.layerLocal, QPointF(1, 1)); QVERIFY(coordinate.onLayer);
        invalid = initial; invalid.documentCanvas = {}; QVERIFY(!view.SetContent(invalid)); QCOMPARE(view.Mapping(), mapping);
        invalid = initial; invalid.reference = QImage(1, 1, QImage::Format_RGBA8888); QVERIFY(!view.SetContent(invalid)); QCOMPARE(view.Mapping(), mapping);
        invalid = initial; invalid.atlasToDocument = QTransform::fromScale(0, 0); QVERIFY(!view.SetContent(invalid)); QCOMPARE(view.Mapping(), mapping);
        auto next = initial; next.atlasToDocument.rotate(90); QVERIFY(view.SetContent(next)); QCOMPARE(view.Mapping(), next.atlasToDocument);
    }
    void firstShowFitsNegativeSourceAndNonSquareCanvas_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {580, 1100}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void firstShowFitsNegativeSourceAndNonSquareCanvas()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        const auto previous = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        const auto restore = qScopeGuard([previous] { qApp->setStyleSheet(previous); });
        QWidget parent; auto* layout = new QVBoxLayout(&parent); auto* view = new CArtworkSourceView(&parent); layout->addWidget(view); initialize(*view);
        parent.resize(width, 650); parent.show(); stable(*view);
        const auto top = view->DocumentToViewport({-80, -30}), bottom = view->DocumentToViewport({640, 480});
        QVERIFY(top.x() >= 0 && top.y() >= 0); QVERIFY(bottom.x() <= view->width() && bottom.y() <= view->height());
        if (qEnvironmentVariableIsSet("LIVE2D_ARTWORK_SOURCE_SCREENSHOTS")) {
            const auto directory = qEnvironmentVariable("LIVE2D_ARTWORK_SOURCE_SCREENSHOTS"); QDir().mkpath(directory);
            QVERIFY(parent.grab().save(directory + QString("/source-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
    }
    void coordinatesDistinguishDocumentLayerAndAtlasUnderRotation()
    {
        QTransform mapping; mapping.translate(30, -40); mapping.rotate(31); mapping.scale(1.2, .65);
        CArtworkSourceView view; initialize(view, mapping); view.resize(900, 650); view.show(); QTest::qWait(30);
        const QPointF atlas(227.25, 118.75); const auto document = mapping.map(atlas);
        const auto hit = view.CoordinatesAt(view.DocumentToViewport(document));
        closePoint(hit.document, document); closePoint(hit.atlas, atlas); closePoint(hit.layerLocal, document + QPointF(80, 30));
        QVERIFY(hit.hasAtlas); QVERIFY(hit.onReference);
        const auto negative = view.CoordinatesAt(view.DocumentToViewport({-64.25, -20.75})); closePoint(negative.layerLocal, {15.75, 9.25}); QVERIFY(negative.onLayer);
        const auto outside = view.CoordinatesAt(view.DocumentToViewport(mapping.map(QPointF(280.5, 120)))); QVERIFY(!outside.onReference);
        QSignalSpy pointer(&view, &CArtworkSourceView::PointerMoved);
        mouse(&view, QEvent::MouseMove, view.DocumentToViewport(document), Qt::NoButton, {});
        QCOMPARE(pointer.size(), 1); closePoint(pointer[0][0].toPointF(), document); closePoint(pointer[0][1].toPointF(), document + QPointF(80, 30)); closePoint(pointer[0][2].toPointF(), atlas);
    }
    void invalidInputKeepsTheAcceptedMapping_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 6; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void invalidInputKeepsTheAcceptedMapping()
    {
        QFETCH(int, fault); CArtworkSourceView view; initialize(view); const auto before = view.Mapping(); QString error; bool accepted = true;
        if (fault == 0) accepted = view.SetMapping(QTransform(0, 0, 0, 0, 0, 0), &error);
        if (fault == 1) accepted = view.SetMapping(QTransform(1, 0, .1, 0, 1, 0, 0, 0, 1), &error);
        if (fault == 2) accepted = view.SetMapping(QTransform::fromTranslate(std::numeric_limits<double>::infinity(), 0), &error);
        if (fault == 3) accepted = view.SetMapping(QTransform::fromScale(1, 1e-14), &error);
        if (fault == 4) accepted = view.SetReference(image({40, 20}, Qt::red), {10, 20, 20, 40}, {}, &error);
        if (fault == 5) accepted = view.SetSource(image({40, 20}, Qt::red), {0, 0, 40, 20}, {0, 0}, &error);
        QVERIFY(!accepted); QVERIFY(!error.isEmpty()); QCOMPARE(view.Mapping(), before);
    }
    void highUnsignedTriangleIndicesRemainValidAndBadIndicesAreRejected()
    {
        CArtworkSourceView view; initialize(view); QVector<QPointF> points(65536, QPointF(210, 110));
        points[32768] = {205, 105}; points[65535] = {270, 105}; points[1] = {205, 135}; QString error;
        QVERIFY(view.SetMeshOverlay(points, {32768, 65535, 1}, &error)); view.resize(600, 500); view.show(); QTest::qWait(20); QVERIFY(!view.grab().isNull());
        points.removeLast(); QVERIFY(!view.SetMeshOverlay(points, {32768, 65535, 1}, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!view.SetMeshOverlay({QPointF(std::numeric_limits<double>::quiet_NaN(), 1)}, {}, &error));
    }
    void transparentBackgroundAndIndependentOpacitiesPreserveRgbaInputs_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void transparentBackgroundAndIndependentOpacitiesPreserveRgbaInputs()
    {
        QFETCH(bool, dark); const auto old = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); const auto reset = qScopeGuard([old] { qApp->setStyleSheet(old); });
        const QColor sourceColor(240, 80, 40, 128), referenceColor(30, 100, 220, 96);
        auto source = image({64, 32}, sourceColor), reference = image({32, 16}, referenceColor); source.setPixelColor(0, 0, QColor(77, 55, 33, 0));
        const auto originalSource = source.copy(), originalReference = reference.copy(); CArtworkSourceView view;
        QVERIFY(view.SetSource(source, {-16, 8, 64, 32}, {96, 64})); QVERIFY(view.SetReference(reference, {10, 20, 32, 16}, QTransform::fromTranslate(-20, -12)));
        view.resize(640, 480); view.show(); stable(view, {-16, 0, 112, 64});
        const QPointF sample(2, 15); QVERIFY(view.SetSourceOpacity(0)); QVERIFY(view.SetReferenceOpacity(0)); const auto background = rendered(view, sample);
        const auto checker = view.grab().toImage(); const int cell = std::max(2, view.fontMetrics().height() / 2); const double dpr = checker.devicePixelRatio();
        QVERIFY(checker.pixelColor(int(cell * .5 * dpr), int(cell * .5 * dpr)) != checker.pixelColor(int(cell * 1.5 * dpr), int(cell * .5 * dpr)));
        QVERIFY(view.SetSourceOpacity(1)); closeColor(rendered(view, sample), over(background, sourceColor, 1));
        QVERIFY(view.SetReferenceOpacity(.5)); closeColor(rendered(view, sample), over(over(background, sourceColor, 1), referenceColor, .5));
        QVERIFY(view.SetSourceOpacity(.25)); closeColor(rendered(view, sample), over(over(background, sourceColor, .25), referenceColor, .5));
        QVERIFY(!view.SetReferenceOpacity(-1)); QVERIFY(!view.SetSourceOpacity(std::numeric_limits<double>::quiet_NaN()));
        QCOMPARE(view.SourceOpacity(), .25); QCOMPARE(view.ReferenceOpacity(), .5); QCOMPARE(source, originalSource); QCOMPARE(reference, originalReference);
    }
    void rotatedReferenceUsesAtlasBoundsOriginAndPixelCenters()
    {
        QImage reference(2, 3, QImage::Format_RGBA8888);
        const QColor colors[] = {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta, Qt::cyan};
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x) reference.setPixelColor(x, y, colors[y * 2 + x]);
        CArtworkSourceView view; QVERIFY(view.SetSource({}, {}, {100, 100}));
        const QTransform mapping(0, 10, -10, 0, 370, -180); QVERIFY(view.SetReference(reference, {20, 30, 2, 3}, mapping)); QVERIFY(view.SetReferenceOpacity(1));
        view.resize(640, 520); view.show(); stable(view, {0, 0, 100, 100});
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x) closeColor(rendered(view, mapping.map(QPointF(20 + x + .5, 30 + y + .5))), colors[y * 2 + x]);
    }
    void dragTranslatesInDocumentSpaceAndCommitsOnce()
    {
        QTransform mapping; mapping.translate(-180, -140); mapping.rotate(20); mapping.scale(.8, 1.3);
        CArtworkSourceView view; initialize(view, mapping); view.resize(900, 650); view.show(); QTest::qWait(30);
        auto published = mapping; int previews = 0, commits = 0;
        view.SetPreviewHandler([&](const auto& next, QString*) { published = next; ++previews; return true; });
        view.SetCommitHandler([&](const auto& next, QString*) { ++commits; return next == published; });
        const auto press = referenceCenter(view), document = view.ViewportToDocument(press); const QPointF delta(24.5, -13.25);
        start(view, delta); QCOMPARE(commits, 0); QVERIFY(previews > 0); closePoint(published.map(QPointF()), mapping.map(QPointF()) + delta);
        QCOMPARE(published.m11(), mapping.m11()); QCOMPARE(published.m12(), mapping.m12()); QCOMPARE(published.m21(), mapping.m21()); QCOMPARE(published.m22(), mapping.m22());
        mouse(&view, QEvent::MouseButtonRelease, view.DocumentToViewport(document + delta), Qt::LeftButton, {});
        QCOMPARE(commits, 1); QVERIFY(!view.HasGesture()); QCOMPARE(view.Mapping(), published);
    }
    void shiftConstrainsTheDominantDocumentAxis()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping();
        start(view, {30, 12}, Qt::ShiftModifier); closePoint(view.Mapping().map(QPointF()), before.map(QPointF()) + QPointF(30, 0));
        QVERIFY(view.CancelGesture()); QCOMPARE(view.Mapping(), before);
    }
    void resizingDuringDragKeepsTheOriginalDocumentAnchor()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping();
        const auto anchor = view.ViewportToDocument(referenceCenter(view)); start(view);
        view.resize(1000, 720); QTest::qWait(10); const auto origin = view.DocumentToViewport({0, 0});
        view.FitAll(); QTest::qWait(20); QCOMPARE(view.DocumentToViewport({0, 0}), origin);
        const QPointF delta(47.5, 23.25);
        mouse(&view, QEvent::MouseMove, view.DocumentToViewport(anchor + delta), Qt::NoButton, Qt::LeftButton);
        closePoint(view.Mapping().map(QPointF()), before.map(QPointF()) + delta); QVERIFY(view.CancelGesture()); QCOMPARE(view.Mapping(), before);
    }
    void returningToOriginDoesNotCreateAnOperation()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping(); int commits = 0;
        QSignalSpy finished(&view, &CArtworkSourceView::GestureFinished);
        view.SetCommitHandler([&](const auto&, QString*) { ++commits; return true; }); const auto press = referenceCenter(view);
        start(view); QVERIFY(view.Mapping() != before);
        mouse(&view, QEvent::MouseMove, press, Qt::NoButton, Qt::LeftButton); mouse(&view, QEvent::MouseButtonRelease, press, Qt::LeftButton, {});
        QCOMPARE(view.Mapping(), before); QCOMPARE(commits, 0); QVERIFY(!view.HasGesture()); QCOMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toBool(), false);
    }
    void escapeRestoresTheAcceptedPreviewAndNoCommit()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping(); auto published = before; int commits = 0, cancels = 0;
        QSignalSpy finished(&view, &CArtworkSourceView::GestureFinished);
        view.SetPreviewHandler([&](const auto& next, QString*) { published = next; return true; });
        view.SetCommitHandler([&](const auto&, QString*) { ++commits; return true; });
        view.SetCancellationHandler([&](QString*) { ++cancels; published = before; return true; });
        start(view); QVERIFY(published != before); QTest::keyClick(&view, Qt::Key_Escape);
        QCOMPARE(view.Mapping(), before); QCOMPARE(published, before); QCOMPARE(commits, 0); QCOMPARE(cancels, 1); QVERIFY(!view.HasGesture()); QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
    }
    void rejectedOrThrowingPreviewKeepsTheLastAcceptedMapping_data()
    {
        QTest::addColumn<bool>("exception"); QTest::newRow("false") << false; QTest::newRow("throw") << true;
    }
    void rejectedOrThrowingPreviewKeepsTheLastAcceptedMapping()
    {
        QFETCH(bool, exception); CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping(); bool reject = false;
        view.SetPreviewHandler([&](const auto&, QString* error) { if (reject) { if (exception) throw std::runtime_error("controlled"); *error = "rejected"; return false; } return true; });
        const auto press = referenceCenter(view), document = view.ViewportToDocument(press); start(view); const auto accepted = view.Mapping(); reject = true;
        mouse(&view, QEvent::MouseMove, view.DocumentToViewport(document + QPointF(40, 20)), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(view.Mapping(), accepted); QVERIFY(!view.LastError().isEmpty()); QVERIFY(view.HasGesture()); reject = false;
        QVERIFY(view.CancelGesture()); QCOMPARE(view.Mapping(), before);
    }
    void failedCommitAndCancellationRemainRetryable_data()
    {
        QTest::addColumn<bool>("exception"); QTest::newRow("false") << false; QTest::newRow("throw") << true;
    }
    void failedCommitAndCancellationRemainRetryable()
    {
        QFETCH(bool, exception);
        CArtworkSourceView view; initialize(view); show(view); const auto original = view.Mapping(); bool rejectCommit = true, rejectCancel = true; int commits = 0;
        view.SetCommitHandler([&](const auto&, QString* error) { if (rejectCommit) { if (exception) throw std::runtime_error("controlled commit"); *error = "commit refused"; return false; } ++commits; return true; });
        view.SetCancellationHandler([&](QString* error) { if (rejectCancel) { if (exception) throw std::runtime_error("controlled cancel"); *error = "cancel refused"; return false; } return true; });
        start(view); const auto accepted = view.Mapping(); QString error;
        QVERIFY(!view.FinishGesture(&error)); QVERIFY(view.HasGesture()); QCOMPARE(view.Mapping(), accepted);
        QVERIFY(!view.CancelGesture(&error)); QVERIFY(view.HasGesture()); QCOMPARE(view.Mapping(), accepted);
        rejectCancel = false; QVERIFY(view.CancelGesture(&error)); QCOMPARE(view.Mapping(), original); QVERIFY(!view.HasGesture());
        start(view); rejectCommit = false; QVERIFY(view.FinishGesture(&error)); QCOMPARE(commits, 1); QVERIFY(!view.HasGesture());
    }
    void reflectedReferenceCoordinatesAndProgrammaticMappingKeepTheViewport()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto origin = view.DocumentToViewport({0, 0});
        const QTransform mapping(-1.5, .2, .4, 1.2, 300, -200); QVERIFY(view.SetMapping(mapping));
        const QPointF atlas(241.75, 117.25); const auto hit = view.CoordinatesAt(view.DocumentToViewport(mapping.map(atlas)));
        closePoint(hit.atlas, atlas); closePoint(hit.document, mapping.map(atlas)); QVERIFY(hit.onReference);
        QTest::qWait(20); QCOMPARE(view.DocumentToViewport({0, 0}), origin);
    }
    void keyboardRepeatsProduceOneCommitAndFocusLossCancels()
    {
        CArtworkSourceView view; initialize(view); show(view); const auto before = view.Mapping(); int commits = 0;
        view.SetCommitHandler([&](const auto&, QString*) { ++commits; return true; });
        QKeyEvent first(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier, {}, false); QApplication::sendEvent(&view, &first);
        QKeyEvent repeated(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier, {}, true); QApplication::sendEvent(&view, &repeated);
        QCOMPARE(commits, 0); closePoint(view.Mapping().map(QPointF()), before.map(QPointF()) + QPointF(20, 0));
        QKeyEvent released(QEvent::KeyRelease, Qt::Key_Right, Qt::ShiftModifier); QApplication::sendEvent(&view, &released);
        QCOMPARE(commits, 1); QVERIFY(!view.HasGesture()); const auto accepted = view.Mapping();
        start(view); QFocusEvent blur(QEvent::FocusOut); QApplication::sendEvent(&view, &blur);
        QCOMPARE(view.Mapping(), accepted); QCOMPARE(commits, 1); QVERIFY(!view.HasGesture());
    }
    void gestureBlocksSourceReplacementAndPublisherReentry()
    {
        CArtworkSourceView view; initialize(view); show(view); bool reentry = true, nestedFinish = true; const auto original = view.Mapping();
        view.SetPreviewHandler([&](const auto&, QString*) { reentry = view.SetMapping({}); nestedFinish = view.FinishGesture(); return true; });
        start(view); QVERIFY(!reentry); QVERIFY(!nestedFinish); QString error;
        QVERIFY(!view.SetSource(image({10, 10}, Qt::red), {0, 0, 10, 10}, {20, 20}, &error));
        QVERIFY(!view.SetReference({}, {}, {}, &error)); QVERIFY(!view.SetMeshOverlay({}, {}, &error));
        QVERIFY(view.CancelGesture()); QCOMPARE(view.Mapping(), original);
    }
    void wheelZoomKeepsTheDocumentPointUnderTheCursor()
    {
        CArtworkSourceView view; initialize(view); show(view); const QPointF anchor(273.25, 188.5); const auto document = view.ViewportToDocument(anchor); const auto mapping = view.Mapping();
        const double before = (view.DocumentToViewport({1, 0}) - view.DocumentToViewport({0, 0})).x();
        QWheelEvent event(anchor, view.mapToGlobal(anchor.toPoint()), {}, {0, 120}, {}, Qt::NoModifier, Qt::NoScrollPhase, false); QApplication::sendEvent(&view, &event);
        closePoint(view.ViewportToDocument(anchor), document); QCOMPARE(view.Mapping(), mapping);
        const double after = (view.DocumentToViewport({1, 0}) - view.DocumentToViewport({0, 0})).x(); QVERIFY(std::abs(after / before - 1.15) < 1e-10);
    }
    void navigationCancelsQueuedFitAndSurvivesReshow_data()
    {
        QTest::addColumn<bool>("pan"); QTest::newRow("pan") << true; QTest::newRow("zoom") << false;
    }
    void navigationCancelsQueuedFitAndSurvivesReshow()
    {
        QFETCH(bool, pan); CArtworkSourceView view; initialize(view); view.resize(700, 500); view.show();
        if (pan) {
            mouse(&view, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
            mouse(&view, QEvent::MouseMove, {130, 110}, Qt::NoButton, Qt::MiddleButton); mouse(&view, QEvent::MouseButtonRelease, {130, 110}, Qt::MiddleButton, {});
        } else {
            QWheelEvent event({120, 150}, view.mapToGlobal(QPoint(120, 150)), {}, {0, 120}, {}, Qt::NoModifier, Qt::NoScrollPhase, false); QApplication::sendEvent(&view, &event);
        }
        const auto origin = view.DocumentToViewport({0, 0}), unit = view.DocumentToViewport({1, 1}) - origin;
        QTest::qWait(30); QCOMPARE(view.DocumentToViewport({0, 0}), origin); QCOMPARE(view.DocumentToViewport({1, 1}) - origin, unit);
        view.hide(); view.show(); QTest::qWait(30); QCOMPARE(view.DocumentToViewport({0, 0}), origin); QCOMPARE(view.DocumentToViewport({1, 1}) - origin, unit);
    }
    void callbacksMayDestroyTheViewOrParent_data()
    {
        QTest::addColumn<int>("phase"); QTest::addColumn<bool>("parent");
        for (int phase = 0; phase < 3; ++phase) for (bool parent : {false, true}) QTest::newRow(qPrintable(QString("%1-%2").arg(phase).arg(parent))) << phase << parent;
    }
    void callbacksMayDestroyTheViewOrParent()
    {
        QFETCH(int, phase); QFETCH(bool, parent); QPointer<QWidget> owner = new QWidget; auto* layout = new QVBoxLayout(owner);
        QPointer<CArtworkSourceView> view = new CArtworkSourceView(owner); layout->addWidget(view); initialize(*view); owner->resize(900, 650); owner->show(); stable(*view);
        const auto destroy = [&] { if (parent) delete owner.data(); else delete view.data(); return true; };
        if (phase == 0) view->SetPreviewHandler([&](const auto&, QString*) { return destroy(); });
        if (phase == 1) view->SetCommitHandler([&](const auto&, QString*) { return destroy(); });
        if (phase == 2) view->SetCancellationHandler([&](QString*) { return destroy(); });
        start(*view);
        if (phase == 1) view->FinishGesture(); if (phase == 2) view->CancelGesture();
        QVERIFY(!view); if (parent) QVERIFY(!owner); else delete owner.data();
    }
    void pendingFitIsDiscardedWhenTheParentIsDestroyed()
    {
        auto* parent = new QWidget; auto* layout = new QVBoxLayout(parent); QPointer<CArtworkSourceView> view = new CArtworkSourceView(parent); layout->addWidget(view); initialize(*view);
        parent->show(); delete parent; QCoreApplication::processEvents(); QVERIFY(!view);
    }
};
QTEST_MAIN(ArtworkSourceViewTests)
#include "artwork_source_view_tests.moc"
