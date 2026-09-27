#include "live2dstruct/cartworksampler.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <cmath>
#include <limits>
using namespace live2dstruct;
namespace {
using Boundary = CModelArtwork::Boundary; using Filter = CModelArtwork::Filter;
QImage sample(const QImage& source, const QTransform& transform, QSize size, Boundary boundary = Boundary::Transparent,
              Filter filter = Filter::ExactTexels, quint8 opacity = 255) {
    QImage result; QString error;
    if (!CArtworkSampler::Render(source, transform, size, boundary, filter, opacity, result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
QImage pattern(int width = 3, int height = 2) {
    QImage result(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        result.setPixel(x, y, qRgba((20 + x * 51) % 256, (31 + y * 79) % 256, (17 + x * 11 + y * 23) % 256, x ? (63 + y * 91) % 256 : 0));
    return result;
}
}
class ArtworkSamplerTests : public QObject {
    Q_OBJECT
private slots:
    void exactIdentityOwnsPixelsAndPreservesTransparentRgb_data() {
        QTest::addColumn<int>("format"); QTest::newRow("argb") << int(QImage::Format_ARGB32); QTest::newRow("rgba") << int(QImage::Format_RGBA8888);
    }
    void exactIdentityOwnsPixelsAndPreservesTransparentRgb() {
        QFETCH(int, format); auto source = pattern().convertToFormat(QImage::Format(format));
        const auto expected = source.convertToFormat(QImage::Format_RGBA8888); const auto result = sample(source, {}, source.size());
        QCOMPARE(result.format(), QImage::Format_RGBA8888); QCOMPARE(result, expected); source.fill(Qt::white); QCOMPARE(result, expected);
        QCOMPARE(result.pixel(0, 0), qRgba(20, 31, 17, 0));
    }
    void exactRotationMirrorAndNonSquareRepeat() {
        const auto source = pattern();
        const auto rotated = sample(source, QTransform(0, -1, 1, 0, 0, 2), QSize(2, 3));
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x) QCOMPARE(rotated.pixel(x, y), source.pixel(y, 1 - x));
        const auto mirrored = sample(source, QTransform(-1, 0, 0, 1, 3, 0), source.size());
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) QCOMPARE(mirrored.pixel(x, y), source.pixel(2 - x, y));
        const auto repeated = sample(source, QTransform::fromTranslate(-4, 3), QSize(7, 5), Boundary::Repeat);
        for (int y = 0; y < 5; ++y) for (int x = 0; x < 7; ++x) QCOMPARE(repeated.pixel(x, y), source.pixel((x + 2) % 3, (y + 1) % 2));
    }
    void largeFiniteCoordinatesNeverConvertToUnboundedIntegers() {
        const auto source = pattern(4, 2); const double large = std::ldexp(1.0, 60);
        for (auto filter : {Filter::ExactTexels, Filter::LinearLightPremultiplied}) {
            QCOMPARE(sample(source, QTransform::fromTranslate(large, -large), source.size(), Boundary::Repeat, filter),
                     source.convertToFormat(QImage::Format_RGBA8888));
            const auto outside = sample(source, QTransform::fromTranslate(large, -large), source.size(), Boundary::Transparent, filter);
            for (int y = 0; y < outside.height(); ++y) for (int x = 0; x < outside.width(); ++x) QCOMPARE(outside.pixel(x, y), QRgb(0));
        }
    }
    void linearLightInterpolationDoesNotAverageSrgbCodes() {
        QImage source(2, 1, QImage::Format_RGBA8888); source.setPixel(0, 0, qRgba(0, 0, 0, 255)); source.setPixel(1, 0, qRgba(255, 255, 255, 255));
        const auto result = sample(source, QTransform::fromTranslate(0.5, 0), QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied);
        QCOMPARE(result.pixel(0, 0), qRgba(188, 188, 188, 255));
    }
    void arbitraryRotationSamplesTheDeclaredSourcePoint() {
        QImage source(2, 2, QImage::Format_ARGB32);
        source.setPixel(0, 0, qRgb(255, 0, 0)); source.setPixel(1, 0, qRgb(0, 255, 0));
        source.setPixel(0, 1, qRgb(0, 0, 255)); source.setPixel(1, 1, qRgb(255, 255, 255));
        const double angle = 37 * std::acos(-1.0) / 180.0, c = std::cos(angle), s = std::sin(angle);
        const QTransform transform(c, s, -s, c, 0.75 - (c - s) * 0.5, 1.25 - (s + c) * 0.5);
        const auto result = sample(source, transform, QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied);
        QCOMPARE(result.pixel(0, 0), qRgba(165, 137, 225, 255));
    }
    void premultipliedFilteringIgnoresInvisibleColorsAndAppliesOpacityOnce() {
        QImage source(2, 1, QImage::Format_ARGB32); source.setPixel(0, 0, qRgba(255, 0, 0, 255)); source.setPixel(1, 0, qRgba(0, 255, 0, 0));
        const auto result = sample(source, QTransform::fromTranslate(0.5, 0), QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied, 128);
        QCOMPARE(result.pixel(0, 0), qRgba(255, 0, 0, 64)); QCOMPARE(source.pixel(1, 0), qRgba(0, 255, 0, 0));
        source.setPixel(0, 0, qRgba(255, 0, 0, 64)); source.setPixel(1, 0, qRgba(0, 0, 255, 192));
        QCOMPARE(sample(source, QTransform::fromTranslate(0.5, 0), QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied).pixel(0, 0), qRgba(137, 0, 225, 128));
        source.fill(qRgba(255, 19, 27, 0));
        QCOMPARE(sample(source, QTransform::fromTranslate(0.5, 0), QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied).pixel(0, 0), QRgb(0));
        QCOMPARE(sample(source, {}, source.size(), Boundary::Transparent, Filter::LinearLightPremultiplied).pixel(0, 0), source.pixel(0, 0));
    }
    void transparentBoundaryAndThinLinesRetainColorWithoutDarkFringe() {
        QImage source(3, 1, QImage::Format_RGBA8888); source.fill(qRgba(255, 0, 0, 0)); source.setPixel(1, 0, qRgba(255, 255, 255, 255));
        const auto result = sample(source, QTransform::fromScale(0.5, 1), QSize(6, 1), Boundary::Transparent, Filter::LinearLightPremultiplied);
        const int alpha[]{0, 64, 191, 191, 64, 0};
        for (int x = 0; x < 6; ++x) {
            QCOMPARE(qAlpha(result.pixel(x, 0)), alpha[x]);
            QCOMPARE(result.pixel(x, 0), alpha[x] ? qRgba(255, 255, 255, alpha[x]) : QRgb(0));
        }
        QImage one(1, 1, QImage::Format_RGBA8888); one.fill(Qt::white);
        QCOMPARE(sample(one, QTransform::fromTranslate(-0.5, 0), QSize(1, 1), Boundary::Transparent, Filter::LinearLightPremultiplied).pixel(0, 0), qRgba(255, 255, 255, 128));
        QCOMPARE(sample(one, QTransform::fromTranslate(-0.5, 0), QSize(1, 1), Boundary::Repeat, Filter::LinearLightPremultiplied).pixel(0, 0), qRgba(255, 255, 255, 255));
    }
    void repeatedScaleOperationsAlwaysStartFromOriginalSource() {
        const auto original = pattern(25, 25); const auto frozen = original.copy();
        const auto quarter = sample(original, QTransform::fromScale(4, 4), QSize(6, 6), Boundary::Transparent, Filter::LinearLightPremultiplied);
        const auto enlarged = sample(original, QTransform::fromScale(0.125, 0.125), QSize(200, 200), Boundary::Transparent, Filter::LinearLightPremultiplied);
        const auto restored = sample(original, {}, QSize(25, 25));
        QCOMPARE(restored, frozen.convertToFormat(QImage::Format_RGBA8888)); QCOMPARE(original, frozen);
        QCOMPARE(sample(original, QTransform::fromScale(4, 4), QSize(6, 6), Boundary::Transparent, Filter::LinearLightPremultiplied), quarter);
        QCOMPARE(sample(original, QTransform::fromScale(0.125, 0.125), QSize(200, 200), Boundary::Transparent, Filter::LinearLightPremultiplied), enlarged);
    }
    void borrowedSourceAndOptionsAreFrozenBeforeCallbacks() {
        QByteArray memory(3 * 2 * 4, Qt::Uninitialized); QImage source(reinterpret_cast<uchar*>(memory.data()), 3, 2, QImage::Format_RGBA8888);
        source.fill(qRgba(71, 82, 93, 64)); const auto expected = source.copy();
        QTransform transform; QSize destination(3, 2); CArtworkSampleLimits limits;
        CExportControl control; bool changed = false;
        control.SetHandler([&](const auto&, QString*) { changed = true; memory.fill(char(255)); transform.scale(0, 0); destination = {}; limits.maximumSamples = 0; return true; });
        QImage result; QString error;
        QVERIFY2(CArtworkSampler::Render(source, transform, destination, Boundary::Transparent, Filter::ExactTexels, 255, result, &error, &control, limits), qPrintable(error));
        QVERIFY(changed); QCOMPARE(result, expected); memory.clear(); QCOMPARE(result, expected);
    }
    void invalidModesMatricesAndBudgetsLeavePreviousResultIntact() {
        const auto source = pattern(); QImage result(1, 1, QImage::Format_RGBA8888); result.fill(Qt::magenta); const auto previous = result.copy(); QString error;
        const QVector<QTransform> transforms{QTransform::fromScale(0, 1), QTransform::fromScale(2, 2), QTransform::fromTranslate(0.25, 0),
            QTransform(1, 0, 0.1, 0, 1, 0, 0, 0, 1), QTransform::fromTranslate(std::numeric_limits<double>::infinity(), 0)};
        for (const auto& transform : transforms) {
            QVERIFY(!CArtworkSampler::Render(source, transform, source.size(), Boundary::Transparent, Filter::ExactTexels, 255, result, &error)); QCOMPARE(result, previous);
        }
        for (int kind = 0; kind < 4; ++kind) {
            CArtworkSampleLimits limits;
            if (kind == 0) limits.maximumSourcePixels = 5;
            if (kind == 1) limits.maximumDestinationPixels = 5;
            if (kind == 2) limits.maximumTotalPixels = 11;
            if (kind == 3) limits.maximumSamples = 23;
            QVERIFY(!CArtworkSampler::Render(source, {}, source.size(), Boundary::Transparent, Filter::LinearLightPremultiplied, 255, result, &error, nullptr, limits)); QCOMPARE(result, previous);
        }
        QVERIFY(!CArtworkSampler::Render(source.convertToFormat(QImage::Format_ARGB32_Premultiplied), {}, source.size(), Boundary::Transparent, Filter::ExactTexels, 255, result, &error)); QCOMPARE(result, previous);
        QVERIFY(!CArtworkSampler::Render({}, {}, source.size(), Boundary::Transparent, Filter::ExactTexels, 255, result, &error)); QCOMPARE(result, previous);
        QVERIFY(!CArtworkSampler::Render(source, {}, QSize(40000, 40000), Boundary::Transparent, Filter::ExactTexels, 255, result, &error)); QCOMPARE(result, previous);
    }
    void cancellationAfterPartialRowsDiscardsOutputAndAllowsRetry() {
        const auto source = pattern(16, 16); QImage result(1, 1, QImage::Format_RGBA8888); result.fill(Qt::magenta); const auto previous = result.copy();
        CExportControl control; bool reached = false;
        control.SetHandler([&](const auto& progress, QString*) { if (progress.phase == CExportControl::Phase::Rendering && progress.current == 8) { reached = true; control.Cancel(); } return true; });
        QString error; QVERIFY(!CArtworkSampler::Render(source, {}, source.size(), Boundary::Transparent, Filter::ExactTexels, 255, result, &error, &control));
        QVERIFY(reached); QVERIFY(control.WasCanceled()); QCOMPARE(result, previous); QCOMPARE(sample(source, {}, source.size()), source.convertToFormat(QImage::Format_RGBA8888));
    }
};
QTEST_GUILESS_MAIN(ArtworkSamplerTests)
#include "artwork_sampler_tests.moc"
