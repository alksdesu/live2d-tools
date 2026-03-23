#include "cpsdlist.h"

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

void CPsdList::SetPsd(const QString &name, int w, int h)
{

    m_canvasWidth = w;
    m_canvasHeight = h;
    m_docname = name;
    document = CreateExportDocument(&allocator, m_canvasWidth, m_canvasHeight, m_bitsPerChannel, m_colorMode);

}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect)
{
    AddLayer(name, image, rect, 255, 0x6E6F726D /*'norm'*/, false, false);
}

void CPsdList::AddLayer(const QString &name, const QImage &image, const QRect &rect,
                        uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                        bool isPremultiplied)
{
    int width = rect.width();
    int height = rect.height();
    int size = width * height;

    uint8_t* planarDataR = new uint8_t[size];
    uint8_t* planarDataG = new uint8_t[size];
    uint8_t* planarDataB = new uint8_t[size];
    uint8_t* planarDataA = new uint8_t[size];

    // 确保图像格式兼容 scanLine 读取
    QImage img32 = image.format() == QImage::Format_ARGB32
                       ? image
                       : image.convertToFormat(QImage::Format_ARGB32);

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
    const unsigned int groupEnd = psd::AddLayerGroupEnd(document, &allocator, groupName);

    uint8_t* point = new uint8_t[1];
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::RED, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::GREEN, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::BLUE, 0, 0, 0, 0, point, compressionType::RAW);
    psd::UpdateLayerGroup(document, &allocator, groupEnd, exportChannel::ALPHA, 0, 0, 0, 0, point, compressionType::RAW);
    delete[] point;
}

void CPsdList::CreatePsd()
{

    NativeFile file(&allocator);

    if (!file.OpenWrite(m_docname.toStdWString().c_str()))
    {
        qDebug("不能打开文件 CPsdList::CreatePsd");
        return ;
    }
    WriteDocument(document, &allocator, &file);
    DestroyExportDocument(document, &allocator);
    document =nullptr;
    file.Close();
}
