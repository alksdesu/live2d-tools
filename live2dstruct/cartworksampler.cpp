#include "cartworksampler.h"
#include "std/cexportcontrol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
struct Pixel { int red = 0, green = 0, blue = 0, alpha = 0; };
Pixel pixel(const QImage& image, int x, int y, Artwork::Boundary boundary)
{
    if (boundary == Artwork::Boundary::Repeat) {
        x = (x % image.width() + image.width()) % image.width();
        y = (y % image.height() + image.height()) % image.height();
    } else if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) return {};
    const auto* value = image.constScanLine(y) + qsizetype(x) * 4;
    return {value[0], value[1], value[2], value[3]};
}
void write(uchar* target, Pixel value)
{
    target[0] = uchar(value.red); target[1] = uchar(value.green); target[2] = uchar(value.blue); target[3] = uchar(value.alpha);
}
int rounded(double value) { return std::clamp(int(std::round(value * 255.0)), 0, 255); }
int opacity(int alpha, quint8 value) { return (alpha * int(value) + 127) / 255; }
bool exact(const QTransform& transform)
{
    const auto axis = [](double first, double second) {
        return (std::abs(first) == 1.0 && second == 0.0) || (first == 0.0 && std::abs(second) == 1.0);
    };
    return axis(transform.m11(), transform.m21()) && axis(transform.m12(), transform.m22()) &&
        std::floor(transform.dx()) == transform.dx() && std::floor(transform.dy()) == transform.dy();
}
double repeat(double coordinate, int extent)
{
    double value = std::fmod(coordinate, double(extent));
    if (value < 0.0) value += extent;
    return value;
}
bool map(const QTransform& transform, double x, double y, QSize source, Artwork::Boundary boundary, QPointF& result)
{
    const auto component = [&](double a, double b, double translation, int extent) {
        if (boundary == Artwork::Boundary::Transparent) return std::fma(a, x, std::fma(b, y, translation));
        return repeat(repeat(a * x, extent) + repeat(b * y, extent) + repeat(translation, extent), extent);
    };
    result = {component(transform.m11(), transform.m21(), transform.dx(), source.width()),
              component(transform.m12(), transform.m22(), transform.dy(), source.height())};
    return std::isfinite(result.x()) && std::isfinite(result.y());
}
Pixel nearest(const QImage& source, const QTransform& transform, int x, int y, Artwork::Boundary boundary, quint8 layerOpacity)
{
    const auto coordinate = [&](double a, double b, double translation, int extent) {
        const double offset = boundary == Artwork::Boundary::Repeat ? repeat(translation, extent) : translation;
        const double index = offset + a * x + b * y + (a + b < 0.0 ? -1.0 : 0.0);
        return boundary == Artwork::Boundary::Repeat ? repeat(index, extent) : index;
    };
    const double sx = coordinate(transform.m11(), transform.m21(), transform.dx(), source.width());
    const double sy = coordinate(transform.m12(), transform.m22(), transform.dy(), source.height());
    if (sx < 0 || sy < 0 || sx >= source.width() || sy >= source.height()) return {};
    auto value = pixel(source, int(sx), int(sy), boundary); value.alpha = opacity(value.alpha, layerOpacity); return value;
}
Pixel bilinear(const QImage& source, QPointF position, Artwork::Boundary boundary, quint8 layerOpacity)
{
    if (boundary == Artwork::Boundary::Transparent &&
        (position.x() <= -0.5 || position.y() <= -0.5 || position.x() >= source.width() + 0.5 || position.y() >= source.height() + 0.5)) return {};
    const double sx = position.x() - 0.5, sy = position.y() - 0.5;
    const int x = int(std::floor(sx)), y = int(std::floor(sy));
    const double fx = sx - x, fy = sy - y;
    if (fx == 0.0 && fy == 0.0) {
        auto value = pixel(source, x, y, boundary); value.alpha = opacity(value.alpha, layerOpacity); return value;
    }
    static const auto linear = [] {
        std::array<double, 256> values{};
        for (int index = 0; index < 256; ++index) {
            const double value = index / 255.0;
            values[index] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    const std::array<Pixel, 4> values{pixel(source, x, y, boundary), pixel(source, x + 1, y, boundary),
                                     pixel(source, x, y + 1, boundary), pixel(source, x + 1, y + 1, boundary)};
    const double weights[]{(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy};
    double alpha = 0.0, red = 0.0, green = 0.0, blue = 0.0;
    for (int index = 0; index < 4; ++index) {
        const auto& value = values[index]; const double weight = weights[index] * (value.alpha / 255.0);
        alpha += weight; red += weight * linear[value.red]; green += weight * linear[value.green]; blue += weight * linear[value.blue];
    }
    const int outputAlpha = rounded(alpha * (layerOpacity / 255.0));
    if (outputAlpha == 0) return {};
    const auto srgb = [&](double premultiplied) {
        const double value = std::clamp(premultiplied / alpha, 0.0, 1.0);
        return rounded(value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055);
    };
    return {srgb(red), srgb(green), srgb(blue), outputAlpha};
}
bool validLimits(const CArtworkSampleLimits& limits)
{
    const qint64 maximum = std::numeric_limits<int>::max() / 4;
    return limits.maximumSourcePixels > 0 && limits.maximumSourcePixels <= maximum &&
        limits.maximumDestinationPixels > 0 && limits.maximumDestinationPixels <= maximum &&
        limits.maximumTotalPixels > 0 && limits.maximumTotalPixels <= maximum && limits.maximumSamples > 0 &&
        limits.maximumDimension > 0 && limits.maximumDimension <= 32768;
}
}

bool CArtworkSampler::Render(const QImage& source, const QTransform& destinationToSource, const QSize& destination,
                             Artwork::Boundary boundary, Artwork::Filter filter, quint8 layerOpacity,
                             QImage& result, QString* error, CExportControl* requested, const CArtworkSampleLimits& requestedLimits)
{
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        const auto limits = requestedLimits; const QTransform transform = destinationToSource; const QSize size = destination;
        const QSize sourceSize = source.size(); const auto format = source.format();
        if (!validLimits(limits)) return fail(error, QStringLiteral("素材采样预算配置无效"));
        if (source.isNull() || (format != QImage::Format_ARGB32 && format != QImage::Format_RGBA8888) ||
            source.bytesPerLine() < qint64(source.width()) * 4 || size.width() <= 0 || size.height() <= 0 ||
            sourceSize.width() > limits.maximumDimension || sourceSize.height() > limits.maximumDimension ||
            size.width() > limits.maximumDimension || size.height() > limits.maximumDimension)
            return fail(error, QStringLiteral("素材采样需要有效的直通 RGBA 图像与完整目标尺寸"));
        if ((boundary != Artwork::Boundary::Transparent && boundary != Artwork::Boundary::Repeat) ||
            (filter != Artwork::Filter::ExactTexels && filter != Artwork::Filter::LinearLightPremultiplied))
            return fail(error, QStringLiteral("素材边界或采样方式无效"));
        const qint64 sourcePixels = qint64(sourceSize.width()) * sourceSize.height();
        const qint64 destinationPixels = qint64(size.width()) * size.height();
        if (sourcePixels > limits.maximumSourcePixels || destinationPixels > limits.maximumDestinationPixels ||
            sourcePixels > limits.maximumTotalPixels - destinationPixels ||
            destinationPixels > limits.maximumSamples / (filter == Artwork::Filter::ExactTexels ? 1 : 4))
            return fail(error, QStringLiteral("素材采样的源、目标或四邻点工作量超出预算"));
        if (!Artwork::ValidateTransform(transform, error)) return false;
        if (filter == Artwork::Filter::ExactTexels && !exact(transform))
            return fail(error, QStringLiteral("无损像素采样仅支持整数平移、直角旋转和镜像，请使用线性光采样"));
        if (filter == Artwork::Filter::LinearLightPremultiplied) {
            for (double x : {0.5, size.width() - 0.5}) for (double y : {0.5, size.height() - 0.5}) {
                QPointF point;
                if (!map(transform, x, y, sourceSize, boundary, point)) return fail(error, QStringLiteral("素材采样坐标超出有限范围"));
            }
        }
        QImage frozen(sourceSize, QImage::Format_RGBA8888);
        if (frozen.isNull()) return fail(error, QStringLiteral("无法冻结来源素材像素"));
        for (int y = 0; y < sourceSize.height(); ++y) {
            if (control.IsCancellationRequested()) { control.Checkpoint(); return fail(error, control.Error()); }
            auto* row = frozen.scanLine(y);
            if (format == QImage::Format_RGBA8888) std::memcpy(row, source.constScanLine(y), size_t(sourceSize.width()) * 4);
            else {
                const auto* sourceRow = reinterpret_cast<const QRgb*>(source.constScanLine(y));
                for (int x = 0; x < sourceSize.width(); ++x) {
                    const auto value = sourceRow[x]; write(row + qsizetype(x) * 4, {qRed(value), qGreen(value), qBlue(value), qAlpha(value)});
                }
            }
        }
        if (!control.Checkpoint()) return fail(error, control.Error());
        QImage output(size, QImage::Format_RGBA8888);
        if (output.isNull()) return fail(error, QStringLiteral("无法分配完整采样结果"));
        for (int y = 0; y < size.height(); ++y) {
            if (!control.Report(CExportControl::Phase::Rendering, y, size.height(), QStringLiteral("采样原始素材"))) return fail(error, control.Error());
            auto* row = output.scanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                if (x % 4096 == 0 && !control.Checkpoint()) return fail(error, control.Error());
                Pixel value;
                if (filter == Artwork::Filter::ExactTexels) value = nearest(frozen, transform, x, y, boundary, layerOpacity);
                else {
                    QPointF point;
                    if (!map(transform, x + 0.5, y + 0.5, sourceSize, boundary, point)) return fail(error, QStringLiteral("素材采样坐标超出有限范围"));
                    value = bilinear(frozen, point, boundary, layerOpacity);
                }
                write(row + qsizetype(x) * 4, value);
            }
        }
        if (!control.Checkpoint()) return fail(error, control.Error());
        result = std::move(output); return true;
    } catch (...) { return fail(error, QStringLiteral("素材采样失败，原结果与来源保持不变")); }
}
}
