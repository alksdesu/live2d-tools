#include "cmocparameterstructurebuilder.h"
#include "cmoclayoutrewriter.h"
#include "ckeyformbindinggrid.h"
#include "csectiontable.h"
#include "csectionvalidation.h"

#include <QSet>
#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocParameterStructureBuilder;
constexpr int MaximumCount = 1000000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool range(qint64 begin, qint64 count, qint64 size)
{
    return count >= 0 && (count == 0 ? begin >= -1 && begin <= size : begin >= 0 && begin <= size && count <= size - begin);
}

qint32 word(const QByteArray& bytes, qsizetype offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

QByteArray integer(int value)
{
    QByteArray result(4, Qt::Uninitialized);
    qToLittleEndian(qint32(value), result.data());
    return result;
}

QByteArray floating(float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    QByteArray result(4, Qt::Uninitialized);
    qToLittleEndian(bits, result.data());
    return result;
}

struct Layout
{
    QByteArray bytes;
    CReader reader;
    CSectionTable table;
    QMap<int, CSectionValidation::ColumnSpan> columns;
    QHash<QString, int> names;
    QVector<int> counts;
    QMap<int, QByteArray> changes;

    explicit Layout(const QByteArray& input) : bytes(input), reader(CReader::FromBytes(input)), table(reader)
    {
        if (!table.IsValid()) return;
        for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            if (std::strcmp(span.name, "parameters.runtime_space_0") == 0 ||
                std::strcmp(span.name, "parameters.extension_runtime_space_0") == 0) span.width = 8;
            columns.insert(span.slot, span);
            names.insert(QString::fromLatin1(span.name), span.slot);
        }
        const int total = CSectionValidation::KnownCountCount(table.sectionOffsetTable->mocVersion);
        for (int index = 0; index < total; ++index)
            counts.append(word(bytes, table.sectionOffsetTable->count_info->count_info_offset + index * 4));
    }

    int slot(const QString& name) const { return names.value(name, -1); }
    const CSectionValidation::ColumnSpan& column(const QString& name) const { return columns.constFind(slot(name)).value(); }
    bool present(const QString& name) const { return names.contains(name) && (column(name).offset || changes.contains(slot(name))); }
    QByteArray data(int key) const
    {
        const auto changed = changes.constFind(key);
        if (changed != changes.cend()) return changed.value();
        const auto& span = columns.constFind(key).value();
        return span.offset ? bytes.mid(span.offset, span.count * span.width) : QByteArray();
    }
    QByteArray data(const QString& name) const { return data(slot(name)); }
    const char* address(const QString& name, int row, qint64 width) const
    {
        const auto& span = column(name);
        const auto changed = changes.constFind(span.slot);
        const qint64 size = changed != changes.cend() ? changed->size() : (span.offset ? span.count * span.width : 0);
        if (!size) return nullptr;
        if (!range(qint64(row) * width, width, size)) throw std::length_error("MOC field read outside column");
        return (changed != changes.cend() ? changed->constData() : bytes.constData() + span.offset) + qint64(row) * width;
    }
    int value(const QString& name, int row) const
    {
        const auto* pointer = address(name, row, 4);
        return pointer ? qFromLittleEndian<qint32>(pointer) : 0;
    }
    float real(const QString& name, int row) const
    {
        const quint32 bits = quint32(value(name, row));
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }
    QString id(const QString& name, int row) const
    {
        const auto* pointer = address(name, row, 64);
        if (!pointer) return {};
        const auto* end = static_cast<const char*>(std::memchr(pointer, 0, 64));
        return QString::fromUtf8(pointer, end ? end - pointer : 64);
    }
    QByteArray& writable(int key)
    {
        auto found = changes.find(key);
        if (found == changes.end()) found = changes.insert(key, data(key));
        return found.value();
    }
    void set(const QString& name, int row, const QByteArray& field)
    {
        const auto& span = column(name);
        auto& contents = writable(span.slot);
        if (field.size() != span.width || !range(row, 1, contents.size() / span.width))
            throw std::length_error("MOC field write outside column");
        std::memcpy(contents.data() + qsizetype(row) * span.width, field.constData(), span.width);
    }
    void append(const QString& name, const QByteArray& contents)
    {
        auto& bytes = writable(slot(name));
        if (contents.size() > std::numeric_limits<int>::max() - bytes.size())
            throw std::length_error("MOC column too large");
        bytes.append(contents);
    }
    int appendRows(const QString& group, const QVector<int>& rows)
    {
        int countIndex = -1;
        for (const auto& span : columns) {
            if (QString::fromLatin1(span.name).section('.', 0, 0) != group) continue;
            countIndex = span.countIndex;
            if (span.optional && !present(QString::fromLatin1(span.name))) continue;
            const auto changed = changes.constFind(span.slot);
            const qint64 size = changed != changes.cend() ? changed->size() : span.count * span.width;
            const auto* original = changed != changes.cend() ? changed->constData() : bytes.constData() + span.offset;
            if (rows.size() > MaximumCount - counts[countIndex])
                throw std::length_error("MOC row count limit");
            QByteArray added(rows.size() * span.width, Qt::Uninitialized);
            for (int index = 0; index < rows.size(); ++index) {
                const int row = rows[index];
                if (!range(row, 1, size / span.width))
                    throw std::length_error("MOC source row outside column");
                std::memcpy(added.data() + qint64(index) * span.width, original + qint64(row) * span.width, span.width);
            }
            writable(span.slot).append(added);
        }
        if (countIndex < 0) throw std::length_error("Unknown MOC row group");
        const int begin = counts[countIndex];
        counts[countIndex] += rows.size();
        return begin;
    }
};

std::unique_ptr<Layout> load(const QByteArray& bytes, QString* error)
{
    auto result = std::make_unique<Layout>(bytes);
    if (!result->table.IsValid()) {
        fail(error, result->reader.Error());
        return {};
    }
    for (const auto& span : result->columns) {
        if (span.slot < 0 || span.countIndex < 0 || span.countIndex >= result->counts.size() ||
            span.count != result->counts[span.countIndex] ||
            (span.offset && !range(span.offset, span.count * span.width, bytes.size()))) {
            fail(error, QStringLiteral("MOC 区段定义或完整存储空间无效：%1").arg(QString::fromLatin1(span.name)));
            return {};
        }
    }
    return result;
}

bool writeLayout(Layout& layout, QByteArray& result, QString* error)
{
    QByteArray candidate = layout.bytes;
    QMap<int, qint64> relocated;
    for (auto it = layout.changes.cbegin(); it != layout.changes.cend(); ++it) {
        const auto& span = layout.columns.constFind(it.key()).value();
        const qint64 required = qint64(layout.counts[span.countIndex]) * span.width;
        if (it->isEmpty() && span.optional) {
            qToLittleEndian(quint32(0), candidate.data() + span.slot);
            relocated.insert(span.slot, 0);
            continue;
        }
        if (it->size() != required)
            return fail(error, QStringLiteral("MOC 列长度与数量不一致：%1").arg(QString::fromLatin1(span.name)));
        const qint64 offset = (qint64(candidate.size()) + 63) & ~qint64(63);
        const qint64 end = (offset + it->size() + 63) & ~qint64(63);
        if (end > std::numeric_limits<int>::max()) return fail(error, QStringLiteral("MOC 文件长度超出范围"));
        const auto previous = candidate.size();
        candidate.resize(end);
        std::memset(candidate.data() + previous, 0, end - previous);
        if (!it->isEmpty()) std::memcpy(candidate.data() + offset, it->constData(), it->size());
        qToLittleEndian(quint32(offset), candidate.data() + span.slot);
        relocated.insert(span.slot, it->size());
    }
    const int countOffset = layout.table.sectionOffsetTable->count_info->count_info_offset;
    for (int index = 0; index < layout.counts.size(); ++index) {
        if (layout.counts[index] < 0 || layout.counts[index] > MaximumCount)
            return fail(error, QStringLiteral("MOC 区段数量越界"));
        qToLittleEndian(qint32(layout.counts[index]), candidate.data() + countOffset + index * 4);
    }
    return CMocLayoutRewriter::Rewrite(layout.bytes, candidate, relocated, result, error);
}

bool catalog(const Layout& layout, QHash<QString, int>& ids, QString* error)
{
    const auto raw = layout.data(QStringLiteral("parameters.ids"));
    for (int index = 0; index < layout.counts[5]; ++index) {
        const auto record = raw.mid(index * 64, 64);
        const QString id = layout.id(QStringLiteral("parameters.ids"), index);
        if (!record.contains('\0') || id.isEmpty() || ids.contains(id))
            return fail(error, QStringLiteral("源参数标识为空、重复或未终止"));
        ids.insert(id, index);
    }
    return true;
}

bool validDefinition(const Builder::Definition& definition, QString* error)
{
    const auto id = definition.id.toUtf8();
    if (id.isEmpty() || id.size() >= 64 || id.contains('\0') || QString::fromUtf8(id) != definition.id)
        return fail(error, QStringLiteral("参数 ID 必须非空、无空字符且 UTF-8 长度小于 64 字节"));
    if (definition.type != 0) return fail(error, QStringLiteral("当前只支持创建普通参数"));
    if (!std::isfinite(definition.minimum) || !std::isfinite(definition.maximum) ||
        !std::isfinite(definition.defaultValue) || definition.minimum > definition.maximum ||
        definition.defaultValue < definition.minimum || definition.defaultValue > definition.maximum ||
        definition.decimalPlaces < 0 || definition.decimalPlaces > 9)
        return fail(error, QStringLiteral("参数范围、默认值或精度无效"));
    return true;
}

