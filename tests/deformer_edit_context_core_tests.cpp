#include "live2dstruct/cdeformereditcontext.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"
#include <QtEndian>

using namespace live2dstruct;
namespace {
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void real(QByteArray& bytes, int offset, float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, bytes.data() + offset);
}
struct Fixture
{
    QByteArray bytes;
    QString id, type, witness;
    QVector<float> localPoints;
    int targetIndex = -1, witnessIndex = -1;
};
Fixture fixture(const QString& type, const QString& parent, float scale, int reflections)
{
    Fixture result;
    result.bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    auto reader = CReader::FromBytes(result.bytes); CSectionTable table(reader);
    if (!table.IsValid()) return {};
    const auto& o = *table.sectionOffsetTable;
    int warpIndex = -1, rootIndex = -1, childIndex = -1;
    for (int index = 0; index < table.deformers.size(); ++index) {
        const auto* node = table.deformers[index];
        if (node->types == 0 && node->specific_sources_indices == 0) warpIndex = index;
        if (node->types == 1 && node->specific_sources_indices == 0) rootIndex = index;
        if (node->types == 1 && node->specific_sources_indices == 1) childIndex = index;
    }
    if (warpIndex < 0 || rootIndex < 0 || childIndex < 0) return {};
    result.type = type; result.targetIndex = type == "warp" ? warpIndex : childIndex;
    result.id = table.deformers[result.targetIndex]->ids;
    int largest = 0;
    for (int index = 0; index < table.art_meshes.size(); ++index)
        if (table.art_meshes[index]->vertex_counts > largest) { largest = table.art_meshes[index]->vertex_counts; result.witnessIndex = index; }
    if (result.witnessIndex < 0) return {};
    const auto* mesh = table.art_meshes[result.witnessIndex]; result.witness = mesh->ids;
    put(result.bytes, o.art_meshes->parent_deformer_indices + result.witnessIndex * 4, result.targetIndex);
    put(result.bytes, o.deformers->parent_deformer_indices + rootIndex * 4, -1);
    put(result.bytes, o.deformers->parent_deformer_indices + warpIndex * 4, parent == "nested" || (type == "warp" && parent == "rotation") ? rootIndex : -1);
    put(result.bytes, o.deformers->parent_deformer_indices + childIndex * 4,
        parent == "root" ? -1 : parent == "rotation" ? rootIndex : warpIndex);
    const auto patchRotation = [&](int specific, float angle, float originX, float originY, float ownScale, int reflect, float base) {
        const auto* source = table.rotation_deformers[specific];
        real(result.bytes, o.rotation_deformers->base_angles + specific * 4, base);
        for (int index = 0; index < source->keyform_sources_counts; ++index) {
            const int row = source->keyform_sources_begin_indices + index;
            real(result.bytes, o.rotation_deformer_keyforms->angles + row * 4, angle);
            real(result.bytes, o.rotation_deformer_keyforms->origin_x + row * 4, originX);
            real(result.bytes, o.rotation_deformer_keyforms->origin_y + row * 4, originY);
            real(result.bytes, o.rotation_deformer_keyforms->scales + row * 4, ownScale);
            put(result.bytes, o.rotation_deformer_keyforms->is_reflect_x + row * 4, reflect & 1);
            put(result.bytes, o.rotation_deformer_keyforms->is_reflect_y + row * 4, (reflect >> 1) & 1);
        }
    };
    patchRotation(0, 23, 0.4f, -0.3f, parent == "rotation" || parent == "nested" ? -0.7f : 1, 1, 11);
    patchRotation(1, 31, 0.27f, 0.38f, scale, reflections, 17);
    const auto* warp = table.warp_deformers[0];
    if (mesh->vertex_counts < warp->vertex_counts) return {};
    put(result.bytes, o.warp_deformers->is_quad_source, 1);
    for (int key = 0; key < warp->keyform_sources_counts; ++key) {
        const int begin = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int y = 0; y <= warp->rows; ++y) for (int x = 0; x <= warp->columns; ++x) {
            const float u = float(x) / warp->columns, v = float(y) / warp->rows;
            const float px = parent == "folded" ? 2 * u + 2 * v - 4 * u * v : 0.15f + 0.8f * u + 0.3f * v + 0.2f * u * v;
            const float py = parent == "folded" ? 2 * v : -0.1f + 0.9f * v + 0.15f * u * v;
            real(result.bytes, o.keyform_positions->xys + (begin + 2 * (y * (warp->columns + 1) + x)) * 4, px);
            real(result.bytes, o.keyform_positions->xys + (begin + 2 * (y * (warp->columns + 1) + x) + 1) * 4, py);
            if (key == 0 && type == "warp") result.localPoints << px << py;
        }
    }
    if (type == "rotation") result.localPoints = {0.27f, 0.38f};
    for (int key = 0; key < mesh->keyform_sources_counts; ++key) {
        const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
            const int gridVertex = vertex % warp->vertex_counts;
            const float x = type == "rotation" ? (vertex % 3 == 1 ? 1 : 0) : float(gridVertex % (warp->columns + 1)) / warp->columns;
            const float y = type == "rotation" ? (vertex % 3 == 2 ? 1 : 0) : float(gridVertex / (warp->columns + 1)) / warp->rows;
            real(result.bytes, o.keyform_positions->xys + (begin + vertex * 2) * 4, x);
            real(result.bytes, o.keyform_positions->xys + (begin + vertex * 2 + 1) * 4, y);
        }
    }
    QByteArray normalized;
    if (!CMocDeformerOrder::Normalize(result.bytes, normalized)) return {};
    result.bytes = normalized;
    auto normalizedReader = CReader::FromBytes(result.bytes); CSectionTable normalizedTable(normalizedReader);
    if (!normalizedTable.IsValid()) return {};
    for (int index = 0; index < normalizedTable.deformers.size(); ++index)
        if (normalizedTable.deformers[index]->ids == result.id) result.targetIndex = index;
    return result;
}
QMap<QString, float> targetPose(const CModelDocument& document, const Fixture& source)
{
    QMap<QString, float> pose, corner;
    for (const auto& parameter : document.Parameters()) pose[parameter.id] = parameter.defaultValue;
    if (!document.KeyformParameterValues(source.id, source.type, 0, corner)) return {};
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) pose[it.key()] = it.value();
    return pose;
}
void setPose(CoreModel& core, const QMap<QString, float>& pose)
{
    QVector<float> values;
    for (int index = 0; index < csmGetParameterCount(core.value); ++index)
        values.append(pose.value(QString::fromUtf8(csmGetParameterIds(core.value)[index])));
    core.pose(values);
}
QVector<float> witnessPoints(CoreModel& core, const Fixture& source, int count)
{
    const int index = source.witnessIndex;
    if (index < 0 || QString::fromUtf8(csmGetDrawableIds(core.value)[index]) != source.witness ||
        csmGetDrawableVertexCounts(core.value)[index] < count) return {};
    QVector<float> points;
    const auto* values = csmGetDrawableVertexPositions(core.value)[index];
    for (int vertex = 0; vertex < count; ++vertex) points << values[vertex].X << values[vertex].Y;
    return points;
}
bool near(const QVector<float>& a, const QVector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(double(a[i]) - b[i]) >
            CKeyformMappingContext::AbsoluteTolerance + CKeyformMappingContext::RelativeTolerance * std::max(std::abs(double(a[i])), std::abs(double(b[i])))) return false;
    return true;
}
}

class DeformerEditContextCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void witnessGeometryMatchesCore_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("parent"); QTest::addColumn<float>("scale"); QTest::addColumn<int>("reflections");
        for (const QString type : {QString("warp"), QString("rotation")}) {
            const QStringList parents = type == "warp" ? QStringList{"root", "rotation", "folded"} : QStringList{"root", "rotation", "warp", "nested"};
            for (const auto& parent : parents) for (float scale : {-0.5f, 0.0f, 0.5f}) for (int reflect : {0, 1, 2, 3})
                QTest::newRow(qPrintable(type + '-' + parent + '-' + QString::number(scale) + '-' + QString::number(reflect))) << type << parent << scale << reflect;
        }
    }
    void witnessGeometryMatchesCore()
    {
        QFETCH(QString, type); QFETCH(QString, parent); QFETCH(float, scale); QFETCH(int, reflections);
        const auto source = fixture(type, parent, scale, reflections); QVERIFY(!source.bytes.isEmpty());
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source.bytes, "witness.moc3", &error), qPrintable(error));
        const auto pose = targetPose(document, source); QVERIFY(!pose.isEmpty());
        CDeformerEditContext context;
        QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        CoreModel core; QVERIFY(core.load(source.bytes)); setPose(core, pose); QVERIFY(core.intact());
        const auto points = witnessPoints(core, source, type == "warp" ? source.localPoints.size() / 2 : 3);
        const auto expected = type == "warp" ? points : points.mid(0, 2);
        QVERIFY2(near(context.ModelPoints(), expected), qPrintable(QString("actual %1,%2 vs %3,%4").arg(context.ModelPoints().value(0), 0, 'g', 9).arg(context.ModelPoints().value(1), 0, 'g', 9).arg(expected.value(0), 0, 'g', 9).arg(expected.value(1), 0, 'g', 9)));
        QVector<float> mapped, restored;
        QVERIFY(context.ToModel(source.localPoints, mapped)); QVERIFY(near(mapped, expected));
        QVERIFY2(context.FromModel(expected.mid(0, 2), restored, &error), qPrintable(error));
        QVERIFY(near(restored, source.localPoints.mid(0, 2)));
        if (type == "rotation" && scale != 0) {
            QPointF direction(points[0] - points[4], points[1] - points[5]);
            direction /= std::hypot(direction.x(), direction.y());
            QVERIFY(std::hypot(direction.x() - context.RotationDirection().x(), direction.y() - context.RotationDirection().y()) < 0.00002);
        }
        if (type == "rotation" && scale == 0) {
            QVERIFY(near(points.mid(2, 2), expected)); QVERIFY(near(points.mid(4, 2), expected));
            const auto direction = context.RotationDirection(); QVERIFY(std::abs(std::hypot(direction.x(), direction.y()) - 1) < 0.000001);
        }
    }
    void originInverseAndAngleDeltaMatchNewCoreCandidate_data()
    {
        QTest::addColumn<QString>("parent");
        for (const char* parent : {"root", "rotation", "warp", "nested"}) QTest::newRow(parent) << QString::fromLatin1(parent);
    }
    void originInverseAndAngleDeltaMatchNewCoreCandidate()
    {
        QFETCH(QString, parent);
        const auto source = fixture("rotation", parent, -0.5f, 1);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(source.bytes, "move.moc3"));
        const auto pose = targetPose(document, source);
        CDeformerEditContext context; QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        QVector<float> clicked; QVERIFY(context.ToModel({0.35f, 0.42f}, clicked));
        QVector<float> local; QVERIFY(context.FromModel(clicked, local));
        auto reader = CReader::FromBytes(source.bytes); CSectionTable table(reader);
        QByteArray movedBytes = source.bytes;
        const auto* rotation = table.rotation_deformers[table.deformers[source.targetIndex]->specific_sources_indices];
        const auto* offsets = table.sectionOffsetTable->rotation_deformer_keyforms;
        for (int i = 0; i < rotation->keyform_sources_counts; ++i) {
            const int row = rotation->keyform_sources_begin_indices + i;
            real(movedBytes, offsets->origin_x + row * 4, local[0]); real(movedBytes, offsets->origin_y + row * 4, local[1]);
        }
        CoreModel moved; QVERIFY(moved.load(movedBytes)); setPose(moved, pose);
        const auto before = witnessPoints(moved, source, 3); QVERIFY(near(before.mid(0, 2), clicked));
        QByteArray rotatedBytes = movedBytes;
        constexpr double degrees = 47;
        for (int i = 0; i < rotation->keyform_sources_counts; ++i) {
            const int row = rotation->keyform_sources_begin_indices + i;
            real(rotatedBytes, offsets->angles + row * 4, 31 + degrees);
        }
        CoreModel rotated; QVERIFY(rotated.load(rotatedBytes)); setPose(rotated, pose);
        const auto after = witnessPoints(rotated, source, 3);
        const double radians = -degrees * std::acos(-1.0) / 180;
        QVector<float> expected = before;
        for (int vertex = 1; vertex < 3; ++vertex) {
            const double x = before[vertex * 2] - before[0], y = before[vertex * 2 + 1] - before[1];
            expected[vertex * 2] = before[0] + std::cos(radians) * x - std::sin(radians) * y;
            expected[vertex * 2 + 1] = before[1] + std::sin(radians) * x + std::cos(radians) * y;
        }
        QVERIFY(near(after, expected)); QVERIFY(moved.intact()); QVERIFY(rotated.intact());
    }
};
QTEST_GUILESS_MAIN(DeformerEditContextCoreTests)
#include "deformer_edit_context_core_tests.moc"
