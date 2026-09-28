#include "cpsdcomposite.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "std/cexportcontrol.h"
#include <QtEndian>
#include <QScopedValueRollback>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <vector>

namespace {
constexpr uint32_t normal = 0x6e6f726d, multiply = 0x6d756c20, additive = 0x6c646467, passThrough = 0x70617373;

bool unpack(const uint8_t* data, uint32_t length, int width, uint8_t* destination)
{
    uint32_t cursor = 0;
    int written = 0;
    while (cursor < length) {
        const int value = int(int8_t(data[cursor++]));
        if (value == -128) continue;
        const int count = value < 0 ? 1 - value : value + 1;
        if (count > width - written) return false;
        if (value < 0) {
            if (cursor == length) return false;
            if (destination) std::memset(destination + written, data[cursor], size_t(count));
            ++cursor;
        } else {
            if (uint32_t(count) > length - cursor) return false;
            if (destination) std::memcpy(destination + written, data + cursor, size_t(count));
            cursor += uint32_t(count);
        }
        written += count;
    }
    return written == width;
}

struct Plane
{
    const uint8_t* data = nullptr;
    uint32_t size = 0, position = 0;
    int width = 0, height = 0, row = 0;
    uint16_t compression = 0;

    bool Validate(CExportControl* control) const
    {
        if (!data) return size == 0;
        if (compression == 0) return quint64(width) * height == size;
        if (compression != 1 || quint64(height) * 2 > size) return false;
        uint32_t offset = uint32_t(height) * 2;
        for (int y = 0; y < height; ++y) {
            if (control && !control->Checkpoint()) return false;
            const uint32_t count = qFromBigEndian<quint16>(data + y * 2);
            if (count > size - offset || !unpack(data + offset, count, width, nullptr)) return false;
            offset += count;
        }
        return offset == size;
    }

    bool Read(int requested, uint8_t* destination, uint8_t fallback, CExportControl* control)
    {
        if (requested < row || requested >= height) return false;
        if (!data) { std::memset(destination, fallback, size_t(width)); row = requested + 1; return true; }
        if (compression == 0) {
            std::memcpy(destination, data + quint64(requested) * width, size_t(width));
            row = requested + 1;
            return true;
        }
        while (row < requested) {
            if (control && !control->Checkpoint()) return false;
            position += qFromBigEndian<quint16>(data + row++ * 2);
        }
        const uint32_t count = qFromBigEndian<quint16>(data + row++ * 2);
        if (count > size - position || !unpack(data + position, count, width, destination)) return false;
        position += count;
        return true;
    }
};

struct Raster
{
    std::array<Plane, 4> planes;
    int left = 0, top = 0, right = 0, bottom = 0, sourceLeft = 0, sourceTop = 0;
    float opacity = 1;
    uint32_t blend = normal;
};

uint8_t quantize(float value)
{
    return uint8_t(std::floor(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f));
}
}

struct CPsdComposite::State
{
    CExportControl* control = nullptr;
    int width = 0, height = 0, maximumLayerWidth = 0;
    bool supported = false, started = false;
    std::vector<Raster> layers;

    bool Fail(QString* error, const QString& message)
    {
        if (error) *error = control && !control->Error().isEmpty() ? control->Error() : message;
        if (control && !control->IsCancellationRequested() && !control->WasCanceled()) control->Fail(message);
        return false;
    }
};

CPsdComposite::CPsdComposite() = default;
CPsdComposite::~CPsdComposite() = default;

