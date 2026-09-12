#include "cpsdlist.h"
#include <QFile>
#include <limits>

CPsdList::CPsdList() {
    m_canvasWidth = 0;
    m_canvasHeight = 0;
    m_bitsPerChannel = 8;
    m_colorMode = exportColorMode::RGB;

}

CPsdList::~CPsdList()
{
    if(document !=nullptr){
        DestroyExportDocument(document, &allocator);
    }
}

void CPsdList::SetPsd(const QString &name, int w, int h, bool cubismCompat)
{

    m_canvasWidth = w;
    m_canvasHeight = h;
    m_docname = name;
    if (m_canvasWidth <= 0 || m_canvasHeight <= 0) return;
    document = CreateExportDocument(&allocator, m_canvasWidth, m_canvasHeight, m_bitsPerChannel, m_colorMode);

    if (cubismCompat) {
        // Cubism 导入要求 sRGB 色彩配置
        QFile iccFile(":/icc/sRGB.icc");
        if (iccFile.open(QIODevice::ReadOnly)) {
            QByteArray icc = iccFile.readAll();
            if (!icc.isEmpty()) {
                psd::SetICCProfile(document, &allocator, icc.data(), static_cast<uint32_t>(icc.size()));
            }
        } else {
            qWarning("CPsdList::SetPsd: 无法读取内置 sRGB ICC 资源");
        }
    }
}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect)
{
    AddLayer(name, image, rect, 255, 0x6E6F726D /*'norm'*/, false, false);
}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect,
                        uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                        bool isPremultiplied)
{
    if (!document || image.isNull() || rect.width() <= 0 || rect.height() <= 0) return;
    int width = rect.width();
    int height = rect.height();
    const qint64 pixelCount = static_cast<qint64>(width) * height;
    if (pixelCount <= 0 || pixelCount > 100000000 || pixelCount > std::numeric_limits<int>::max()) return;
    const int size = static_cast<int>(pixelCount);
    const QSize targetSize(width, height);

    uint8_t* planarDataR = new uint8_t[size];
    uint8_t* planarDataG = new uint8_t[size];
    uint8_t* planarDataB = new uint8_t[size];
    uint8_t* planarDataA = new uint8_t[size];

    // 确保图像格式兼容 scanLine 读取
    QImage img32 = image.format() == QImage::Format_ARGB32
                       ? image
                       : image.convertToFormat(QImage::Format_ARGB32);
    if (img32.size() != targetSize) img32 = img32.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (img32.size() != targetSize) {
        delete[] planarDataR;
        delete[] planarDataG;
        delete[] planarDataB;
        delete[] planarDataA;
        return;
    }

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
                // 反乘预乘 alpha
                if (isPremultiplied && a < 255) {
                    r = static_cast<uint8_t>(qMin(255, (int)r * 255 / a));
                    g = static_cast<uint8_t>(qMin(255, (int)g * 255 / a));
                    b = static_cast<uint8_t>(qMin(255, (int)b * 255 / a));
                }
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

    int left = rect.x();
    int top = rect.y();
    int right = rect.x() + width;
    int bottom = rect.y() + height;

    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::RED,   left, top, right, bottom, planarDataR, compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::GREEN, left, top, right, bottom, planarDataG, compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::BLUE,  left, top, right, bottom, planarDataB, compressionType::RLE);
    psd::UpdateLayer(document, &allocator, layerIdx, exportChannel::ALPHA, left, top, right, bottom, planarDataA, compressionType::RLE);

    delete[] planarDataR;
    delete[] planarDataG;
    delete[] planarDataB;
    delete[] planarDataA;
}

void CPsdList::AddGroupStart()
{
    if (!document) return;
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
    if (!document) return;
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

    NativeFile file(&allocator);

    if (!file.OpenWrite(m_docname.toStdWString().c_str()))
    {
        qWarning("CPsdList::CreatePsd 打开文件失败: %s", m_docname.toUtf8().constData());
        DestroyExportDocument(document, &allocator);
        document = nullptr;
        return false;
    }
    WriteDocument(document, &allocator, &file);
    DestroyExportDocument(document, &allocator);
    document =nullptr;
    file.Close();
    return true;
}
