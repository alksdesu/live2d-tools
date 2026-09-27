#include "cmodelartwork.h"
#include "cmaterialvalidationcache.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <limits>

namespace live2dstruct {
struct CModelArtwork::State::Verification {
    State owned;
    Usage usage;
    QByteArray fingerprint;
    CMaterialValidationCache cache;
};
namespace {
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray owned(const QByteArray& value) { return QByteArray(value.constData(), value.size()); }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool text(const QString& value, int maximum, bool required = false)
{ return value.size() <= maximum && !value.contains(QChar::Null) && (!required || !value.trimmed().isEmpty()); }
bool id(const QString& value) { return text(value, 1024, true); }
bool same(double first, double second) { return std::memcmp(&first, &second, sizeof(double)) == 0; }
std::array<double, 9> coefficients(const QTransform& value)
{ return {value.m11(), value.m12(), value.m13(), value.m21(), value.m22(), value.m23(), value.m31(), value.m32(), value.m33()}; }
bool same(const QTransform& first, const QTransform& second)
{
    const auto a = coefficients(first), b = coefficients(second);
    for (size_t index = 0; index < a.size(); ++index) if (!same(a[index], b[index])) return false;
    return true;
}
bool same(const QRectF& a, const QRectF& b)
{ return same(a.x(), b.x()) && same(a.y(), b.y()) && same(a.width(), b.width()) && same(a.height(), b.height()); }
bool same(const QVector<QPointF>& first, const QVector<QPointF>& second)
{
    if (first.size() != second.size()) return false;
    for (qsizetype index = 0; index < first.size(); ++index)
        if (!same(first[index].x(), second[index].x()) || !same(first[index].y(), second[index].y())) return false;
    return true;
}
bool finite(QPointF point) { return std::isfinite(point.x()) && std::isfinite(point.y()); }
QByteArray doubles(const double* values, qsizetype count)
{
    QByteArray bytes(count * 8, Qt::Uninitialized);
    for (qsizetype index = 0; index < count; ++index) {
        quint64 bits; std::memcpy(&bits, values + index, 8); qToLittleEndian(bits, bytes.data() + index * 8);
    }
    return bytes;
}
QByteArray transformBits(const QTransform& value)
{ const auto numbers = coefficients(value); return doubles(numbers.data(), numbers.size()); }
QByteArray domainBits(const QRectF& value)
{ const double numbers[]{value.x(), value.y(), value.width(), value.height()}; return doubles(numbers, 4); }
QByteArray coordinateBits(const QVector<QPointF>& values)
{
    QByteArray bytes; bytes.reserve(values.size() * 16);
    for (const auto& point : values) { const double numbers[]{point.x(), point.y()}; bytes.append(doubles(numbers, 2)); }
    return bytes;
}
bool fields(const QJsonObject& value, std::initializer_list<const char*> names)
{
    if (value.size() != qsizetype(names.size())) return false;
    for (const auto* name : names) if (!value.contains(QLatin1String(name))) return false;
    return true;
}
bool number(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& result)
{
    const double n = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < minimum || n > maximum) return false;
    result = qint64(n); return true;
}
bool integer(const QJsonValue& value, int& result, int minimum = 0, int maximum = std::numeric_limits<int>::max())
{ qint64 n; if (!number(value, minimum, maximum, n)) return false; result = int(n); return true; }
bool string(const QJsonValue& value, QString& result, int maximum = 32768)
{ if (!value.isString() || !text(value.toString(), maximum)) return false; result = value.toString(); return true; }
bool boolean(const QJsonValue& value, bool& result)
{ if (!value.isBool()) return false; result = value.toBool(); return true; }
bool hex(const QJsonValue& value, QByteArray& result, qsizetype maximum, qsizetype exact = -1)
{
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.size() % 2 || text.size() / 2 > maximum || (exact >= 0 && text.size() != exact * 2)) return false;
    for (const auto ch : text) if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')))) return false;
    result = QByteArray::fromHex(text.toLatin1()); return true;
}
bool hashValue(const QJsonValue& value, QByteArray& result, bool empty = false)
{ return hex(value, result, 32) && (result.size() == 32 || (empty && result.isEmpty())); }
bool readDoubles(const QJsonValue& value, QVector<double>& result, qsizetype maximum, qsizetype exact = -1)
{
    QByteArray bytes;
    if (!hex(value, bytes, maximum * 8, exact < 0 ? -1 : exact * 8) || bytes.size() % 8) return false;
    QVector<double> numbers(bytes.size() / 8);
    for (qsizetype index = 0; index < numbers.size(); ++index) {
        const auto bits = qFromLittleEndian<quint64>(bytes.constData() + index * 8);
        std::memcpy(numbers.data() + index, &bits, 8);
        if (!std::isfinite(numbers[index])) return false;
    }
    result.swap(numbers); return true;
}
bool readTransform(const QJsonValue& value, QTransform& result)
{
    QVector<double> numbers;
    if (!readDoubles(value, numbers, 9, 9)) return false;
    result = QTransform(numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5], numbers[6], numbers[7], numbers[8]);
    return true;
}
bool readSize(const QJsonValue& value, QSize& result)
{
    if (!value.isArray()) return false;
    const auto values = value.toArray(); int width, height;
    if (values.size() != 2 || !integer(values[0], width, 1) || !integer(values[1], height, 1)) return false;
    result = QSize(width, height); return true;
}
bool readBounds(const QJsonValue& value, QRect& result)
{
    if (!value.isArray()) return false;
    const auto values = value.toArray(); int x, y, width, height;
    if (values.size() != 4 || !integer(values[0], x, std::numeric_limits<int>::min()) ||
        !integer(values[1], y, std::numeric_limits<int>::min()) || !integer(values[2], width) || !integer(values[3], height) ||
        qint64(x) + width - 1 > std::numeric_limits<int>::max() || qint64(y) + height - 1 > std::numeric_limits<int>::max() ||
        qint64(x) + width - 1 < std::numeric_limits<int>::min() || qint64(y) + height - 1 < std::numeric_limits<int>::min()) return false;
    result = QRect(x, y, width, height); return true;
}
struct Budget {
    explicit Budget(CMaterialValidationCache& value) : cache(value) {}
    CMaterialValidationCache& cache;
    Artwork::Usage usage;
    qint64 records = 0;
    QMap<QByteArray, QByteArray> blobs;
    bool metadata(qint64 bytes)
    {
        if (bytes < 0 || bytes > Artwork::MaximumMetadataBytes - usage.metadataBytes) return false;
        usage.metadataBytes += bytes; return true;
    }
    bool payload(qint64 bytes) { return bytes == 0 || metadata(bytes + 64); }
    bool addRecords(qint64 count, qint64 width = 0)
    {
        if (count < 0 || count > Artwork::MaximumRecords - records || !payload(count * width)) return false;
        records += count; return true;
    }
    template<class Key, class Value> bool map(const QMap<Key, Value>& values)
    { return addRecords(values.size(), sizeof(Key) + sizeof(Value) + 4 * sizeof(void*)); }
    bool addText(const QString& value)
    { return value.isEmpty() || payload((value.size() + 1) * 2); }
    bool blob(const QByteArray& value, QByteArray* output = nullptr, const QByteArray& expected = {})
    {
        if (value.isEmpty()) { if (output) output->clear(); return true; }
        if (value.size() > Artwork::MaximumBytes) return false;
        QByteArray key = expected;
        auto previous = blobs.constFind(key);
        if (previous == blobs.cend()) {
            if (!cache.ValidateBlob(value, key, nullptr, expected)) return false;
            previous = blobs.constFind(key);
        }
        if (previous != blobs.cend()) { if (*previous != value) return false; }
        else {
            if (value.size() > Artwork::MaximumBytes - usage.encodedBytes || !metadata(64)) return false;
            usage.encodedBytes += value.size(); blobs.insert(key, value);
        }
        if (output) *output = key;
        return true;
    }
};
class FingerprintWriter
{
public:
    explicit FingerprintWriter(QByteArray& bytes) : stream(&bytes, QIODevice::WriteOnly)
    { stream.setVersion(QDataStream::Qt_6_0); }
    FingerprintWriter& operator<<(const QString& value)
    { stream << qint64(value.size()); for (QChar character : value) stream << character.unicode(); return *this; }
    FingerprintWriter& operator<<(const QByteArray& value)
    { stream << qint64(value.size()); if (!value.isEmpty()) stream.writeRawData(value.constData(), value.size()); return *this; }
    FingerprintWriter& operator<<(const QStringList& values)
    { *this << qint64(values.size()); for (const auto& value : values) *this << value; return *this; }
    template<class T> FingerprintWriter& operator<<(const T& value) { stream << value; return *this; }
    QDataStream::Status status() const { return stream.status(); }
private:
    QDataStream stream;
};
bool collect(const Artwork::State& state, Budget& budget, QString* error)
{
    if (!budget.map(state.images) || !budget.map(state.families) || !budget.map(state.revisions) ||
        !budget.map(state.materials) || !budget.map(state.placements) || !budget.map(state.bindings))
        return fail(error, QStringLiteral("作者素材记录或元数据总量超过限制"));
    for (auto it = state.images.cbegin(); it != state.images.cend(); ++it) {
        QByteArray hash; QSize size;
        if (it.key().size() != 32 || !budget.payload(it.key().size()) || it->png.isEmpty() || !budget.blob(it->png, &hash, it.key()) || hash != it.key() ||
            !CPngImage::Size(it->png, size, error) || size != it->size)
            return fail(error, QStringLiteral("作者素材 PNG 摘要或尺寸无效"));
        const qint64 bytes = qint64(size.width()) * size.height() * 4;
        if (bytes > Artwork::MaximumBytes - budget.usage.decodedBytes) return fail(error, QStringLiteral("作者素材解码像素总量超过限制"));
        budget.usage.decodedBytes += bytes;
    }
    const auto texts = [&](std::initializer_list<QString> values) {
        for (const auto& value : values) if (!budget.addText(value)) return false;
        return true;
    };
    for (auto it = state.families.cbegin(); it != state.families.cend(); ++it) {
        const auto& family = *it;
        if (it.key() != family.id || !id(family.id) || !text(family.name, 32768) || !id(family.currentRevision) ||
            (family.kind != Artwork::SourceKind::Psd && family.kind != Artwork::SourceKind::Png && family.kind != Artwork::SourceKind::AtlasSnapshot) ||
            !texts({it.key(), family.id, family.name, family.currentRevision})) return fail(error, QStringLiteral("素材来源标识或类型无效"));
        const auto revision = state.revisions.constFind(family.currentRevision);
        if (revision == state.revisions.cend() || revision->familyId != family.id) return fail(error, QStringLiteral("素材来源的当前修订不存在"));
    }
    QMap<QString, QMap<QString, int>> layerDirectory;
    for (auto it = state.revisions.cbegin(); it != state.revisions.cend(); ++it) {
        const auto& revision = *it; QByteArray source;
        if (it.key() != revision.id || !id(revision.id) || !id(revision.familyId) || !state.families.contains(revision.familyId) ||
            !text(revision.path, 32768) || !text(revision.fileName, 32768) || revision.sourceBytes.isEmpty() ||
            revision.sourceDigest.size() != 32 || !budget.blob(revision.sourceBytes, &source, revision.sourceDigest) || source != revision.sourceDigest || !budget.blob(revision.iccProfile) ||
            revision.canvasSize.width() < 1 || revision.canvasSize.height() < 1 ||
            qint64(revision.canvasSize.width()) * revision.canvasSize.height() > Artwork::MaximumBytes / 4 ||
            (revision.colorPolicy != Artwork::ColorPolicy::PreserveValues && revision.colorPolicy != Artwork::ColorPolicy::ConvertToSRgb) ||
            !Artwork::ValidateTransform(revision.documentToFamily, error) || !budget.addRecords(revision.layers.size(), sizeof(Artwork::Layer)) ||
            !budget.payload(revision.sourceDigest.size()) ||
            !texts({it.key(), revision.id, revision.familyId, revision.path, revision.fileName}))
            return fail(error, QStringLiteral("素材修订的来源、坐标或数量无效"));
        QMap<QString, int> layerIds; QSet<int> sourceIndices;
        for (int index = 0; index < revision.layers.size(); ++index) {
            const auto& layer = revision.layers[index];
            const qint64 width = qint64(layer.bounds.right()) - layer.bounds.x() + 1;
            const qint64 height = qint64(layer.bounds.bottom()) - layer.bounds.y() + 1;
            if (!id(layer.id) || !text(layer.name, 32768) || layerIds.contains(layer.id) || layer.sourceIndex < -1 ||
                (layer.sourceIndex >= 0 && sourceIndices.contains(layer.sourceIndex)) ||
                (layer.kind != Artwork::LayerKind::Raster && layer.kind != Artwork::LayerKind::Group) ||
                layer.parentIndex < -1 || layer.parentIndex >= revision.layers.size() || layer.parentIndex == index ||
                (layer.parentIndex >= 0 && revision.layers[layer.parentIndex].kind != Artwork::LayerKind::Group) ||
                width < 0 || height < 0 || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() || layer.blendMode.size() != 4 ||
                !texts({layer.id, layer.name}) || !budget.addRecords(layer.diagnostics.size(), sizeof(QString)) ||
                !budget.payload(layer.imageDigest.size()) || !budget.payload(layer.blendMode.size()))
                return fail(error, QStringLiteral("素材图层标识、父级或范围无效"));
            for (char value : layer.blendMode) if (value < 32 || value > 126) return fail(error, QStringLiteral("素材图层混合模式编码无效"));
            layerIds.insert(layer.id, index); if (layer.sourceIndex >= 0) sourceIndices.insert(layer.sourceIndex);
            if (layer.kind == Artwork::LayerKind::Group || layer.bounds.isEmpty()) {
                if (!layer.imageDigest.isEmpty()) return fail(error, QStringLiteral("空图层或组不能包含像素引用"));
            } else if (layer.pixelEquivalent || !layer.imageDigest.isEmpty()) {
                const auto image = state.images.constFind(layer.imageDigest);
                if (image == state.images.cend() || image->size != layer.bounds.size()) return fail(error, QStringLiteral("素材图层像素引用或尺寸不匹配"));
            }
            for (const auto& diagnostic : layer.diagnostics)
                if (!text(diagnostic, 32768, true) || !budget.addText(diagnostic)) return fail(error, QStringLiteral("图层诊断文本无效或超过限制"));
            int parent = layer.parentIndex, depth = 0;
            while (parent >= 0) {
                if (parent >= revision.layers.size() || parent == index || ++depth > Artwork::MaximumDepth)
                    return fail(error, QStringLiteral("素材图层父链成环或超过深度限制"));
                parent = revision.layers[parent].parentIndex;
            }
            const double left = layer.bounds.x(), top = layer.bounds.y(), right = left + width, bottom = top + height;
            const QPointF corners[]{QPointF(left, top), QPointF(right, top), QPointF(left, bottom), QPointF(right, bottom)};
            for (auto point : corners) if (!finite(revision.documentToFamily.map(point))) return fail(error, QStringLiteral("素材图层映射超出有限范围"));
        }
        layerDirectory.insert(revision.id, std::move(layerIds));
    }
    for (auto it = state.materials.cbegin(); it != state.materials.cend(); ++it) {
        const auto& material = *it;
        const auto revision = state.revisions.constFind(material.revisionId);
        if (it.key() != material.id || !id(material.id) || !text(material.name, 32768) || !id(material.familyId) || !id(material.layerId) ||
            revision == state.revisions.cend() || revision->familyId != material.familyId ||
            (material.boundary != Artwork::Boundary::Transparent && material.boundary != Artwork::Boundary::Repeat) ||
            !text(material.missingInRevision, 1024) || !texts({it.key(), material.id, material.name, material.familyId, material.revisionId, material.layerId, material.missingInRevision}))
            return fail(error, QStringLiteral("作者材质来源或标识无效"));
        const auto directory = layerDirectory.constFind(material.revisionId);
        const auto index = directory->constFind(material.layerId);
        if (index == directory->cend() || revision->layers[*index].kind != Artwork::LayerKind::Raster || !revision->layers[*index].pixelEquivalent)
            return fail(error, QStringLiteral("作者材质必须引用已支持的栅格图层"));
        if (!material.missingInRevision.isEmpty()) {
            const auto missing = state.revisions.constFind(material.missingInRevision);
            if (missing == state.revisions.cend() || missing->familyId != material.familyId || missing->id == material.revisionId)
                return fail(error, QStringLiteral("保留旧素材的缺失修订记录不一致"));
        }
    }
    for (auto it = state.placements.cbegin(); it != state.placements.cend(); ++it) {
        const auto& placement = *it; const auto& domain = placement.sourceDomain;
        if (it.key() != placement.id || !id(placement.id) || !id(placement.atlasId) || !state.materials.contains(placement.materialId) ||
            placement.gutter < 1 || placement.gutter > 64 ||
            (placement.filter != Artwork::Filter::ExactTexels && placement.filter != Artwork::Filter::LinearLightPremultiplied) ||
            !Artwork::ValidateTransform(placement.familyToAtlas, error) || !finite(domain.topLeft()) ||
            !std::isfinite(domain.width()) || !std::isfinite(domain.height()) || domain.width() <= 0 || domain.height() <= 0 ||
            !finite(domain.bottomRight()) || !texts({it.key(), placement.id, placement.materialId, placement.atlasId}))
            return fail(error, QStringLiteral("素材排布标识、范围或采样规则无效"));
        const QPointF corners[]{domain.topLeft(), domain.topRight(), domain.bottomLeft(), domain.bottomRight()};
        for (auto point : corners) if (!finite(placement.familyToAtlas.map(point))) return fail(error, QStringLiteral("素材排布映射超出有限范围"));
    }
    CModelMaterialState sampled;
    for (auto it = state.bindings.cbegin(); it != state.bindings.cend(); ++it) {
        const auto& binding = *it;
        const auto placement = state.placements.constFind(binding.placementId);
        if (it.key() != binding.meshId || !id(binding.meshId) || placement == state.placements.cend() ||
            binding.sampledMaterial.id != binding.meshId || binding.topologyDigest.size() != 32 ||
            binding.familyCoordinates.size() * 2 != binding.sampledMaterial.uvs.size() ||
            binding.familyCoordinates.size() > Artwork::MaximumCoordinates - budget.usage.coordinateCount ||
            (binding.status != Artwork::BindingStatus::Linked && binding.status != Artwork::BindingStatus::Detached) ||
            !text(binding.detachedReason, 32768, binding.status == Artwork::BindingStatus::Detached) ||
            (binding.status == Artwork::BindingStatus::Linked && !binding.detachedReason.isEmpty()) ||
            !texts({it.key(), binding.meshId, binding.sampledMaterial.id, binding.placementId, binding.detachedReason}) ||
            !budget.payload(binding.topologyDigest.size()) ||
            !budget.payload(binding.familyCoordinates.size() * qint64(sizeof(QPointF))) ||
            !budget.payload(binding.sampledMaterial.uvs.size() * qint64(sizeof(float))))
            return fail(error, QStringLiteral("网格素材绑定标识、顶点或状态无效"));
        for (const auto& point : binding.familyCoordinates)
            if (!finite(point) || !finite(placement->familyToAtlas.map(point)))
                return fail(error, QStringLiteral("网格来源坐标超出有限范围"));
        budget.usage.coordinateCount += binding.familyCoordinates.size(); sampled.append(binding.sampledMaterial);
    }
    return CModelMaterialEdit::Validate(sampled, error);
}
}

