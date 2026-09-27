#ifndef CMOCGEOMETRYPOOL_H
#define CMOCGEOMETRYPOOL_H

#include <QMap>
#include <QString>
#include <QVector>
#include <algorithm>
#include <utility>

namespace live2dstruct::detail {
struct CMocGeometryAllocationFailure
{
    QString message;
};

class CMocGeometryPool
{
public:
    CMocGeometryPool() = default;
    CMocGeometryPool(int count, QString label)
        : highWater(count)
        , label(std::move(label))
    {
        if (count < 0 || count > MaximumCount)
            throw CMocGeometryAllocationFailure { QStringLiteral("网格结构的 %1 数量超过 1000000").arg(this->label) };
    }
    void protect(int first, qint64 count)
    {
        if (!count) {
            if (first < -1 || first > highWater)
                throw CMocGeometryAllocationFailure { QStringLiteral("网格结构的 %1 空引用范围无效").arg(label) };
            return;
        }
        if (first < 0 || count < 0 || first > highWater || count > highWater - first)
            throw CMocGeometryAllocationFailure { QStringLiteral("网格结构的 %1 存活引用越界").arg(label) };
        occupied.append({ first, first + int(count) });
    }
    void prepare()
    {
        std::sort(occupied.begin(), occupied.end());
        int end = 0;
        for (const auto& interval : occupied) {
            if (interval.first > end)
                free.insert(end, interval.first);
            end = std::max(end, interval.second);
        }
        if (end < MaximumCount)
            free.insert(end, MaximumCount);
        occupied.clear();
    }
    int allocate(int count, qint64& work)
    {
        if (count < 0 || count > MaximumCount)
            throw CMocGeometryAllocationFailure { QStringLiteral("网格结构的 %1 数量超过 1000000").arg(label) };
        if (!count)
            return 0;
        for (auto it = free.begin(); it != free.end(); ++it) {
            if (++work > 16000000)
                throw CMocGeometryAllocationFailure { QStringLiteral("网格结构空闲区间分配超过工作量限制") };
            if (count > it.value() - it.key())
                continue;
            const int first = it.key(), end = it.value();
            free.erase(it);
            if (first + count < end)
                free.insert(first + count, end);
            highWater = std::max(highWater, first + count);
            return first;
        }
        throw CMocGeometryAllocationFailure {
            QStringLiteral("网格结构的 %1 没有足够连续空间，池数量限制为 1000000").arg(label)
        };
    }
    int size() const { return highWater; }

private:
    static constexpr int MaximumCount = 1000000;
    int highWater = 0;
    QString label;
    QVector<QPair<int, int>> occupied;
    QMap<int, int> free;
};
}

#endif
