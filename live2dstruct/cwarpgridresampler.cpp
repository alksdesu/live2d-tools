#include "cwarpgridresampler.h"
#include "std/cexportcontrol.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool dimensions(int rows, int columns, int& count)
{
    if (rows < 1 || columns < 1 || rows > CWarpGridResampler::MaximumDimension
        || columns > CWarpGridResampler::MaximumDimension)
        return false;
    const qint64 points = (qint64(rows) + 1) * (qint64(columns) + 1);
    if (points > CWarpGridResampler::MaximumCoordinates / 2)
        return false;
    count = int(points);
    return true;
}

bool progress(CExportControl* control, qint64 current, qint64 total, QString* error)
{
    if (!control
        || control->Report(CExportControl::Phase::Preparing, current, total, QStringLiteral("正在重采样 Warp 网格")))
        return true;
    return fail(error, control->Error().isEmpty() ? QStringLiteral("Warp 网格重采样已取消") : control->Error());
}

bool canceled(CExportControl* control, QString* error)
{
    if (!control || !control->IsCancellationRequested())
        return false;
    control->Checkpoint();
    fail(error, control->Error().isEmpty() ? QStringLiteral("Warp 网格重采样已取消") : control->Error());
    return true;
}
}

struct CWarpGridResampler::Plan::Data
{
    int sourceRows = 0, sourceColumns = 0, rows = 0, columns = 0;
    int denominator = 1;
    bool quad = false, exact = false;
    QVector<int> oldToNew;
    QVector<Influence> influences;
    QVector<std::array<int, 4>> numerators;
};

int CWarpGridResampler::Plan::SourceRows() const
{
    return m_data ? m_data->sourceRows : 0;
}
int CWarpGridResampler::Plan::SourceColumns() const
{
    return m_data ? m_data->sourceColumns : 0;
}
int CWarpGridResampler::Plan::Rows() const
{
    return m_data ? m_data->rows : 0;
}
int CWarpGridResampler::Plan::Columns() const
{
    return m_data ? m_data->columns : 0;
}
bool CWarpGridResampler::Plan::Quad() const
{
    return m_data && m_data->quad;
}
bool CWarpGridResampler::Plan::IsUnchanged() const
{
    return m_data && SourceRows() == Rows() && SourceColumns() == Columns();
}
bool CWarpGridResampler::Plan::ExactInteriorSubdivision() const
{
    return m_data && m_data->exact;
}
const QVector<int>& CWarpGridResampler::Plan::OldToNew() const
{
    static const QVector<int> empty;
    return m_data ? m_data->oldToNew : empty;
}
const QVector<CWarpGridResampler::Influence>& CWarpGridResampler::Plan::Influences() const
{
    static const QVector<Influence> empty;
    return m_data ? m_data->influences : empty;
}

bool CWarpGridResampler::Prepare(int sourceRows, int sourceColumns, bool quad, int rows, int columns, Plan& result,
    QString* error, CExportControl* control)