bool CModelArtwork::Image::operator==(const Image& o) const { return size == o.size && png == o.png; }
bool CModelArtwork::Family::operator==(const Family& o) const
{ return id == o.id && name == o.name && kind == o.kind && currentRevision == o.currentRevision; }
bool CModelArtwork::Layer::operator==(const Layer& o) const
{
    return id == o.id && name == o.name && parentIndex == o.parentIndex && sourceIndex == o.sourceIndex && kind == o.kind &&
        externalId == o.externalId && bounds == o.bounds && visible == o.visible && clipping == o.clipping && pixelEquivalent == o.pixelEquivalent &&
        opacity == o.opacity && blendMode == o.blendMode && imageDigest == o.imageDigest && diagnostics == o.diagnostics;
}
bool CModelArtwork::Revision::operator==(const Revision& o) const
{
    return id == o.id && familyId == o.familyId && path == o.path && fileName == o.fileName && sourceDigest == o.sourceDigest &&
        sourceBytes == o.sourceBytes && iccProfile == o.iccProfile && canvasSize == o.canvasSize && colorPolicy == o.colorPolicy &&
        same(documentToFamily, o.documentToFamily) && layers == o.layers;
}
bool CModelArtwork::Material::operator==(const Material& o) const
{ return id == o.id && name == o.name && familyId == o.familyId && revisionId == o.revisionId && layerId == o.layerId && boundary == o.boundary && applyLayerOpacity == o.applyLayerOpacity && missingInRevision == o.missingInRevision; }
bool CModelArtwork::Placement::operator==(const Placement& o) const
{ return id == o.id && materialId == o.materialId && atlasId == o.atlasId && same(familyToAtlas, o.familyToAtlas) && same(sourceDomain, o.sourceDomain) && gutter == o.gutter && filter == o.filter; }
bool CModelArtwork::Binding::operator==(const Binding& o) const
{ return meshId == o.meshId && placementId == o.placementId && same(familyCoordinates, o.familyCoordinates) && sampledMaterial == o.sampledMaterial && topologyDigest == o.topologyDigest && status == o.status && detachedReason == o.detachedReason; }
bool CModelArtwork::State::IsEmpty() const
{ return images.isEmpty() && families.isEmpty() && revisions.isEmpty() && materials.isEmpty() && placements.isEmpty() && bindings.isEmpty(); }
bool CModelArtwork::State::operator==(const State& o) const
{ return images == o.images && families == o.families && revisions == o.revisions && materials == o.materials && placements == o.placements && bindings == o.bindings; }

