#include "cpsdsourcewriter.h"
#include "cpsdlist.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <limits>

namespace {
constexpr qint64 MaximumBytes = 2LL * 1024 * 1024 * 1024 - 1;
constexpr qint64 MaximumPngBytes = 512LL * 1024 * 1024;
constexpr qint64 MaximumMetadataBytes = 64LL * 1024 * 1024;
constexpr qint64 MaximumLayerPixels = 100000000;
constexpr qint64 MaximumTotalPixels = 256LL * 1024 * 1024;
constexpr int MaximumDimension = 30000;
constexpr int MaximumDepth = 128;
constexpr int MaximumText = 32768;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool text(const QString& value)
{
    return value.size() <= MaximumText && !value.contains(QChar::Null) && value.isValidUtf16();
}

bool knownBlend(const QByteArray& value, bool group)
{
    static const QSet<QByteArray> modes{
        "norm", "diss", "dark", "mul ", "idiv", "lbrn", "dkCl", "lite", "scrn", "div ",
        "lddg", "lgCl", "over", "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff",
        "smud", "fsub", "fdiv", "hue ", "sat ", "colr", "lum "
    };
    return value.size() == 4 && (modes.contains(value) || (group && value == "pass"));
}

bool pngSize(const QByteArray& bytes, const QSize& size, QString* error)
{
    if (bytes.size() < 33 || bytes.left(8) != QByteArray::fromHex("89504e470d0a1a0a") ||
        qFromBigEndian<quint32>(bytes.constData() + 8) != 13 || bytes.mid(12, 4) != "IHDR" ||
        quint8(bytes[24]) != 8 || (quint8(bytes[25]) != 2 && quint8(bytes[25]) != 6))
        return fail(error, QStringLiteral("来源图层必须提供 RGB 或 RGBA 8 位 PNG"));
    QSize actual;
    if (!CPngImage::Size(bytes, actual, error)) return false;
    if (actual != size) return fail(error, QStringLiteral("来源 PNG 尺寸与图层边界不一致，不能自动缩放"));
    return true;
}

struct Budget
{
    qint64 metadata = 0, encoded = 0, pixels = 0, rows = 0, largest = 0;
    int records = 0;

