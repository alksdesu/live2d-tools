#include "cwarpgridpreview.h"
#include "core/Live2DCubismCore.h"
#include "std/cexportcontrol.h"
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {
struct Failure { QString message; };
void require(bool condition, const QString& message)
{
    if (!condition) throw Failure{message};
}
struct Budget {
    CWarpGridPreview::Limits limits;
    CExportControl* control;
    qint64 memory = 0, work = 0;
    qsizetype vertices = 0, indices = 0, segments = 0;
    void retain(qint64 size) {
        require(size >= 0 && size <= limits.maximumMemoryBytes - memory, QStringLiteral("网格预览内存超出限制"));
        memory += size;
    }
    void spend(qint64 size = 1) {
        require(size >= 0 && size <= limits.maximumWork - work, QStringLiteral("网格预览工作量超出限制"));
        const auto previous = work; work += size;
        if (control && (previous / 4096 != work / 4096) && !control->Checkpoint())
            throw Failure{control->Error().isEmpty() ? QStringLiteral("已取消网格预览") : control->Error()};
    }
    void report(qint64 current, qint64 total, const QString& detail) {
        if (control && !control->Report(CExportControl::Phase::Preparing, current, total, detail))
            throw Failure{control->Error().isEmpty() ? QStringLiteral("已取消网格预览") : control->Error()};
    }
};
void validateLimits(const CWarpGridPreview::Limits& value)
{
    const CWarpGridPreview::Limits cap;
    require(value.maximumMocBytes > 0 && value.maximumMocBytes <= cap.maximumMocBytes &&
        value.maximumMemoryBytes > 0 && value.maximumMemoryBytes <= cap.maximumMemoryBytes &&
        value.maximumVertices >= 0 && value.maximumVertices <= cap.maximumVertices &&
        value.maximumIndices >= 0 && value.maximumIndices <= cap.maximumIndices &&
        value.maximumSegments >= 0 && value.maximumSegments <= cap.maximumSegments &&
        value.maximumWork > 0 && value.maximumWork <= cap.maximumWork &&
        value.maximumObjects > 0 && value.maximumObjects <= cap.maximumObjects, QStringLiteral("网格预览预算无效"));
}
QString ownedId(const QString& id)
{
    require(!id.isEmpty() && id.size() <= 64 && !id.contains(QChar::Null) &&
        id.toUtf8().size() <= 64 && QString::fromUtf8(id.toUtf8()) == id, QStringLiteral("网格预览包含无效 ID"));
    return QString(id.constData(), id.size());
}
QMap<QString, float> freeze(const QMap<QString, float>& source, Budget& budget)
{
    require(source.size() <= budget.limits.maximumObjects, QStringLiteral("网格预览姿态数量超出限制"));
    budget.spend(qint64(source.size()) * 64);
    budget.retain(qint64(source.size()) * 512);
    QMap<QString, float> result;
    for (auto item = source.cbegin(); item != source.cend(); ++item) {
        require(std::isfinite(item.value()), QStringLiteral("网格预览姿态必须是有限数值"));
        result.insert(ownedId(item.key()), item.value());
    }
    return result;
}
struct AlignedDelete {
    size_t alignment;
    void operator()(void* value) const { ::operator delete(value, std::align_val_t(alignment)); }
};
using Memory = std::unique_ptr<void, AlignedDelete>;
Memory allocate(size_t size, size_t alignment, Budget& budget)
{
    budget.retain(qint64(size) + qint64(alignment));
    return Memory(::operator new(size, std::align_val_t(alignment)), {alignment});
}
class Model {
    Memory moc{nullptr, {csmAlignofMoc}}, storage{nullptr, {csmAlignofModel}};
public:
    csmModel* value = nullptr;
    void load(const QByteArray& bytes, Budget& budget) {
        moc = allocate(size_t(bytes.size()), csmAlignofMoc, budget);
        std::memcpy(moc.get(), bytes.constData(), size_t(bytes.size()));
        require(csmHasMocConsistency(moc.get(), unsigned(bytes.size())) == 1, QStringLiteral("网格预览 MOC 未通过 Core 一致性检查"));
        std::memcpy(moc.get(), bytes.constData(), size_t(bytes.size()));
        const auto revived = csmReviveMocInPlace(moc.get(), unsigned(bytes.size()));
        require(revived, QStringLiteral("Core 无法载入网格预览 MOC"));
        const auto size = csmGetSizeofModel(revived);
        require(size > 0, QStringLiteral("Core 网格预览模型大小无效"));
        storage = allocate(size, csmAlignofModel, budget);
        value = csmInitializeModelInPlace(revived, storage.get(), size);
        require(value, QStringLiteral("Core 无法初始化网格预览模型"));
    }
};
QStringList directory(int count, const char** ids, Budget& budget)
{
    require(count >= 0 && count <= budget.limits.maximumObjects && (!count || ids), QStringLiteral("Core 网格预览目录无效"));
    budget.retain(qint64(count) * 512);
    QStringList result; result.reserve(count); QSet<QString> unique;
    for (int i = 0; i < count; ++i) {
        budget.spend(); require(ids[i], QStringLiteral("Core 网格预览 ID 缺失"));
        const size_t length = strnlen(ids[i], 65);
        require(length > 0 && length <= 64, QStringLiteral("Core 网格预览 ID 长度无效"));
        const QByteArray raw(ids[i], qsizetype(length));
        const auto id = QString::fromUtf8(raw);
        require(id.toUtf8() == raw && !unique.contains(id), QStringLiteral("Core 网格预览 ID 无效或重复"));
        unique.insert(id); result.append(id);
    }
    return result;
}
template<class T> void same(const T* a, const T* b, int count, const QString& field, Budget& budget)
{
    require(count >= 0 && (!count || (a && b)), QStringLiteral("Core 网格预览字段缺失：%1").arg(field));
    budget.spend(qint64(count) * std::max<size_t>(1, sizeof(T) / sizeof(float)));
    require(!count || std::memcmp(a, b, size_t(count) * sizeof(T)) == 0,
        QStringLiteral("网格预览仅允许坐标改变：%1").arg(field));
}
void finite(float value)
{
    require(std::isfinite(value), QStringLiteral("Core 网格预览包含非有限数值"));
}
void pose(csmModel* a, csmModel* b, const QMap<QString, float>& parameters,
          const QMap<QString, float>& parts, Budget& budget)
{
    const int count = csmGetParameterCount(a), partCount = csmGetPartCount(a);
    const auto ids = directory(count, csmGetParameterIds(a), budget);
    require(ids == directory(csmGetParameterCount(b), csmGetParameterIds(b), budget), QStringLiteral("网格预览参数目录不一致"));
    same(csmGetParameterTypes(a), csmGetParameterTypes(b), count, "parameter types", budget);
    same(csmGetParameterMinimumValues(a), csmGetParameterMinimumValues(b), count, "parameter minima", budget);
    same(csmGetParameterMaximumValues(a), csmGetParameterMaximumValues(b), count, "parameter maxima", budget);
    same(csmGetParameterDefaultValues(a), csmGetParameterDefaultValues(b), count, "parameter defaults", budget);
    same(csmGetParameterKeyCounts(a), csmGetParameterKeyCounts(b), count, "parameter keys", budget);
    auto* av = csmGetParameterValues(a); auto* bv = csmGetParameterValues(b);
    require(!count || (av && bv && csmGetParameterKeyValues(a) && csmGetParameterKeyValues(b)), QStringLiteral("Core 参数数据缺失"));
    for (int i = 0; i < count; ++i) {
        budget.spend(); require(parameters.contains(ids[i]), QStringLiteral("当前姿态缺少参数：%1").arg(ids[i]));
        finite(csmGetParameterMinimumValues(a)[i]); finite(csmGetParameterMaximumValues(a)[i]); finite(csmGetParameterDefaultValues(a)[i]);
        const int keys = csmGetParameterKeyCounts(a)[i];
        require(keys >= 0 && keys <= budget.limits.maximumObjects, QStringLiteral("Core 参数关键值数量无效"));
        budget.spend(keys); same(csmGetParameterKeyValues(a)[i], csmGetParameterKeyValues(b)[i], keys, "parameter key values", budget);
        for (int k = 0; k < keys; ++k) finite(csmGetParameterKeyValues(a)[i][k]);
        av[i] = bv[i] = parameters.value(ids[i]);
    }
    const auto partIds = directory(partCount, csmGetPartIds(a), budget);
    require(partIds == directory(csmGetPartCount(b), csmGetPartIds(b), budget), QStringLiteral("网格预览部件目录不一致"));
    same(csmGetPartParentPartIndices(a), csmGetPartParentPartIndices(b), partCount, "part parents", budget);
    auto* ap = csmGetPartOpacities(a); auto* bp = csmGetPartOpacities(b);
    require(!partCount || (ap && bp), QStringLiteral("Core 部件数据缺失"));
    for (int i = 0; i < partCount; ++i) {
        budget.spend(); require(parts.contains(partIds[i]), QStringLiteral("当前姿态缺少部件：%1").arg(partIds[i]));
        require(csmGetPartParentPartIndices(a)[i] >= -1 && csmGetPartParentPartIndices(a)[i] < partCount, QStringLiteral("Core 部件父级无效"));
        ap[i] = bp[i] = parts.value(partIds[i]);
    }

}
}