try {
    if (error)
        error->clear();
    int sourceCount = 0, count = 0;
    if (!dimensions(sourceRows, sourceColumns, sourceCount) || !dimensions(rows, columns, count))
        return fail(error, QStringLiteral("Warp 行列必须在 1 到 1000 之间，且控制点坐标数量不能超过 1000000"));
    if (canceled(control, error))
        return false;
    auto data = std::make_shared<Plan::Data>();
    data->sourceRows = sourceRows;
    data->sourceColumns = sourceColumns;
    data->rows = rows;
    data->columns = columns;
    data->quad = quad;
    data->denominator = rows * columns;
    data->exact = rows % sourceRows == 0 && columns % sourceColumns == 0
        && (quad || rows / sourceRows == columns / sourceColumns);
    data->oldToNew.fill(-1, sourceCount);
    data->influences.reserve(count);
    data->numerators.reserve(count);
    if (!progress(control, 0, qint64(sourceCount) + count, error))
        return false;
    for (int row = 0; row <= sourceRows; ++row) {
        for (int column = 0; column <= sourceColumns; ++column) {
            const int index = row * (sourceColumns + 1) + column;
            if ((index & 4095) == 0 && !progress(control, index, qint64(sourceCount) + count, error))
                return false;
            const qint64 x = qint64(column) * columns, y = qint64(row) * rows;
            if (x % sourceColumns == 0 && y % sourceRows == 0)
                data->oldToNew[index] = int(y / sourceRows) * (columns + 1) + int(x / sourceColumns);
        }
    }
    for (int row = 0; row <= rows; ++row) {
        for (int column = 0; column <= columns; ++column) {
            const int index = row * (columns + 1) + column;
            if ((index & 4095) == 0
                && !progress(control, sourceCount + qint64(index), qint64(sourceCount) + count, error))
                return false;
            const qint64 x = qint64(column) * sourceColumns, y = qint64(row) * sourceRows;
            const int cellX = std::min(int(x / columns), sourceColumns - 1);
            const int cellY = std::min(int(y / rows), sourceRows - 1);
            const qint64 remainderX = x - qint64(cellX) * columns, remainderY = y - qint64(cellY) * rows;
            const qint64 diagonal = remainderX * rows + remainderY * columns;
            const qint64 denominator = qint64(columns) * rows;
            const int first = cellY * (sourceColumns + 1) + cellX;
            Influence influence;
            std::array<int, 4> numerators { };
            const auto add = [&](int source, qint64 numerator) {
                if (!numerator)
                    return;
                influence.indices[influence.count] = source;
                numerators[influence.count] = int(numerator);
                influence.weights[influence.count++] = double(numerator) / denominator;
            };
            if (x % columns == 0 && y % rows == 0) {
                add(int(y / rows) * (sourceColumns + 1) + int(x / columns), denominator);
            } else if (quad) {
                add(first, (columns - remainderX) * (rows - remainderY));
                add(first + 1, remainderX * (rows - remainderY));
                add(first + sourceColumns + 1, (columns - remainderX) * remainderY);
                add(first + sourceColumns + 2, remainderX * remainderY);
            } else if (diagonal < denominator) {
                add(first, denominator - diagonal);
                add(first + 1, remainderX * rows);
                add(first + sourceColumns + 1, remainderY * columns);
            } else {
                add(first + sourceColumns + 2, diagonal - denominator);
                add(first + sourceColumns + 1, (columns - remainderX) * rows);
                add(first + 1, (rows - remainderY) * columns);
            }
            data->influences.append(influence);
            data->numerators.append(numerators);
        }
    }
    if (!progress(control, qint64(sourceCount) + count, qint64(sourceCount) + count, error))
        return false;
    Plan candidate;
    candidate.m_data = std::move(data);
    result = std::move(candidate);
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法准备 Warp 网格映射，原结果保持不变"));
}

bool CWarpGridResampler::Resample(
    const Plan& plan, const QVector<float>& source, QVector<float>& result, QString* error, CExportControl* control)
try {
    if (error)
        error->clear();
    const Plan frozen = plan;
    if (!frozen.IsValid() || source.size() != qint64(frozen.OldToNew().size()) * 2)
        return fail(error, QStringLiteral("Warp 关键形坐标数量与采样映射不一致"));
    if (canceled(control, error))
        return false;
    const QVector<float> owned(source.cbegin(), source.cend());
    for (float value : owned)
        if (!std::isfinite(value))
            return fail(error, QStringLiteral("Warp 关键形包含非有限坐标"));
    const auto& influences = frozen.Influences();
    QVector<float> candidate;
    candidate.reserve(influences.size() * 2);
    if (!progress(control, 0, influences.size(), error))
        return false;
    for (qsizetype index = 0; index < influences.size(); ++index) {
        if ((index & 4095) == 0 && !progress(control, index, influences.size(), error))
            return false;
        const auto& influence = influences[index];
        for (int axis = 0; axis < 2; ++axis) {
            if (influence.count == 1 && influence.weights[0] == 1) {
                candidate.append(owned[influence.indices[0] * 2 + axis]);
                continue;
            }
            double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
            std::array<double, 4> partials { };
            int terms = 0;
            for (int term = 0; term < influence.count; ++term) {
                const double component = owned[influence.indices[term] * 2 + axis];
                double value = component * frozen.m_data->numerators[index][term];
                int written = 0;
                for (int previous = 0; previous < terms; ++previous) {
                    const double sum = value + partials[previous];
                    const double recovered = sum - value;
                    const double remainder = (value - (sum - recovered)) + (partials[previous] - recovered);
                    if (remainder != 0)
                        partials[written++] = remainder;
                    value = sum;
                }
                if (value != 0)
                    partials[written++] = value;
                terms = written;
                minimum = std::min(minimum, component);
                maximum = std::max(maximum, component);
            }
            double value = 0;
            for (int term = 0; term < terms; ++term)
                value += partials[term];
            value /= frozen.m_data->denominator;
            if (!std::isfinite(value))
                return fail(error, QStringLiteral("Warp 重采样结果超出有限范围"));
            candidate.append(float(std::clamp(value, minimum, maximum)));
        }
    }
    if (!progress(control, influences.size(), influences.size(), error))
        return false;
    result.swap(candidate);
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法重采样 Warp 关键形，原结果保持不变"));
}
}
