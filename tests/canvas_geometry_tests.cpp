#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QtEndian>
#include <QTemporaryDir>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
class CanvasFixture
{
public:
    CanvasFixture()
        : reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), offsets(reader),
          canvas(reader, *offsets.canvas_info) {}

    CReader reader;
    CSectionOffsetTable offsets;
    CCanvas_info canvas;
};

QPointF ndcPoint(const std::array<float, 16>& matrix, const QPointF& model)
{
    return QPointF(matrix[0] * model.x() + matrix[4] * model.y() + matrix[12],
                   matrix[1] * model.x() + matrix[5] * model.y() + matrix[13]);
}
}

class CanvasGeometryTests : public QObject
{
    Q_OBJECT
private slots:
    void pixelSizeRoundsWithinIntegerLimits_data()
    {
        QTest::addColumn<double>("width");
        QTest::addColumn<double>("height");
        QTest::addColumn<QSize>("expected");
        const double maximum = std::numeric_limits<int>::max();
        const double infinity = std::numeric_limits<double>::infinity();
        QTest::newRow("pixels") << 2400.0 << 3000.0 << QSize(2400, 3000);
        QTest::newRow("nearest-integer") << 1.49 << 2.5 << QSize(1, 3);
        QTest::newRow("minimum-rounded-pixel") << 0.5 << 0.5 << QSize(1, 1);
        QTest::newRow("rounds-to-zero") << 0.49 << 1.0 << QSize();
        QTest::newRow("zero") << 0.0 << 1.0 << QSize();
        QTest::newRow("negative") << -1.0 << 1.0 << QSize();
        QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN() << 1.0 << QSize();
        QTest::newRow("infinity") << infinity << 1.0 << QSize();
        QTest::newRow("negative-infinity") << -infinity << 1.0 << QSize();
        QTest::newRow("int-maximum") << maximum << maximum << QSize(INT_MAX, INT_MAX);
        QTest::newRow("above-int-maximum") << std::nextafter(maximum, infinity) << 1.0 << QSize();
        QTest::newRow("largest-float-int") << 2147483520.0 << 1.0 << QSize(2147483520, 1);
        QTest::newRow("float-rounded-int-maximum") << double(float(INT_MAX)) << 1.0 << QSize();
        QTest::newRow("float-maximum") << double(std::numeric_limits<float>::max()) << 1.0 << QSize();
    }

    void pixelSizeRoundsWithinIntegerLimits()
    {
        QFETCH(double, width);
        QFETCH(double, height);
        QFETCH(QSize, expected);
        QCOMPARE(CCanvasGeometry::PixelSize(width, height), expected);
        QCOMPARE(CCanvasGeometry::PixelSize(height, width), QSize(expected.height(), expected.width()));
    }