bool CWarpGridPreview::Build(const Request& input, Result& result, QString* error, CExportControl* control) try
{
    if (error) error->clear();
    const auto limits = input.limits; validateLimits(limits);
    Budget budget{limits, control};
    require(!input.beforeMoc.isEmpty() && !input.afterMoc.isEmpty() && input.beforeMoc.size() <= limits.maximumMocBytes &&
        input.afterMoc.size() <= limits.maximumMocBytes - input.beforeMoc.size(), QStringLiteral("网格预览 MOC 大小超出限制"));
    budget.retain(input.beforeMoc.size() + input.afterMoc.size());
    const QByteArray before(input.beforeMoc.constData(), input.beforeMoc.size()), after(input.afterMoc.constData(), input.afterMoc.size());
    const auto parameters = freeze(input.parameters, budget), parts = freeze(input.partOpacities, budget);
    budget.report(0, 6, QStringLiteral("载入独立 Core 模型"));
    Model original, candidate; original.load(before, budget);
    budget.report(1, 6, QStringLiteral("载入候选 Core 模型"));
    candidate.load(after, budget);
    budget.report(2, 6, QStringLiteral("求值当前参数姿态"));
    auto* a = original.value; auto* b = candidate.value;
    pose(a, b, parameters, parts, budget);
    const int drawableCount = csmGetDrawableCount(a);
    require(drawableCount >= 0 && drawableCount <= limits.maximumObjects && drawableCount == csmGetDrawableCount(b),
        QStringLiteral("网格预览 drawable 数量无效"));
    same(csmGetDrawableVertexCounts(a), csmGetDrawableVertexCounts(b), drawableCount, "vertex counts", budget);
    same(csmGetDrawableIndexCounts(a), csmGetDrawableIndexCounts(b), drawableCount, "index counts", budget);
    qsizetype totalVertices = 0, totalIndices = 0;
    for (int i = 0; i < drawableCount; ++i) {
        budget.spend(); const int vertices = csmGetDrawableVertexCounts(a)[i], indices = csmGetDrawableIndexCounts(a)[i];
        require(vertices >= 0 && vertices <= limits.maximumVertices - totalVertices && indices >= 0 && indices % 3 == 0 &&
            indices <= limits.maximumIndices - totalIndices, QStringLiteral("网格预览坐标或索引总量超出限制"));
        totalVertices += vertices; totalIndices += indices;
    }
    csmResetDrawableDynamicFlags(a); csmUpdateModel(a);
    budget.report(3, 6, QStringLiteral("求值候选网格"));
    csmResetDrawableDynamicFlags(b); csmUpdateModel(b);
    same(csmGetParameterValues(a), csmGetParameterValues(b), csmGetParameterCount(a), "evaluated parameters", budget);
    same(csmGetPartOpacities(a), csmGetPartOpacities(b), csmGetPartCount(a), "evaluated parts", budget);
    for (int i = 0; i < csmGetParameterCount(a); ++i) finite(csmGetParameterValues(a)[i]);
    for (int i = 0; i < csmGetPartCount(a); ++i) finite(csmGetPartOpacities(a)[i]);
    csmVector2 sizeA{}, originA{}, sizeB{}, originB{}; float ppuA = 0, ppuB = 0;
    csmReadCanvasInfo(a, &sizeA, &originA, &ppuA); csmReadCanvasInfo(b, &sizeB, &originB, &ppuB);
    for (float value : {sizeA.X, sizeA.Y, originA.X, originA.Y, ppuA}) finite(value);
    same(&sizeA, &sizeB, 1, "canvas size", budget); same(&originA, &originB, 1, "canvas origin", budget); same(&ppuA, &ppuB, 1, "pixels per unit", budget);
    require(sizeA.X > 0 && sizeA.Y > 0 && ppuA > 0, QStringLiteral("Core 画布尺寸或像素比例无效"));
    const int count = csmGetDrawableCount(a);
    const auto ids = directory(count, csmGetDrawableIds(a), budget);
    require(ids == directory(csmGetDrawableCount(b), csmGetDrawableIds(b), budget), QStringLiteral("网格预览 drawable 目录不一致"));
    for (const auto pair : {std::pair{csmGetDrawableTextureIndices(a), csmGetDrawableTextureIndices(b)},
            {csmGetDrawableDrawOrders(a), csmGetDrawableDrawOrders(b)}, {csmGetDrawableRenderOrders(a), csmGetDrawableRenderOrders(b)},
            {csmGetDrawableParentPartIndices(a), csmGetDrawableParentPartIndices(b)}, {csmGetDrawableVertexCounts(a), csmGetDrawableVertexCounts(b)},
            {csmGetDrawableIndexCounts(a), csmGetDrawableIndexCounts(b)}, {csmGetDrawableMaskCounts(a), csmGetDrawableMaskCounts(b)}})
        same(pair.first, pair.second, count, "drawable definitions", budget);
    same(csmGetDrawableConstantFlags(a), csmGetDrawableConstantFlags(b), count, "drawable flags", budget);
    same(csmGetDrawableOpacities(a), csmGetDrawableOpacities(b), count, "drawable opacity", budget);
    same(csmGetDrawableMultiplyColors(a), csmGetDrawableMultiplyColors(b), count, "multiply color", budget);
    same(csmGetDrawableScreenColors(a), csmGetDrawableScreenColors(b), count, "screen color", budget);
    require(!count || (csmGetDrawableIndices(a) && csmGetDrawableIndices(b) && csmGetDrawableVertexUvs(a) && csmGetDrawableVertexUvs(b) &&
        csmGetDrawableVertexPositions(a) && csmGetDrawableVertexPositions(b) && csmGetDrawableMasks(a) && csmGetDrawableMasks(b) &&
        csmGetDrawableDynamicFlags(a) && csmGetDrawableDynamicFlags(b)), QStringLiteral("Core 网格数据缺失"));
    Result output; output.canvasPixels = QSizeF(sizeA.X, sizeA.Y);
    budget.report(4, 6, QStringLiteral("比较网格坐标和边界"));
    for (int mesh = 0; mesh < count; ++mesh) {
        budget.spend();
        const int vertices = csmGetDrawableVertexCounts(a)[mesh], indices = csmGetDrawableIndexCounts(a)[mesh], masks = csmGetDrawableMaskCounts(a)[mesh];
        require(vertices >= 0 && vertices <= limits.maximumVertices - budget.vertices && indices >= 0 && indices % 3 == 0 &&
            indices <= limits.maximumIndices - budget.indices && masks >= 0 && masks <= count, QStringLiteral("网格预览坐标或索引总量超出限制"));
        budget.vertices += vertices; budget.indices += indices;
        budget.spend(masks); same(csmGetDrawableMasks(a)[mesh], csmGetDrawableMasks(b)[mesh], masks, "drawable masks", budget);
        for (int i = 0; i < masks; ++i) require(csmGetDrawableMasks(a)[mesh][i] >= 0 && csmGetDrawableMasks(a)[mesh][i] < count, QStringLiteral("网格蒙版引用无效"));
        require((csmGetDrawableDynamicFlags(a)[mesh] & csmIsVisible) == (csmGetDrawableDynamicFlags(b)[mesh] & csmIsVisible), QStringLiteral("网格预览可见性改变"));
        finite(csmGetDrawableOpacities(a)[mesh]);
        for (auto color : {csmGetDrawableMultiplyColors(a)[mesh], csmGetDrawableScreenColors(a)[mesh]})
            for (float value : {color.X, color.Y, color.Z, color.W}) finite(value);
        same(csmGetDrawableVertexUvs(a)[mesh], csmGetDrawableVertexUvs(b)[mesh], vertices, "drawable UV", budget);
        same(csmGetDrawableIndices(a)[mesh], csmGetDrawableIndices(b)[mesh], indices, "drawable indices", budget);
        const auto* av = csmGetDrawableVertexPositions(a)[mesh]; const auto* bv = csmGetDrawableVertexPositions(b)[mesh];
        require(!vertices || (av && bv), QStringLiteral("Core 顶点坐标缺失"));
        bool changed = false;
        for (int i = 0; i < vertices; ++i) {
            budget.spend(); finite(av[i].X); finite(av[i].Y); finite(bv[i].X); finite(bv[i].Y);
            finite(csmGetDrawableVertexUvs(a)[mesh][i].X); finite(csmGetDrawableVertexUvs(a)[mesh][i].Y);
            const double delta = std::hypot(double(av[i].X) - bv[i].X, double(av[i].Y) - bv[i].Y) * ppuA;
            if (delta > 0) { changed = true; ++output.changedVertices; output.maximumDisplacementPixels = std::max(output.maximumDisplacementPixels, delta); }
        }
        if (changed) output.changedDrawables.append(ids[mesh]);
        budget.retain(qint64(indices) * sizeof(quint64));
        std::vector<quint64> edges; edges.reserve(size_t(indices));
        const auto* triangles = csmGetDrawableIndices(a)[mesh];
        for (int i = 0; i < indices; i += 3) for (int k = 0; k < 3; ++k) {
            budget.spend(); quint32 first = triangles[i + k], second = triangles[i + (k + 1) % 3];
            require(first < quint32(vertices) && second < quint32(vertices), QStringLiteral("Core 网格三角索引无效"));
            if (first == second) continue;
            if (first > second) std::swap(first, second);
            edges.push_back((quint64(first) << 32) | second);
        }
        std::sort(edges.begin(), edges.end(), [&](quint64 x, quint64 y) { budget.spend(); return x < y; });
        bool open = false;
        for (size_t i = 0; i < edges.size();) {
            size_t end = i + 1; while (end < edges.size() && edges[end] == edges[i]) { budget.spend(); ++end; }
            if (end == i + 1) open = true;
            i = end;
        }
        if (!open && indices > 0) output.closedMeshEdges = true;
        for (size_t i = 0; i < edges.size();) {
            budget.spend(); size_t end = i + 1; while (end < edges.size() && edges[end] == edges[i]) { budget.spend(); ++end; }
            if (!open || end == i + 1) {
                require(budget.segments < limits.maximumSegments, QStringLiteral("网格预览线段总量超出限制"));
                ++budget.segments; budget.retain(4 * qint64(sizeof(QLineF)) + 32);
                const auto first = quint32(edges[i] >> 32), second = quint32(edges[i]);
                const auto point = [ppuA](const csmVector2& p) { return QPointF(double(p.X) * ppuA, double(p.Y) * ppuA); };
                output.beforeContours.append(QLineF(point(av[first]), point(av[second])));
                output.afterContours.append(QLineF(point(bv[first]), point(bv[second])));
            }
            i = end;
        }
        budget.memory -= qint64(indices) * sizeof(quint64);
        if (mesh % 32 == 0) budget.report(4 + qint64(mesh) * 2 / count, 6, QStringLiteral("比较网格边界"));
    }
    budget.report(6, 6, QStringLiteral("当前姿态比较完成"));
    result = std::move(output);
    return true;
}
catch (const Failure& failure) { if (error) *error = failure.message; return false; }
catch (const std::exception&) { if (error) *error = QStringLiteral("无法分配或计算网格预览，原结果保持不变"); return false; }