bool CModelArtwork::TopologyDigest(int vertexCount, const QVector<quint16>& indices, QByteArray& result, QString* error) try
{
    if (vertexCount < 0 || vertexCount > CModelMaterialEdit::MaximumValues / 2 || indices.size() > MaximumTopologyIndices || indices.size() % 3)
        return fail(error, QStringLiteral("素材绑定拓扑数量无效"));
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << QByteArrayLiteral("artwork-topology") << quint32(1) << qint32(vertexCount) << qint64(indices.size());
    for (quint16 index : indices) {
        if (index >= vertexCount) return fail(error, QStringLiteral("素材绑定拓扑索引越界"));
        stream << index;
    }
    if (stream.status() != QDataStream::Ok) return fail(error, QStringLiteral("无法计算素材拓扑指纹"));
    result = digest(bytes); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法计算素材拓扑指纹")); }

bool CModelArtwork::ValidateTransform(const QTransform& transform, QString* error)
{
    for (double value : coefficients(transform)) if (!std::isfinite(value)) return fail(error, QStringLiteral("素材变换包含非有限数值"));
    if (transform.m13() != 0 || transform.m23() != 0 || transform.m33() != 1)
        return fail(error, QStringLiteral("素材变换必须是二维仿射矩阵"));
    const double scale = std::max({std::abs(transform.m11()), std::abs(transform.m12()), std::abs(transform.m21()), std::abs(transform.m22())});
    if (scale == 0) return fail(error, QStringLiteral("素材变换不可逆"));
    const double a = transform.m11() / scale, b = transform.m12() / scale, c = transform.m21() / scale, d = transform.m22() / scale;
    const double determinant = a * d - b * c;
    const double norm = std::max(std::abs(a) + std::abs(c), std::abs(b) + std::abs(d));
    const double inverseNorm = std::max(std::abs(d) + std::abs(c), std::abs(b) + std::abs(a));
    if (determinant == 0 || !std::isfinite(determinant) || norm * inverseNorm > std::abs(determinant) * 1e12)
        return fail(error, QStringLiteral("素材变换不可逆或精度不足"));
    const double inverse[]{(d / determinant) / scale, (-b / determinant) / scale, (-c / determinant) / scale, (a / determinant) / scale};
    for (double value : inverse) if (!std::isfinite(value)) return fail(error, QStringLiteral("素材逆变换超出有限范围"));
    if (!std::isfinite(inverse[0] * transform.dx() + inverse[2] * transform.dy()) ||
        !std::isfinite(inverse[1] * transform.dx() + inverse[3] * transform.dy()))
        return fail(error, QStringLiteral("素材逆变换平移超出有限范围"));
    if (error) error->clear(); return true;
}

bool CModelArtwork::Validate(const State& state, Usage* usage, QString* error)
{
    CMaterialValidationCache cache;
    return ValidateWithCache(state, cache, usage, error);
}

bool CModelArtwork::ValidateWithCache(const State& state, CMaterialValidationCache& cache, Usage* usage, QString* error) try
{
    if (state.m_verification) {
        const auto& verified = *state.m_verification;
        if (state == verified.owned) {
            if (!cache.Merge(verified.cache, error)) return false;
            if (usage) *usage = verified.usage;
            if (error) error->clear(); return true;
        }
        QSet<QByteArray> referenced;
        for (auto image = state.images.cbegin(); image != state.images.cend(); ++image) referenced.insert(image.key());
        for (const auto& revision : state.revisions) {
            referenced.insert(revision.sourceDigest);
            if (!revision.iccProfile.isEmpty()) referenced.insert(digest(revision.iccProfile));
        }
        auto reusable = verified.cache; reusable.Retain(referenced);
        if (!cache.Merge(reusable, error)) return false;
    }
    Budget budget(cache);
    if (!collect(state, budget, error)) return false;
    for (auto image = state.images.cbegin(); image != state.images.cend(); ++image) {
        QByteArray digest;
        if (!cache.ValidatePng(image->png, image->size, digest, error, image.key())) return false;
    }
    if (usage) *usage = budget.usage;
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证作者素材")); }

bool CModelArtwork::Freeze(const State& state, State& result, QString* error) try
{
    if (state.IsEmpty()) { result = State{}; if (error) error->clear(); return true; }
    if (state.m_verification && state == state.m_verification->owned) {
        auto candidate = state.m_verification->owned; candidate.m_verification = state.m_verification;
        result = std::move(candidate); if (error) error->clear(); return true;
    }
    CMaterialValidationCache cache; Usage usage;
    if (!ValidateWithCache(state, cache, &usage, error)) return false;
    State candidate; QSet<QByteArray> retained;
    const auto copyBlob = [&](const QByteArray& value, const QByteArray& expected = QByteArray()) {
        if (value.isEmpty()) return QByteArray();
        QByteArray key;
        if (!cache.ValidateBlob(value, key, error, expected)) throw std::runtime_error("material bytes changed during freeze");
        retained.insert(key); return cache.m_records.value(key).bytes;
    };
    for (auto it = state.images.cbegin(); it != state.images.cend(); ++it) candidate.images.insert(owned(it.key()), {it->size, copyBlob(it->png, it.key())});
    for (auto it = state.families.cbegin(); it != state.families.cend(); ++it)
        candidate.families.insert(owned(it.key()), {owned(it->id), owned(it->name), it->kind, owned(it->currentRevision)});
    for (auto it = state.revisions.cbegin(); it != state.revisions.cend(); ++it) {
        Revision revision = *it;
        revision.id = owned(it->id); revision.familyId = owned(it->familyId); revision.path = owned(it->path); revision.fileName = owned(it->fileName);
        revision.sourceDigest = owned(it->sourceDigest); revision.sourceBytes = copyBlob(it->sourceBytes, it->sourceDigest); revision.iccProfile = copyBlob(it->iccProfile);
        revision.layers.clear(); revision.layers.reserve(it->layers.size());
        for (const auto& original : it->layers) {
            Layer layer = original; layer.id = owned(original.id); layer.name = owned(original.name);
            layer.blendMode = owned(original.blendMode); layer.imageDigest = owned(original.imageDigest); layer.diagnostics.clear();
            for (const auto& message : original.diagnostics) layer.diagnostics.append(owned(message));
            revision.layers.append(std::move(layer));
        }
        candidate.revisions.insert(owned(it.key()), std::move(revision));
    }
    for (auto it = state.materials.cbegin(); it != state.materials.cend(); ++it)
        candidate.materials.insert(owned(it.key()), {owned(it->id), owned(it->name), owned(it->familyId), owned(it->revisionId), owned(it->layerId),
            it->boundary, it->applyLayerOpacity, owned(it->missingInRevision)});
    for (auto it = state.placements.cbegin(); it != state.placements.cend(); ++it)
        candidate.placements.insert(owned(it.key()), {owned(it->id), owned(it->materialId), owned(it->atlasId), it->familyToAtlas,
            it->sourceDomain, it->gutter, it->filter});
    for (auto it = state.bindings.cbegin(); it != state.bindings.cend(); ++it) {
        Binding binding = *it; binding.meshId = owned(it->meshId); binding.placementId = owned(it->placementId);
        binding.familyCoordinates = QVector<QPointF>(it->familyCoordinates.cbegin(), it->familyCoordinates.cend());
        binding.sampledMaterial.id = owned(it->sampledMaterial.id);
        binding.sampledMaterial.uvs = QVector<float>(it->sampledMaterial.uvs.cbegin(), it->sampledMaterial.uvs.cend());
        binding.topologyDigest = owned(it->topologyDigest); binding.detachedReason = owned(it->detachedReason);
        candidate.bindings.insert(owned(it.key()), std::move(binding));
    }
    QByteArray fingerprint;
    if (!WriteFingerprint(candidate, fingerprint, error)) return false;
    cache.Retain(retained);
    auto verified = std::make_shared<State::Verification>();
    verified->owned = candidate; verified->usage = usage; verified->fingerprint = std::move(fingerprint); verified->cache = std::move(cache);
    candidate.m_verification = std::move(verified);
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法冻结作者素材，原状态保持不变")); }

bool CModelArtwork::EncodeArchive(const State& input, Archive& result, QString* error) try
{
    State state; if (!Freeze(input, state, error)) return false;
    CMaterialValidationCache cache;
    if (!ValidateWithCache(state, cache, nullptr, error)) return false;
    Budget budget(cache); if (!collect(state, budget, error)) return false;
    QJsonObject images, families, revisions, materials, placements, bindings;
    for (auto it = state.images.cbegin(); it != state.images.cend(); ++it) images.insert(QString::fromLatin1(it.key().toHex()), QJsonArray{it->size.width(), it->size.height()});
    for (auto it = state.families.cbegin(); it != state.families.cend(); ++it)
        families.insert(it.key(), QJsonObject{{"name", it->name}, {"kind", int(it->kind)}, {"currentRevision", it->currentRevision}});
    for (auto it = state.revisions.cbegin(); it != state.revisions.cend(); ++it) {
        QJsonArray layers;
        for (const auto& layer : it->layers) {
            QJsonArray diagnostics; for (const auto& message : layer.diagnostics) diagnostics.append(message);
            layers.append(QJsonObject{{"id", layer.id}, {"name", layer.name}, {"parentIndex", layer.parentIndex}, {"sourceIndex", layer.sourceIndex},
                {"kind", int(layer.kind)}, {"externalId", layer.externalId ? QJsonValue(qint64(*layer.externalId)) : QJsonValue(QJsonValue::Null)},
                {"bounds", QJsonArray{layer.bounds.x(), layer.bounds.y(), layer.bounds.width(), layer.bounds.height()}}, {"visible", layer.visible},
                {"clipping", layer.clipping}, {"pixelEquivalent", layer.pixelEquivalent}, {"opacity", layer.opacity},
                {"blendMode", QString::fromLatin1(layer.blendMode)}, {"imageDigest", QString::fromLatin1(layer.imageDigest.toHex())}, {"diagnostics", diagnostics}});
        }
        revisions.insert(it.key(), QJsonObject{{"familyId", it->familyId}, {"path", it->path}, {"fileName", it->fileName},
            {"sourceDigest", QString::fromLatin1(it->sourceDigest.toHex())}, {"iccDigest", it->iccProfile.isEmpty() ? QString() : QString::fromLatin1(digest(it->iccProfile).toHex())},
            {"canvasSize", QJsonArray{it->canvasSize.width(), it->canvasSize.height()}}, {"colorPolicy", int(it->colorPolicy)},
            {"documentToFamily", QString::fromLatin1(transformBits(it->documentToFamily).toHex())}, {"layers", layers}});
    }
    for (auto it = state.materials.cbegin(); it != state.materials.cend(); ++it)
        materials.insert(it.key(), QJsonObject{{"name", it->name}, {"familyId", it->familyId}, {"revisionId", it->revisionId}, {"layerId", it->layerId},
            {"boundary", int(it->boundary)}, {"applyLayerOpacity", it->applyLayerOpacity}, {"missingInRevision", it->missingInRevision}});
    for (auto it = state.placements.cbegin(); it != state.placements.cend(); ++it)
        placements.insert(it.key(), QJsonObject{{"materialId", it->materialId}, {"atlasId", it->atlasId}, {"familyToAtlas", QString::fromLatin1(transformBits(it->familyToAtlas).toHex())},
            {"sourceDomain", QString::fromLatin1(domainBits(it->sourceDomain).toHex())}, {"gutter", it->gutter}, {"filter", int(it->filter)}});
    for (auto it = state.bindings.cbegin(); it != state.bindings.cend(); ++it) {
        QJsonObject sampled;
        if (!CModelMaterialEdit::ToJson({{it->meshId, it->sampledMaterial}}, sampled, error)) return false;
        bindings.insert(it.key(), QJsonObject{{"placementId", it->placementId}, {"familyCoordinates", QString::fromLatin1(coordinateBits(it->familyCoordinates).toHex())},
            {"sampledMaterial", sampled}, {"topologyDigest", QString::fromLatin1(it->topologyDigest.toHex())}, {"status", int(it->status)}, {"detachedReason", it->detachedReason}});
    }
    Archive candidate; candidate.blobs = std::move(budget.blobs);
    candidate.metadata = QJsonObject{{"version", 1}, {"images", images}, {"families", families}, {"revisions", revisions},
        {"materials", materials}, {"placements", placements}, {"bindings", bindings}};
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法保存作者素材")); }

bool CModelArtwork::DecodeArchive(const Archive& archive, State& result, QString* error) try
{
    const QJsonObject object = archive.metadata;
    const auto invalid = [&] { return fail(error, QStringLiteral("作者素材存储格式或引用无效")); };
    int version;
    if (!fields(object, {"version", "images", "families", "revisions", "materials", "placements", "bindings"}) ||
        !integer(object["version"], version, 1, 1)) return invalid();
    qint64 records = 0;
    if (archive.blobs.size() > MaximumRecords * 3) return invalid();
    for (const auto* name : {"images", "families", "revisions", "materials", "placements", "bindings"}) {
        if (!object[QLatin1String(name)].isObject()) return invalid();
        const auto count = object[QLatin1String(name)].toObject().size();
        if (count > MaximumRecords - records) return invalid(); records += count;
    }
    State candidate; QMap<QByteArray, QByteArray> blobs; QSet<QByteArray> used; qint64 bytes = 0, coordinates = 0;
    for (auto it = archive.blobs.cbegin(); it != archive.blobs.cend(); ++it) {
        if (it.key().size() != 32 || it->isEmpty() || it->size() > MaximumBytes - bytes) return invalid();
        const auto value = owned(*it);
        if (digest(value) != it.key()) return invalid();
        bytes += value.size(); blobs.insert(owned(it.key()), value);
    }
    const auto blob = [&](const QByteArray& key, QByteArray& target, bool empty = false) {
        if (empty && key.isEmpty()) { target.clear(); return true; }
        const auto found = blobs.constFind(key); if (found == blobs.cend()) return false;
        used.insert(key); target = *found; return true;
    };
    const auto images = object["images"].toObject();
    for (auto it = images.begin(); it != images.end(); ++it) {
        QByteArray key; Image image;
        if (!hashValue(it.key(), key) || !readSize(*it, image.size) || !blob(key, image.png)) return invalid();
        candidate.images.insert(key, std::move(image));
    }
    const auto families = object["families"].toObject();
    for (auto it = families.begin(); it != families.end(); ++it) {
        if (!it->isObject() || !id(it.key())) return invalid();
        const auto row = it->toObject(); Family family; family.id = it.key(); int kind;
        if (!fields(row, {"name", "kind", "currentRevision"}) || !string(row["name"], family.name) ||
            !integer(row["kind"], kind, 0, 2) || !string(row["currentRevision"], family.currentRevision, 1024)) return invalid();
        family.kind = SourceKind(kind); candidate.families.insert(it.key(), std::move(family));
    }
    const auto revisions = object["revisions"].toObject();
    for (auto it = revisions.begin(); it != revisions.end(); ++it) {
        if (!it->isObject() || !id(it.key())) return invalid();
        const auto row = it->toObject(); Revision revision; revision.id = it.key(); QByteArray icc; int policy;
        if (!fields(row, {"familyId", "path", "fileName", "sourceDigest", "iccDigest", "canvasSize", "colorPolicy", "documentToFamily", "layers"}) ||
            !string(row["familyId"], revision.familyId, 1024) || !string(row["path"], revision.path) || !string(row["fileName"], revision.fileName) ||
            !hashValue(row["sourceDigest"], revision.sourceDigest) || !blob(revision.sourceDigest, revision.sourceBytes) ||
            !hashValue(row["iccDigest"], icc, true) || !blob(icc, revision.iccProfile, true) || !readSize(row["canvasSize"], revision.canvasSize) ||
            !integer(row["colorPolicy"], policy, 0, 1) || !readTransform(row["documentToFamily"], revision.documentToFamily) || !row["layers"].isArray()) return invalid();
        revision.colorPolicy = ColorPolicy(policy); const auto layers = row["layers"].toArray();
        if (layers.size() > MaximumRecords - records) return invalid(); records += layers.size();
        for (const auto& item : layers) {
            if (!item.isObject()) return invalid(); const auto layerRow = item.toObject(); Layer layer; int kind, opacity; QString blend;
            if (!fields(layerRow, {"id", "name", "parentIndex", "sourceIndex", "kind", "externalId", "bounds", "visible", "clipping", "pixelEquivalent", "opacity", "blendMode", "imageDigest", "diagnostics"}) ||
                !string(layerRow["id"], layer.id, 1024) || !string(layerRow["name"], layer.name) || !integer(layerRow["parentIndex"], layer.parentIndex, -1) ||
                !integer(layerRow["sourceIndex"], layer.sourceIndex, -1) || !integer(layerRow["kind"], kind, 0, 1) || !readBounds(layerRow["bounds"], layer.bounds) ||
                !boolean(layerRow["visible"], layer.visible) || !boolean(layerRow["clipping"], layer.clipping) || !boolean(layerRow["pixelEquivalent"], layer.pixelEquivalent) ||
                !integer(layerRow["opacity"], opacity, 0, 255) || !string(layerRow["blendMode"], blend, 4) ||
                !hashValue(layerRow["imageDigest"], layer.imageDigest, true) || !layerRow["diagnostics"].isArray()) return invalid();
            layer.kind = LayerKind(kind); layer.opacity = quint8(opacity); layer.blendMode = blend.toLatin1();
            if (QString::fromLatin1(layer.blendMode) != blend) return invalid();
            if (!layerRow["externalId"].isNull()) {
                qint64 external; if (!number(layerRow["externalId"], 0, std::numeric_limits<quint32>::max(), external)) return invalid();
                layer.externalId = quint32(external);
            }
            const auto diagnostics = layerRow["diagnostics"].toArray();
            if (diagnostics.size() > MaximumRecords - records) return invalid(); records += diagnostics.size();
            for (const auto& entry : diagnostics) { QString message; if (!string(entry, message)) return invalid(); layer.diagnostics.append(message); }
            revision.layers.append(std::move(layer));
        }
        candidate.revisions.insert(it.key(), std::move(revision));
    }
    const auto materials = object["materials"].toObject();
    for (auto it = materials.begin(); it != materials.end(); ++it) {
        if (!it->isObject() || !id(it.key())) return invalid(); const auto row = it->toObject(); Material value; value.id = it.key(); int boundary;
        if (!fields(row, {"name", "familyId", "revisionId", "layerId", "boundary", "applyLayerOpacity", "missingInRevision"}) ||
            !string(row["name"], value.name) || !string(row["familyId"], value.familyId, 1024) || !string(row["revisionId"], value.revisionId, 1024) ||
            !string(row["layerId"], value.layerId, 1024) || !integer(row["boundary"], boundary, 0, 1) || !boolean(row["applyLayerOpacity"], value.applyLayerOpacity) ||
            !string(row["missingInRevision"], value.missingInRevision, 1024)) return invalid();
        value.boundary = Boundary(boundary); candidate.materials.insert(it.key(), std::move(value));
    }
    const auto placements = object["placements"].toObject();
    for (auto it = placements.begin(); it != placements.end(); ++it) {
        if (!it->isObject() || !id(it.key())) return invalid(); const auto row = it->toObject(); Placement value; value.id = it.key(); int filter; QVector<double> domain;
        if (!fields(row, {"materialId", "atlasId", "familyToAtlas", "sourceDomain", "gutter", "filter"}) ||
            !string(row["materialId"], value.materialId, 1024) || !string(row["atlasId"], value.atlasId, 1024) || !readTransform(row["familyToAtlas"], value.familyToAtlas) ||
            !readDoubles(row["sourceDomain"], domain, 4, 4) || !integer(row["gutter"], value.gutter, 1, 64) || !integer(row["filter"], filter, 0, 1)) return invalid();
        value.sourceDomain = QRectF(domain[0], domain[1], domain[2], domain[3]); value.filter = Filter(filter); candidate.placements.insert(it.key(), std::move(value));
    }
    const auto bindings = object["bindings"].toObject();
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        if (!it->isObject() || !id(it.key())) return invalid(); const auto row = it->toObject(); Binding value; value.meshId = it.key(); int status;
        QVector<double> points; CModelMaterialOverrides sampled;
        if (!fields(row, {"placementId", "familyCoordinates", "sampledMaterial", "topologyDigest", "status", "detachedReason"}) ||
            !string(row["placementId"], value.placementId, 1024) || !readDoubles(row["familyCoordinates"], points, CModelMaterialEdit::MaximumValues - coordinates * 2) || points.size() % 2 ||
            !row["sampledMaterial"].isObject() || row["sampledMaterial"].toObject().size() != 1 ||
            !row["sampledMaterial"].toObject().value(value.meshId).isObject() ||
            row["sampledMaterial"].toObject().value(value.meshId).toObject().value("uvs").toString().size() != points.size() * 8 ||
            !CModelMaterialEdit::FromJson(row["sampledMaterial"].toObject(), sampled, error) || sampled.size() != 1 || !sampled.contains(value.meshId) ||
            !hashValue(row["topologyDigest"], value.topologyDigest) || !integer(row["status"], status, 0, 1) || !string(row["detachedReason"], value.detachedReason)) return invalid();
        coordinates += points.size() / 2; value.familyCoordinates.reserve(points.size() / 2);
        for (qsizetype index = 0; index < points.size(); index += 2) value.familyCoordinates.append(QPointF(points[index], points[index + 1]));
        value.sampledMaterial = sampled.value(value.meshId); value.status = BindingStatus(status); candidate.bindings.insert(it.key(), std::move(value));
    }
    if (used.size() != blobs.size() || !Validate(candidate, nullptr, error)) return invalid();
    return Freeze(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取作者素材，原状态保持不变")); }

bool CModelArtwork::ToJson(const State& state, QJsonObject& result, QString* error) try
{
    Archive archive; if (!EncodeArchive(state, archive, error)) return false;
    QJsonObject blobs;
    for (auto it = archive.blobs.cbegin(); it != archive.blobs.cend(); ++it)
        blobs.insert(QString::fromLatin1(it.key().toHex()), QString::fromLatin1(it->toBase64()));
    archive.metadata.insert("blobs", blobs); result = std::move(archive.metadata);
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法保存作者素材")); }

bool CModelArtwork::FromJson(const QJsonObject& object, State& result, QString* error) try
{
    const auto invalid = [&] { return fail(error, QStringLiteral("作者素材存储格式或引用无效")); };
    if (!fields(object, {"version", "blobs", "images", "families", "revisions", "materials", "placements", "bindings"}) ||
        !object["blobs"].isObject() || object["blobs"].toObject().size() > MaximumRecords * 3) return invalid();
    QMap<QByteArray, QByteArray> blobs; qint64 bytes = 0;
    const auto encoded = object["blobs"].toObject();
    for (auto it = encoded.begin(); it != encoded.end(); ++it) {
        QByteArray key; if (!hashValue(it.key(), key) || !it->isString()) return invalid();
        const auto value = it->toString(); const qint64 maximum = ((MaximumBytes - bytes + 2) / 3) * 4;
        if (value.isEmpty() || value.size() % 4 || value.size() > maximum) return invalid();
        for (const auto ch : value) if (ch.unicode() > 127) return invalid();
        const auto raw = value.toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(raw, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > MaximumBytes - bytes ||
            decoded.decoded.toBase64() != raw || digest(decoded.decoded) != key) return invalid();
        bytes += decoded.decoded.size(); blobs.insert(key, decoded.decoded);
    }
    Archive archive{object, std::move(blobs)}; archive.metadata.remove("blobs");
    return DecodeArchive(archive, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取作者素材，原状态保持不变")); }

bool CModelArtwork::Fingerprint(const State& state, QByteArray& result, QString* error) try
{
    if (state.m_verification && state == state.m_verification->owned) {
        result = state.m_verification->fingerprint; if (error) error->clear(); return true;
    }
    if (!Validate(state, nullptr, error)) return false;
    return WriteFingerprint(state, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法计算作者素材指纹")); }

bool CModelArtwork::WriteFingerprint(const State& state, QByteArray& result, QString* error) try
{
    QByteArray bytes; FingerprintWriter stream(bytes);
    stream << quint32(1) << qint64(state.images.size());
    for (auto it = state.images.cbegin(); it != state.images.cend(); ++it) stream << it.key() << it->size << qint64(it->png.size());
    stream << qint64(state.families.size());
    for (const auto& value : state.families) stream << value.id << value.name << qint32(value.kind) << value.currentRevision;
    stream << qint64(state.revisions.size());
    for (const auto& value : state.revisions) {
        stream << value.id << value.familyId << value.path << value.fileName << value.sourceDigest << qint64(value.sourceBytes.size())
            << digest(value.iccProfile) << qint64(value.iccProfile.size()) << value.canvasSize << qint32(value.colorPolicy)
            << transformBits(value.documentToFamily) << qint64(value.layers.size());
        for (const auto& layer : value.layers) {
            stream << layer.id << layer.name << qint32(layer.parentIndex) << qint32(layer.sourceIndex) << qint32(layer.kind) << bool(layer.externalId);
            if (layer.externalId) stream << *layer.externalId;
            stream << layer.bounds << layer.visible << layer.clipping << layer.pixelEquivalent << layer.opacity << layer.blendMode << layer.imageDigest << layer.diagnostics;
        }
    }
    stream << qint64(state.materials.size());
    for (const auto& value : state.materials) stream << value.id << value.name << value.familyId << value.revisionId << value.layerId << qint32(value.boundary) << value.applyLayerOpacity << value.missingInRevision;
    stream << qint64(state.placements.size());
    for (const auto& value : state.placements) stream << value.id << value.materialId << value.atlasId << transformBits(value.familyToAtlas) << domainBits(value.sourceDomain) << qint32(value.gutter) << qint32(value.filter);
    stream << qint64(state.bindings.size());
    for (const auto& value : state.bindings) stream << value.meshId << value.placementId << coordinateBits(value.familyCoordinates)
        << CModelMaterialEdit::Fingerprint({{value.meshId, value.sampledMaterial}}) << value.topologyDigest << qint32(value.status) << value.detachedReason;
    if (stream.status() != QDataStream::Ok) return fail(error, QStringLiteral("无法计算作者素材指纹"));
    result = digest(bytes); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法计算作者素材指纹")); }
}
