#include "cmocblendshapeconstraintbuilder.h"
#include "cmocparameterstructurebuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocBlendShapeConstraintBuilder;
using Span = CSectionValidation::ColumnSpan;
constexpr int MaximumBytes = 256 * 1024 * 1024;
constexpr int MaximumValues = 1000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool range(qint64 begin, qint64 count, qint64 size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}

QByteArray digest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QMap<QByteArray, Span> spans(const CSectionTable& table)
{
    QMap<QByteArray, Span> result;
    for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        span.width = CSectionValidation::StorageWidth(span);
        result.insert(span.name, span);
    }
    return result;
}

float real(const QByteArray& bytes, const Span& span, int row)
{
    if (span.width != 4 || !span.offset || !range(row, 1, span.count)) throw std::length_error("float column bounds");
    quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + span.offset + row * 4);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString id(const QByteArray& bytes, const Span& span, int row)
{
    if (span.width != 64 || !span.offset || !range(row, 1, span.count)) throw std::length_error("id column bounds");
    const char* pointer = bytes.constData() + span.offset + row * 64;
    const char* end = static_cast<const char*>(std::memchr(pointer, 0, 64));
    return QString::fromUtf8(pointer, end ? end - pointer : 64);
}

bool validValues(const QVector<Builder::Value>& values, float minimum, float maximum, QString* error)
{
    if (values.isEmpty() || values.size() > MaximumValues)
        return fail(error, QStringLiteral("融合限制曲线不能为空或超出数量限制"));
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
        return fail(error, QStringLiteral("融合限制参数范围无效"));
    float previous = 0.0f;
    for (int index = 0; index < values.size(); ++index) {
        const auto& value = values[index];
        if (!std::isfinite(value.key) || !std::isfinite(value.weight) || value.key < minimum || value.key > maximum ||
            value.weight < 0.0f || value.weight > 1.0f)
            return fail(error, QStringLiteral("融合限制曲线包含超出参数范围、权重范围或非有限值"));
        if (index && value.key <= previous)
            return fail(error, QStringLiteral("融合限制曲线关键值必须严格递增"));
        previous = value.key;
    }
    return true;
}

bool capture(const QByteArray& bytes, const Builder::Key& key, Builder::Frame& result, QString* error)
{
    if (bytes.isEmpty() || bytes.size() > MaximumBytes || key.constraintIndex < 0)
        return fail(error, QStringLiteral("融合限制来源或索引无效"));
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (!table.IsValid()) return fail(error, QStringLiteral("融合限制需要有效的模型区段"));
    const auto columns = spans(table);
    const auto constraintParameter = columns.constFind("blend_shape_constraints.parameter_indices");
    const auto constraintBegin = columns.constFind("blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices");
    const auto constraintCount = columns.constFind("blend_shape_constraints.blend_shape_constraint_value_sources_counts");
    const auto parameterIds = columns.constFind("parameters.ids");
    const auto parameterMinimum = columns.constFind("parameters.min_values");
    const auto parameterMaximum = columns.constFind("parameters.max_values");
    const auto valueKeys = columns.constFind("blend_shape_constraint_values.keys");
    const auto valueWeights = columns.constFind("blend_shape_constraint_values.weights");
    if (constraintParameter == columns.cend() || constraintBegin == columns.cend() || constraintCount == columns.cend() ||
        parameterIds == columns.cend() || parameterMinimum == columns.cend() || parameterMaximum == columns.cend() ||
        valueKeys == columns.cend() || valueWeights == columns.cend())
        return fail(error, QStringLiteral("模型缺少融合限制曲线列"));
    if (constraintParameter->count != constraintBegin->count || constraintParameter->count != constraintCount->count ||
        key.constraintIndex >= constraintParameter->count)
        return fail(error, QStringLiteral("融合限制索引超出范围"));
    const int parameterIndex = qFromLittleEndian<qint32>(bytes.constData() + constraintParameter->offset + key.constraintIndex * 4);
    if (parameterIndex < 0 || parameterIndex >= parameterIds->count)
        return fail(error, QStringLiteral("融合限制参数索引无效"));
    const int begin = qFromLittleEndian<qint32>(bytes.constData() + constraintBegin->offset + key.constraintIndex * 4);
    const int count = qFromLittleEndian<qint32>(bytes.constData() + constraintCount->offset + key.constraintIndex * 4);
    if (valueKeys->width != 4 || valueWeights->width != 4 || !range(begin, count, valueKeys->count) || !range(begin, count, valueWeights->count))
        return fail(error, QStringLiteral("融合限制曲线值区间无效"));
    const float minimum = real(bytes, *parameterMinimum, parameterIndex);
    const float maximum = real(bytes, *parameterMaximum, parameterIndex);
    QVector<Builder::Value> values;
    values.reserve(count);
    for (int index = 0; index < count; ++index)
        values.append({real(bytes, *valueKeys, begin + index), real(bytes, *valueWeights, begin + index)});
    if (!validValues(values, minimum, maximum, error)) return false;
    result.key = key;
    result.sourceDigest = digest(bytes);
    result.parameterIndex = parameterIndex;
    result.parameterId = id(bytes, *parameterIds, parameterIndex);
    result.parameterMinimum = minimum;
    result.parameterMaximum = maximum;
    result.values = std::move(values);
    return true;
}
}

