#include "cartworksourceimport.h"
#include "psd/cpsdreader.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QtEndian>
#include <algorithm>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QString owned(const QString& text) { return QString(text.constData(), text.size()); }
bool validId(const QString& id) { return !id.trimmed().isEmpty() && id.size() <= 1024 && !id.contains(QChar::Null); }
QString diagnostic(const CPsdDiagnostic& value) { return value.code + QStringLiteral(": ") + value.message; }
QByteArray pixelDigest(const QImage& image, CExportControl& control)
{
    QCryptographicHash hash(QCryptographicHash::Sha256); char size[8];
    qToBigEndian(quint32(image.width()), size); qToBigEndian(quint32(image.height()), size + 4);
    hash.addData(QByteArrayView(size, 8));
    for (int y = 0; y < image.height(); ++y) {
        if (!control.Checkpoint()) return {};
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), qsizetype(image.width()) * 4));
    }
    return hash.result();
}
}

bool CArtworkSourceImport::Prepare(const CPsdSnapshot& snapshot, const QString& familyId, const QString& revisionId,
                                    Artwork::ColorPolicy colorPolicy, const QTransform& documentToFamily,
                                    SourceCandidate& result, QString* error, CExportControl* requested)
{
    if (error) error->clear(); CExportControl local; auto& control = requested ? *requested : local;
    try {
        const auto family = owned(familyId), revision = owned(revisionId), path = owned(snapshot.canonicalPath);
        const QTransform transform = documentToFamily;
        if (!validId(family) || !validId(revision) || path.size() > 32768 || path.contains(QChar::Null) ||
            (colorPolicy != Artwork::ColorPolicy::PreserveValues && colorPolicy != Artwork::ColorPolicy::ConvertToSRgb))
            return fail(error, QStringLiteral("素材来源、修订标识或色彩策略无效"));
        if (!Artwork::ValidateTransform(transform, error)) return false;
        CPsdSnapshot frozen;
        if (!CPsdReader::ReadBytes(snapshot.bytes, frozen, error, &control)) return false;
        SourceCandidate candidate;
        auto& target = candidate.revision;
        target.id = revision; target.familyId = family; target.path = path; target.fileName = QFileInfo(path).fileName();
        target.sourceBytes = frozen.bytes; target.sourceDigest = frozen.sha256; target.iccProfile = frozen.iccProfile;
        target.canvasSize = frozen.canvasSize; target.colorPolicy = colorPolicy; target.documentToFamily = transform;
        for (const auto& value : frozen.diagnostics) candidate.diagnostics.append(diagnostic(value));
        QColorSpace source(QColorSpace::SRgb);
        if (frozen.iccProfile.isEmpty()) {
            candidate.diagnostics.append(QStringLiteral("missing-icc: 来源没有 ICC，按 sRGB 解释像素数值"));
        } else {
            source = QColorSpace::fromIccProfile(frozen.iccProfile);
            if (!source.isValid() || source.colorModel() != QColorSpace::ColorModel::Rgb) {
                if (colorPolicy == Artwork::ColorPolicy::ConvertToSRgb)
                    return fail(error, QStringLiteral("来源 ICC 无效或不是 RGB，不能转换到 sRGB"));
                candidate.diagnostics.append(QStringLiteral("invalid-icc: 保留原始数值与 ICC；该配置不能用于色彩转换"));
            }
        }
        candidate.diagnostics.append(colorPolicy == Artwork::ColorPolicy::PreserveValues
            ? QStringLiteral("color-policy: 保留来源 RGB 数值，不进行色彩转换")
            : QStringLiteral("color-policy: 已依据来源色彩配置显式转换到 sRGB"));
        QVector<int> remap(frozen.layers.size(), -1);
        for (int index : frozen.displayOrder) {
            if (index < 0 || index >= frozen.layers.size() || remap[index] >= 0 || frozen.layers[index].kind == CPsdLayer::Kind::GroupEnd)
                return fail(error, QStringLiteral("PSD 显示目录无效"));
            remap[index] = target.layers.size(); target.layers.append(Artwork::Layer());
        }
        QMap<QByteArray, QByteArray> reused;
        qint64 encoded = target.sourceBytes.size() + target.iccProfile.size(), decoded = 0;
        if (encoded > Artwork::MaximumBytes) return fail(error, QStringLiteral("素材来源字节总量超过限制"));
        for (int sourceIndex : frozen.displayOrder) {
            const auto& layer = frozen.layers[sourceIndex]; auto& output = target.layers[remap[sourceIndex]];
            if (!control.Report(CExportControl::Phase::Preparing, remap[sourceIndex], target.layers.size(), layer.name))
                return fail(error, control.Error());
            output.id = QStringLiteral("record:%1").arg(layer.recordIndex); output.name = layer.name; output.sourceIndex = layer.recordIndex;
            output.parentIndex = layer.parentIndex < 0 ? -1 : remap.value(layer.parentIndex, -1);
            if (layer.parentIndex >= 0 && output.parentIndex < 0) return fail(error, QStringLiteral("PSD 图层父级未包含在素材目录中"));
            output.kind = layer.kind == CPsdLayer::Kind::Group ? Artwork::LayerKind::Group : Artwork::LayerKind::Raster;
            output.externalId = layer.layerId; output.bounds = layer.bounds; output.visible = layer.visible; output.clipping = layer.clipping;
            output.pixelEquivalent = layer.pixelEquivalent; output.opacity = layer.opacity; output.blendMode = layer.blendMode;
            for (const auto& value : layer.diagnostics) output.diagnostics.append(diagnostic(value));
            if (layer.image.isNull() || output.kind == Artwork::LayerKind::Group) continue;
            QImage image = layer.image;
            if (colorPolicy == Artwork::ColorPolicy::ConvertToSRgb) {
                image.setColorSpace(source);
                image = image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_RGBA8888);
            } else image = image.convertToFormat(QImage::Format_RGBA8888);
            if (image.isNull() || image.size() != layer.bounds.size()) return fail(error, QStringLiteral("素材像素格式或色彩转换失败"));
            image.setColorSpace({}); image.setDevicePixelRatio(1.0);
            const auto pixels = pixelDigest(image, control);
            if (pixels.isEmpty()) return fail(error, control.Error());
            const auto previous = reused.constFind(pixels);
            if (previous != reused.cend()) { output.imageDigest = *previous; continue; }
            const qint64 bytes = qint64(image.width()) * image.height() * 4;
            if (bytes > Artwork::MaximumBytes - decoded) return fail(error, QStringLiteral("素材像素总量超过限制"));
            decoded += bytes;
            QByteArray png; QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG") || png.isEmpty())
                return fail(error, QStringLiteral("无法编码素材 PNG"));
            if (!control.Checkpoint()) return fail(error, control.Error());
            if (png.size() > Artwork::MaximumBytes - encoded) return fail(error, QStringLiteral("素材 PNG 总量超过限制"));
            QSize size;
            if (!CPngImage::Size(png, size, error) || size != image.size()) return false;
            output.imageDigest = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
            encoded += png.size(); candidate.images.insert(output.imageDigest, {size, std::move(png)}); reused.insert(pixels, output.imageDigest);
        }
        Artwork::State checked;
        checked.images = candidate.images; checked.revisions.insert(revision, target);
        checked.families.insert(family, {family, target.fileName, Artwork::SourceKind::Psd, revision});
        if (!Artwork::Validate(checked, nullptr, error)) return false;
        if (!control.Checkpoint()) return fail(error, control.Error());
        result = std::move(candidate); return true;
    } catch (...) { return fail(error, QStringLiteral("无法准备来源素材，现有素材保持不变")); }
}
}