    bool addText(const QString& value, QString* error)
    {
        if (!text(value)) return fail(error, QStringLiteral("PSD 来源文本无效或超过长度限制"));
        metadata += qint64(value.size()) * 8 + 64;
        if (metadata > MaximumMetadataBytes) return fail(error, QStringLiteral("PSD 来源元数据超过预算"));
        return true;
    }
};

bool validate(const CPsdSourceDocument& document, bool decode, QString* error)
{
    Budget budget;
    const QSize canvas = document.canvasSize;
    const qint64 canvasPixels = qint64(canvas.width()) * canvas.height();
    if (canvas.width() <= 0 || canvas.height() <= 0 || canvas.width() > MaximumDimension ||
        canvas.height() > MaximumDimension || canvasPixels > MaximumLayerPixels)
        return fail(error, QStringLiteral("PSD 来源画布超出尺寸或像素预算"));
    if (document.layers.size() > psd::ExportDocument::MAX_LAYER_COUNT || document.diagnostics.size() > 100000 ||
        document.iccProfile.size() > MaximumMetadataBytes)
        return fail(error, QStringLiteral("PSD 来源目录或色彩配置超过预算"));
    budget.metadata = document.iccProfile.size();
    if (!budget.addText(document.id, error) || !budget.addText(document.name, error)) return false;
    QSet<QString> ids;
    QVector<int> ancestors;
    for (int index = 0; index < document.layers.size(); ++index) {
        const auto& layer = document.layers[index];
        const bool group = layer.kind == CPsdSourceLayer::Kind::Group;
        if ((layer.kind != CPsdSourceLayer::Kind::Raster && !group) || layer.id.isEmpty() || ids.contains(layer.id) ||
            !budget.addText(layer.id, error) || !budget.addText(layer.name, error))
            return fail(error, QStringLiteral("PSD 来源图层标识、类型或名称无效"));
        ids.insert(layer.id);
        if (layer.parentIndex < -1 || layer.parentIndex >= index)
            return fail(error, QStringLiteral("PSD 来源父级必须是前序目录中的分组"));
        while (!ancestors.isEmpty() && ancestors.last() != layer.parentIndex) ancestors.removeLast();
        if (layer.parentIndex >= 0 && ancestors.isEmpty())
            return fail(error, QStringLiteral("PSD 来源层级不符合完整的前序分组顺序"));
        if (group) {
            ancestors.append(index);
            if (ancestors.size() > MaximumDepth) return fail(error, QStringLiteral("PSD 来源分组深度超过预算"));
        }
        budget.records += group ? 2 : 1;
        if (budget.records > int(psd::ExportDocument::MAX_LAYER_COUNT))
            return fail(error, QStringLiteral("PSD 来源图层及分组结束记录超过预算"));
        if (!knownBlend(layer.blendMode, group))
            return fail(error, QStringLiteral("不支持或无法识别来源混合模式：%1").arg(QString::fromLatin1(layer.blendMode.left(16))));
        int left, top, lastX, lastY;
        layer.bounds.getCoords(&left, &top, &lastX, &lastY);
        const qint64 width = qint64(lastX) - left + 1, height = qint64(lastY) - top + 1;
        const qint64 right = qint64(lastX) + 1, bottom = qint64(lastY) + 1;
        if (width < 0 || height < 0 || width > MaximumDimension || height > MaximumDimension ||
            right > std::numeric_limits<qint32>::max() || bottom > std::numeric_limits<qint32>::max() ||
            width * height > MaximumLayerPixels)
            return fail(error, QStringLiteral("PSD 来源图层边界无效或超过预算"));
        const bool empty = width == 0 && height == 0;
        if (group || empty) {
            if (!layer.png.isEmpty()) return fail(error, QStringLiteral("PSD 分组或空图层不能附带栅格像素"));
            continue;
        }
        if (!width || !height || layer.png.isEmpty())
            return fail(error, QStringLiteral("PSD 空图层必须是 0×0；非空图层必须具有 PNG 像素"));
        if (layer.png.size() > MaximumPngBytes - budget.encoded)
            return fail(error, QStringLiteral("PSD 来源 PNG 总量超过预算"));
        budget.encoded += layer.png.size();
        const qint64 pixels = width * height;
        if (pixels > MaximumTotalPixels - budget.pixels)
            return fail(error, QStringLiteral("PSD 来源图层累计像素超过预算"));
        budget.pixels += pixels;
        budget.rows += height;
        budget.largest = qMax(budget.largest, pixels);
        if (!pngSize(layer.png, layer.bounds.size(), error)) return false;
    }
    for (const auto& diagnostic : document.diagnostics) {
        if (diagnostic.severity != CPsdSourceDiagnostic::Severity::Information &&
            diagnostic.severity != CPsdSourceDiagnostic::Severity::Warning &&
            diagnostic.severity != CPsdSourceDiagnostic::Severity::Error)
            return fail(error, QStringLiteral("PSD 来源诊断级别无效"));
        if (!budget.addText(diagnostic.code, error) || !budget.addText(diagnostic.message, error) ||
            !budget.addText(diagnostic.layerId, error)) return false;
        if (diagnostic.severity == CPsdSourceDiagnostic::Severity::Error)
            return fail(error, QStringLiteral("来源文档含未解决的错误：%1").arg(diagnostic.message));
    }
    const qint64 channels = budget.pixels * 8 + budget.rows * 8;
    if (channels + budget.metadata + budget.records * 512LL + canvasPixels * 3 + 4096 > MaximumBytes ||
        channels + budget.encoded + budget.metadata * 2 + budget.largest * 13 + canvasPixels > MaximumBytes)
        return fail(error, QStringLiteral("PSD 来源编码工作集或文件大小超过预算"));
    if (decode) {
        QMap<QByteArray, QByteArray> verified;
        for (const auto& layer : document.layers) {
            if (layer.png.isEmpty()) continue;
            const auto digest = QCryptographicHash::hash(layer.png, QCryptographicHash::Sha256);
            const auto found = verified.constFind(digest);
            if (found != verified.cend() && *found == layer.png) continue;
            QImage pixels;
            if (!CPngImage::Decode(layer.png, pixels, error) || pixels.size() != layer.bounds.size())
                return fail(error, QStringLiteral("无法完整解码来源图层 PNG：%1").arg(layer.name));
            verified.insert(digest, layer.png);
        }
    }
    return true;
}

QString owned(const QString& value)
{
    return QString(value.constData(), value.size());
}

CPsdSourceDocument freeze(const CPsdSourceDocument& source)
{
    CPsdSourceDocument result;
    result.id = owned(source.id);
    result.name = owned(source.name);
    result.canvasSize = source.canvasSize;
    result.iccProfile = QByteArray(source.iccProfile.constData(), source.iccProfile.size());
    QMap<QByteArray, QByteArray> blobs;
    result.layers.reserve(source.layers.size());
    for (const auto& layer : source.layers) {
        auto next = layer;
        next.id = owned(layer.id);
        next.name = owned(layer.name);
        next.blendMode = QByteArray(layer.blendMode.constData(), layer.blendMode.size());
        if (!layer.png.isEmpty()) {
            const auto digest = QCryptographicHash::hash(layer.png, QCryptographicHash::Sha256);
            const auto found = blobs.constFind(digest);
            if (found != blobs.cend() && *found == layer.png) next.png = *found;
            else {
                next.png = QByteArray(layer.png.constData(), layer.png.size());
                blobs.insert(digest, next.png);
            }
        }
        result.layers.append(std::move(next));
    }
    result.diagnostics.reserve(source.diagnostics.size());
    for (const auto& value : source.diagnostics)
        result.diagnostics.append({value.severity, owned(value.code), owned(value.message), owned(value.layerId)});
    return result;
}

struct Record
{
    int index = -1;
};

QVector<Record> records(const CPsdSourceDocument& document)
{
    QVector<Record> result;
    QVector<int> ancestors;
    for (int index = 0; index < document.layers.size(); ++index) {
        const auto& layer = document.layers[index];
        while (!ancestors.isEmpty() && ancestors.last() != layer.parentIndex) {
            result.append({-1});
            ancestors.removeLast();
        }
        result.append({index});
        if (layer.kind == CPsdSourceLayer::Kind::Group) ancestors.append(index);
    }
    for (int index = 0; index < ancestors.size(); ++index) result.append({-1});
    return result;
}
}

