#include "cpsdexportcoverage.h"
#include <QJsonArray>
#include <QSet>
#include <exception>
#include <limits>

struct CPsdExportCoverage::Data
{
    Context context;
    QVector<Record> records;
    Counts counts;
    bool committed = false;
};

namespace {
QString stateKey(CPsdExportCoverage::State state)
{
    switch (state) {
    case CPsdExportCoverage::State::Pending: return QStringLiteral("pending");
    case CPsdExportCoverage::State::Raster: return QStringLiteral("raster");
    case CPsdExportCoverage::State::Empty: return QStringLiteral("empty");
    case CPsdExportCoverage::State::Excluded: return QStringLiteral("excluded");
    }
    return {};
}

QString reasonKey(CPsdExportCoverage::Reason reason)
{
    switch (reason) {
    case CPsdExportCoverage::Reason::None: return QStringLiteral("none");
    case CPsdExportCoverage::Reason::RuntimeHidden: return QStringLiteral("runtime_hidden");
    case CPsdExportCoverage::Reason::EditorHidden: return QStringLiteral("editor_hidden");
    case CPsdExportCoverage::Reason::NoVertices: return QStringLiteral("no_vertices");
    case CPsdExportCoverage::Reason::NoTriangles: return QStringLiteral("no_triangles");
    case CPsdExportCoverage::Reason::DegenerateBounds: return QStringLiteral("degenerate_bounds");
    case CPsdExportCoverage::Reason::OutsideCanvas: return QStringLiteral("outside_canvas");
    }
    return {};
}

bool validBounds(const QRect& bounds)
{
    if (bounds.isEmpty()) return bounds == QRect();
    int left, top, right, bottom;
    bounds.getCoords(&left, &top, &right, &bottom);
    const qint64 width = qint64(right) - left + 1;
    const qint64 height = qint64(bottom) - top + 1;
    return width <= 30000 && height <= 30000 &&
           qint64(right) + 1 <= std::numeric_limits<int>::max() &&
           qint64(bottom) + 1 <= std::numeric_limits<int>::max();
}
}

bool CPsdExportCoverage::Capture(const Context& context, const QVector<Record>& records, bool committed,
                                 CPsdExportCoverage& result, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    if (context.sourceMocDigest.size() != 32 || context.runtimeMocDigest.size() != 32 ||
        context.canvas.width() <= 0 || context.canvas.height() <= 0 ||
        context.canvas.width() > 30000 || context.canvas.height() > 30000 ||
        (context.method != Method::Gpu && context.method != Method::Cpu) || context.runtimeCount < 0 ||
        context.runtimeCount > MaximumRecords || records.size() != context.runtimeCount)
        return fail(QStringLiteral("PSD 对象报告的模型身份、画布或对象数量无效"));
    try {
        QSet<QString> ids;
        qsizetype characters = 0;
        Counts counts;
        counts.source = context.runtimeCount;
        for (int index = 0; index < records.size(); ++index) {
            const auto& record = records[index];
            if (record.runtimeIndex != index || record.psdLayerId != quint32(index) + 1 ||
                record.id.isEmpty() || ids.contains(record.id) || !validBounds(record.bounds))
                return fail(QStringLiteral("PSD 对象报告存在无效或重复的对象身份、图层编号或边界"));
            for (const QString* value : {&record.id, &record.displayName, &record.partId}) {
                if (value->size() > 32768 || value->size() > MaximumCharacters - characters ||
                    value->contains(QChar::Null) || !value->isValidUtf16())
                    return fail(QStringLiteral("PSD 对象报告的名称无效或超出文本预算"));
                characters += value->size();
            }
            ids.insert(record.id);
            const bool geometric = record.reason >= Reason::NoVertices && record.reason <= Reason::OutsideCanvas;
            switch (record.state) {
            case State::Pending:
                if (record.reason != Reason::None || !record.bounds.isNull())
                    return fail(QStringLiteral("尚未接受的 PSD 对象不能声明输出边界或完成原因"));
                ++counts.pending;
                break;
            case State::Raster:
                if (record.reason != Reason::None || record.bounds.isEmpty())
                    return fail(QStringLiteral("PSD 像素图层缺少有效边界"));
                ++counts.raster;
                break;
            case State::Empty:
                if (!geometric || !record.bounds.isNull())
                    return fail(QStringLiteral("PSD 空图层缺少明确的几何原因"));
                ++counts.empty;
                break;
            case State::Excluded:
                if (context.includeHidden || !record.bounds.isNull() ||
                    (record.reason != Reason::RuntimeHidden && record.reason != Reason::EditorHidden))
                    return fail(QStringLiteral("PSD 排除对象与导出范围不一致"));
                ++counts.excluded;
                break;
            default: return fail(QStringLiteral("PSD 对象报告包含未知状态"));
            }
        }
        if (committed && counts.pending)
            return fail(QStringLiteral("PSD 文件已提交，但对象报告仍有未完成项"));
        counts.requested = counts.source - counts.excluded;
        auto data = std::make_shared<Data>();
        data->context = context;
        data->context.sourceMocDigest = QByteArray(context.sourceMocDigest.constData(), context.sourceMocDigest.size());
        data->context.runtimeMocDigest = QByteArray(context.runtimeMocDigest.constData(), context.runtimeMocDigest.size());
        data->records.reserve(records.size());
        for (const auto& record : records) {
            auto copy = record;
            copy.id = QString(record.id.constData(), record.id.size());
            copy.displayName = QString(record.displayName.constData(), record.displayName.size());
            copy.partId = QString(record.partId.constData(), record.partId.size());
            data->records.append(std::move(copy));
        }
        data->counts = counts;
        data->committed = committed;
        result.m_data = std::move(data);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法分配 PSD 对象报告"));
    }
}

