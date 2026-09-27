#ifndef WIDE_MESH_FIXTURE_H
#define WIDE_MESH_FIXTURE_H

#include "deformer_replay_fixture.h"
#include <algorithm>
#include <cmath>

namespace WideMeshData {
enum class Layout { Sparse, Grid };
struct Fixture {
    QByteArray bytes;
    QStringList ids;
    QVector<float> positions, uvs;
    QVector<quint16> indices;
    int vertexCount = 0;
};

inline void appendFloat(QByteArray& bytes, float value)
{
    quint32 bits; std::memcpy(&bits, &value, sizeof(bits));
    char encoded[4]; qToLittleEndian(bits, encoded); bytes.append(encoded, 4);
}

inline bool make(int count, Layout layout, Fixture& result, QString* error = nullptr,
                 int meshCount = 1, int minimumKeyforms = 1)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (count < 3 || count > 1000000 || meshCount < 1 || meshCount > 2 || minimumKeyforms < 1)
        return fail(QStringLiteral("Invalid wide mesh fixture request"));
    const auto bytes = DeformerReplayFixture::read(QStringLiteral("Mark"));
    auto reader = live2dstruct::CReader::FromBytes(bytes); live2dstruct::CSectionTable table(reader);
    if (!table.IsValid()) return fail(QStringLiteral("Invalid source model"));
    QVector<int> candidates;
    for (int i = 0; i < table.art_meshes.size(); ++i)
        if (table.art_meshes[i]->keyform_sources_counts >= minimumKeyforms) candidates.append(i);
    std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
        return table.art_meshes[a]->keyform_sources_counts < table.art_meshes[b]->keyform_sources_counts;
    });
    if (candidates.size() < meshCount) return fail(QStringLiteral("No suitable source meshes"));
    Fixture fixture; fixture.vertexCount = count;
    const int columns = std::min(256, std::max(2, count / 2));
    const int rows = (count + columns - 1) / columns;
    for (int i = 0; i < count; ++i) {
        const float x = float(i % columns) / (columns - 1), y = float(i / columns) / (rows - 1);
        if (layout == Layout::Sparse) {
            fixture.uvs.append({x * .2f + .4f, y * .2f + .4f});
            fixture.positions.append({x * .4f - .2f, y * .4f - .2f});
        } else {
            fixture.uvs.append({x, y}); fixture.positions.append({x * 1.6f - .8f, y * 1.6f - .8f});
        }
    }
    if (layout == Layout::Sparse) {
        fixture.indices = count >= 65536 ? QVector<quint16>{32768, 65534, 65535} : QVector<quint16>{0, 1, quint16(count - 1)};
        const float positions[]{-.8f, -.8f, .8f, -.8f, 0, .8f};
        const float uvs[]{.1f, .1f, .9f, .1f, .5f, .9f};
        for (int corner = 0; corner < 3; ++corner) for (int axis = 0; axis < 2; ++axis) {
            fixture.positions[fixture.indices[corner] * 2 + axis] = positions[corner * 2 + axis];
            fixture.uvs[fixture.indices[corner] * 2 + axis] = uvs[corner * 2 + axis];
        }
    } else {
        for (int y = 0; y + 1 < rows; ++y) for (int x = 0; x + 1 < columns; ++x) {
            const int a = y * columns + x, b = a + 1, c = a + columns, d = c + 1;
            if (c < count && c <= 65535) fixture.indices.append({quint16(a), quint16(b), quint16(c)});
            if (d < count && d <= 65535) fixture.indices.append({quint16(b), quint16(d), quint16(c)});
        }
        if (fixture.indices.isEmpty()) return fail(QStringLiteral("Grid fixture has no triangles"));
    }
    DeformerReplayFixture::RawColumns raw(bytes);
    const auto appendPool = [&](const QByteArray& name, const QByteArray& added) {
        const auto span = raw.spans.value(name); auto values = raw.column(name);
        const int start = values.size() / span.width; values.append(added);
        raw.state.columns[span.slot] = values; raw.state.counts[span.countIndex] = values.size() / span.width;
        return start;
    };
    QByteArray uvBytes, indexBytes;
    for (float value : fixture.uvs) appendFloat(uvBytes, value);
    for (quint16 value : fixture.indices) { char encoded[2]; qToLittleEndian(value, encoded); indexBytes.append(encoded, 2); }
    for (int mesh = 0; mesh < meshCount; ++mesh) {
        const int index = candidates[mesh]; const auto* source = table.art_meshes[index]; fixture.ids.append(source->ids);
        const int oldBegin = source->keyform_sources_begin_indices, forms = source->keyform_sources_counts;
        const int newBegin = raw.count("art_mesh_keyforms.opacities");
        for (auto it = raw.spans.cbegin(); it != raw.spans.cend(); ++it) {
            if (!it.key().startsWith("art_mesh_keyforms.") || !it->offset) continue;
            const auto original = raw.column(it.key());
            appendPool(it.key(), original.mid(oldBegin * it->width, forms * it->width));
        }
        raw.set("art_meshes.keyform_sources_begin_indices", index, newBegin);
        raw.set("art_meshes.vertex_counts", index, count);
        raw.set("art_meshes.parent_deformer_indices", index, -1);
        raw.set("art_meshes.parent_part_indices", index, -1);
        raw.set("art_meshes.uv_sources_begin_indices", index, appendPool("uvs.uvs", uvBytes));
        raw.set("art_meshes.position_index_sources_begin_indices", index, appendPool("position_indices.indices", indexBytes));
        raw.set("art_meshes.position_index_sources_counts", index, fixture.indices.size());
        for (int key = 0; key < forms; ++key) {
            QByteArray points;
            for (int i = 0; i < fixture.positions.size(); ++i) appendFloat(points, fixture.positions[i] + float(key) * (i % 2 ? -.02f : .03f));
            raw.set("art_mesh_keyforms.keyform_position_sources_begin_indices", newBegin + key, appendPool("keyform_positions.xys", points));
        }
    }
    fixture.bytes = raw.encode(error);
    if (fixture.bytes.isEmpty()) return false;
    result = std::move(fixture);
    if (error) error->clear();
    return true;
}
}

#endif