bool understoodStructure(const Layout& layout, QString* error)
{
    const int headerEnd = layout.table.sectionOffsetTable->count_info->count_info_offset;
    if (headerEnd % 4 || headerEnd > 0x40 + MaximumCount * 4)
        return fail(error, QStringLiteral("MOC 扩展文件头范围无效"));
    QVector<int> offsets;
    for (int slot = 0x40; slot < headerEnd; slot += 4) {
        const int offset = word(layout.bytes, slot);
        if (offset && (offset < headerEnd || offset > layout.bytes.size()))
            return fail(error, QStringLiteral("MOC 未解释区段偏移无效"));
        if (offset) offsets.append(offset);
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    const auto nextCount = std::upper_bound(offsets.cbegin(), offsets.cend(), headerEnd);
    const int metadataEnd = nextCount == offsets.cend() ? layout.bytes.size() : *nextCount;
    for (int offset = headerEnd + layout.counts.size() * 4; offset + 4 <= metadataEnd; offset += 4)
        if (word(layout.bytes, offset))
            return fail(error, QStringLiteral("模型含未解释的活动区段数量 %1，不能重排参数结构").arg((offset - headerEnd) / 4));
    for (int slot = 0x48; slot < headerEnd; slot += 4) {
        if (layout.columns.contains(slot)) continue;
        const int offset = word(layout.bytes, slot);
        if (!offset || offset == layout.bytes.size()) continue;
        const auto next = std::upper_bound(offsets.cbegin(), offsets.cend(), offset);
        const int end = next == offsets.cend() ? layout.bytes.size() : *next;
        if (end > offset)
            return fail(error, QStringLiteral("模型含未解释的活动区段 0x%1，不能重排参数结构").arg(slot, 0, 16));
    }
    return true;
}

bool opaqueCompatible(const Layout& original, const Layout& effective, QString* error)
{
    if (original.bytes.left(0x40) != effective.bytes.left(0x40))
        return fail(error, QStringLiteral("结构状态不能更改 MOC 版本或文件头"));
    const auto beforeCanvas = original.table.sectionOffsetTable->canvas_info->CanvasOffset;
    const auto afterCanvas = effective.table.sectionOffsetTable->canvas_info->CanvasOffset;
    if (original.bytes.mid(beforeCanvas, 20) != effective.bytes.mid(afterCanvas, 20))
        return fail(error, QStringLiteral("结构状态不能更改画布定义"));
    const int beforeHeader = original.table.sectionOffsetTable->count_info->count_info_offset;
    const int afterHeader = effective.table.sectionOffsetTable->count_info->count_info_offset;
    if (beforeHeader != afterHeader || beforeHeader % 4 || beforeHeader > 0x40 + MaximumCount * 4)
        return fail(error, QStringLiteral("结构状态的扩展文件头范围不一致"));
    QVector<int> physical;
    for (int slot = 0x40; slot < beforeHeader; slot += 4) {
        const int offset = word(original.bytes, slot);
        if (offset && (offset < beforeHeader || offset > original.bytes.size()))
            return fail(error, QStringLiteral("源 MOC 未解释区段偏移无效"));
        if (offset) physical.append(offset);
    }
    std::sort(physical.begin(), physical.end());
    physical.erase(std::unique(physical.begin(), physical.end()), physical.end());
    const auto extent = [&](int offset) {
        const auto next = std::upper_bound(physical.cbegin(), physical.cend(), offset);
        return (next == physical.cend() ? original.bytes.size() : *next) - offset;
    };
    const int countBytes = CSectionValidation::CountInfoSize(original.table.sectionOffsetTable->mocVersion);
    const qint64 countTail = extent(beforeHeader) - countBytes;
    if (countTail > 0 && original.bytes.mid(beforeHeader + countBytes, countTail) !=
        effective.bytes.mid(afterHeader + countBytes, countTail))
        return fail(error, QStringLiteral("结构状态不能更改未解释的区段数量"));
    for (int slot = 0x48; slot < beforeHeader; slot += 4) {
        if (original.columns.contains(slot)) continue;
        const int before = word(original.bytes, slot), after = word(effective.bytes, slot);
        if (!before && !after) continue;
        if (!before || !after || !range(after, extent(before), effective.bytes.size()) ||
            original.bytes.mid(before, extent(before)) != effective.bytes.mid(after, extent(before)))
            return fail(error, QStringLiteral("结构状态不能更改未解释的区段 0x%1").arg(slot, 0, 16));
    }
    return true;
}

template<typename Function> bool guarded(QString* error, Function&& function)
{
    if (error) error->clear();
    try { return function(); }
    catch (const std::bad_alloc&) { return fail(error, QStringLiteral("参数结构操作内存不足")); }
    catch (const std::length_error&) { return fail(error, QStringLiteral("参数结构数量或索引超出范围")); }
}
}

bool CMocParameterStructureBuilder::CaptureColumnState(const QByteArray& original, const QByteArray& effective,
                                                      ColumnState& state, QString* error)
{
    return guarded(error, [&] {
        auto before = load(original, error), after = load(effective, error);
        if (!before || !after || !opaqueCompatible(*before, *after, error)) return false;
        if (before->columns.keys() != after->columns.keys() || before->counts.size() != after->counts.size())
            return fail(error, QStringLiteral("结构状态区段定义不一致"));
        ColumnState candidate;
        for (auto it = before->columns.cbegin(); it != before->columns.cend(); ++it) {
            const auto& updated = after->columns.constFind(it.key()).value();
            const auto oldData = before->data(it.key()), newData = after->data(it.key());
            if (oldData != newData || it->count != updated.count ||
                (it->optional && bool(it->offset) != bool(updated.offset) && updated.count))
                candidate.columns.insert(it.key(), newData);
        }
        for (int index = 0; index < before->counts.size(); ++index)
            if (before->counts[index] != after->counts[index]) candidate.counts.insert(index, after->counts[index]);
        if ((!candidate.columns.isEmpty() || !candidate.counts.isEmpty()) && !understoodStructure(*before, error)) return false;
        state = std::move(candidate);
        return true;
    });
}

bool CMocParameterStructureBuilder::BuildColumnState(const QByteArray& original, const ColumnState& state,
                                                    QByteArray& bytes, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(original, error);
        if (!layout) return false;
        if ((!state.columns.isEmpty() || !state.counts.isEmpty()) && !understoodStructure(*layout, error)) return false;
        for (auto it = state.counts.cbegin(); it != state.counts.cend(); ++it) {
            if (it.key() < 0 || it.key() >= layout->counts.size() || it.value() < 0 || it.value() > MaximumCount)
                return fail(error, QStringLiteral("结构状态包含未知或无效的区段数量"));
            layout->counts[it.key()] = it.value();
        }
        for (auto it = state.columns.cbegin(); it != state.columns.cend(); ++it) {
            if (!layout->columns.contains(it.key())) return fail(error, QStringLiteral("结构状态包含未解释的区段"));
            layout->changes.insert(it.key(), it.value());
        }
        return writeLayout(*layout, bytes, error);
    });
}

namespace {
struct OwnerInfo
{
    Builder::Owner owner;
    int index = -1;
    int source = -1;
    QString group;
    QString keyforms;
    int first = 0;
    int count = 0;
    QVector<int> bindings;
};

bool bindingOwners(const Layout& layout, QVector<int>& owners, QString* error)
{
    owners.fill(-1, layout.counts[13]);
    for (int parameter = 0; parameter < layout.counts[5]; ++parameter) {
        const int first = layout.value(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), parameter);
        const int count = layout.value(QStringLiteral("parameters.parameter_binding_sources_counts"), parameter);
        if (!range(first, count, owners.size())) return fail(error, QStringLiteral("参数绑定所属范围无效"));
        for (int index = 0; index < count; ++index) {
            if (owners[first + index] >= 0) return fail(error, QStringLiteral("参数绑定同时属于多个参数"));
            owners[first + index] = parameter;
        }
    }
    return true;
}

bool readOwnerGrid(const Layout& layout, const Builder::Owner& owner,
               const QVector<int>& owners, const QHash<QPair<int, QString>, int>& objectIndices,
               OwnerInfo& result, QString* error)
{
    if (owner.id.isEmpty() || owner.type < Builder::Owner::ArtMesh || owner.type > Builder::Owner::Rotation)
        return fail(error, QStringLiteral("绑定对象的类型或 ID 无效"));
    OwnerInfo candidate;
    candidate.owner = owner;
    const bool mesh = owner.type == Builder::Owner::ArtMesh;
    const QString objects = mesh ? QStringLiteral("art_meshes") : QStringLiteral("deformers");
    candidate.index = objectIndices.value(qMakePair(int(owner.type), owner.id), -1);
    if (candidate.index == -2) return fail(error, QStringLiteral("绑定对象 ID 重复：%1").arg(owner.id));
    if (candidate.index < 0) return fail(error, QStringLiteral("绑定对象不存在：%1").arg(owner.id));
    candidate.group = mesh ? QStringLiteral("art_meshes") : owner.type == Builder::Owner::Warp
        ? QStringLiteral("warp_deformers") : QStringLiteral("rotation_deformers");
    candidate.keyforms = mesh ? QStringLiteral("art_mesh_keyforms") : owner.type == Builder::Owner::Warp
        ? QStringLiteral("warp_deformer_keyforms") : QStringLiteral("rotation_deformer_keyforms");
    candidate.source = mesh ? candidate.index : layout.value(QStringLiteral("deformers.specific_sources_indices"), candidate.index);
    const int sourceCount = layout.counts[mesh ? 4 : owner.type == Builder::Owner::Warp ? 2 : 3];
    if (!range(candidate.source, 1, sourceCount)) return fail(error, QStringLiteral("变形器专用源索引无效"));
    const int binding = layout.value(objects + QStringLiteral(".keyform_binding_sources_indices"), candidate.index);
    if (!mesh && binding != layout.value(candidate.group + QStringLiteral(".keyform_binding_sources_indices"), candidate.source))
        return fail(error, QStringLiteral("通用变形器与专用源绑定不一致：%1").arg(owner.id));
    candidate.first = layout.value(candidate.group + QStringLiteral(".keyform_sources_begin_indices"), candidate.source);
    candidate.count = layout.value(candidate.group + QStringLiteral(".keyform_sources_counts"), candidate.source);
    if (candidate.count <= 0 || !range(candidate.first, candidate.count, layout.counts[mesh ? 9 : owner.type == Builder::Owner::Warp ? 7 : 8]))
        return fail(error, QStringLiteral("对象关键形范围无效：%1").arg(owner.id));
    qint64 product = 1;
    if (binding != -1) {
        if (!range(binding, 1, layout.counts[12])) return fail(error, QStringLiteral("对象绑定索引无效"));
        const int first = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), binding);
        const int count = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), binding);
        if (!range(first, count, layout.counts[11])) return fail(error, QStringLiteral("对象绑定轴范围无效"));
        for (int index = 0; index < count; ++index) {
            const int reference = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), first + index);
            if (!range(reference, 1, owners.size()) || owners[reference] < 0)
                return fail(error, QStringLiteral("绑定轴缺少唯一参数"));
            const int ownerParameter = owners[reference];
            if (layout.value(QStringLiteral("parameters.parameter_types"), ownerParameter) != 0)
                return fail(error, QStringLiteral("对象含非普通参数绑定轴"));
            const int begin = layout.value(QStringLiteral("parameter_bindings.keys_sources_begin_indices"), reference);
            const int keyCount = layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), reference);
            if (keyCount <= 0 || !range(begin, keyCount, layout.counts[14]) || product > candidate.count / keyCount)
                return fail(error, QStringLiteral("对象绑定关键值数量无效"));
            float previous = 0;
            for (int key = 0; key < keyCount; ++key) {
                const float value = layout.real(QStringLiteral("keys.values"), begin + key);
                if (!std::isfinite(value) || (key && value <= previous))
                    return fail(error, QStringLiteral("源绑定关键值不是有限严格递增序列"));
                previous = value;
            }
            product *= keyCount;
            candidate.bindings.append(reference);
        }
    }
    if (product != candidate.count) return fail(error, QStringLiteral("对象关键形数量与绑定轴不一致"));
    result = std::move(candidate);
    return true;
}

bool requestKeys(const QVector<float>& values, float minimum, float maximum, QByteArray& encoded, QString* error)
{
    if (values.size() < 2 || values.size() > MaximumCount)
        return fail(error, QStringLiteral("新参数轴至少需要两个关键值"));
    for (int index = 0; index < values.size(); ++index) {
        const float value = values[index];
        if (!std::isfinite(value) || value < minimum || value > maximum || (index && value <= values[index - 1]))
            return fail(error, QStringLiteral("新关键值必须有限、严格递增且位于参数范围内"));
        encoded.append(floating(value));
    }
    return true;
}

void ownerCatalog(const Layout& layout, QHash<QPair<int, QString>, int>& objectIndices,
                  QHash<QPair<int, int>, int>& specificUsers)
{
    const auto addObject = [&](int type, const QString& id, int index) {
        const auto key = qMakePair(type, id);
        objectIndices.insert(key, objectIndices.contains(key) ? -2 : index);
    };
    for (int index = 0; index < layout.counts[4]; ++index)
        addObject(Builder::Owner::ArtMesh, layout.id(QStringLiteral("art_meshes.ids"), index), index);
    for (int index = 0; index < layout.counts[1]; ++index) {
        const int type = layout.value(QStringLiteral("deformers.types"), index);
        if (type != 0 && type != 1) continue;
        addObject(type + 1, layout.id(QStringLiteral("deformers.ids"), index), index);
        ++specificUsers[qMakePair(type, layout.value(QStringLiteral("deformers.specific_sources_indices"), index))];
    }
}

struct BindingInsertion
{
    QVector<int> references;
    int position = 0;
    int count = 0;
};

