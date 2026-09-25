#include "cdeformerdrag.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool valid(const CDeformerDrag::Parameter& parameter)
{
    return !parameter.id.isEmpty() && std::isfinite(parameter.minimum) && std::isfinite(parameter.maximum) &&
           std::isfinite(parameter.defaultValue) && parameter.minimum <= parameter.maximum;
}

bool validVertices(const CDeformerDrag::Vertices& vertices, int count)
{
    if (vertices.size() != count) return false;
    for (const auto& points : vertices) {
        if (points.size() % 2) return false;
        for (float point : points) if (!std::isfinite(point)) return false;
    }
    return true;
}

bool moved(const QVector<float>& before, const QVector<float>& after)
{
    for (qsizetype index = 0; index < before.size(); index += 2) {
        const double x = double(after[index]) - before[index];
        const double y = double(after[index + 1]) - before[index + 1];
        if (x * x + y * y > double(1e-10f)) return true;
    }
    return false;
}
}

bool CDeformerDrag::SetParameters(const QStringList& ids, const QList<float>& minimums,
                                  const QList<float>& maximums, const QList<float>& defaults, QString* error)
{
    if (error) error->clear();
    Stop();
    m_parameters = {};
    m_ids = {};
    try {
        CDeformerDrag candidate;
        const qsizetype count = std::min({ids.size(), minimums.size(), maximums.size(), defaults.size()});
        for (qsizetype index = 0; index < count; ++index) {
            Parameter parameter{ids[index], minimums[index], maximums[index], defaults[index]};
            if (!valid(parameter)) continue;
            parameter.defaultValue = std::clamp(parameter.defaultValue, parameter.minimum, parameter.maximum);
            candidate.m_ids.append(parameter.id);
            candidate.m_parameters.append(std::move(parameter));
        }
        Swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备变形器拖动参数"));
    }
}

bool CDeformerDrag::MapValue(float input, float minimum, float maximum, float& output)
{
    if (!std::isfinite(input) || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) return false;
    const double ratio = (std::clamp(double(input), -1.0, 1.0) + 1.0) * 0.5;
    const double value = double(minimum) + (double(maximum) - minimum) * ratio;
    output = static_cast<float>(std::clamp(value, double(minimum), double(maximum)));
    return true;
}

bool CDeformerDrag::Apply(float viewX, float viewY, QString* error)
{
    if (error) error->clear();
    if (m_parameters.isEmpty() || !std::isfinite(viewX) || !std::isfinite(viewY))
        return fail(error, QStringLiteral("变形器拖动参数或鼠标位置无效"));
    try {
        QList<float> candidate;
        candidate.reserve(m_parameters.size());
        for (qsizetype index = 0; index < m_parameters.size(); ++index) {
            const auto& parameter = m_parameters[index];
            float value;
            if (!MapValue(index % 2 ? viewY : viewX, parameter.minimum, parameter.maximum, value))
                return fail(error, QStringLiteral("变形器参数范围无效：%1").arg(parameter.id));
            candidate.append(value);
        }
        m_values.swap(candidate);
        m_active = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法计算变形器拖动参数"));
    }
}

void CDeformerDrag::Stop() noexcept
{
    m_active = false;
    QList<float> empty;
    m_values.swap(empty);
}

void CDeformerDrag::Swap(CDeformerDrag& other) noexcept
{
    m_parameters.swap(other.m_parameters);
    m_ids.swap(other.m_ids);
    m_values.swap(other.m_values);
    std::swap(m_active, other.m_active);
}

