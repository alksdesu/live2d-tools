#include "cpngimage.h"
#include <QImage>
#include <limits>
#include <memory>

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool CPngImage::Size(const QByteArray& png, QSize& result, QString* error)
{
    int width = 0, height = 0, channels = 0;
    if (png.isEmpty() || png.size() > std::numeric_limits<int>::max() ||
        !stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(png.constData()), static_cast<int>(png.size()),
                               &width, &height, &channels) || width <= 0 || height <= 0 ||
        qint64(width) * height > std::numeric_limits<int>::max() / 4) {
        if (error) *error = QStringLiteral("PNG 格式或图像尺寸无效");
        return false;
    }
    result = QSize(width, height);
    if (error) error->clear();
    return true;
}

bool CPngImage::Decode(const QByteArray& png, QImage& result, QString* error)
{
    QSize expected;
    if (!Size(png, expected, error)) return false;
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.constData()), static_cast<int>(png.size()),
                             &width, &height, &channels, STBI_rgb_alpha), &stbi_image_free);
    if (!pixels || QSize(width, height) != expected) {
        if (error) *error = QStringLiteral("无法完整解码 PNG 图像");
        return false;
    }
    const QImage view(pixels.get(), width, height, width * 4, QImage::Format_RGBA8888);
    QImage image = view.copy();
    if (image.isNull()) {
        if (error) *error = QStringLiteral("没有足够内存加载 PNG 图像");
        return false;
    }
    result.swap(image);
    if (error) error->clear();
    return true;
}