bool insertParameterBindings(Layout& layout, int parameter, const QVector<QByteArray>& sets,
                             BindingInsertion& result, QString* error)
{
    const int oldBegin = layout.value(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), parameter);
    const int oldCount = layout.value(QStringLiteral("parameters.parameter_binding_sources_counts"), parameter);
    if (!range(oldBegin, oldCount, layout.counts[13])) return fail(error, QStringLiteral("参数原绑定范围无效"));
    QHash<QByteArray, int> existing;
    QSet<QPair<int, int>> encodedSlices;
    qint64 keyReferences = 0;
    for (int index = 0; index < oldCount; ++index) {
        const int binding = oldBegin + index;
        const int first = layout.value(QStringLiteral("parameter_bindings.keys_sources_begin_indices"), binding);
        const int count = layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), binding);
        if (!range(first, count, layout.counts[14]) || count <= 0) return fail(error, QStringLiteral("源参数绑定关键值范围无效"));
        const auto slice = qMakePair(first, count);
        if (encodedSlices.contains(slice)) continue;
        encodedSlices.insert(slice);
        if (count > MaximumCount - keyReferences) return fail(error, QStringLiteral("参数绑定关键值引用总量超出范围"));
        keyReferences += count;
        const QByteArray keys(layout.address(QStringLiteral("keys.values"), first, 4), count * 4);
        if (!existing.contains(keys)) existing.insert(keys, binding);
    }
    QVector<QByteArray> added;
    QHash<QByteArray, int> addedIndex;
    for (const auto& keys : sets) if (!existing.contains(keys) && !addedIndex.contains(keys)) {
        addedIndex.insert(keys, added.size());
        added.append(keys);
    }
    const int insertAt = oldCount ? oldBegin + oldCount : layout.counts[13];
    if (added.size() > MaximumCount - layout.counts[13]) return fail(error, QStringLiteral("参数绑定数量超出范围"));
    QByteArray newBegins, newCounts;
    for (const auto& keys : added) {
        if (keys.size() / 4 > MaximumCount - layout.counts[14]) return fail(error, QStringLiteral("关键值数量超出范围"));
        newBegins.append(integer(layout.counts[14]));
        newCounts.append(integer(keys.size() / 4));
        layout.append(QStringLiteral("keys.values"), keys);
        layout.counts[14] += keys.size() / 4;
    }
    if (!added.isEmpty()) {
        layout.writable(layout.slot(QStringLiteral("parameter_bindings.keys_sources_begin_indices"))).insert(insertAt * 4, newBegins);
        layout.writable(layout.slot(QStringLiteral("parameter_bindings.keys_sources_counts"))).insert(insertAt * 4, newCounts);
        for (int index = 0; index < layout.counts[5]; ++index) {
            const int begin = layout.value(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), index);
            if (index != parameter && begin >= insertAt)
                layout.set(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), index, integer(begin + added.size()));
        }
        layout.set(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), parameter, integer(oldCount ? oldBegin : insertAt));
        layout.set(QStringLiteral("parameters.parameter_binding_sources_counts"), parameter, integer(oldCount + added.size()));
        for (int index = 0; index < layout.counts[11]; ++index) {
            const int value = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), index);
            if (!range(value, 1, layout.counts[13])) return fail(error, QStringLiteral("源关键形引用参数绑定越界"));
            if (value >= insertAt) layout.set(QStringLiteral("parameter_binding_indices.binding_sources_indices"), index, integer(value + added.size()));
        }
        layout.counts[13] += added.size();
    }
    result.position = insertAt;
    result.count = added.size();
    for (const auto& keys : sets)
        result.references.append(existing.contains(keys) ? existing.value(keys) : insertAt + addedIndex.value(keys));
    return true;
}

bool bind(Layout& layout, int parameter, const QVector<Builder::BindingRequest>& requests,
          QVector<Builder::KeyformRemap>& remaps, QString* error)
{
    if (requests.isEmpty()) return true;
    if (layout.value(QStringLiteral("parameters.parameter_types"), parameter) != 0)
        return fail(error, QStringLiteral("融合参数不能绑定为普通关键形轴"));
    const float minimum = layout.real(QStringLiteral("parameters.min_values"), parameter);
    const float maximum = layout.real(QStringLiteral("parameters.max_values"), parameter);
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
        return fail(error, QStringLiteral("参数范围无效"));
    QVector<int> owners;
    if (!bindingOwners(layout, owners, error)) return false;
    QHash<QPair<int, QString>, int> objectIndices;
    QHash<QPair<int, int>, int> specificUsers;
    ownerCatalog(layout, objectIndices, specificUsers);
    QVector<OwnerInfo> targets;
    QVector<QByteArray> keySets;
    QSet<QPair<int, QString>> uniqueOwners;
    qint64 addedReferences = 0, addedColors = 0;
    QMap<int, qint64> addedKeyforms;
    for (const auto& request : requests) {
        const auto key = qMakePair(int(request.owner.type), request.owner.id);
        if (uniqueOwners.contains(key)) return fail(error, QStringLiteral("绑定请求包含重复对象"));
        uniqueOwners.insert(key);
        OwnerInfo owner;
        QByteArray keys;
        if (!readOwnerGrid(layout, request.owner, owners, objectIndices, owner, error) ||
            !requestKeys(request.keys, minimum, maximum, keys, error)) return false;
        QSet<int> boundParameters;
        for (int reference : owner.bindings) {
            const int bound = owners[reference];
            if (bound == parameter) return fail(error, QStringLiteral("对象已绑定此参数：%1").arg(request.owner.id));
            if (boundParameters.contains(bound)) return fail(error, QStringLiteral("对象含重复参数轴或不支持的绑定类型"));
            boundParameters.insert(bound);
        }
        if (owner.owner.type != Builder::Owner::ArtMesh &&
            specificUsers.value(qMakePair(owner.owner.type == Builder::Owner::Warp ? 0 : 1, owner.source)) > 1)
            return fail(error, QStringLiteral("共享专用变形器源无法保证无损拆分：%1").arg(owner.owner.id));
        if (request.keys.size() > MaximumCount / owner.count)
            return fail(error, QStringLiteral("新增参数轴后的关键形数量超出范围"));
        const int keyformCountIndex = layout.column(owner.keyforms + QStringLiteral(".opacities")).countIndex;
        const qint64 newRows = qint64(request.keys.size()) * owner.count;
        addedReferences += owner.bindings.size() + 1;
        addedKeyforms[keyformCountIndex] += newRows;
        if (layout.present(owner.group + QStringLiteral(".keyform_color_sources_begin_indices"))) addedColors += newRows;
        if (addedReferences > MaximumCount - layout.counts[11] || requests.size() > MaximumCount - layout.counts[12] ||
            addedKeyforms[keyformCountIndex] > MaximumCount - layout.counts[keyformCountIndex] ||
            addedColors > MaximumCount - layout.counts[23] || addedColors > MaximumCount - layout.counts[24])
            return fail(error, QStringLiteral("绑定后的数组总量超出范围"));
        targets.append(std::move(owner));
        keySets.append(std::move(keys));
    }
    BindingInsertion inserted;
    if (!insertParameterBindings(layout, parameter, keySets, inserted, error)) return false;
    for (auto& owner : targets) for (int& reference : owner.bindings)
        if (reference >= inserted.position) reference += inserted.count;
    if (layout.present(QStringLiteral("parameters.keys_sources_begin_indices")) &&
        layout.present(QStringLiteral("parameters.keys_sources_counts"))) {
        const int first = layout.value(QStringLiteral("parameters.keys_sources_begin_indices"), parameter);
        const int count = layout.value(QStringLiteral("parameters.keys_sources_counts"), parameter);
        if (!range(first, count, layout.counts[14])) return fail(error, QStringLiteral("参数显示关键值范围无效"));
        QVector<float> keys;
        for (int index = 0; index < count; ++index) keys.append(layout.real(QStringLiteral("keys.values"), first + index));
        for (const auto& request : requests) keys += request.keys;
        for (float value : keys) if (!std::isfinite(value)) return fail(error, QStringLiteral("参数关键值包含非有限值"));
        std::stable_sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (keys.size() > MaximumCount - layout.counts[14]) return fail(error, QStringLiteral("参数关键值数量超出范围"));
        layout.set(QStringLiteral("parameters.keys_sources_begin_indices"), parameter, integer(layout.counts[14]));
        layout.set(QStringLiteral("parameters.keys_sources_counts"), parameter, integer(keys.size()));
        QByteArray encoded;
        for (float value : keys) encoded.append(floating(value));
        layout.append(QStringLiteral("keys.values"), encoded);
        layout.counts[14] += keys.size();
    }
    for (int targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        auto owner = targets[targetIndex];
        const auto& request = requests[targetIndex];
        const int binding = inserted.references[targetIndex];
        const int axisBegin = layout.counts[11];
        for (int reference : owner.bindings) layout.append(QStringLiteral("parameter_binding_indices.binding_sources_indices"), integer(reference));
        layout.append(QStringLiteral("parameter_binding_indices.binding_sources_indices"), integer(binding));
        layout.counts[11] += owner.bindings.size() + 1;
        const int newBinding = layout.counts[12]++;
        layout.append(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), integer(axisBegin));
        layout.append(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), integer(owner.bindings.size() + 1));
        QVector<int> rows;
        Builder::KeyformRemap remap{owner.owner, owner.count, owner.count * int(request.keys.size()), int(request.keys.size()), {}};
        rows.reserve(remap.newKeyformCount);
        remap.newLocalToOldLocal.reserve(remap.newKeyformCount);
        for (int key = 0; key < request.keys.size(); ++key) for (int local = 0; local < owner.count; ++local) {
            rows.append(owner.first + local);
            remap.newLocalToOldLocal.append(local);
        }
        const int newFirst = layout.appendRows(owner.keyforms, rows);
        if (owner.owner.type != Builder::Owner::ArtMesh) {
            layout.set(QStringLiteral("deformers.keyform_binding_sources_indices"), owner.index, integer(newBinding));
        }
        layout.set(owner.group + QStringLiteral(".keyform_binding_sources_indices"), owner.source, integer(newBinding));
        layout.set(owner.group + QStringLiteral(".keyform_sources_begin_indices"), owner.source, integer(newFirst));
        layout.set(owner.group + QStringLiteral(".keyform_sources_counts"), owner.source, integer(rows.size()));
        const QString colorBegin = owner.group + QStringLiteral(".keyform_color_sources_begin_indices");
        if (layout.present(colorBegin)) {
            const int original = layout.value(colorBegin, owner.source);
            const int newColorBegin = layout.counts[23];
            if (layout.counts[23] != layout.counts[24])
                return fail(error, QStringLiteral("两种颜色池数量不同，尚不能安全扩展连续颜色源"));
            for (const auto& channel : {qMakePair(QStringLiteral("multiply"), QStringLiteral("keyform_multiply_colors")),
                                       qMakePair(QStringLiteral("screen"), QStringLiteral("keyform_screen_colors"))}) {
                const QString indices = owner.keyforms + '.' + channel.first + QStringLiteral("_color_sources_indices");
                QVector<int> colors;
                for (int row : rows) colors.append(layout.present(indices) ? layout.value(indices, row) : original + row - owner.first);
                const int begin = layout.appendRows(channel.second, colors);
                if (layout.present(indices)) for (int index = 0; index < colors.size(); ++index)
                    layout.set(indices, newFirst + index, integer(begin + index));
            }
            layout.set(colorBegin, owner.source, integer(newColorBegin));
        }
        remaps.append(std::move(remap));
    }
    return true;
}

QVector<int> identity(int size)
{
    QVector<int> result(size);
    for (int index = 0; index < size; ++index) result[index] = index;
    return result;
}
}

