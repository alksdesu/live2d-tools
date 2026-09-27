#include "cpsdlist.h"
#include "cpsdsourcedocument.h"
#include <QtEndian>
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include <QFile>
#include <QSaveFile>
#include "std/cexportcontrol.h"
#include <exception>
#include <algorithm>
#include <limits>
#include <vector>

namespace {
class CPsdOutputFile : public psd::File
{
public:
    CPsdOutputFile(psd::Allocator* allocator, QIODevice& device, CExportControl* control)
        : File(allocator), m_device(device), m_control(control) {}
private:
    bool DoOpenRead(const wchar_t*) override { return false; }
    bool DoOpenWrite(const wchar_t*) override { return false; }
    bool DoClose() override { return true; }
    ReadOperation DoRead(void*, uint32_t, uint64_t) override { return nullptr; }
    bool DoWaitForRead(ReadOperation& operation) override { operation = nullptr; return false; }
    WriteOperation DoWrite(const void* data, uint32_t count, uint64_t position) override
    {
        if ((!data && count) || position > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) - count)
            return new bool(false);
        const char* bytes = static_cast<const char*>(data);
        uint32_t offset = 0;
        while (offset < count) {
            if (m_control && !m_control->Report(CExportControl::Phase::Writing, position + offset)) return new bool(false);
            const uint32_t chunk = std::min(uint32_t(1024 * 1024), count - offset);
            if (!m_device.seek(static_cast<qint64>(position + offset)) || m_device.write(bytes + offset, chunk) != chunk) {
                if (m_control) m_control->Fail(QStringLiteral("PSD 写入失败：%1").arg(m_device.errorString()));
                return new bool(false);
            }
            offset += chunk;
        }
        return new bool(true);
    }

    bool DoWaitForWrite(WriteOperation& operation) override
    {
        if (!operation) return false;
        const bool success = *static_cast<bool*>(operation);
        delete static_cast<bool*>(operation);
        operation = nullptr;
        return success;
    }
    uint64_t DoGetSize() const override { return static_cast<uint64_t>(qMax<qint64>(0, m_device.size())); }
    QIODevice& m_device;
    CExportControl* m_control;
};
}

CPsdList::CPsdList() {
    m_canvasWidth = 0;
    m_canvasHeight = 0;
    m_bitsPerChannel = 8;
    m_colorMode = exportColorMode::RGB;
    document = nullptr;

}

CPsdList::~CPsdList()
{
    if(document !=nullptr){
        DestroyExportDocument(document, &allocator);
    }
}

bool CPsdList::SetPsd(const QString &name, int w, int h, bool cubismCompat)
{
    m_docname = name;
    if (document) {
        DestroyExportDocument(document, &allocator);
        document = nullptr;
    }
    m_failed = w <= 0 || h <= 0 || w > 30000 || h > 30000 || name.isEmpty() || name.contains(QChar::Null) ||
               static_cast<qint64>(w) * h > 100000000;
    if (m_failed) return Fail(QStringLiteral("PSD 画布尺寸或输出路径无效"));
    if (m_control && !m_control->Checkpoint()) { m_failed = true; return false; }
    m_canvasWidth = static_cast<unsigned int>(w);
    m_canvasHeight = static_cast<unsigned int>(h);
    document = CreateExportDocument(&allocator, m_canvasWidth, m_canvasHeight, m_bitsPerChannel, m_colorMode);
    if (!document) return Fail(QStringLiteral("无法创建 PSD 文档"));

    if (cubismCompat) {
        // Cubism 导入要求 sRGB 色彩配置
        QFile iccFile(":/icc/sRGB.icc");
        if (iccFile.open(QIODevice::ReadOnly)) {
            QByteArray icc = iccFile.readAll();
            if (!icc.isEmpty()) {
                psd::SetICCProfile(document, &allocator, icc.data(), static_cast<uint32_t>(icc.size()));
            } else m_failed = true;
        } else {
            qWarning("CPsdList::SetPsd: 无法读取内置 sRGB ICC 资源");
            m_failed = true;
        }
    }
    if (m_failed) return Fail(QStringLiteral("无法读取 PSD 所需的 sRGB 色彩配置"));
    return true;
}

bool CPsdList::Fail(const QString& error)
{
    m_failed = true;
    if (m_control && !m_control->WasCanceled()) m_control->Fail(error);
    return false;
}

bool CPsdList::CanAppendLayer()
{
    if (!document || m_failed) return false;
    if (m_control && !m_control->Checkpoint()) { m_failed = true; return false; }
    if (document->layerCount < ExportDocument::MAX_LAYER_COUNT) return true;
    return Fail(QStringLiteral("PSD 图层及分组记录超过上限 %1").arg(ExportDocument::MAX_LAYER_COUNT));
}

bool CPsdList::AddLayer(const QString& name, const QImage& image, const QRect& rect)
{
    return AddLayer(name, image, rect, 255, 0x6E6F726D, false, false);
}