bool CPsdComposite::Prepare(const psd::ExportDocument& document, CExportControl* control, QString* error) try
{
    if (error) error->clear();
    if (m_busy) {
        if (error) *error = QStringLiteral("PSD 合成预览正在处理");
        return false;
    }
    QScopedValueRollback<bool> busy(m_busy, true);
    m_state.reset();
    auto candidate = std::make_unique<State>();
    candidate->control = control;
    if (!document.width || !document.height || document.width > 30000 || document.height > 30000 ||
        quint64(document.width) * document.height > 100000000 || document.layerCount > psd::ExportDocument::MAX_LAYER_COUNT)
        return candidate->Fail(error, QStringLiteral("PSD 合成预览的画布或图层数量无效"));
    candidate->width = int(document.width); candidate->height = int(document.height);
    if (document.bitsPerChannel != 8 || document.colorMode != psd::exportColorMode::RGB || document.omitMergedPreview) {
        m_state = std::move(candidate);
        return true;
    }
    std::vector<bool> visibility{true};
    candidate->layers.reserve(document.layerCount);
    quint64 samples = 0;
    bool supported = true;
    for (int index = document.layerCount - 1; index >= 0; --index) {
        if (control && !control->Checkpoint()) return candidate->Fail(error, control->Error());
        const auto& layer = document.layers[index];
        const bool visible = visibility.back() && !(layer.flags & 2);
        if (layer.type == 1 || layer.type == 2) {
            if (layer.blendModeKey != passThrough || !layer.hasSectionBlendMode || layer.opacity != 255 || layer.clipping)
                supported = false;
            visibility.push_back(visible);
            continue;
        }
        if (layer.type == 3) {
            if (visibility.size() == 1) return candidate->Fail(error, QStringLiteral("PSD 合成预览的分组结构不平衡"));
            visibility.pop_back();
            continue;
        }
        if (layer.type != 0 || layer.clipping || (layer.flags & 16) ||
            (layer.blendModeKey != normal && layer.blendModeKey != multiply && layer.blendModeKey != additive)) supported = false;
        const qint64 width = qint64(layer.right) - layer.left, height = qint64(layer.bottom) - layer.top;
        if (width < 0 || height < 0 || width > 30000 || height > 30000 || width * height > 100000000 ||
            ((width == 0) != (height == 0))) return candidate->Fail(error, QStringLiteral("PSD 合成预览的图层边界无效"));
        if (!width || !visible || !layer.opacity) continue;
        Raster raster;
        raster.left = std::max(0, layer.left); raster.top = std::max(0, layer.top);
        raster.right = std::min(candidate->width, layer.right); raster.bottom = std::min(candidate->height, layer.bottom);
        if (raster.left >= raster.right || raster.top >= raster.bottom) continue;
        const quint64 count = quint64(raster.right - raster.left) * (raster.bottom - raster.top);
        if (count > MaximumSamples - samples) supported = false;
        else samples += count;
        raster.sourceLeft = layer.left; raster.sourceTop = layer.top;
        raster.opacity = float(layer.opacity) / 255.0f; raster.blend = layer.blendModeKey;
        for (int channel = 0; channel < 4; ++channel) {
            auto& plane = raster.planes[size_t(channel)];
            plane = {static_cast<const uint8_t*>(layer.channelData[channel]), layer.channelSize[channel],
                     layer.channelCompression[channel] == 1 ? uint32_t(height) * 2 : 0u,
                     int(width), int(height), 0, layer.channelCompression[channel]};
            if (plane.compression > 1) supported = false;
            if (!plane.data && plane.size) return candidate->Fail(error, QStringLiteral("PSD 合成预览缺少图层通道数据"));
            if (channel < 3 && !plane.data) supported = false;
        }
        candidate->maximumLayerWidth = std::max(candidate->maximumLayerWidth, int(width));
        candidate->layers.push_back(raster);
    }
    if (visibility.size() != 1) return candidate->Fail(error, QStringLiteral("PSD 合成预览的分组结构不平衡"));
    if (supported) {
        for (const auto& layer : candidate->layers) for (const auto& plane : layer.planes)
            if (!plane.Validate(control)) return candidate->Fail(error, QStringLiteral("PSD 合成预览的通道编码无效"));
        std::reverse(candidate->layers.begin(), candidate->layers.end());
    }
    candidate->supported = supported;
    m_state = std::move(candidate);
    return true;
} catch (const std::exception&) {
    if (error) *error = QStringLiteral("无法准备 PSD 合成预览");
    if (control && !control->IsCancellationRequested() && !control->WasCanceled()) control->Fail(QStringLiteral("无法准备 PSD 合成预览"));
    return false;
}

bool CPsdComposite::IsSupported() const { return m_state && m_state->supported; }