bool CMocParameterStructureBuilder::Create(const QByteArray& base, const CreateRequest& request,
                                          Result& result, QString* error)
{
    return guarded(error, [&] {
        if (!validDefinition(request.parameter, error)) return false;
        auto layout = load(base, error);
        if (!layout) return false;
        if (!understoodStructure(*layout, error)) return false;
        QHash<QString, int> ids;
        if (!catalog(*layout, ids, error)) return false;
        if (ids.contains(request.parameter.id)) return fail(error, QStringLiteral("参数 ID 已存在"));
        const int index = layout->counts[5];
        if (index == MaximumCount) return fail(error, QStringLiteral("参数数量超出范围"));
        for (const auto& span : layout->columns) {
            if (span.countIndex != 5 || (span.optional && !span.offset)) continue;
            const QString name = QString::fromLatin1(span.name), field = name.section('.', 1);
            QByteArray value;
            if (field.contains(QStringLiteral("runtime_space"))) value = QByteArray(span.width, '\0');
            else if (field == QLatin1String("ids")) value = request.parameter.id.toUtf8().leftJustified(64, '\0');
            else if (field == QLatin1String("min_values")) value = floating(request.parameter.minimum);
            else if (field == QLatin1String("max_values")) value = floating(request.parameter.maximum);
            else if (field == QLatin1String("default_values")) value = floating(request.parameter.defaultValue);
            else if (field == QLatin1String("decimal_places")) value = integer(request.parameter.decimalPlaces);
            else if (field == QLatin1String("is_repeat")) value = integer(request.parameter.repeat ? 1 : 0);
            else if (field == QLatin1String("parameter_binding_sources_begin_indices") ||
                     field == QLatin1String("blend_shape_parameter_binding_sources_begin_indices")) value = integer(-1);
            else value = integer(0);
            layout->append(name, value);
        }
        ++layout->counts[5];
        Result candidate;
        candidate.oldParameterToNew = identity(index);
        if (!bind(*layout, index, request.bindings, candidate.keyforms, error) || !writeLayout(*layout, candidate.bytes, error)) return false;
        result = std::move(candidate);
        return true;
    });
}

bool CMocParameterStructureBuilder::BindExistingParameter(const QByteArray& base, const QString& parameterId,
                                                         const QVector<BindingRequest>& bindings,
                                                         Result& result, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout) return false;
        if (!understoodStructure(*layout, error)) return false;
        QHash<QString, int> ids;
        if (!catalog(*layout, ids, error)) return false;
        if (!ids.contains(parameterId)) return fail(error, QStringLiteral("参数不存在"));
        if (bindings.isEmpty()) return fail(error, QStringLiteral("没有选择绑定对象"));
        Result candidate;
        candidate.oldParameterToNew = identity(layout->counts[5]);
        if (!bind(*layout, ids.value(parameterId), bindings, candidate.keyforms, error) || !writeLayout(*layout, candidate.bytes, error)) return false;
        result = std::move(candidate);
        return true;
    });
}

namespace {
bool analyze(const Layout& layout, int parameter, QVector<Builder::Reference>& references, QString* error)
{
    const QString parameterId = layout.id(QStringLiteral("parameters.ids"), parameter);
    const int parameterBegin = layout.value(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), parameter);
    const int parameterCount = layout.value(QStringLiteral("parameters.parameter_binding_sources_counts"), parameter);
    if (!range(parameterBegin, parameterCount, layout.counts[13])) return fail(error, QStringLiteral("参数绑定范围无效"));
    QVector<int> prefix(layout.counts[11] + 1);
    for (int index = 0; index < layout.counts[11]; ++index) {
        const int binding = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), index);
        if (!range(binding, 1, layout.counts[13])) return fail(error, QStringLiteral("关键形参数引用越界"));
        prefix[index + 1] = prefix[index] + (binding >= parameterBegin && binding < qint64(parameterBegin) + parameterCount);
    }
    QVector<bool> uses(layout.counts[12]);
    for (int index = 0; index < uses.size(); ++index) {
        const int first = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), index);
        const int count = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), index);
        if (!range(first, count, layout.counts[11])) return fail(error, QStringLiteral("关键形轴范围无效"));
        uses[index] = count && prefix[first + count] != prefix[first];
    }
    const auto add = [&](const QString& kind, const QString& type, const QString& id, const QString& detail) {
        references.append({kind, type, id, detail});
    };
    const auto checkBinding = [&](int binding, const QString& type, const QString& id) {
        if (binding == -1) return true;
        if (!range(binding, 1, uses.size())) return fail(error, QStringLiteral("对象绑定索引无效"));
        if (uses[binding]) add(QStringLiteral("normalBinding"), type, id, QStringLiteral("对象关键形使用此参数"));
        return true;
    };
    const struct { const char* group; const char* type; int count; } objects[]{
        {"art_meshes", "ArtMesh", 4}, {"parts", "Part", 0}, {"glue", "Glue", 20}, {"deformers", "Deformer", 1}
    };
    QMap<QPair<int, int>, QVector<int>> deformerOwners;
    for (const auto& object : objects) {
        const QString group = QString::fromLatin1(object.group);
        for (int index = 0; index < layout.counts[object.count]; ++index) {
            const QString id = layout.id(group + QStringLiteral(".ids"), index);
            const int type = object.count == 1 ? layout.value(QStringLiteral("deformers.types"), index) : -1;
            const QString displayType = type == 0 ? QStringLiteral("WarpDeformer") : type == 1 ? QStringLiteral("RotationDeformer")
                : QString::fromLatin1(object.type);
            if (!checkBinding(layout.value(group + QStringLiteral(".keyform_binding_sources_indices"), index), displayType, id)) return false;
            if (object.count == 1 && (type == 0 || type == 1)) {
                const int specific = layout.value(QStringLiteral("deformers.specific_sources_indices"), index);
                if (!range(specific, 1, layout.counts[type == 0 ? 2 : 3])) return fail(error, QStringLiteral("专用变形器索引无效"));
                deformerOwners[qMakePair(type, specific)].append(index);
                const QString specificGroup = type == 0 ? QStringLiteral("warp_deformers") : QStringLiteral("rotation_deformers");
                const int binding = layout.value(specificGroup + QStringLiteral(".keyform_binding_sources_indices"), specific);
                if (binding != layout.value(QStringLiteral("deformers.keyform_binding_sources_indices"), index) &&
                    !checkBinding(binding, displayType, id)) return false;
            }
        }
    }
    for (int type = 0; type < 2; ++type) {
        const QString group = type == 0 ? QStringLiteral("warp_deformers") : QStringLiteral("rotation_deformers");
        for (int index = 0; index < layout.counts[type == 0 ? 2 : 3]; ++index)
            if (!deformerOwners.contains(qMakePair(type, index)) &&
                !checkBinding(layout.value(group + QStringLiteral(".keyform_binding_sources_indices"), index),
                              type == 0 ? QStringLiteral("WarpSource") : QStringLiteral("RotationSource"), QString::number(index))) return false;
    }
    if (parameterCount && references.isEmpty())
        add(QStringLiteral("bindingRecords"), QStringLiteral("Parameter"), parameterId, QStringLiteral("参数仍拥有源绑定记录"));
    const int blendBegin = layout.value(QStringLiteral("parameters.blend_shape_parameter_binding_sources_begin_indices"), parameter);
    const int blendCount = layout.value(QStringLiteral("parameters.blend_shape_parameter_binding_sources_counts"), parameter);
    if (!range(blendBegin, blendCount, layout.counts[25])) return fail(error, QStringLiteral("融合参数绑定范围无效"));
    QVector<bool> constraints(layout.counts[30]);
    bool hasConstraint = false;
    for (int index = 0; index < constraints.size(); ++index) {
        const int target = layout.value(QStringLiteral("blend_shape_constraints.parameter_indices"), index);
        if (!range(target, 1, layout.counts[5])) return fail(error, QStringLiteral("融合限制参数索引越界"));
        constraints[index] = target == parameter;
        hasConstraint = hasConstraint || constraints[index];
    }
    QVector<int> constraintPrefix(layout.counts[29] + 1);
    for (int index = 0; index < layout.counts[29]; ++index) {
        const int target = layout.value(QStringLiteral("blend_shape_constraint_indices.blend_shape_constraint_sources_indices"), index);
        if (!range(target, 1, constraints.size())) return fail(error, QStringLiteral("融合限制引用索引越界"));
        constraintPrefix[index + 1] = constraintPrefix[index] + constraints[target];
    }
    QVector<int> drivers(layout.counts[26] + 1), limiters(layout.counts[26] + 1);
    for (int index = 0; index < layout.counts[26]; ++index) {
        const int binding = layout.value(QStringLiteral("blend_shape_keyform_bindings.parameter_binding_sources_indices"), index);
        if (!range(binding, 1, layout.counts[25])) return fail(error, QStringLiteral("融合关键形参数引用越界"));
        drivers[index + 1] = drivers[index] + (binding >= blendBegin && binding < qint64(blendBegin) + blendCount);
        const int first = layout.value(QStringLiteral("blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices"), index);
        const int count = layout.value(QStringLiteral("blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts"), index);
        if (!range(first, count, layout.counts[29])) return fail(error, QStringLiteral("融合关键形限制范围无效"));
        limiters[index + 1] = limiters[index] + (count && constraintPrefix[first + count] != constraintPrefix[first]);
    }
    const struct { const char* group; const char* type; int count; int targetCount; int deformerType; } shapes[]{
        {"blend_shapes_art_meshes", "ArtMesh", 28, 4, -1}, {"blend_shapes_warp_deformers", "WarpDeformer", 27, 2, 0},
        {"blend_shapes_parts", "Part", 32, 0, -1}, {"blend_shapes_rotation_deformers", "RotationDeformer", 33, 3, 1}
    };
    bool locatedDriver = false, locatedLimiter = false;
    for (const auto& shape : shapes) {
        if (shape.count >= layout.counts.size()) continue;
        const QString group = QString::fromLatin1(shape.group), type = QString::fromLatin1(shape.type);
        for (int index = 0; index < layout.counts[shape.count]; ++index) {
            const int target = layout.value(group + QStringLiteral(".target_indices"), index);
            const int first = layout.value(group + QStringLiteral(".blend_shape_keyform_binding_sources_begin_indices"), index);
            const int count = layout.value(group + QStringLiteral(".blend_shape_keyform_binding_sources_counts"), index);
            if (!range(target, 1, layout.counts[shape.targetCount]) || !range(first, count, layout.counts[26]))
                return fail(error, QStringLiteral("融合目标或绑定范围无效"));
            const bool driver = count && drivers[first + count] != drivers[first];
            const bool limiter = count && limiters[first + count] != limiters[first];
            if (!driver && !limiter) continue;
            QStringList targets;
            if (shape.deformerType >= 0) {
                for (int owner : deformerOwners.value(qMakePair(shape.deformerType, target)))
                    targets.append(layout.id(QStringLiteral("deformers.ids"), owner));
                if (targets.isEmpty()) targets.append(QStringLiteral("源 %1").arg(target));
            } else targets.append(layout.id(shape.targetCount == 4 ? QStringLiteral("art_meshes.ids") : QStringLiteral("parts.ids"), target));
            for (const auto& id : targets) {
                if (driver) add(QStringLiteral("blendShapeDriver"), type, id, QStringLiteral("对象融合关键形使用此参数"));
                if (limiter) add(QStringLiteral("blendShapeLimiter"), type, id, QStringLiteral("对象融合权重受此参数限制"));
            }
            locatedDriver = locatedDriver || driver;
            locatedLimiter = locatedLimiter || limiter;
        }
    }
    if (blendCount && !locatedDriver) add(QStringLiteral("blendShapeRecords"), QStringLiteral("Parameter"), parameterId, QStringLiteral("参数仍拥有融合绑定记录"));
    if (hasConstraint && !locatedLimiter) add(QStringLiteral("blendShapeLimiter"), QStringLiteral("Parameter"), parameterId, QStringLiteral("融合限制记录引用此参数"));
    return true;
}
}