    void markCoordinatesUsePixelOrigins()
    {
        CanvasFixture fixture;
        QVERIFY(fixture.reader.IsValid());
        const auto& canvas = fixture.canvas;
        QCOMPARE(canvas.pixels_per_unit, 2400.0f);
        QCOMPARE(canvas.origin_x, 1200.0f);
        QCOMPARE(canvas.origin_y, 1500.0f);
        QCOMPARE(CCanvasGeometry::PixelSize(canvas.width, canvas.height), QSize(2400, 3000));
        QString error = QStringLiteral("previous error");
        QVERIFY2(CCanvasGeometry::Validate(&canvas, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QPointF pixels;
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(0, 0), pixels));
        QCOMPARE(pixels, QPointF(1200, 1500));
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(0.25, -0.25), pixels));
        QCOMPARE(pixels, QPointF(1800, 2100));
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(-0.5, 0.625), pixels));
        QCOMPARE(pixels, QPointF(0, 0));
    }

    void negativeOriginsAndPositiveSubnormalDensityAreValid()
    {
        CanvasFixture fixture;
        QVERIFY(fixture.reader.IsValid());
        auto& canvas = fixture.canvas;
        canvas.origin_x = -25.0f;
        canvas.origin_y = -40.0f;
        canvas.pixels_per_unit = 10.0f;
        QVERIFY(CCanvasGeometry::Validate(&canvas));
        QPointF pixels;
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(2, -3), pixels));
        QCOMPARE(pixels, QPointF(-5, -10));
        canvas.pixels_per_unit = std::numeric_limits<float>::denorm_min();
        QVERIFY(CCanvasGeometry::Validate(&canvas));
    }

    void mappingUsesDoubleBeforeMultiplicationAndFailsAtomically()
    {
        CanvasFixture fixture;
        QVERIFY(fixture.reader.IsValid());
        auto& canvas = fixture.canvas;
        canvas.origin_x = 0.0f;
        canvas.origin_y = 0.0f;
        canvas.pixels_per_unit = std::ldexp(1.0f, 100);
        QPointF pixels;
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(std::ldexp(1.0, 100), -std::ldexp(1.0, 100)), pixels));
        QCOMPARE(pixels, QPointF(std::ldexp(1.0, 200), std::ldexp(1.0, 200)));
        const float maximum = std::numeric_limits<float>::max();
        canvas.pixels_per_unit = maximum;
        QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, QPointF(maximum, -maximum), pixels));
        QVERIFY(std::isfinite(pixels.x()) && pixels.x() > 1.0e76);
        QCOMPARE(pixels.x(), pixels.y());

        const QPointF original(17, 23);
        const double infinity = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (QPointF invalid : {QPointF(infinity, 0), QPointF(0, -infinity), QPointF(nan, 0),
                               QPointF(0, nan), QPointF(std::numeric_limits<double>::max(), 0),
                               QPointF(0, -std::numeric_limits<double>::max())}) {
            pixels = original;
            QVERIFY(!CCanvasGeometry::ModelToPixels(&canvas, invalid, pixels));
            QCOMPARE(pixels, original);
        }
        pixels = original;
        QVERIFY(!CCanvasGeometry::ModelToPixels(nullptr, QPointF(), pixels));
        QCOMPARE(pixels, original);
        canvas.origin_x = std::numeric_limits<float>::infinity();
        QVERIFY(!CCanvasGeometry::ModelToPixels(&canvas, QPointF(maximum, maximum), pixels));
        QCOMPARE(pixels, original);
        QString error;
        QVERIFY(!CCanvasGeometry::Validate(nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("canvas_info")));
    }

    void ndcUsesColumnMajorOrderAndNonCentralPixelOrigins()
    {
        std::array<float, 16> matrix;
        matrix.fill(99.0f);
        QVERIFY(CCanvasGeometry::ModelToNdc(16, 16, 3, 5, 4, QSize(16, 16), matrix));
        const std::array<float, 16> expected{0.5f, 0, 0, 0, 0, 0.5f, 0, 0,
                                            0, 0, 1, 0, -0.625f, 0.375f, 0, 1};
        QVERIFY(matrix == expected);
        QCOMPARE(ndcPoint(matrix, QPointF(-0.75, 1.25)), QPointF(-1, 1));
        QCOMPARE(ndcPoint(matrix, QPointF(3.25, 1.25)), QPointF(1, 1));
        QCOMPARE(ndcPoint(matrix, QPointF(-0.75, -2.75)), QPointF(-1, -1));
        QCOMPARE(ndcPoint(matrix, QPointF(3.25, -2.75)), QPointF(1, -1));
        QCOMPARE(ndcPoint(matrix, QPointF()), QPointF(-0.625, 0.375));
        QCOMPARE(ndcPoint(matrix, QPointF(-0.5, 0.75)), QPointF(-0.875, 0.75));
        QCOMPARE(ndcPoint(matrix, QPointF(0.5, 0.75)), QPointF(-0.375, 0.75));
        QCOMPARE(ndcPoint(matrix, QPointF(-0.5, -0.25)), QPointF(-0.875, 0.25));
    }

    void ndcHandlesLandscapePortraitAndNegativeOrigins()
    {
        for (const QSize size : {QSize(16, 8), QSize(8, 16)}) {
            std::array<float, 16> matrix;
            QVERIFY(CCanvasGeometry::ModelToNdc(size.width(), size.height(), -4, -8, 4, size, matrix));
            const QPointF topLeft(1, -2);
            const QPointF bottomRight(1 + size.width() / 4.0, -2 - size.height() / 4.0);
            QCOMPARE(ndcPoint(matrix, topLeft), QPointF(-1, 1));
            QCOMPARE(ndcPoint(matrix, bottomRight), QPointF(1, -1));
            QCOMPARE(ndcPoint(matrix, (topLeft + bottomRight) / 2.0), QPointF());
            QVERIFY(matrix[0] > 0 && matrix[5] > 0);
        }
    }

    void ndcUsesRoundedOutputPixelsWithoutRescalingMetadata()
    {
        const QSize size = CCanvasGeometry::PixelSize(16.49, 7.5);
        QCOMPARE(size, QSize(16, 8));
        std::array<float, 16> matrix;
        QVERIFY(CCanvasGeometry::ModelToNdc(16.49, 7.5, 3, 5, 4, size, matrix));
        QCOMPARE(matrix[0], 0.5f);
        QCOMPARE(matrix[5], 1.0f);
        QCOMPARE(matrix[12], -0.625f);
        QCOMPARE(matrix[13], -0.25f);
        QCOMPARE(ndcPoint(matrix, QPointF(-0.75, 1.25)), QPointF(-1, 1));
        QCOMPARE(ndcPoint(matrix, QPointF(3.25, -0.75)), QPointF(1, -1));
        QVERIFY(CCanvasGeometry::ModelToNdc(0.5, 0.5, 0, 0, 1, QSize(1, 1), matrix));
        QCOMPARE(ndcPoint(matrix, QPointF()), QPointF(-1, 1));
        QCOMPARE(ndcPoint(matrix, QPointF(1, -1)), QPointF(1, -1));
    }

    void ndcMatchesFourSampleCanvasCoordinates_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<float>("ppu");
        QTest::newRow("Mark") << QString("Mark") << QSize(2400, 3000) << 2400.0f;
        QTest::newRow("Natori") << QString("Natori") << QSize(4000, 7000) << 4000.0f;
        QTest::newRow("Rice") << QString("Rice") << QSize(4500, 3000) << 3000.0f;
        QTest::newRow("Wanko") << QString("Wanko") << QSize(1200, 1200) << 1200.0f;
    }

    void ndcMatchesFourSampleCanvasCoordinates()
    {
        QFETCH(QString, name);
        QFETCH(QSize, size);
        QFETCH(float, ppu);
        CReader reader(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
        CSectionOffsetTable offsets(reader);
        CCanvas_info canvas(reader, *offsets.canvas_info);
        QVERIFY2(reader.IsValid(), qPrintable(reader.Error()));
        QCOMPARE(canvas.width, float(size.width()));
        QCOMPARE(canvas.height, float(size.height()));
        QCOMPARE(canvas.pixels_per_unit, ppu);
        QCOMPARE(canvas.origin_x, size.width() / 2.0f);
        QCOMPARE(canvas.origin_y, size.height() / 2.0f);
        std::array<float, 16> matrix;
        QVERIFY(CCanvasGeometry::ModelToNdc(canvas.width, canvas.height, canvas.origin_x,
                                           canvas.origin_y, canvas.pixels_per_unit, size, matrix));
        const double halfWidth = size.width() / (2.0 * ppu);
        const double halfHeight = size.height() / (2.0 * ppu);
        const std::array<QPointF, 5> models{
            QPointF(-halfWidth, halfHeight), QPointF(halfWidth, halfHeight),
            QPointF(-halfWidth, -halfHeight), QPointF(halfWidth, -halfHeight), QPointF()};
        const std::array<QPointF, 5> expected{
            QPointF(-1, 1), QPointF(1, 1), QPointF(-1, -1), QPointF(1, -1), QPointF()};
        for (std::size_t index = 0; index < models.size(); ++index) {
            const QPointF ndc = ndcPoint(matrix, models[index]);
            QVERIFY(std::abs(ndc.x() - expected[index].x()) < 1.0e-6);
            QVERIFY(std::abs(ndc.y() - expected[index].y()) < 1.0e-6);
            QPointF pixel;
            QVERIFY(CCanvasGeometry::ModelToPixels(&canvas, models[index], pixel));
            QVERIFY(std::abs((ndc.x() + 1) * size.width() / 2.0 - pixel.x()) < 0.001);
            QVERIFY(std::abs((1 - ndc.y()) * size.height() / 2.0 - pixel.y()) < 0.001);
        }
    }

    void ndcFailuresKeepTheEntireOutputUnchanged()
    {
        std::array<float, 16> original;
        for (std::size_t index = 0; index < original.size(); ++index) original[index] = float(index + 17);
        const std::array<double, 5> valid{16, 8, 3, 5, 4};
        const auto rejected = [&](const std::array<double, 5>& values, const QSize& size) {
            auto matrix = original;
            return !CCanvasGeometry::ModelToNdc(values[0], values[1], values[2], values[3], values[4], size, matrix) &&
                matrix == original;
        };
        for (std::size_t field = 0; field < valid.size(); ++field) {
            for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
                auto values = valid;
                values[field] = invalid;
                QVERIFY(rejected(values, QSize(16, 8)));
            }
        }
        for (std::size_t field : {std::size_t(0), std::size_t(1)}) {
            for (double invalid : {0.0, -1.0, 0.49, double(INT_MAX) + 1.0}) {
                auto values = valid;
                values[field] = invalid;
                QVERIFY(rejected(values, QSize(16, 8)));
            }
        }
        for (double invalid : {0.0, -1.0, std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::denorm_min()}) {
            auto values = valid;
            values[4] = invalid;
            QVERIFY(rejected(values, QSize(16, 8)));
        }
        for (std::size_t field : {std::size_t(2), std::size_t(3)}) {
            auto values = valid;
            values[field] = std::numeric_limits<double>::max();
            QVERIFY(rejected(values, QSize(16, 8)));
            values[field] = -std::numeric_limits<double>::max();
            QVERIFY(rejected(values, QSize(16, 8)));
        }
        for (const QSize size : {QSize(), QSize(0, 8), QSize(16, -1), QSize(8, 16), QSize(17, 8)})
            QVERIFY(rejected(valid, size));
        const double tiny = std::numeric_limits<float>::denorm_min();
        QVERIFY(rejected({8, 2, 0, 0, tiny}, QSize(8, 2)));
        QVERIFY(rejected({2, 8, 0, 0, tiny}, QSize(2, 8)));
        QVERIFY(rejected({1, 2, 0, 0, std::numeric_limits<float>::max()}, QSize(1, 2)));
        QVERIFY(rejected({2, 1, 0, 0, std::numeric_limits<float>::max()}, QSize(2, 1)));
    }

    void ndcAcceptsRepresentableScaleExtremesAndCanBeReused()
    {
        std::array<float, 16> matrix;
        const float tiny = std::numeric_limits<float>::denorm_min();
        const float maximum = std::numeric_limits<float>::max();
        for (const float ppu : {tiny, maximum}) {
            QVERIFY(CCanvasGeometry::ModelToNdc(2, 2, 1, 1, ppu, QSize(2, 2), matrix));
            QCOMPARE(matrix[0], ppu);
            QCOMPARE(matrix[5], ppu);
            QCOMPARE(matrix[12], 0.0f);
            QCOMPARE(matrix[13], 0.0f);
            QVERIFY(matrix[0] > 0 && std::isfinite(matrix[0]));
        }
        QVERIFY(CCanvasGeometry::ModelToNdc(INT_MAX, INT_MAX, 0, 0, 1, QSize(INT_MAX, INT_MAX), matrix));
        QVERIFY(matrix[0] > 0 && matrix[5] > 0);
        QVERIFY(CCanvasGeometry::ModelToNdc(16, 16, 3, 5, 4, QSize(16, 16), matrix));
        QCOMPARE(ndcPoint(matrix, QPointF(-0.75, 1.25)), QPointF(-1, 1));
    }

    void invalidCanvasRejectsBeforeArraysAndPreservesDocument_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<float>("value");
        const char* fields[]{"pixels_per_unit", "origin_x", "origin_y", "width", "height"};
        const float infinity = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (int field = 0; field < 5; ++field) {
            const QByteArray name(fields[field]);
            QTest::newRow((name + "-nan").constData()) << field << nan;
            QTest::newRow((name + "-infinity").constData()) << field << infinity;
            QTest::newRow((name + "-negative-infinity").constData()) << field << -infinity;
        }
        QTest::newRow("zero-density") << 0 << 0.0f;
        QTest::newRow("negative-density") << 0 << -1.0f;
        for (int field : {3, 4}) {
            const QByteArray name(fields[field]);
            QTest::newRow((name + "-zero").constData()) << field << 0.0f;
            QTest::newRow((name + "-negative").constData()) << field << -1.0f;
            QTest::newRow((name + "-rounds-to-zero").constData()) << field << 0.25f;
            QTest::newRow((name + "-float-int-limit").constData()) << field << float(INT_MAX);
            QTest::newRow((name + "-float-maximum").constData()) << field << std::numeric_limits<float>::max();
        }
    }

    void invalidCanvasRejectsBeforeArraysAndPreservesDocument()
    {
        QFETCH(int, field);
        QFETCH(float, value);
        const QString sourcePath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray bytes = source.readAll();
        QVERIFY(bytes.size() >= 0x48);
        const qint32 canvasOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x44);
        QVERIFY(canvasOffset >= 0 && canvasOffset <= bytes.size() - 20);
        quint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        qToLittleEndian(bits, reinterpret_cast<uchar*>(bytes.data() + canvasOffset + field * 4));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString invalidPath = temporary.filePath("canvas.moc3");
        QFile invalid(invalidPath);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QCOMPARE(invalid.write(bytes), bytes.size());
        invalid.close();

        CReader reader(invalidPath);
        CSectionTable table(reader);
        QVERIFY(!table.IsValid());
        QVERIFY(!reader.IsValid());
        QVERIFY2(reader.Error().contains(QStringLiteral("canvas_info")), qPrintable(reader.Error()));
        QVERIFY(table.parts.isEmpty());
        QVERIFY(table.deformers.isEmpty());
        QVERIFY(table.art_meshes.isEmpty());
        QVERIFY(table.parameters.isEmpty());
        QVERIFY(table.keyform_positions == nullptr);
        QVERIFY(table.uvs == nullptr);
        QVERIFY(table.position_indices == nullptr);

        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(sourcePath, &error), qPrintable(error));
        QVERIFY(!document.Drawables().isEmpty());
        const QString id = document.Drawables().first().id;
        const bool visible = !document.Drawables().first().visible;
        QVERIFY(document.SetDrawableVisible(id, visible));
        QVERIFY(!document.Load(invalidPath, &error));
        QVERIFY2(error.contains(QStringLiteral("canvas_info")), qPrintable(error));
        QVERIFY(document.IsLoaded());
        QCOMPARE(document.SourcePath(), sourcePath);
        QCOMPARE(document.CanvasPixels(), QSize(2400, 3000));
        QCOMPARE(document.Drawables().first().id, id);
        QCOMPARE(document.Drawables().first().visible, visible);
    }
};

QTEST_APPLESS_MAIN(CanvasGeometryTests)
#include "canvas_geometry_tests.moc"
