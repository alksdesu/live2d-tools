#ifndef REAL_CORE_MODEL_FIXTURE_H
#define REAL_CORE_MODEL_FIXTURE_H

#include "core/Live2DCubismCore.h"
#include <QtTest>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }

class CoreModel
{
    struct Memory {
        std::vector<unsigned char> bytes;
        unsigned char* data = nullptr;
        size_t length = 0;
        void allocate(size_t size, size_t alignment) {
            bytes.assign(size + alignment + 128, 0xa5);
            data = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(bytes.data()) + alignment - 1) & ~(alignment - 1));
            length = size;
        }
        bool intact() const {
            return data && std::all_of(bytes.data(), static_cast<const unsigned char*>(data), [](unsigned char b) { return b == 0xa5; }) &&
                std::all_of(static_cast<const unsigned char*>(data) + length, bytes.data() + bytes.size(), [](unsigned char b) { return b == 0xa5; });
        }
    } moc, model;
public:
    csmModel* value = nullptr;
    bool load(const QByteArray& bytes) {
        moc.allocate(bytes.size(), csmAlignofMoc);
        std::memcpy(moc.data, bytes.constData(), bytes.size());
        if (!csmHasMocConsistency(moc.data, bytes.size())) return false;
        std::memcpy(moc.data, bytes.constData(), bytes.size());
        auto* revived = csmReviveMocInPlace(moc.data, bytes.size());
        if (!revived) return false;
        const unsigned size = csmGetSizeofModel(revived);
        if (!size || size > 512u * 1024u * 1024u) return false;
        model.allocate(size, csmAlignofModel);
        value = csmInitializeModelInPlace(revived, model.data, size);
        return value && intact();
    }
    bool intact() const { return moc.intact() && model.intact(); }
    void pose(const QVector<float>& values) {
        Q_ASSERT(values.size() == csmGetParameterCount(value));
        std::copy(values.cbegin(), values.cend(), csmGetParameterValues(value));
        csmResetDrawableDynamicFlags(value);
        csmUpdateModel(value);
    }
};

template<class T> QByteArray raw(const T* values, int count) { return count ? QByteArray(reinterpret_cast<const char*>(values), qsizetype(count) * sizeof(T)) : QByteArray(); }

struct Frame { QByteArray definition; QVector<QVector<float>> values; QVector<int> positionFields; float pixelsPerUnit = 0; };
Frame frame(csmModel* model)
{
    Frame result;
    const int parts = csmGetPartCount(model), count = csmGetDrawableCount(model);
    result.definition += raw(csmGetPartParentPartIndices(model), parts);
    for (int part = 0; part < parts; ++part) result.definition += QByteArray(csmGetPartIds(model)[part]) + '\0';
    csmVector2 canvas, origin; float ppu;
    csmReadCanvasInfo(model, &canvas, &origin, &ppu);
    result.pixelsPerUnit = ppu;
    result.definition += raw(&canvas, 1) + raw(&origin, 1) + raw(&ppu, 1);
    for (int index = 0; index < count; ++index) {
        result.definition += QByteArray(csmGetDrawableIds(model)[index]) + '\0';
        result.definition += raw(csmGetDrawableVertexUvs(model)[index], csmGetDrawableVertexCounts(model)[index]);
        result.definition += raw(csmGetDrawableIndices(model)[index], csmGetDrawableIndexCounts(model)[index]);
        result.definition += raw(csmGetDrawableMasks(model)[index], csmGetDrawableMaskCounts(model)[index]);
        result.definition += raw(csmGetDrawableConstantFlags(model) + index, 1);
        for (const auto* field : {csmGetDrawableTextureIndices(model), csmGetDrawableDrawOrders(model), csmGetDrawableRenderOrders(model), csmGetDrawableParentPartIndices(model)})
            result.definition += raw(field + index, 1);
        QVector<float> values;
        const auto* vertices = csmGetDrawableVertexPositions(model)[index];
        for (int vertex = 0; vertex < csmGetDrawableVertexCounts(model)[index]; ++vertex) values << vertices[vertex].X << vertices[vertex].Y;
        result.positionFields.append(values.size());
        values << csmGetDrawableOpacities(model)[index];
        for (const auto& color : {csmGetDrawableMultiplyColors(model)[index], csmGetDrawableScreenColors(model)[index]}) values << color.X << color.Y << color.Z << color.W;
        result.values.append(values);
    }
    result.values.append(QVector<float>(csmGetPartOpacities(model), csmGetPartOpacities(model) + parts));
    result.positionFields.append(0);
    return result;
}

QSet<int> changed(const Frame& a, const Frame& b, const QSet<int>& interpolatedDrawables = {})
{
    QSet<int> result;
    if (a.definition != b.definition || a.values.size() != b.values.size()) return {-1};
    for (int index = 0; index < a.values.size(); ++index) {
        if (a.values[index].size() != b.values[index].size()) return {-1};
        for (int field = 0; field < a.values[index].size(); ++field) {
            const double x = a.values[index][field], y = b.values[index][field];
            // Core changes float interpolation order with a new axis; bound its rounding in source pixels.
            const double limit = interpolatedDrawables.contains(index) && field < a.positionFields[index]
                ? .1 / double(a.pixelsPerUnit) : 1e-6 + 2e-6 * std::max(std::abs(x), std::abs(y));
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x - y) > limit) result.insert(index);
        }
    }
    return result;
}

QString difference(const Frame& a, const Frame& b, const QSet<int>& interpolatedDrawables = {})
{
    if (a.definition != b.definition) {
        for (int index = 0; index < std::min(a.definition.size(), b.definition.size()); ++index)
            if (a.definition[index] != b.definition[index]) return QStringLiteral("static byte %1: %2 != %3").arg(index).arg(quint8(a.definition[index])).arg(quint8(b.definition[index]));
        return QStringLiteral("static length");
    }
    for (int index = 0; index < std::min(a.values.size(), b.values.size()); ++index)
        for (int field = 0; field < std::min(a.values[index].size(), b.values[index].size()); ++field) {
            const double x = a.values[index][field], y = b.values[index][field];
            const double limit = interpolatedDrawables.contains(index) && field < a.positionFields[index]
                ? .1 / double(a.pixelsPerUnit) : 1e-6 + 2e-6 * std::max(std::abs(x), std::abs(y));
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x - y) > limit)
                return QStringLiteral("drawable %1 field %2: %3 != %4 (limit %5)").arg(index).arg(field).arg(x, 0, 'g', 12).arg(y, 0, 'g', 12).arg(limit, 0, 'g', 12);
        }
    return {};
}

QVector<QVector<float>> poses(csmModel* model)
{
    const int count = csmGetParameterCount(model);
    const auto* defaults = csmGetParameterDefaultValues(model);
    QVector<QVector<float>> result{QVector<float>(defaults, defaults + count)};
    for (const auto* limits : {csmGetParameterMinimumValues(model), csmGetParameterMaximumValues(model)}) {
        result.append(QVector<float>(limits, limits + count));
        for (int index = 0; index < count; ++index) { auto pose = result.first(); pose[index] = limits[index]; result.append(pose); }
    }
    return result;
}

QVector<QPair<QString, QString>> models()
{
    QVector<QPair<QString, QString>> result;
    for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) result.append({QString::fromLatin1(name), QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3"});
    const auto extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
    if (!extra.isEmpty()) result.append({QStringLiteral("external-moc"), extra});
    return result;
}

}

#endif