bool CMocParameterStructureBuilder::AnalyzeDelete(const QByteArray& base, const QString& parameterId,
                                                 QVector<Reference>& references, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout) return false;
        if (!understoodStructure(*layout, error)) return false;
        QHash<QString, int> ids;
        if (!catalog(*layout, ids, error)) return false;
        if (!ids.contains(parameterId)) return fail(error, QStringLiteral("参数不存在"));
        QVector<Reference> candidate;
        if (!analyze(*layout, ids.value(parameterId), candidate, error)) return false;
        references = std::move(candidate);
        return true;
    });
}

bool CMocParameterStructureBuilder::DeleteUnreferenced(const QByteArray& base, const QString& parameterId,
                                                     Result& result, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout) return false;
        if (!understoodStructure(*layout, error)) return false;
        QHash<QString, int> ids;
        if (!catalog(*layout, ids, error)) return false;
        if (!ids.contains(parameterId)) return fail(error, QStringLiteral("参数不存在"));
        const int index = ids.value(parameterId);
        if (layout->value(QStringLiteral("parameters.parameter_types"), index) != 0)
            return fail(error, QStringLiteral("当前只支持删除无绑定、无引用的普通参数"));
        QVector<Reference> references;
        if (!analyze(*layout, index, references, error)) return false;
        if (!references.isEmpty()) return fail(error, QStringLiteral("参数仍有模型绑定或融合引用，不能直接删除"));
        Result candidate;
        candidate.oldParameterToNew = identity(layout->counts[5]);
        candidate.oldParameterToNew[index] = -1;
        for (int at = index + 1; at < candidate.oldParameterToNew.size(); ++at) --candidate.oldParameterToNew[at];
        for (const auto& span : layout->columns) {
            if (span.countIndex != 5 || (span.optional && !span.offset)) continue;
            auto data = layout->data(span.slot);
            data.remove(qint64(index) * span.width, span.width);
            layout->changes.insert(span.slot, std::move(data));
        }
        --layout->counts[5];
        for (int row = 0; row < layout->counts[30]; ++row) {
            const int target = layout->value(QStringLiteral("blend_shape_constraints.parameter_indices"), row);
            if (target > index) layout->set(QStringLiteral("blend_shape_constraints.parameter_indices"), row, integer(target - 1));
        }
        if (!writeLayout(*layout, candidate.bytes, error)) return false;
        result = std::move(candidate);
        return true;
    });
}

namespace {
struct DeleteAxis
{
    int parameter = -1;
    int stride = 1;
    int count = 0;
    int first = 0;
    int last = 0;
    double fraction = 0;
};

struct KeyformReduction
{
    Builder::DeleteTarget::Type type;
    QString group;
    QString keyforms;
    int source = -1;
    int first = 0;
    int count = 0;
    int newCount = 0;
    int vertices = 0;
    int colorBegin = -1;
    QVector<DeleteAxis> axes;
    bool interpolated = false;
};

struct FrozenConstraint { int row; float weight; };
struct DeletePlan
{
    int parameter = -1;
    int constantHost = -1;
    QVector<KeyformReduction> reductions;
    QVector<FrozenConstraint> constraints;
    Builder::DeleteImpact impact;
};

QVector<QPair<int, double>> deleteContributors(const KeyformReduction& reduction, int local, int parameter)
{
    int base = reduction.first;
    for (const auto& axis : reduction.axes) {
        if (axis.parameter == parameter) continue;
        base += (local % axis.count) * axis.stride;
        local /= axis.count;
    }
    QVector<QPair<int, double>> result{{base, 1}};
    for (const auto& axis : reduction.axes) {
        if (axis.parameter != parameter) continue;
        if (axis.first == axis.last) {
            for (auto& term : result) term.first += axis.first * axis.stride;
        } else {
            const int size = result.size();
            result.reserve(size * 2);
            for (int index = 0; index < size; ++index) {
                const auto term = result[index];
                result[index] = {term.first + axis.first * axis.stride, term.second * (1 - axis.fraction)};
                result.append({term.first + axis.last * axis.stride, term.second * axis.fraction});
            }
        }
    }
    return result;
}

void keyInterval(const QVector<float>& values, float value, DeleteAxis& axis)
{
    axis.count = values.size();
    axis.fraction = 0;
    const auto high = std::lower_bound(values.cbegin(), values.cend(), value);
    if (high == values.cend()) axis.first = axis.last = values.size() - 1;
    else if (high == values.cbegin() || *high == value) axis.first = axis.last = int(high - values.cbegin());
    else {
        axis.last = int(high - values.cbegin());
        axis.first = axis.last - 1;
        axis.fraction = (double(value) - values[axis.first]) / (double(values[axis.last]) - values[axis.first]);
    }
}

bool deleteInterval(const Layout& layout, const QString& keys, int first, int count,
                    float value, DeleteAxis& axis, QString* error)
{
    if (count <= 0 || !range(first, count, layout.counts[layout.column(keys).countIndex]))
        return fail(error, QStringLiteral("冻结所需关键值区间无效"));
    float previous = 0;
    QVector<float> values;
    values.reserve(count);
    for (int index = 0; index < count; ++index) {
        const float key = layout.real(keys, first + index);
        if (!std::isfinite(key) || (index && key <= previous))
            return fail(error, QStringLiteral("冻结所需关键值不是有限严格递增序列"));
        values.append(key);
        previous = key;
    }
    keyInterval(values, value, axis);
    return true;
}

bool deleteFloat(double value, QByteArray& encoded, QString* error)
{
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        return fail(error, QStringLiteral("冻结后的关键形数值超出有限浮点范围"));
    encoded = floating(static_cast<float>(value));
    return true;
}

bool makeDeletePlan(const Layout& layout, const Builder::DeleteRequest& request, DeletePlan& plan, QString* error)
{
    QHash<QString, int> ids;
    if (!catalog(layout, ids, error)) return false;
    if (!ids.contains(request.id)) return fail(error, QStringLiteral("参数不存在"));
    plan.parameter = ids.value(request.id);
    const float minimum = layout.real(QStringLiteral("parameters.min_values"), plan.parameter);
    const float maximum = layout.real(QStringLiteral("parameters.max_values"), plan.parameter);
    if (!std::isfinite(request.freezeValue) || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum ||
        request.freezeValue < minimum || request.freezeValue > maximum)
        return fail(error, QStringLiteral("冻结值必须有限且位于参数范围内"));
    if (!analyze(layout, plan.parameter, plan.impact.references, error)) return false;
    if (layout.value(QStringLiteral("parameters.parameter_types"), plan.parameter) != 0) {
        plan.impact.blockingReasons.append(QStringLiteral("当前只支持删除普通参数"));
        return true;
    }
    if (layout.value(QStringLiteral("parameters.blend_shape_parameter_binding_sources_counts"), plan.parameter)) {
        plan.impact.blockingReasons.append(QStringLiteral("普通参数仍拥有融合驱动记录，不能直接收缩"));
        return true;
    }
    QVector<int> parameterOwners;
    if (!bindingOwners(layout, parameterOwners, error)) return false;
    QHash<QPair<int, int>, QVector<int>> deformerOwners;
    for (int index = 0; index < layout.counts[1]; ++index)
        deformerOwners[qMakePair(layout.value(QStringLiteral("deformers.types"), index),
                                 layout.value(QStringLiteral("deformers.specific_sources_indices"), index))].append(index);
    const struct { Builder::DeleteTarget::Type type; const char* group; const char* keyforms; const char* field; } groups[]{
        {Builder::DeleteTarget::ArtMesh, "art_meshes", "art_mesh_keyforms", "opacities"},
        {Builder::DeleteTarget::Warp, "warp_deformers", "warp_deformer_keyforms", "opacities"},
        {Builder::DeleteTarget::Rotation, "rotation_deformers", "rotation_deformer_keyforms", "opacities"},
        {Builder::DeleteTarget::Part, "parts", "part_keyforms", "draw_orders"},
        {Builder::DeleteTarget::Glue, "glue", "glue_keyforms", "intensities"}
    };
    QMap<int, qint64> addedRows;
    qint64 addedPositions = 0, addedColors = 0;
    for (const auto& group : groups) {
        const QString name = QString::fromLatin1(group.group), keyforms = QString::fromLatin1(group.keyforms);
        const int rows = layout.counts[layout.column(name + QStringLiteral(".keyform_sources_counts")).countIndex];
        const int keyformCountIndex = layout.column(keyforms + '.' + QString::fromLatin1(group.field)).countIndex;
        for (int row = 0; row < rows; ++row) {
            const int binding = layout.value(name + QStringLiteral(".keyform_binding_sources_indices"), row);
            if (binding == -1) continue;
            if (!range(binding, 1, layout.counts[12])) return fail(error, QStringLiteral("对象关键形绑定越界"));
            const int begin = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), binding);
            const int count = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), binding);
            if (!range(begin, count, layout.counts[11])) return fail(error, QStringLiteral("对象绑定轴区间越界"));
            QVector<int> references;
            bool affected = false;
            for (int axis = 0; axis < count; ++axis) {
                const int reference = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), begin + axis);
                if (!range(reference, 1, parameterOwners.size()) || parameterOwners[reference] < 0)
                    return fail(error, QStringLiteral("绑定轴缺少唯一参数"));
                references.append(reference);
                affected = affected || parameterOwners[reference] == plan.parameter;
            }
            if (!affected) continue;
            KeyformReduction reduction{};
            reduction.type = group.type;
            reduction.group = name;
            reduction.keyforms = keyforms;
            reduction.source = row;
            reduction.first = layout.value(name + QStringLiteral(".keyform_sources_begin_indices"), row);
            reduction.count = layout.value(name + QStringLiteral(".keyform_sources_counts"), row);
            if (reduction.count <= 0 || !range(reduction.first, reduction.count, layout.counts[keyformCountIndex]))
                return fail(error, QStringLiteral("冻结对象关键形区间无效"));
            int product = 1, remaining = 1;
            QStringList uncoveredAxes;
            for (int reference : references) {
                DeleteAxis axis;
                axis.parameter = parameterOwners[reference];
                axis.stride = product;
                const int keyBegin = layout.value(QStringLiteral("parameter_bindings.keys_sources_begin_indices"), reference);
                const int keyCount = layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), reference);
                if (!deleteInterval(layout, QStringLiteral("keys.values"), keyBegin, keyCount, request.freezeValue, axis, error)) return false;
                if (product > reduction.count / keyCount) return fail(error, QStringLiteral("冻结对象绑定轴乘积超出关键形数量"));
                product *= keyCount;
                if (axis.parameter != plan.parameter) remaining *= keyCount;
                else {
                    reduction.interpolated = reduction.interpolated || axis.first != axis.last;
                    const float low = layout.real(QStringLiteral("keys.values"), keyBegin);
                    const float high = layout.real(QStringLiteral("keys.values"), keyBegin + keyCount - 1);
                    if (request.freezeValue < low || request.freezeValue > high)
                        uncoveredAxes.append(QStringLiteral("参数 %1 的第 %2 轴关键值区间 [%3, %4] 不包含冻结值 %5")
                            .arg(request.id).arg(reduction.axes.size() + 1).arg(double(low), 0, 'g', 9)
                            .arg(double(high), 0, 'g', 9).arg(double(request.freezeValue), 0, 'g', 9));
                }
                if (layout.value(QStringLiteral("parameters.parameter_types"), axis.parameter) != 0)
                    plan.impact.blockingReasons.append(QStringLiteral("对象包含非普通参数轴：%1[%2]").arg(name).arg(row));
                reduction.axes.append(axis);
            }
            if (product != reduction.count) return fail(error, QStringLiteral("冻结对象绑定轴乘积与关键形数量不符"));
            reduction.newCount = remaining;
            if (group.type == Builder::DeleteTarget::ArtMesh || group.type == Builder::DeleteTarget::Warp) {
                reduction.vertices = layout.value(name + QStringLiteral(".vertex_counts"), row);
                if (reduction.vertices < 0 || reduction.vertices > MaximumCount / 2)
                    return fail(error, QStringLiteral("冻结对象顶点数量无效"));
                if (reduction.interpolated) addedPositions += qint64(remaining) * reduction.vertices * 2;
            }
            const QString color = name + QStringLiteral(".keyform_color_sources_begin_indices");
            if (layout.present(color)) {
                reduction.colorBegin = layout.value(color, row);
                addedColors += remaining;
                if (layout.counts[23] != layout.counts[24])
                    plan.impact.blockingReasons.append(QStringLiteral("两种颜色池数量不同，尚不能安全收缩连续颜色源"));
            } else if (layout.present(keyforms + QStringLiteral(".multiply_color_sources_indices")) ||
                       layout.present(keyforms + QStringLiteral(".screen_color_sources_indices"))) {
                plan.impact.blockingReasons.append(QStringLiteral("颜色索引存在但对象的连续颜色源缺失，不能安全收缩"));
            }
            QStringList targetIds;
            if (group.type == Builder::DeleteTarget::Warp || group.type == Builder::DeleteTarget::Rotation) {
                const int type = group.type == Builder::DeleteTarget::Warp ? 0 : 1;
                for (int owner : deformerOwners.value(qMakePair(type, row))) {
                    if (layout.value(QStringLiteral("deformers.keyform_binding_sources_indices"), owner) != binding)
                        return fail(error, QStringLiteral("通用变形器与专用源绑定不一致"));
                    targetIds.append(layout.id(QStringLiteral("deformers.ids"), owner));
                }
            } else targetIds.append(layout.id(name + QStringLiteral(".ids"), row));
            if (targetIds.isEmpty()) targetIds.append(QString());
            for (const auto& id : targetIds) for (const auto& detail : uncoveredAxes)
                plan.impact.blockingReasons.append(QStringLiteral("%1：%2；区间外结果依赖历史姿态，不能静态冻结")
                    .arg(id.isEmpty() ? QStringLiteral("%1[%2]").arg(name).arg(row) : id, detail));
            for (const auto& id : targetIds)
                plan.impact.targets.append({group.type, id, row, reduction.count, remaining, reduction.interpolated});
            if (group.type == Builder::DeleteTarget::Rotation) {
                bool unsupported = false;
                for (int local = 0; local < reduction.count && !unsupported; ++local) {
                    for (const auto& field : {QStringLiteral("is_reflect_x"), QStringLiteral("is_reflect_y")}) {
                        const int value = layout.value(keyforms + '.' + field, reduction.first + local);
                        unsupported = unsupported || (value != 0 && value != 1);
                    }
                }
                if (unsupported) plan.impact.blockingReasons.append(QStringLiteral("旋转关键形包含当前 Core 不支持的反射编码：%1").arg(targetIds.join(QStringLiteral("、"))));
            }
            addedRows[keyformCountIndex] += remaining;
            if (addedRows[keyformCountIndex] > MaximumCount - layout.counts[keyformCountIndex] ||
                addedPositions > MaximumCount - layout.counts[10] || addedColors > MaximumCount - layout.counts[23] ||
                addedColors > MaximumCount - layout.counts[24]) return fail(error, QStringLiteral("冻结后的关键形或坐标总量超出范围"));
            plan.reductions.append(std::move(reduction));
        }
    }
    for (int index = 0; index < layout.counts[5]; ++index) {
        if (index == plan.parameter) continue;
        if (plan.constantHost < 0 || layout.value(QStringLiteral("parameters.parameter_types"), index) == 0) plan.constantHost = index;
        if (layout.value(QStringLiteral("parameters.parameter_types"), index) == 0) break;
    }
    for (int row = 0; row < layout.counts[30]; ++row) {
        if (layout.value(QStringLiteral("blend_shape_constraints.parameter_indices"), row) != plan.parameter) continue;
        const int first = layout.value(QStringLiteral("blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices"), row);
        const int count = layout.value(QStringLiteral("blend_shape_constraints.blend_shape_constraint_value_sources_counts"), row);
        DeleteAxis interval;
        if (!deleteInterval(layout, QStringLiteral("blend_shape_constraint_values.keys"), first, count, request.freezeValue, interval, error)) return false;
        const double a = layout.real(QStringLiteral("blend_shape_constraint_values.weights"), first + interval.first);
        const double b = layout.real(QStringLiteral("blend_shape_constraint_values.weights"), first + interval.last);
        const double value = a * (1 - interval.fraction) + b * interval.fraction;
        QByteArray encoded;
        if (!deleteFloat(value, encoded, error)) return false;
        if (value < 0 || value > 1) return fail(error, QStringLiteral("融合限制权重不在 0 到 1 之间"));
        plan.constraints.append({row, static_cast<float>(value)});
        if (plan.constantHost < 0) plan.impact.blockingReasons.append(QStringLiteral("最后一个参数仍被融合限制记录引用，缺少可承载常量曲线的参数"));
        else plan.impact.references.append({QStringLiteral("constantLimiter"), QStringLiteral("Parameter"),
            layout.id(QStringLiteral("parameters.ids"), plan.constantHost),
            QStringLiteral("限制记录 %1 将保持常量权重 %2，不随承载参数变化").arg(row).arg(value, 0, 'g', 9)});
    }
    if (plan.constraints.size() > (MaximumCount - layout.counts[31]) / 2)
        return fail(error, QStringLiteral("冻结后的融合限制曲线总量超出范围"));
    plan.impact.blockingReasons.removeDuplicates();
    return true;
}

