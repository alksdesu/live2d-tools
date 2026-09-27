#ifndef GEOMETRY_BATCH_FIXTURE_H
#define GEOMETRY_BATCH_FIXTURE_H

#include "deformer_snapshot_fixture.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectionvalidation.h"
#include <stdexcept>

namespace GeometryBatchData {
using namespace live2dstruct;
using Frame = CModelGeometryKeyform;
using State = CModelGeometryState;

inline QByteArray fixture()
{
    DeformerSnapshotData::Fixture source;
    source.BindRoot(false);
    const auto& offsets = *source.table.sectionOffsetTable;
    source.column(offsets.art_meshes->keyform_binding_sources_indices, 0, 0);
    source.column(offsets.art_meshes->keyform_sources_counts, 0, 2);
    source.column(offsets.deformers->keyform_binding_sources_indices, 1, 0);
    source.column(offsets.warp_deformers->keyform_sources_counts, 0, 2);
    source.count(7, 2);
    source.count(10, 28);
    source.appendColumn(0x12c, {0, 0});
    source.appendColumn(0x130, {12, 20});
    DeformerSnapshotData::real(source.bytes, DeformerSnapshotData::integer(source.bytes, 0x12c), 0.65f);
    DeformerSnapshotData::real(source.bytes, DeformerSnapshotData::integer(source.bytes, 0x12c) + 4, 0.85f);
    QVector<float> positions;
    for (int index = 0; index < 20; ++index) {
        const quint32 bits = qFromLittleEndian<quint32>(source.bytes.constData() + offsets.keyform_positions->xys + index * 4);
        float value; std::memcpy(&value, &bits, 4); positions.append(value);
    }
    for (int index = 12; index < 20; ++index) positions.append(positions[index] + 0.125f);
    source.appendColumn(0x15c, QVector<int>(positions.size(), 0));
    const int begin = DeformerSnapshotData::integer(source.bytes, 0x15c);
    for (int index = 0; index < positions.size(); ++index) DeformerSnapshotData::real(source.bytes, begin + index * 4, positions[index]);
    return source.bytes;
}

inline QByteArray sharedFixture(bool specific)
{
    DeformerSnapshotData::Fixture source;
    const QByteArray prefix = specific ? QByteArray("deformers.") : QByteArray("art_meshes.");
    const int target = specific ? 3 : 1;
    for (const auto& span : CSectionValidation::ArraySpans(*source.table.sectionOffsetTable, *source.table.count_info)) {
        if (!QByteArray(span.name).startsWith(prefix) || !span.offset || span.count <= target) continue;
        source.bytes.replace(span.offset + target * span.width, span.width, source.bytes.mid(span.offset, span.width));
    }
    source.count(specific ? 1 : 4, target + 1);
    source.id(specific ? source.table.sectionOffsetTable->deformers->ids : source.table.sectionOffsetTable->art_meshes->ids,
              target, "Twin");
    return source.bytes;
}

inline bool load(CModelDocument& document, QString* error = nullptr)
{
    return document.LoadBytes(fixture(), QStringLiteral("geometry-batch-fixture"), error);
}

inline State all(const CModelDocument& document)
{
    State result;
    for (const auto& endpoint : State{{Frame::ArtMesh, "Mesh", 0, {}}, {Frame::ArtMesh, "Mesh", 1, {}},
             {Frame::Warp, "Warp", 0, {}}, {Frame::Warp, "Warp", 1, {}},
             {Frame::Rotation, "Root", 0, {}}, {Frame::Rotation, "Root", 1, {}}}) {
        Frame frame;
        if (!document.CaptureGeometryKeyform(endpoint.type, endpoint.id, endpoint.keyform, frame)) return {};
        result.append(std::move(frame));
    }
    return result;
}

inline State changed(State frames)
{
    int offset = 0;
    for (auto& frame : frames) {
        ++offset;
        if (auto* points = std::get_if<QVector<float>>(&frame.value)) {
            for (int index = 0; index < points->size(); ++index) (*points)[index] += (index % 2 ? -0.007f : 0.013f) * offset;
        } else {
            auto& transform = std::get<CModelRotationTransform>(frame.value);
            transform.originX += 0.003f * offset;
            transform.angle += 0.7f * offset;
            transform.scale *= 0.99f;
        }
    }
    return frames;
}

inline CModelEdit command(const CModelDocument& document)
{
    const auto before = all(document);
    CModelEdit result;
    document.MakeGeometryEdit(before, changed(before), before.value(0), result);
    return result;
}

inline QMap<QString, float> pose(const CModelDocument& document)
{
    return DeformerSnapshotData::pose(document);
}

inline CModelSession session(const CModelDocument& document)
{
    CModelSession result;
    result.sourceMoc = document.SourcePath(); result.sourceDigest = document.SourceDigest();
    result.parameters = pose(document);
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    result.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    result.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    return result;
}

inline int historyCount(CModelDocument document)
{
    int count = 0;
    while (document.UndoEdit()) ++count;
    return count;
}
}

#endif