bool CPsdSourceWriter::Validate(const CPsdSourceDocument& document, QString* error) try
{
    if (error) error->clear();
    return validate(document, true, error);
} catch (...) {
    return fail(error, QStringLiteral("无法验证 PSD 来源文档，内存或数据无效"));
}

bool CPsdSourceWriter::Write(const QString& path, const CPsdSourceDocument& document,
                           CExportControl* requested, QString* error)
{
    if (error) error->clear();
    CExportControl local;
    auto& control = requested ? *requested : local;
    const auto failed = [&](const QString& message) {
        if (!control.WasCanceled()) control.Fail(message);
        return fail(error, control.Error().isEmpty() ? message : control.Error());
    };
    try {
        if (control.IsCancellationRequested()) {
            control.Checkpoint();
            return failed(control.Error());
        }
        if (!text(path) || path.isEmpty()) return failed(QStringLiteral("PSD 输出路径无效"));
        if (!validate(document, false, error)) return failed(error ? *error : QStringLiteral("PSD 来源文档无效"));
        const QString frozenPath = owned(path);
        const auto frozen = freeze(document);
        if (!control.Report(CExportControl::Phase::Preparing)) return failed(control.Error());
        CPsdList writer;
        writer.SetControl(&control);
        if (!writer.SetSourcePsd(frozenPath, frozen.canvasSize, frozen.iccProfile)) return failed(control.Error());
        const auto order = records(frozen);
        for (auto record = order.crbegin(); record != order.crend(); ++record) {
            if (!control.Checkpoint()) return failed(control.Error());
            if (record->index < 0) {
                if (!writer.AddSourceDivider()) return failed(control.Error());
                continue;
            }
            const auto& layer = frozen.layers[record->index];
            QImage pixels;
            QString detail;
            if (!layer.png.isEmpty() && (!CPngImage::Decode(layer.png, pixels, &detail) || pixels.size() != layer.bounds.size()))
                return failed(QStringLiteral("无法完整解码来源图层 %1：%2").arg(layer.name, detail));
            if (!writer.AddSourceLayer(layer, pixels)) return failed(control.Error());
        }
        if (!writer.CreatePsd()) return failed(control.Error());
        return true;
    } catch (...) {
        return failed(QStringLiteral("PSD 来源写出失败，未替换已有文件"));
    }
}