struct KeyformSourceValidation
{
    QSet<QPair<int, int>> positions;
};

template<class Contributors>
bool writeReducedForms(Layout& layout, const KeyformReduction& reduction, Contributors contributors,
                       KeyformSourceValidation& validated, QString* error)
{
    QVector<int> columns;
    int countIndex = -1;
    for (const auto& span : layout.columns) if (QString::fromLatin1(span.name).section('.', 0, 0) == reduction.keyforms) {
        countIndex = span.countIndex;
        if (span.offset || layout.changes.contains(span.slot) || !span.optional) columns.append(span.slot);
    }
    if (countIndex < 0) return fail(error, QStringLiteral("缺少关键形列定义"));
    const int first = layout.counts[countIndex], colorBegin = layout.counts[23];
    for (int local = 0; local < reduction.newCount; ++local) {
        const auto terms = contributors(local);
        long double weight = 0;
        for (const auto& term : terms) {
            if (!range(term.first - qint64(reduction.first), 1, reduction.count) ||
                !std::isfinite(term.second) || term.second < 0)
                return fail(error, QStringLiteral("关键形贡献的来源或权重无效"));
            weight += term.second;
        }
        if (terms.isEmpty() || std::abs(weight - 1.0L) > 1e-12L)
            return fail(error, QStringLiteral("关键形贡献权重必须归一化"));
        QHash<QString, int> colors;
        if (reduction.colorBegin >= 0) {
            for (const auto& channel : {QStringLiteral("multiply"), QStringLiteral("screen")}) {
                const QString pool = QStringLiteral("keyform_") + channel + QStringLiteral("_colors");
                const QString indices = reduction.keyforms + '.' + channel + QStringLiteral("_color_sources_indices");
                const int index = channel == QLatin1String("multiply") ? 23 : 24;
                colors.insert(channel, layout.counts[index]++);
                for (const auto& component : {QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")}) {
                    const QString field = pool + '.' + component;
                    double value = 0;
                    double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                    int source = 0;
                    for (const auto& term : terms) {
                        source = layout.present(indices) ? layout.value(indices, term.first)
                            : reduction.colorBegin + term.first - reduction.first;
                        if (!range(source, 1, layout.column(field).count)) return fail(error, QStringLiteral("冻结颜色索引越界"));
                        const double cell = layout.real(field, source);
                        if (!std::isfinite(cell)) return fail(error, QStringLiteral("源关键形颜色不是有限值"));
                        value += cell * term.second;
                        minimum = std::min(minimum, cell); maximum = std::max(maximum, cell);
                    }
                    QByteArray encoded;
                    if (terms.size() == 1) encoded = layout.data(field).mid(source * 4, 4);
                    else if (!deleteFloat(std::clamp(value, minimum, maximum), encoded, error)) return false;
                    layout.append(field, encoded);
                }
            }
        }
        for (int slot : columns) {
            const auto& column = layout.columns.constFind(slot).value();
            const QString name = QString::fromLatin1(column.name), field = name.section('.', 1);
            QByteArray encoded;
            if (field == QLatin1String("multiply_color_sources_indices") && colors.contains(QStringLiteral("multiply"))) encoded = integer(colors.value(QStringLiteral("multiply")));
            else if (field == QLatin1String("screen_color_sources_indices") && colors.contains(QStringLiteral("screen"))) encoded = integer(colors.value(QStringLiteral("screen")));
            else if (field == QLatin1String("keyform_position_sources_begin_indices")) {
                const int position = layout.value(name, terms.first().first);
                if (!range(position, reduction.vertices * 2, layout.column(QStringLiteral("keyform_positions.xys")).count))
                    return fail(error, QStringLiteral("冻结坐标区间越界"));
                if (terms.size() == 1) {
                    const auto slice = qMakePair(position, reduction.vertices * 2);
                    if (!validated.positions.contains(slice)) {
                        for (int component = 0; component < slice.second; ++component)
                            if (!std::isfinite(layout.real(QStringLiteral("keyform_positions.xys"), position + component)))
                                return fail(error, QStringLiteral("源关键形坐标不是有限值"));
                        validated.positions.insert(slice);
                    }
                    encoded = integer(position);
                }
                else {
                    const int destination = layout.counts[10];
                    for (int component = 0; component < reduction.vertices * 2; ++component) {
                        double value = 0;
                        double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                        for (const auto& term : terms) {
                            const int source = layout.value(name, term.first);
                            if (!range(source, reduction.vertices * 2, layout.column(QStringLiteral("keyform_positions.xys")).count))
                                return fail(error, QStringLiteral("冻结坐标区间越界"));
                            const double cell = layout.real(QStringLiteral("keyform_positions.xys"), source + component);
                            if (!std::isfinite(cell)) return fail(error, QStringLiteral("源关键形坐标不是有限值"));
                            value += cell * term.second;
                            minimum = std::min(minimum, cell); maximum = std::max(maximum, cell);
                        }
                        QByteArray coordinate;
                        if (!deleteFloat(std::clamp(value, minimum, maximum), coordinate, error)) return false;
                        layout.append(QStringLiteral("keyform_positions.xys"), coordinate);
                        ++layout.counts[10];
                    }
                    encoded = integer(destination);
                }
            } else if (field.startsWith(QStringLiteral("is_reflect_"))) {
                // Core selects reflection flags from the lower corner instead of interpolating them.
                encoded = layout.data(slot).mid(terms.first().first * column.width, column.width);
            } else if (terms.size() == 1) {
                if (!std::isfinite(layout.real(name, terms.first().first)))
                    return fail(error, QStringLiteral("源关键形数值不是有限值"));
                encoded = layout.data(slot).mid(terms.first().first * column.width, column.width);
            }
            else {
                double value = 0;
                double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                for (const auto& term : terms) {
                    const double cell = layout.real(name, term.first);
                    if (!std::isfinite(cell)) return fail(error, QStringLiteral("源关键形数值不是有限值"));
                    value += cell * term.second;
                    minimum = std::min(minimum, cell); maximum = std::max(maximum, cell);
                }
                if (!deleteFloat(std::clamp(value, minimum, maximum), encoded, error)) return false;
            }
            layout.append(name, encoded);
        }
        ++layout.counts[countIndex];
    }
    layout.set(reduction.group + QStringLiteral(".keyform_sources_begin_indices"), reduction.source, integer(first));
    layout.set(reduction.group + QStringLiteral(".keyform_sources_counts"), reduction.source, integer(reduction.newCount));
    if (reduction.colorBegin >= 0) layout.set(reduction.group + QStringLiteral(".keyform_color_sources_begin_indices"), reduction.source, integer(colorBegin));
    return true;
}

bool reduceDeleteRows(Layout& layout, const DeletePlan& plan, QString* error)
{
    KeyformSourceValidation validated;
    for (const auto& reduction : plan.reductions)
        if (!writeReducedForms(layout, reduction, [&](int local) {
            return deleteContributors(reduction, local, plan.parameter);
        }, validated, error)) return false;
    return true;
}

bool bindingDisplayKeys(const Layout& layout, const OwnerInfo& owner, int axis, int parameter,
                        const QVector<int>& parameterOwners, const QVector<float>& oldKeys,
                        const QVector<Builder::BindingKey>& newKeys, QByteArray& result, QString* error)
{
    if (!layout.present(QStringLiteral("parameters.keys_sources_begin_indices")) ||
        !layout.present(QStringLiteral("parameters.keys_sources_counts"))) return true;
    QHash<int, int> activeBindings;
    const struct { const char* name; int count; } groups[] {
        {"art_meshes", 4}, {"deformers", 1}, {"parts", 0}, {"glue", 20}
    };
    for (const auto& group : groups) for (int row = 0; row < layout.counts[group.count]; ++row) {
        const int binding = layout.value(QString::fromLatin1(group.name) + QStringLiteral(".keyform_binding_sources_indices"), row);
        if (binding == -1) continue;
        if (!range(binding, 1, layout.counts[12])) return fail(error, QStringLiteral("活跃对象的绑定索引无效"));
        ++activeBindings[binding];
    }
    const QString objects = owner.owner.type == Builder::Owner::ArtMesh ? QStringLiteral("art_meshes") : QStringLiteral("deformers");
    const int edited = layout.value(objects + QStringLiteral(".keyform_binding_sources_indices"), owner.index);
    QMap<QPair<int, int>, bool> activeRanges;
    for (auto binding = activeBindings.cbegin(); binding != activeBindings.cend(); ++binding) {
        const int first = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), binding.key());
        const int count = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), binding.key());
        if (!range(first, count, layout.counts[11])) return fail(error, QStringLiteral("活跃对象的绑定轴区间无效"));
        const auto slice = qMakePair(first, count);
        activeRanges[slice] = activeRanges.value(slice, false) || binding.key() != edited || binding.value() > 1;
    }
    QSet<int> includedBindings;
    qint64 references = 0;
    for (auto binding = activeRanges.cbegin(); binding != activeRanges.cend(); ++binding) {
        const int first = binding.key().first, count = binding.key().second;
        if (count > MaximumCount - references) return fail(error, QStringLiteral("活跃对象的绑定轴扫描总量超出范围"));
        references += count;
        for (int index = 0; index < count; ++index) {
            if (!binding.value() && index == axis) continue;
            const int reference = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), first + index);
            if (!range(reference, 1, parameterOwners.size()) || parameterOwners[reference] < 0)
                return fail(error, QStringLiteral("活跃绑定轴缺少唯一参数"));
            if (parameterOwners[reference] == parameter) includedBindings.insert(reference);
        }
    }
    QSet<float> otherKeys;
    QSet<QPair<int, int>> scannedKeys;
    qint64 keyReferences = 0;
    for (int binding : includedBindings) {
        const int first = layout.value(QStringLiteral("parameter_bindings.keys_sources_begin_indices"), binding);
        const int count = layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), binding);
        if (count <= 0 || !range(first, count, layout.counts[14])) return fail(error, QStringLiteral("活跃参数关键值区间无效"));
        const auto slice = qMakePair(first, count);
        if (scannedKeys.contains(slice)) continue;
        scannedKeys.insert(slice);
        if (count > MaximumCount - keyReferences) return fail(error, QStringLiteral("活跃参数关键值扫描总量超出范围"));
        keyReferences += count;
        float previous = 0;
        for (int index = 0; index < count; ++index) {
            const float value = layout.real(QStringLiteral("keys.values"), first + index);
            if (!std::isfinite(value) || (index && value <= previous))
                return fail(error, QStringLiteral("活跃参数关键值不是有限严格递增序列"));
            previous = value; otherKeys.insert(value);
        }
    }
    const int first = layout.value(QStringLiteral("parameters.keys_sources_begin_indices"), parameter);
    const int count = layout.value(QStringLiteral("parameters.keys_sources_counts"), parameter);
    if (!range(first, count, layout.counts[14])) return fail(error, QStringLiteral("参数显示关键值区间无效"));
    const QSet<float> previousKeys(oldKeys.cbegin(), oldKeys.cend());
    QVector<float> values;
    for (int index = 0; index < count; ++index) {
        const float value = layout.real(QStringLiteral("keys.values"), first + index);
        if (!std::isfinite(value)) return fail(error, QStringLiteral("参数显示关键值包含非有限值"));
        if (!previousKeys.contains(value) || otherKeys.contains(value)) values.append(value);
    }
    for (float value : otherKeys) values.append(value);
    for (const auto& key : newKeys) values.append(key.value);
    std::stable_sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() > MaximumCount - layout.counts[14]) return fail(error, QStringLiteral("参数显示关键值总量超出范围"));
    for (float value : values) result.append(floating(value));
    return true;
}