bool CPsdExportCoverage::IsCommitted() const { return m_data && m_data->committed; }
const CPsdExportCoverage::Context& CPsdExportCoverage::GetContext() const
{
    static const Context empty;
    return m_data ? m_data->context : empty;
}
const QVector<CPsdExportCoverage::Record>& CPsdExportCoverage::Records() const
{
    static const QVector<Record> empty;
    return m_data ? m_data->records : empty;
}
CPsdExportCoverage::Counts CPsdExportCoverage::Statistics() const { return m_data ? m_data->counts : Counts{}; }

QJsonObject CPsdExportCoverage::ToJson() const
{
    if (!m_data) return {};
    QJsonArray records;
    for (const auto& record : m_data->records) {
        records.append(QJsonObject{{"runtimeIndex", record.runtimeIndex}, {"id", record.id}, {"displayName", record.displayName},
            {"partId", record.partId}, {"psdLayerId", qint64(record.psdLayerId)}, {"state", stateKey(record.state)},
            {"reason", reasonKey(record.reason)}, {"bounds", QJsonObject{{"x", record.bounds.x()}, {"y", record.bounds.y()},
                {"width", record.bounds.width()}, {"height", record.bounds.height()}}}});
    }
    const auto& context = m_data->context;
    const auto& counts = m_data->counts;
    return {{"version", 1}, {"scope", "runtime_artmesh"}, {"method", context.method == Method::Gpu ? "gpu" : "cpu"},
        {"sourceMocDigest", QString::fromLatin1(context.sourceMocDigest.toHex())},
        {"runtimeMocDigest", QString::fromLatin1(context.runtimeMocDigest.toHex())}, {"includeHidden", context.includeHidden},
        {"canvas", QJsonObject{{"width", context.canvas.width()}, {"height", context.canvas.height()}}},
        {"clipsToCanvas", context.method == Method::Gpu}, {"runtimeCount", context.runtimeCount}, {"outputCommitted", m_data->committed}, {"counts", QJsonObject{{"source", counts.source}, {"requested", counts.requested},
            {"raster", counts.raster}, {"empty", counts.empty}, {"excluded", counts.excluded}, {"pending", counts.pending}}},
        {"records", records}};
}

