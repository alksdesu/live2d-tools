#ifndef CMASKEDITPOLICY_H
#define CMASKEDITPOLICY_H

#include <QHash>
#include <QString>
#include <QVector>
#include <algorithm>

namespace live2dstruct {
class CMaskEditPolicy
{
public:
    static constexpr int MaximumReferences = 1000000;
    static constexpr int MaximumOverrideReferences = 10000;

    static bool Validate(const QVector<int>& baseline, const QVector<int>& requested,
                         int ownerIndex, int drawableCount, QString* error = nullptr)
    {
        if (error) error->clear();
        if (ownerIndex < 0 || ownerIndex >= drawableCount || baseline.size() > MaximumReferences ||
            requested.size() > MaximumReferences) {
            if (error) *error = QStringLiteral("蒙版目标或引用数量超出范围");
            return false;
        }
        QHash<int, int> available, used;
        for (int index : baseline) ++available[index];
        for (int index : requested) {
            if (index < 0 || index >= drawableCount || index == ownerIndex ||
                ++used[index] > std::max(1, available.value(index))) {
                if (error) *error = QStringLiteral("蒙版索引越界、自引用或新增重复次数");
                return false;
            }
        }
        return true;
    }
};
}

#endif