bool editBindingKeys(Layout& layout, const Builder::EditBindingKeysRequest& request, Builder::Result& result, QString* error)
{
    QHash<QString, int> parameters;
    if (!catalog(layout, parameters, error)) return false;
    const int parameter = parameters.value(request.parameterId, -1);
    if (parameter < 0 || layout.value(QStringLiteral("parameters.parameter_types"), parameter) != 0)
        return fail(error, QStringLiteral("只能编辑现有普通参数的绑定轴"));
    QVector<int> parameterOwners;
    if (!bindingOwners(layout, parameterOwners, error)) return false;
    QHash<QPair<int, QString>, int> objects;
    QHash<QPair<int, int>, int> specificUsers;
    ownerCatalog(layout, objects, specificUsers);
    OwnerInfo owner;
    if (!readOwnerGrid(layout, request.owner, parameterOwners, objects, owner, error)) return false;
    if (request.axisIndex < 0 || request.axisIndex >= owner.bindings.size() ||
        parameterOwners[owner.bindings[request.axisIndex]] != parameter)
        return fail(error, QStringLiteral("绑定轴位置与参数 ID 不匹配"));
    const int reference = owner.bindings[request.axisIndex];
    const int keyBegin = layout.value(QStringLiteral("parameter_bindings.keys_sources_begin_indices"), reference);
    const int oldKeyCount = layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), reference);
    if (request.expectedKeys.size() != oldKeyCount) return fail(error, QStringLiteral("绑定轴关键值已改变，请重新读取"));
    QVector<float> oldKeys;
    for (int index = 0; index < oldKeyCount; ++index) {
        const float value = layout.real(QStringLiteral("keys.values"), keyBegin + index);
        if (std::memcmp(&value, &request.expectedKeys[index], sizeof(float)))
            return fail(error, QStringLiteral("绑定轴关键值已改变，请重新读取"));
        oldKeys.append(value);
    }
    if (request.keys.isEmpty() || request.keys.size() > MaximumCount ||
        request.keys.size() > MaximumCount / (owner.count / oldKeyCount))
        return fail(error, QStringLiteral("绑定轴必须保留至少一个关键值且关键形数量不能超出范围"));
    const float minimum = layout.real(QStringLiteral("parameters.min_values"), parameter);
    const float maximum = layout.real(QStringLiteral("parameters.max_values"), parameter);
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
        return fail(error, QStringLiteral("参数范围无效"));
    QVector<QVector<QPair<int, double>>> slices;
    QSet<int> retained;
    QByteArray encodedKeys;
    bool noOp = request.keys.size() == oldKeyCount;
    int interpolatedKeys = 0;
    for (int index = 0; index < request.keys.size(); ++index) {
        const auto& key = request.keys[index];
        if (!std::isfinite(key.value) || (index && key.value <= request.keys[index - 1].value) ||
            key.oldKeyIndex < -1 || key.oldKeyIndex >= oldKeyCount ||
            (key.source != Builder::BindingKey::InterpolateOldAxis && key.source != Builder::BindingKey::CopyOldSlice))
            return fail(error, QStringLiteral("新关键值、来源索引或插值方式无效"));
        const bool unchanged = key.oldKeyIndex >= 0 &&
            std::memcmp(&key.value, &oldKeys[key.oldKeyIndex], sizeof(float)) == 0;
        if (!unchanged && (key.value < minimum || key.value > maximum))
            return fail(error, QStringLiteral("移动或新增的关键值必须位于参数范围内"));
        QVector<QPair<int, double>> terms;
        if (key.oldKeyIndex >= 0) {
            if (retained.contains(key.oldKeyIndex) || key.source != Builder::BindingKey::InterpolateOldAxis || key.copyOldKeyIndex != -1)
                return fail(error, QStringLiteral("旧关键值重复保留或附带了不一致的复制来源"));
            retained.insert(key.oldKeyIndex);
            terms.append({key.oldKeyIndex, 1});
        } else if (key.source == Builder::BindingKey::CopyOldSlice) {
            if (key.copyOldKeyIndex < 0 || key.copyOldKeyIndex >= oldKeyCount)
                return fail(error, QStringLiteral("复制的旧关键值索引越界"));
            terms.append({key.copyOldKeyIndex, 1});
        } else {
            if (key.copyOldKeyIndex != -1) return fail(error, QStringLiteral("插值关键值不能附带复制来源"));
            if (key.value < oldKeys.first() || key.value > oldKeys.last())
                return fail(error, QStringLiteral("新增值不在旧轴关键值区间 [%1, %2] 内，区间外不能静态插值；请明确选择复制旧关键形（CopyOldSlice）")
                    .arg(double(oldKeys.first()), 0, 'g', 9).arg(double(oldKeys.last()), 0, 'g', 9));
            DeleteAxis interval;
            keyInterval(oldKeys, key.value, interval);
            if (interval.first == interval.last) terms.append({interval.first, 1});
            else {
                terms.append({interval.first, 1 - interval.fraction}); terms.append({interval.last, interval.fraction});
                ++interpolatedKeys;
            }
        }
        noOp = noOp && terms.size() == 1 && terms.first().first == index &&
            std::memcmp(&key.value, &oldKeys[index], sizeof(float)) == 0;
        slices.append(std::move(terms)); encodedKeys.append(floating(key.value));
    }
    int stride = 1;
    for (int index = 0; index < request.axisIndex; ++index)
        stride *= layout.value(QStringLiteral("parameter_bindings.keys_sources_counts"), owner.bindings[index]);
    const int newCount = owner.count / oldKeyCount * request.keys.size();
    const auto makeRemap = [&] {
        Builder::KeyformRemap remap{owner.owner, owner.count, newCount, int(request.keys.size()), {}, {}};
        remap.newLocalToOldLocal.reserve(newCount); remap.newLocalSources.reserve(newCount);
        for (int local = 0; local < newCount; ++local) {
            const int inner = local % stride, digit = (local / stride) % request.keys.size();
            const int outer = local / (stride * request.keys.size());
            QVector<Builder::KeyformRemap::Contribution> terms;
            for (const auto& slice : slices[digit]) terms.append({inner + slice.first * stride + outer * stride * oldKeyCount, slice.second});
            remap.newLocalToOldLocal.append(terms.size() == 1 ? terms.first().oldLocal : -1);
            remap.newLocalSources.append(std::move(terms));
        }
        return remap;
    };
    result.oldParameterToNew = identity(layout.counts[5]);
    if (noOp) { result.bytes = layout.bytes; result.keyforms.append(makeRemap()); return true; }
    if (!understoodStructure(layout, error)) return false;
    if (owner.owner.type != Builder::Owner::ArtMesh &&
        specificUsers.value(qMakePair(owner.owner.type == Builder::Owner::Warp ? 0 : 1, owner.source)) > 1)
        return fail(error, QStringLiteral("共享专用变形器源无法保证无损拆分：%1").arg(owner.owner.id));
    KeyformReduction reduction{};
    reduction.type = Builder::DeleteTarget::Type(owner.owner.type);
    reduction.group = owner.group; reduction.keyforms = owner.keyforms; reduction.source = owner.source;
    reduction.first = owner.first; reduction.count = owner.count; reduction.newCount = newCount;
    reduction.interpolated = interpolatedKeys > 0;
    const int countIndex = layout.column(owner.keyforms + QStringLiteral(".opacities")).countIndex;
    if (newCount > MaximumCount - layout.counts[countIndex] || owner.bindings.size() > MaximumCount - layout.counts[11] ||
        layout.counts[12] == MaximumCount)
        return fail(error, QStringLiteral("编辑后的关键形或绑定引用总量超出范围"));
    if (owner.owner.type != Builder::Owner::Rotation) {
        reduction.vertices = layout.value(owner.group + QStringLiteral(".vertex_counts"), owner.source);
        const qint64 positions = qint64(interpolatedKeys) * (owner.count / oldKeyCount) * reduction.vertices * 2;
        if (reduction.vertices < 0 || reduction.vertices > MaximumCount / 2 || positions > MaximumCount - layout.counts[10])
            return fail(error, QStringLiteral("插值后的关键形坐标总量超出范围"));
    } else if (interpolatedKeys) {
        for (int local = 0; local < owner.count; ++local)
            for (const auto& field : {QStringLiteral("is_reflect_x"), QStringLiteral("is_reflect_y")}) {
                const int value = layout.value(owner.keyforms + '.' + field, owner.first + local);
                if (value != 0 && value != 1) return fail(error, QStringLiteral("旋转插值包含当前 Core 不支持的反射编码"));
            }
    }
    const QString color = owner.group + QStringLiteral(".keyform_color_sources_begin_indices");
    if (layout.present(color)) {
        reduction.colorBegin = layout.value(color, owner.source);
        if (reduction.colorBegin < 0 || layout.counts[23] != layout.counts[24] || newCount > MaximumCount - layout.counts[23])
            return fail(error, QStringLiteral("连续颜色源无效、两种颜色池数量不同或编辑后的颜色总量超出范围"));
    } else if (layout.present(owner.keyforms + QStringLiteral(".multiply_color_sources_indices")) ||
               layout.present(owner.keyforms + QStringLiteral(".screen_color_sources_indices")))
        return fail(error, QStringLiteral("颜色索引存在但对象的连续颜色源缺失，不能安全编辑关键值"));
    QByteArray display;
    if (!bindingDisplayKeys(layout, owner, request.axisIndex, parameter, parameterOwners, oldKeys, request.keys, display, error)) return false;
    auto remap = makeRemap();
    BindingInsertion inserted;
    if (!insertParameterBindings(layout, parameter, {encodedKeys}, inserted, error)) return false;
    if (!display.isEmpty()) {
        if (display.size() / 4 > MaximumCount - layout.counts[14]) return fail(error, QStringLiteral("参数显示关键值总量超出范围"));
        layout.set(QStringLiteral("parameters.keys_sources_begin_indices"), parameter, integer(layout.counts[14]));
        layout.set(QStringLiteral("parameters.keys_sources_counts"), parameter, integer(display.size() / 4));
        layout.append(QStringLiteral("keys.values"), display); layout.counts[14] += display.size() / 4;
    }
    const int axisBegin = layout.counts[11];
    for (int index = 0; index < owner.bindings.size(); ++index) {
        const int old = owner.bindings[index];
        const int reference = index == request.axisIndex ? inserted.references.first() : old + (old >= inserted.position ? inserted.count : 0);
        layout.append(QStringLiteral("parameter_binding_indices.binding_sources_indices"), integer(reference));
    }
    layout.counts[11] += owner.bindings.size();
    const int binding = layout.counts[12]++;
    layout.append(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), integer(axisBegin));
    layout.append(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), integer(owner.bindings.size()));
    if (owner.owner.type != Builder::Owner::ArtMesh)
        layout.set(QStringLiteral("deformers.keyform_binding_sources_indices"), owner.index, integer(binding));
    layout.set(owner.group + QStringLiteral(".keyform_binding_sources_indices"), owner.source, integer(binding));
    KeyformSourceValidation validated;
    if (!writeReducedForms(layout, reduction, [&](int local) {
        QVector<QPair<int, double>> terms;
        for (const auto& source : remap.newLocalSources[local]) terms.append({owner.first + source.oldLocal, source.weight});
        return terms;
    }, validated, error) || !writeLayout(layout, result.bytes, error)) return false;
    result.keyforms.append(std::move(remap));
    return true;
}