bool CDeformerDrag::Probe(const QVector<Parameter>& parameters, const QVector<float>& current, int drawableCount,
                         const QVector<int>& targets, const Evaluator& evaluate, const Restorer& restore,
                         QVector<int>& result, QString* error)
{
    if (error) error->clear();
    if (parameters.size() != current.size() || parameters.size() > std::numeric_limits<int>::max() ||
        drawableCount < 0 || targets.isEmpty() || !evaluate || !restore)
        return fail(error, QStringLiteral("参数探测输入不完整"));
    bool evaluated = false, complete = false;
    QString problem;
    QVector<int> selected;
    try {
        QSet<QString> ids;
        for (qsizetype index = 0; index < parameters.size(); ++index) {
            if (!valid(parameters[index]) || !std::isfinite(current[index]) || ids.contains(parameters[index].id))
                return fail(error, QStringLiteral("参数探测定义无效：%1").arg(parameters[index].id));
            ids.insert(parameters[index].id);
        }
        QSet<int> targetSet;
        for (int target : targets) {
            if (target < 0 || target >= drawableCount || targetSet.contains(target))
                return fail(error, QStringLiteral("参数探测目标无效"));
            targetSet.insert(target);
        }
        struct Score { int parameter; int affected; };
        QVector<Score> scores;
        Vertices baseline;
        evaluated = true;
        if (!evaluate(current, baseline, &problem) || !validVertices(baseline, drawableCount)) {
            if (problem.isEmpty()) problem = QStringLiteral("参数探测基准顶点无效");
        } else {
            complete = true;
            QVector<float> values = current;
            for (qsizetype index = 0; index < parameters.size(); ++index) {
                const auto& parameter = parameters[index];
                if (parameter.minimum == parameter.maximum) continue;
                const double center = (double(parameter.minimum) + parameter.maximum) * 0.5;
                values[index] = double(current[index]) < center ? parameter.maximum : parameter.minimum;
                Vertices sample;
                if (!evaluate(values, sample, &problem) || !validVertices(sample, drawableCount)) {
                    if (problem.isEmpty()) problem = QStringLiteral("参数探测顶点无效：%1").arg(parameter.id);
                    complete = false;
                    break;
                }
                int affected = 0;
                bool affectsTarget = false;
                for (int drawable = 0; drawable < drawableCount; ++drawable) {
                    if (sample[drawable].size() != baseline[drawable].size()) {
                        problem = QStringLiteral("参数探测期间网格顶点数量改变");
                        complete = false;
                        break;
                    }
                    if (moved(baseline[drawable], sample[drawable])) {
                        ++affected;
                        affectsTarget = affectsTarget || targetSet.contains(drawable);
                    }
                }
                if (!complete) break;
                if (affectsTarget) scores.append({static_cast<int>(index), affected});
                values[index] = current[index];
            }
            if (complete) {
                QVector<Score> local;
                for (const auto& score : scores) if (score.affected <= drawableCount / 2) local.append(score);
                if (local.isEmpty()) local.swap(scores);
                std::sort(local.begin(), local.end(), [](const Score& first, const Score& second) {
                    return first.affected != second.affected ? first.affected < second.affected : first.parameter < second.parameter;
                });
                const qsizetype count = std::min(qsizetype(4), local.size());
                for (qsizetype index = 0; index < count; ++index) selected.append(local[index].parameter);
            }
        }
    } catch (...) {
        complete = false;
        problem = QStringLiteral("参数探测失败");
    }
    if (evaluated) {
        QString restoreError;
        bool restored = false;
        try { restored = restore(&restoreError); }
        catch (...) { restoreError = QStringLiteral("恢复原姿态时发生异常"); }
        if (!restored) return fail(error, restoreError.isEmpty() ? QStringLiteral("无法恢复参数探测前的姿态") : restoreError);
    }
    if (!complete) return fail(error, problem.isEmpty() ? QStringLiteral("参数探测失败") : problem);
    result.swap(selected);
    return true;
}

bool CDeformerDrag::ProbeParameters(const QVector<Parameter>& parameters, const QVector<float>& current, int drawableCount,
                                   const QVector<int>& targets, const Evaluator& evaluate, const Restorer& restore,
                                   QString* error)
{
    QVector<int> selected;
    if (!Probe(parameters, current, drawableCount, targets, evaluate, restore, selected, error)) return false;
    try {
        QStringList ids;
        QList<float> minimums, maximums, defaults;
        for (int index : selected) {
            const auto& parameter = parameters[index];
            ids.append(parameter.id);
            minimums.append(parameter.minimum);
            maximums.append(parameter.maximum);
            defaults.append(parameter.defaultValue);
        }
        CDeformerDrag candidate;
        if (!candidate.SetParameters(ids, minimums, maximums, defaults, error)) return false;
        Swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法保存探测到的变形器参数"));
    }
}
