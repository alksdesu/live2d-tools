#ifndef PARAMETER_DELETE_FIXTURE_H
#define PARAMETER_DELETE_FIXTURE_H

#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include <QtEndian>
#include <cstring>
#include <limits>

namespace ParameterDeleteFixture {
using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

struct Reflection
{
    QByteArray bytes;
    QString parameter;
    QString fastParameter;
    QString owner;
};

inline bool MakeReflection(const QByteArray& source, int pattern, Reflection& fixture, QString* error)
{
    auto reader = CReader::FromBytes(source); CSectionTable table(reader);
    CKeyformBindingGrid grid;
    if (!table.IsValid() || !grid.Build(table, error)) return false;
    QString owner, fast;
    for (const auto* deformer : table.deformers) {
        if (deformer->types != 1) continue;
        const auto axes = grid.Axes(CKeyformBindingGrid::Rotation, deformer->ids);
        if (axes.size() == 1 && axes.first().keys.size() == 3) { owner = deformer->ids; fast = axes.first().parameterId; break; }
    }
    if (owner.isEmpty()) return false;
    Builder::CreateRequest create;
    create.parameter.id = QStringLiteral("ParamDeleteReflection");
    create.bindings.append({{Builder::Owner::Rotation, owner}, {-1, 0, 1}});
    Builder::Result result;
    if (!Builder::Create(source, create, result, error)) return false;
    auto createdReader = CReader::FromBytes(result.bytes); CSectionTable created(createdReader);
    if (!created.IsValid()) return false;
    int specific = -1;
    for (const auto* deformer : created.deformers) if (deformer->ids == owner) specific = deformer->specific_sources_indices;
    if (specific < 0) return false;
    const auto* rotation = created.rotation_deformers[specific];
    if (rotation->keyform_sources_counts != 9) return false;
    for (int local = 0; local < 9; ++local) {
        const int a = local % 3, b = local / 3;
        int x = pattern == 0 ? (a + b) % 2 : int(a == 2 || b == 1);
        int y = int(a == 1 && b > 0);
        if (pattern == 2) { if (x) x = b % 2 ? -1 : 2; if (y) y = std::numeric_limits<int>::min(); }
        const int row = rotation->keyform_sources_begin_indices + local;
        qToLittleEndian(qint32(x), result.bytes.data() + created.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_x + row * 4);
        qToLittleEndian(qint32(y), result.bytes.data() + created.sectionOffsetTable->rotation_deformer_keyforms->is_reflect_y + row * 4);
    }
    fixture = {result.bytes, create.parameter.id, fast, owner};
    return true;
}

inline bool MakeSingleton(const QByteArray& source, const QString& parameter, float key, QByteArray& fixture, QString* error)
{
    Builder::Result projected;
    if (!Builder::Delete(source, {parameter, key}, projected, error)) return false;
    Builder::ColumnState state;
    if (!Builder::CaptureColumnState(source, projected.bytes, state, error)) return false;
    auto reader = CReader::FromBytes(source); CSectionTable table(reader);
    if (!table.IsValid()) return false;
    int parameterIndex = -1;
    for (int index = 0; index < table.parameters.size(); ++index) if (table.parameters[index]->ids == parameter) parameterIndex = index;
    if (parameterIndex < 0) return false;
    const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
    for (const auto& span : spans) {
        const QByteArray name(span.name);
        if (span.countIndex == 5 || span.countIndex == 11 || span.countIndex == 12 || span.countIndex == 13 ||
            name.endsWith(".keyform_binding_sources_indices")) state.columns.remove(span.slot);
    }
    for (int count : {5, 11, 12, 13}) state.counts.remove(count);
    const auto* definition = table.parameters[parameterIndex];
    const auto column = [&](const char* name) {
        for (const auto& span : spans) if (QByteArray(span.name) == name) return span;
        return CSectionValidation::ColumnSpan{nullptr, 0, 0};
    };
    const auto values = column("keys.values");
    if (!values.name) return false;
    auto keys = source.mid(values.offset, values.count * 4);
    quint32 bits; std::memcpy(&bits, &key, 4);
    QByteArray encoded(4, '\0'); qToLittleEndian(bits, encoded.data()); keys += encoded;
    state.columns.insert(values.slot, keys);
    state.counts.insert(values.countIndex, values.count + 1);
    for (const char* name : {"parameter_bindings.keys_sources_begin_indices", "parameter_bindings.keys_sources_counts"}) {
        const auto span = column(name);
        auto data = source.mid(span.offset, span.count * 4);
        for (int index = 0; index < definition->parameter_binding_sources_counts; ++index)
            qToLittleEndian(qint32(QByteArray(name).endsWith("counts") ? 1 : values.count),
                            data.data() + (definition->parameter_binding_sources_begin_indices + index) * 4);
        state.columns.insert(span.slot, data);
    }
    for (const char* name : {"parameters.keys_sources_begin_indices", "parameters.keys_sources_counts"}) {
        const auto span = column(name);
        if (!span.offset) continue;
        auto data = source.mid(span.offset, span.count * 4);
        qToLittleEndian(qint32(QByteArray(name).endsWith("counts") ? 1 : values.count), data.data() + parameterIndex * 4);
        state.columns.insert(span.slot, data);
    }
    return Builder::BuildColumnState(source, state, fixture, error);
}
}

#endif