bool deleteNormalBindingRecords(Layout& layout, const DeletePlan& plan, QString* error)
{
    const int first = layout.value(QStringLiteral("parameters.parameter_binding_sources_begin_indices"), plan.parameter);
    const int count = layout.value(QStringLiteral("parameters.parameter_binding_sources_counts"), plan.parameter);
    if (!range(first, count, layout.counts[13])) return fail(error, QStringLiteral("删除参数绑定区间无效"));
    if (!count) return true;
    QVector<int> bindingMap(layout.counts[12], 0);
    // Core requires an empty binding record when an object has no remaining axes.
    QByteArray references, begins = integer(0), counts = integer(0);
    for (int row = 0; row < layout.counts[12]; ++row) {
        const int begin = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices"), row);
        const int total = layout.value(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts"), row);
        if (!range(begin, total, layout.counts[11])) return fail(error, QStringLiteral("关键形轴引用区间越界"));
        const int start = references.size() / 4;
        for (int offset = 0; offset < total; ++offset) {
            const int old = layout.value(QStringLiteral("parameter_binding_indices.binding_sources_indices"), begin + offset);
            if (!range(old, 1, layout.counts[13])) return fail(error, QStringLiteral("参数绑定索引越界"));
            if (old >= first && old < qint64(first) + count) continue;
            references.append(integer(old >= first + count ? old - count : old));
        }
        const int length = references.size() / 4 - start;
        if (!length) continue;
        bindingMap[row] = begins.size() / 4;
        begins.append(integer(start));
        counts.append(integer(length));
    }
    for (const auto& span : layout.columns) {
        const QString name = QString::fromLatin1(span.name);
        if (!name.endsWith(QStringLiteral(".keyform_binding_sources_indices"))) continue;
        for (int row = 0; row < span.count; ++row) {
            const int old = layout.value(name, row);
            if (old == -1) continue;
            if (!range(old, 1, bindingMap.size())) return fail(error, QStringLiteral("对象关键形绑定索引越界"));
            layout.set(name, row, integer(bindingMap[old]));
        }
    }
    layout.changes.insert(layout.slot(QStringLiteral("parameter_binding_indices.binding_sources_indices")), references);
    layout.changes.insert(layout.slot(QStringLiteral("keyform_bindings.parameter_binding_index_sources_begin_indices")), begins);
    layout.changes.insert(layout.slot(QStringLiteral("keyform_bindings.parameter_binding_index_sources_counts")), counts);
    layout.counts[11] = references.size() / 4;
    layout.counts[12] = begins.size() / 4;
    for (const auto& span : layout.columns) if (span.countIndex == 13)
        layout.writable(span.slot).remove(first * span.width, count * span.width);
    layout.counts[13] -= count;
    for (int row = 0; row < layout.counts[5]; ++row) {
        const QString field = QStringLiteral("parameters.parameter_binding_sources_begin_indices");
        const int begin = layout.value(field, row);
        if (begin >= first + count) layout.set(field, row, integer(begin - count));
        else if (row != plan.parameter && begin >= first) layout.set(field, row, integer(first));
    }
    return true;
}

bool deleteParameterRecords(Layout& layout, const DeletePlan& plan, QString* error)
{
    if (!deleteNormalBindingRecords(layout, plan, error)) return false;
    QHash<int, float> constants;
    for (const auto& constraint : plan.constraints) constants.insert(constraint.row, constraint.weight);
    for (int row = 0; row < layout.counts[30]; ++row) {
        const int parameter = layout.value(QStringLiteral("blend_shape_constraints.parameter_indices"), row);
        if (constants.contains(row)) {
            // Constant curves preserve the minimum of all limits without rescaling BlendShape deltas.
            const int begin = layout.counts[31];
            for (float key : {-1.0f, 1.0f}) {
                layout.append(QStringLiteral("blend_shape_constraint_values.keys"), floating(key));
                layout.append(QStringLiteral("blend_shape_constraint_values.weights"), floating(constants.value(row)));
                ++layout.counts[31];
            }
            layout.set(QStringLiteral("blend_shape_constraints.parameter_indices"), row,
                       integer(plan.constantHost > plan.parameter ? plan.constantHost - 1 : plan.constantHost));
            layout.set(QStringLiteral("blend_shape_constraints.blend_shape_constraint_value_sources_begin_indices"), row, integer(begin));
            layout.set(QStringLiteral("blend_shape_constraints.blend_shape_constraint_value_sources_counts"), row, integer(2));
        } else if (parameter > plan.parameter) layout.set(QStringLiteral("blend_shape_constraints.parameter_indices"), row, integer(parameter - 1));
    }
    for (const auto& span : layout.columns) if (span.countIndex == 5 && (!span.optional || span.offset))
        layout.writable(span.slot).remove(plan.parameter * span.width, span.width);
    --layout.counts[5];
    return true;
}
}

bool CMocParameterStructureBuilder::AnalyzeDelete(const QByteArray& base, const DeleteRequest& request,
                                                 DeleteImpact& impact, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout || !understoodStructure(*layout, error)) return false;
        DeletePlan plan;
        if (!makeDeletePlan(*layout, request, plan, error)) return false;
        impact = std::move(plan.impact);
        return true;
    });
}

bool CMocParameterStructureBuilder::Delete(const QByteArray& base, const DeleteRequest& request,
                                          Result& result, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout || !understoodStructure(*layout, error)) return false;
        DeletePlan plan;
        if (!makeDeletePlan(*layout, request, plan, error)) return false;
        if (!plan.impact.blockingReasons.isEmpty()) return fail(error, plan.impact.blockingReasons.join('\n'));
        Result candidate;
        candidate.oldParameterToNew = identity(layout->counts[5]);
        candidate.oldParameterToNew[plan.parameter] = -1;
        for (int index = plan.parameter + 1; index < candidate.oldParameterToNew.size(); ++index) --candidate.oldParameterToNew[index];
        candidate.affectedTargets = plan.impact.targets;
        if (!reduceDeleteRows(*layout, plan, error) || !deleteParameterRecords(*layout, plan, error) ||
            !writeLayout(*layout, candidate.bytes, error)) return false;
        result = std::move(candidate);
        return true;
    });
}

bool CMocParameterStructureBuilder::EditBindingKeys(const QByteArray& base, const EditBindingKeysRequest& request,
                                                   Result& result, QString* error)
{
    return guarded(error, [&] {
        auto layout = load(base, error);
        if (!layout) return false;
        Result candidate;
        if (!editBindingKeys(*layout, request, candidate, error)) return false;
        result = std::move(candidate);
        return true;
    });
}
}