bool CMocBlendShapeConstraintBuilder::Capture(const QByteArray& bytes, const Key& key, Frame& result, QString* error)
{
    try {
        if (error) error->clear();
        Frame candidate;
        if (!capture(bytes, key, candidate, error)) return false;
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("融合限制曲线读取失败，原输出保持不变"));
    }
}

bool CMocBlendShapeConstraintBuilder::Build(const QByteArray& bytes, const Request& request, Result& result, QString* error)
{
    try {
        if (error) error->clear();
        Frame current;
        if (!capture(bytes, request.before.key, current, error)) return false;
        if (!(current == request.before)) return fail(error, QStringLiteral("融合限制曲线来源已改变"));
        if (request.values == current.values || !validValues(request.values, current.parameterMinimum, current.parameterMaximum, error))
            return request.values == current.values ? fail(error, QStringLiteral("融合限制曲线没有变化")) : false;
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        if (!table.IsValid()) return fail(error, QStringLiteral("融合限制需要有效的模型区段"));
        const auto columns = spans(table);
        const auto beginSpan = columns.constFind("blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices");
        const auto countSpan = columns.constFind("blend_shape_constraints.blend_shape_constraint_value_sources_counts");
        const auto keySpan = columns.constFind("blend_shape_constraint_values.keys");
        const auto weightSpan = columns.constFind("blend_shape_constraint_values.weights");
        if (beginSpan == columns.cend() || countSpan == columns.cend() || keySpan == columns.cend() || weightSpan == columns.cend())
            return fail(error, QStringLiteral("模型缺少融合限制曲线列"));
        if (keySpan->count != weightSpan->count || keySpan->countIndex != weightSpan->countIndex ||
            keySpan->width != 4 || weightSpan->width != 4 || request.values.size() > MaximumValues - keySpan->count ||
            qint64(bytes.size()) + qint64(request.values.size()) * 8 + 512 > MaximumBytes)
            return fail(error, QStringLiteral("融合限制曲线超出区段或字节预算"));
        CMocParameterStructureBuilder::ColumnState state;
        const auto append = [&](const Span& span, bool keys) {
            auto data = bytes.mid(span.offset, span.count * span.width);
            const auto oldSize = data.size();
            data.resize(oldSize + request.values.size() * 4);
            for (int index = 0; index < request.values.size(); ++index) {
                const float value = keys ? request.values[index].key : request.values[index].weight;
                quint32 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                qToLittleEndian(bits, data.data() + oldSize + index * 4);
            }
            state.columns[span.slot] = std::move(data);
        };
        append(*keySpan, true);
        append(*weightSpan, false);
        state.counts[keySpan->countIndex] = int(keySpan->count) + request.values.size();
        const auto redirect = [&](const Span& span, int value) {
            auto data = bytes.mid(span.offset, span.count * span.width);
            qToLittleEndian(qint32(value), data.data() + current.key.constraintIndex * 4);
            state.columns[span.slot] = std::move(data);
        };
        redirect(*beginSpan, int(keySpan->count));
        redirect(*countSpan, request.values.size());
        Result candidate;
        if (!CMocParameterStructureBuilder::BuildColumnState(bytes, state, candidate.bytes, error)) return false;
        auto outputReader = CReader::FromBytes(candidate.bytes);
        CSectionTable output(outputReader);
        if (!output.IsValid()) return fail(error, QStringLiteral("融合限制曲线写回结构无效"));
        const auto outputColumns = spans(output);
        for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
            const auto written = outputColumns.constFind(it.key());
            if (written == outputColumns.cend() || written->width != it->width ||
                written->count != state.counts.value(it->countIndex, int(it->count)))
                return fail(error, QStringLiteral("融合限制曲线写回区段定义不匹配"));
            const auto original = it->offset ? bytes.mid(it->offset, it->count * it->width) : QByteArray();
            const auto expected = state.columns.value(it->slot, original);
            const auto actual = written->offset ? candidate.bytes.mid(written->offset, written->count * written->width) : QByteArray();
            if (actual != expected) return fail(error, QStringLiteral("融合限制曲线改变了未请求的列：%1").arg(QString::fromLatin1(it.key())));
        }
        if (!Capture(candidate.bytes, request.before.key, candidate.frame, error) || candidate.frame.values != request.values)
            return fail(error, QStringLiteral("融合限制曲线写回内容不一致"));
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("融合限制曲线写回失败，原输出保持不变"));
    }
}
}