QString CPsdExportCoverage::StateText(State state)
{
    switch (state) {
    case State::Pending: return QStringLiteral("未完成");
    case State::Raster: return QStringLiteral("像素层");
    case State::Empty: return QStringLiteral("空图层");
    case State::Excluded: return QStringLiteral("已排除");
    }
    return {};
}

QString CPsdExportCoverage::ReasonText(Reason reason)
{
    switch (reason) {
    case Reason::None: return {};
    case Reason::RuntimeHidden: return QStringLiteral("当前运行时隐藏");
    case Reason::EditorHidden: return QStringLiteral("在编辑器中隐藏");
    case Reason::NoVertices: return QStringLiteral("没有顶点");
    case Reason::NoTriangles: return QStringLiteral("没有三角形");
    case Reason::DegenerateBounds: return QStringLiteral("当前姿态的几何边界退化");
    case Reason::OutsideCanvas: return QStringLiteral("当前姿态位于画布外");
    }
    return {};
}

QString CPsdExportCoverage::Summary() const
{
    if (!m_data) return {};
    const auto& counts = m_data->counts;
    return QStringLiteral("对象 %1 · 请求 %2 · 像素层 %3 · 空图层 %4 · 已排除 %5 · 未完成 %6")
        .arg(counts.source).arg(counts.requested).arg(counts.raster).arg(counts.empty).arg(counts.excluded).arg(counts.pending);
}

QString CPsdExportCoverage::Text(int maximumDetails) const
{
    if (!m_data) return {};
    QString text = Summary() + QLatin1Char('\n') + (m_data->committed
        ? QStringLiteral("上述图层已写入目标 PSD。")
        : QStringLiteral("像素层与空图层数量仅表示已准备，尚未提交到目标 PSD。"));
    text += m_data->context.method == Method::Gpu
        ? QStringLiteral("\nGPU 导出按画布裁剪；画布外对象保留为空图层。")
        : QStringLiteral("\n纹理采样保留网格在画布外的像素，不烘焙裁剪蒙版、乘色或屏幕色。");
    if (m_data->counts.empty)
        text += QStringLiteral("\n空图层仅保留对象身份；当前姿态没有可写像素，不代表原始素材已恢复。");
    int displayed = 0;
    int detailed = 0;
    maximumDetails = qBound(0, maximumDetails, 1000);
    for (const auto& record : m_data->records) {
        if (record.state == State::Raster) continue;
        ++detailed;
        if (displayed >= maximumDetails) continue;
        ++displayed;
        text += QStringLiteral("\n%1 [%2]：%3").arg(record.displayName.isEmpty() ? record.id : record.displayName, record.id, StateText(record.state));
        const QString reason = ReasonText(record.reason);
        if (!reason.isEmpty()) text += QStringLiteral("（%1）").arg(reason);
    }
    if (displayed < detailed)
        text += QStringLiteral("\n另有 %1 项；复制报告可查看完整对象记录。").arg(detailed - displayed);
    return text;
}

bool CPsdExportCoverage::operator==(const CPsdExportCoverage& other) const
{
    if (m_data == other.m_data) return true;
    if (!m_data || !other.m_data) return false;
    const auto& a = m_data->context;
    const auto& b = other.m_data->context;
    if (m_data->committed != other.m_data->committed || a.sourceMocDigest != b.sourceMocDigest ||
        a.runtimeMocDigest != b.runtimeMocDigest || a.canvas != b.canvas || a.method != b.method ||
        a.includeHidden != b.includeHidden || a.runtimeCount != b.runtimeCount) return false;
    for (int index = 0; index < m_data->records.size(); ++index) {
        const auto& left = m_data->records[index];
        const auto& right = other.m_data->records[index];
        if (left.runtimeIndex != right.runtimeIndex || left.id != right.id || left.displayName != right.displayName ||
            left.partId != right.partId || left.psdLayerId != right.psdLayerId || left.state != right.state ||
            left.reason != right.reason || left.bounds != right.bounds) return false;
    }
    return true;
}
