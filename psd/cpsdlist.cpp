#include "cpsdlist.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include <QFile>
#include <QSaveFile>
#include <limits>
#include <vector>

namespace {
class CPsdOutputFile : public psd::File
{
public:
    CPsdOutputFile(psd::Allocator* allocator, QIODevice& device) : File(allocator), m_device(device) {}
private:
    bool DoOpenRead(const wchar_t*) override { return false; }
    bool DoOpenWrite(const wchar_t*) override { return false; }
    bool DoClose() override { return true; }
    ReadOperation DoRead(void*, uint32_t, uint64_t) override { return nullptr; }
    bool DoWaitForRead(ReadOperation& operation) override { operation = nullptr; return false; }
    WriteOperation DoWrite(const void* data, uint32_t count, uint64_t position) override
    {
        const bool success = position <= static_cast<uint64_t>(std::numeric_limits<qint64>::max()) &&
            m_device.seek(static_cast<qint64>(position)) &&
            m_device.write(static_cast<const char*>(data), count) == count;
        return new bool(success);
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
    if (m_failed) return false;
    m_canvasWidth = static_cast<unsigned int>(w);
    m_canvasHeight = static_cast<unsigned int>(h);
    document = CreateExportDocument(&allocator, m_canvasWidth, m_canvasHeight, m_bitsPerChannel, m_colorMode);
    if (!document) {
        m_failed = true;
        return false;
    }

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
    return !m_failed;
}

bool CPsdList::CanAppendLayer()
{
    if (!document || m_failed) return false;
    if (document->layerCount < ExportDocument::MAX_LAYER_COUNT) return true;
    m_failed = true;
    qWarning("PSD 图层及分组记录超过上限 %u", ExportDocument::MAX_LAYER_COUNT);
    return false;
}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect)
{
    AddLayer(name, image, rect, 255, 0x6E6F726D /*'norm'*/, false, false);
}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect,
                        uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                        bool isPremultiplied)
{
    if (!CanAppendLayer()) return;
    int left, top, lastX, lastY;
    rect.getCoords(&left, &top, &lastX, &lastY);
    const qint64 wideWidth = static_cast<qint64>(lastX) - left + 1;
    const qint64 wideHeight = static_cast<qint64>(lastY) - top + 1;
    if (name.contains(QChar::Null) || image.isNull() || wideWidth <= 0 || wideHeight <= 0 || wideWidth > 30000 || wideHeight > 30000 ||
        lastX == std::numeric_limits<int>::max() || lastY == std::numeric_limits<int>::max()) {
        m_failed = true;
        return;
    }
    const int width = static_cast<int>(wideWidth);
    const int height = static_cast<int>(wideHeight);
    const qint64 pixelCount = wideWidth * wideHeight;
    if (pixelCount > 100000000) { m_failed = true; return; }
    const int size = static_cast<int>(pixelCount);
    const QSize targetSize(width, height);

    QImage source = image;
    if (isPremultiplied && source.pixelFormat().premultiplied() != QPixelFormat::Premultiplied) {
        source = source.convertToFormat(QImage::Format_ARGB32);
        source.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
    }
    QImage img32 = source.convertToFormat(QImage::Format_ARGB32);
    if (img32.size() != targetSize) img32 = img32.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (img32.size() != targetSize) {
        m_failed = true;
        return;
    }
    std::vector<uint8_t> planarDataR(size), planarDataG(size), planarDataB(size), planarDataA(size);

    for (int y = 0; y < height; ++y) {
        const QRgb* scanLine = reinterpret_cast<const QRgb*>(img32.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            QRgb pixel = scanLine[x];
            uint8_t a = qAlpha(pixel);
            if (a == 0) {
                planarDataR[idx] = 0;
                planarDataG[idx] = 0;
                planarDataB[idx] = 0;
                planarDataA[idx] = 0;
            } else {
                uint8_t r = qRed(pixel);
                uint8_t g = qGreen(pixel);
                uint8_t b = qBlue(pixel);
                planarDataR[idx] = r;
                planarDataG[idx] = g;
                planarDataB[idx] = b;
                planarDataA[idx] = a;
            }
        }
    }

    const unsigned int layerIdx = psd::AddLayer(document, &allocator, name.toUtf8().constData());

    // 设置混合模式、透明度、裁剪
    psd::SetLayerBlendMode(document, layerIdx, blendModeKey);
    psd::SetLayerOpacity(document, layerIdx, opacity);
    psd::SetLayerClipping(document, layerIdx, isClipped ? 1u : 0u);

    const int right = lastX + 1;
    const int bottom = lastY + 1;

    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::RED,   left, top, right, bottom, planarDataR.data(), compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::GREEN, left, top, right, bottom, planarDataG.data(), compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::BLUE,  left, top, right, bottom, planarDataB.data(), compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::ALPHA, left, top, right, bottom, planarDataA.data(), compressionType::RLE);
}

void CPsdList::AddGroupStart()
{
    if (!CanAppendLayer()) return;
    const unsigned int groupStart = psd::AddLayerGroupStart(document, &allocator, "</Layer group>");

    uint8_t* point = new uint8_t[1];
    psd::UpdateLayerGroup(document, &allocator, groupStart, exportChannel::RED, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupStart, exportChannel::GREEN, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupStart, exportChannel::BLUE, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupStart, exportChannel::ALPHA, 0, 0, 0, 0, point, compressionType::RAW);

    delete[] point;
}

void CPsdList::AddGroupEnd(const char* groupName)
{
    if (!CanAppendLayer()) return;
    const unsigned int groupEnd = psd::AddLayerGroupEnd(document, &allocator, groupName);

    uint8_t* point = new uint8_t[1];
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::RED, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::GREEN, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::BLUE, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::ALPHA, 0, 0, 0, 0, point, compressionType::RAW);
    delete[] point;
}

bool CPsdList::CreatePsd()
{
    if (!document || m_failed) return false;

    QSaveFile output(m_docname);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
    {
        qWarning("CPsdList::CreatePsd 打开文件失败: %s", m_docname.toUtf8().constData());
        return false;
    }
    CPsdOutputFile file(&allocator, output);
    if (!WriteDocument(document, &allocator, &file)) {
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) return false;
    DestroyExportDocument(document, &allocator);
    document =nullptr;
    return true;
}
