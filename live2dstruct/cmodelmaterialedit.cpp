#include "cmodelmaterialedit.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QByteArray bits(const QVector<float>& values)
{
    QByteArray result(values.size() * 4, Qt::Uninitialized);
    for (qsizetype index = 0; index < values.size(); ++index) {
        quint32 value; std::memcpy(&value, values.constData() + index, 4);
        qToLittleEndian(value, result.data() + index * 4);
    }
    return result;
}
}
bool CModelDrawableMaterial::operator==(const CModelDrawableMaterial& other) const
{
    return id == other.id && textureIndex == other.textureIndex && uvs.size() == other.uvs.size() &&
        (uvs.isEmpty() || std::memcmp(uvs.constData(), other.uvs.constData(), size_t(uvs.size()) * sizeof(float)) == 0);
}
bool CModelMaterialEdit::Validate(const CModelMaterialState& state, QString* error)
{
    if (state.size() > 100000) return fail(error, QStringLiteral("材质对象数量超出范围"));
    QSet<QString> ids;
    qsizetype values = 0;
    for (const auto& material : state) {
        if (material.id.isEmpty() || material.id.size() > 1024 || material.id.contains(QChar::Null) || ids.contains(material.id) ||
            (material.textureIndex < 0 && (!material.uvs.isEmpty() || material.textureIndex != -1)) || material.uvs.size() % 2 || material.uvs.size() > MaximumValues - values)
            return fail(error, QStringLiteral("材质对象标识、纹理槽或 UV 数量无效：%1").arg(material.id));
        for (const float value : material.uvs) if (!std::isfinite(value))
            return fail(error, QStringLiteral("UV 包含非有限数值：%1").arg(material.id));
        ids.insert(material.id); values += material.uvs.size();
    }
    if (error) error->clear();
    return true;
}
bool CModelMaterialEdit::ToJson(const CModelMaterialOverrides& state, QJsonObject& result, QString* error) try
{
    CModelMaterialState values;
    for (auto it = state.cbegin(); it != state.cend(); ++it) {
        if (it.key() != it->id) return fail(error, QStringLiteral("材质覆盖标识不一致"));
        values.append(*it);
    }
    if (!Validate(values, error)) return false;
    QJsonObject object;
    for (const auto& material : values)
        object.insert(material.id, QJsonObject{{"textureIndex", material.textureIndex}, {"uvs", QString::fromLatin1(bits(material.uvs).toHex())}});
    result = std::move(object); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法保存材质覆盖")); }
bool CModelMaterialEdit::FromJson(const QJsonObject& object, CModelMaterialOverrides& result, QString* error) try
{
    if (object.size() > 100000) return fail(error, QStringLiteral("材质对象数量超出范围"));
    CModelMaterialOverrides candidate;
    CModelMaterialState values;
    qsizetype count = 0;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto row = it->toObject(); const auto index = row.value("textureIndex"); const auto uv = row.value("uvs");
        if (!it->isObject() || row.size() != 2 || !index.isDouble() || !uv.isString() ||
            !std::isfinite(index.toDouble()) || index.toDouble() < -1 || index.toDouble() > std::numeric_limits<int>::max() ||
            std::floor(index.toDouble()) != index.toDouble()) return fail(error, QStringLiteral("材质覆盖格式无效：%1").arg(it.key()));
        const auto text = uv.toString();
        if (text.size() % 16 || text.size() / 8 > MaximumValues - count)
            return fail(error, QStringLiteral("材质 UV 编码长度无效：%1").arg(it.key()));
        const auto bytes = QByteArray::fromHex(text.toLatin1());
        if (QString::fromLatin1(bytes.toHex()) != text) return fail(error, QStringLiteral("材质 UV 编码无效：%1").arg(it.key()));
        CModelDrawableMaterial material{it.key(), int(index.toDouble()), {}};
        material.uvs.resize(bytes.size() / 4);
        for (qsizetype value = 0; value < material.uvs.size(); ++value) {
            const auto raw = qFromLittleEndian<quint32>(bytes.constData() + value * 4);
            std::memcpy(material.uvs.data() + value, &raw, 4);
        }
        count += material.uvs.size(); values.append(material); candidate.insert(material.id, std::move(material));
    }
    if (!Validate(values, error)) return false;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取材质覆盖")); }
QByteArray CModelMaterialEdit::Fingerprint(const CModelMaterialOverrides& state)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << qint64(state.size());
    for (auto it = state.cbegin(); it != state.cend(); ++it) stream << it.key() << it->id << qint32(it->textureIndex) << bits(it->uvs);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
}