bool CPsdComposite::Write(const psd::MergedImageSink& sink, QString* error) try
{
    if (error) error->clear();
    if (m_busy || !IsSupported() || m_state->started || !sink) {
        if (error) *error = QStringLiteral("PSD 合成预览尚未就绪或已经使用");
        return false;
    }
    QScopedValueRollback<bool> busy(m_busy, true);
    const auto frozenSink = sink;
    auto& state = *m_state;
    state.started = true;
    const quint64 overhead = state.layers.capacity() * sizeof(Raster) + quint64(state.maximumLayerWidth) * 4;
    const quint64 rowBytes = quint64(state.width) * (4 * sizeof(float) + 4);
    if (overhead >= MaximumWorkingBytes || rowBytes > MaximumWorkingBytes - overhead)
        return state.Fail(error, QStringLiteral("PSD 合成预览的工作内存超出限制"));
    const int bandRows = int(std::min(quint64(64), (MaximumWorkingBytes - overhead) / rowBytes));
    std::vector<float> pixels(size_t(state.width) * bandRows * 4);
    std::vector<uint8_t> encoded(size_t(state.width) * bandRows * 4);
    std::vector<uint8_t> row(size_t(state.maximumLayerWidth) * 4);
    for (int top = 0; top < state.height; top += bandRows) {
        if (state.control && !state.control->Report(CExportControl::Phase::Writing, top, state.height, QStringLiteral("合成透明预览")))
            return state.Fail(error, state.control->Error());
        const int bottom = std::min(state.height, top + bandRows);
        const size_t count = size_t(state.width) * (bottom - top);
        std::fill(pixels.begin(), pixels.begin() + count * 4, 0.0f);
        for (auto& layer : state.layers) {
            const int first = std::max(top, layer.top), last = std::min(bottom, layer.bottom);
            for (int y = first; y < last; ++y) {
                if (state.control && !state.control->Checkpoint()) return state.Fail(error, state.control->Error());
                for (int channel = 0; channel < 4; ++channel)
                    if (!layer.planes[size_t(channel)].Read(y - layer.sourceTop, row.data() + size_t(channel) * state.maximumLayerWidth,
                                                           channel == 3 ? 255 : 0, state.control))
                        return state.Fail(error, QStringLiteral("PSD 合成预览读取通道失败"));
                for (int x = layer.left; x < layer.right; ++x) {
                    const int sourceX = x - layer.sourceLeft;
                    const float alpha = float(row[size_t(3) * state.maximumLayerWidth + sourceX]) / 255.0f * layer.opacity;
                    if (alpha == 0) continue;
                    float* pixel = pixels.data() + (size_t(y - top) * state.width + x) * 4;
                    const float backAlpha = pixel[3];
                    for (int channel = 0; channel < 3; ++channel) {
                        const float color = float(row[size_t(channel) * state.maximumLayerWidth + sourceX]) / 255.0f;
                        float blend = color;
                        if (backAlpha > 0 && layer.blend != normal) {
                            const float backdrop = std::clamp(pixel[channel] / backAlpha, 0.0f, 1.0f);
                            blend = layer.blend == multiply ? backdrop * color : std::min(1.0f, backdrop + color);
                        }
                        pixel[channel] = pixel[channel] * (1 - alpha) + alpha * ((1 - backAlpha) * color + backAlpha * blend);
                    }
                    pixel[3] = alpha + backAlpha * (1 - alpha);
                }
            }
        }
        for (size_t index = 0; index < count; ++index) {
            if (index % 4096 == 0 && state.control && !state.control->Checkpoint()) return state.Fail(error, state.control->Error());
            const float* pixel = pixels.data() + index * 4;
            for (int channel = 0; channel < 3; ++channel) encoded[size_t(channel) * count + index] = quantize(pixel[channel] + 1 - pixel[3]);
            encoded[3 * count + index] = quantize(pixel[3]);
        }
        if (!frozenSink(uint32_t(top), uint32_t(bottom - top), encoded.data(), encoded.data() + count,
                  encoded.data() + 2 * count, encoded.data() + 3 * count))
            return state.Fail(error, QStringLiteral("PSD 合成预览写入失败"));
    }
    return !state.control || state.control->Checkpoint();
} catch (...) {
    if (m_state) return m_state->Fail(error, QStringLiteral("无法生成 PSD 合成预览"));
    if (error) *error = QStringLiteral("无法生成 PSD 合成预览");
    return false;
}