bool CPsdList::AddLayer(const QString& name, const QImage& image, const QRect& rect,
                       uint8_t opacity, uint32_t blendModeKey, bool isClipped, bool isPremultiplied)
{
    if (!CanAppendLayer()) return false;
    try {
        int left, top, lastX, lastY;
        rect.getCoords(&left, &top, &lastX, &lastY);
        const qint64 wideWidth = static_cast<qint64>(lastX) - left + 1;
        const qint64 wideHeight = static_cast<qint64>(lastY) - top + 1;
        if (name.contains(QChar::Null) || image.isNull() || wideWidth <= 0 || wideHeight <= 0 || wideWidth > 30000 || wideHeight > 30000 ||
            lastX == std::numeric_limits<int>::max() || lastY == std::numeric_limits<int>::max())
            return Fail(QStringLiteral("PSD 图层名称、图像或边界无效：%1").arg(name));
        if (wideWidth * wideHeight > 100000000) return Fail(QStringLiteral("PSD 图层像素数超过上限：%1").arg(name));
        const int width = static_cast<int>(wideWidth), height = static_cast<int>(wideHeight);
        const int size = width * height;
        const QSize targetSize(width, height);
        if (m_control && !m_control->Report(CExportControl::Phase::Encoding, 0, qint64(height) * 5, name)) {
            m_failed = true; return false;
        }
        QImage source = image;
        if (isPremultiplied && source.pixelFormat().premultiplied() != QPixelFormat::Premultiplied) {
            source = source.convertToFormat(QImage::Format_ARGB32);
            source.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
        }
        QImage img32 = source.convertToFormat(QImage::Format_ARGB32);
        if (img32.size() != targetSize) img32 = img32.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (img32.size() != targetSize) return Fail(QStringLiteral("无法转换 PSD 图层像素：%1").arg(name));
        std::vector<uint8_t> planes[4];
        for (auto& plane : planes) {
            if (m_control && !m_control->Checkpoint()) { m_failed = true; return false; }
            plane.resize(size);
        }
        for (int y = 0; y < height; ++y) {
            if (m_control && !m_control->Report(CExportControl::Phase::Encoding, y, qint64(height) * 5, name)) {
                m_failed = true; return false;
            }
            const QRgb* scanLine = reinterpret_cast<const QRgb*>(img32.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const int index = y * width + x;
                const QRgb pixel = scanLine[x];
                const uint8_t alpha = qAlpha(pixel);
                planes[0][index] = alpha ? qRed(pixel) : 0;
                planes[1][index] = alpha ? qGreen(pixel) : 0;
                planes[2][index] = alpha ? qBlue(pixel) : 0;
                planes[3][index] = alpha;
            }
        }
        const unsigned int layerIndex = psd::AddLayer(document, &allocator, name.toUtf8().constData());
        psd::SetLayerBlendMode(document, layerIndex, blendModeKey);
        psd::SetLayerOpacity(document, layerIndex, opacity);
        psd::SetLayerClipping(document, layerIndex, isClipped ? 1u : 0u);
        const exportChannel::Enum channels[]{exportChannel::RED, exportChannel::GREEN, exportChannel::BLUE, exportChannel::ALPHA};
        for (int channel = 0; channel < 4; ++channel) {
            const ExportCheckpoint checkpoint = [&](uint32_t current, uint32_t) {
                return !m_control || m_control->Report(CExportControl::Phase::Encoding,
                    qint64(height) * (channel + 1) + current, qint64(height) * 5, name);
            };
            if (!psd::UpdateLayer(document, &allocator, layerIndex, channels[channel], left, top, lastX + 1, lastY + 1,
                                  planes[channel].data(), compressionType::RLE, checkpoint))
                return Fail(QStringLiteral("PSD 图层通道压缩失败：%1").arg(name));
        }
        return true;
    } catch (const std::exception& exception) {
        return Fail(QStringLiteral("PSD 图层编码异常：%1").arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        return Fail(QStringLiteral("PSD 图层编码发生未知异常"));
    }
}

bool CPsdList::AddGroupChannels(unsigned int index)
{
    const uint8_t empty = 0;
    const ExportCheckpoint checkpoint = [&](uint32_t, uint32_t) { return !m_control || m_control->Checkpoint(); };
    for (auto channel : {exportChannel::RED, exportChannel::GREEN, exportChannel::BLUE, exportChannel::ALPHA})
        if (!psd::UpdateLayerGroup(document, &allocator, index, channel, 0, 0, 0, 0, &empty, compressionType::RAW, checkpoint))
            return Fail(QStringLiteral("PSD 分组记录写入失败"));
    return true;
}

bool CPsdList::AddGroupStart()
{
    if (!CanAppendLayer()) return false;
    return AddGroupChannels(psd::AddLayerGroupStart(document, &allocator, "</Layer group>"));
}

bool CPsdList::AddGroupEnd(const char* groupName)
{
    if (!CanAppendLayer()) return false;
    if (!groupName) return Fail(QStringLiteral("PSD 分组名称为空"));
    return AddGroupChannels(psd::AddLayerGroupEnd(document, &allocator, groupName));
}

bool CPsdList::CreatePsd()
{
    if (!document || m_failed) return false;
    const auto fail = [&](const QString& reason) {
        if (m_control && !m_control->WasCanceled()) m_control->Fail(reason);
        return false;
    };
    QSaveFile output(m_docname);
    output.setDirectWriteFallback(false);
    try {
        if (m_control && !m_control->Report(CExportControl::Phase::Writing)) return false;
        if (!output.open(QIODevice::WriteOnly)) return fail(QStringLiteral("无法打开 PSD 输出文件：%1\n%2").arg(m_docname, output.errorString()));
        CPsdOutputFile file(&allocator, output, m_control);
        const ExportCheckpoint checkpoint = [&](uint32_t, uint32_t) { return !m_control || m_control->Checkpoint(); };
        if (!WriteDocument(document, &allocator, &file, checkpoint)) {
            output.cancelWriting();
            return fail(QStringLiteral("PSD 文件写入未完成：%1").arg(m_docname));
        }
        if (m_control && !m_control->BeginCommit()) { output.cancelWriting(); return false; }
        const bool committed = output.commit();
        if (m_control) m_control->CommitFinished(committed, output.errorString());
        if (!committed) return fail(QStringLiteral("无法提交 PSD 输出文件：%1\n%2").arg(m_docname, output.errorString()));
        DestroyExportDocument(document, &allocator);
        return true;
    } catch (const std::exception& exception) {
        output.cancelWriting();
        return fail(QStringLiteral("PSD 文件写入异常：%1").arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        output.cancelWriting();
        return fail(QStringLiteral("PSD 文件写入发生未知异常"));
    }
}

bool CPsdList::SetSourcePsd(const QString& path, const QSize& size, const QByteArray& iccProfile)
{
    if (!SetPsd(path, size.width(), size.height(), false)) return false;
    psd::SetSourceDocumentOptions(document);
    if (!iccProfile.isEmpty())
        psd::SetICCProfile(document, &allocator, const_cast<char*>(iccProfile.constData()), static_cast<uint32_t>(iccProfile.size()));
    return true;
}

bool CPsdList::AddSourceDivider()
{
    return AddGroupStart();
}

bool CPsdList::AddSourceLayer(const CPsdSourceLayer& layer, const QImage& pixels)
{
    if (!CanAppendLayer()) return false;
    const bool group = layer.kind == CPsdSourceLayer::Kind::Group;
    const QByteArray name = layer.name.toUtf8();
    const unsigned int index = group
        ? psd::AddLayerGroupEnd(document, &allocator, name.constData())
        : psd::AddLayer(document, &allocator, name.constData());
    psd::ExportLayerAttributes attributes;
    attributes.opacity = layer.opacity;
    attributes.clipping = layer.clipping ? 1u : 0u;
    attributes.visible = layer.visible;
    attributes.hasLayerId = layer.externalId.has_value();
    attributes.layerId = layer.externalId.value_or(0);
    attributes.blendModeKey = qFromBigEndian<quint32>(layer.blendMode.constData());
    if (!psd::SetLayerAttributes(document, index, attributes)) return Fail(QStringLiteral("PSD 来源图层属性无效"));
    const int left = layer.bounds.x(), top = layer.bounds.y();
    const int right = int(qint64(left) + layer.bounds.width());
    const int bottom = int(qint64(top) + layer.bounds.height());
    if (group || layer.bounds.isEmpty()) {
        if (!psd::SetEmptyLayerBounds(document, index, left, top, right, bottom))
            return Fail(QStringLiteral("PSD 来源空图层边界无效"));
        return true;
    }
    if (pixels.size() != layer.bounds.size() || pixels.format() != QImage::Format_RGBA8888)
        return Fail(QStringLiteral("PSD 来源像素与图层边界不一致"));
    const int width = pixels.width(), height = pixels.height();
    std::vector<uint8_t> plane(size_t(width) * height);
    const exportChannel::Enum channels[]{exportChannel::RED, exportChannel::GREEN, exportChannel::BLUE, exportChannel::ALPHA};
    for (int channel = 0; channel < 4; ++channel) {
        for (int y = 0; y < height; ++y) {
            if (m_control && !m_control->Report(CExportControl::Phase::Encoding,
                    qint64(channel) * height * 2 + y, qint64(height) * 8, layer.name)) {
                m_failed = true;
                return false;
            }
            const uchar* row = pixels.constScanLine(y);
            for (int x = 0; x < width; ++x) plane[size_t(y) * width + x] = row[x * 4 + channel];
        }
        const ExportCheckpoint checkpoint = [&](uint32_t current, uint32_t) {
            return !m_control || m_control->Report(CExportControl::Phase::Encoding,
                (qint64(channel) * 2 + 1) * height + current, qint64(height) * 8, layer.name);
        };
        if (!psd::UpdateLayer(document, &allocator, index, channels[channel], left, top, right, bottom,
                              plane.data(), compressionType::RLE, checkpoint))
            return Fail(QStringLiteral("PSD 来源图层通道压缩失败：%1").arg(layer.name));
    }
    return true;
}
