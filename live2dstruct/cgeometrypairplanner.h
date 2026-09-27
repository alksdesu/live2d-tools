#ifndef CGEOMETRYPAIRPLANNER_H
#define CGEOMETRYPAIRPLANNER_H

#include "cgeometryobjectpairs.h"
#include "cmodeldocument.h"
#include <QStringList>

namespace live2dstruct {
class CGeometryPairPlanner
{
public:
    class SynchronizationContext {
    public:
        bool IsValid() const { return bool(m_state); }
    private:
        friend class CGeometryPairPlanner;
        struct State;
        std::shared_ptr<const State> m_state;
    };
    struct Result {
        CModelEdit edit;
        QMap<QString, float> sourcePose, targetPose;
        QVector<float> sourceModel, targetBeforeModel, targetModel;
        QStringList diagnostics;
        bool changed = false;
    };

    static bool PrepareMirror(const CModelDocument& document, const CGeometryObjectPairs::Pair& pair,
                              bool firstToSecond, int sourceKey, const QMap<QString, float>& base,
                              Result& result, QString* error = nullptr);
    static bool PrepareSynchronized(const CModelDocument& document, const CGeometryObjectPairs::Pair& pair,
                                    bool firstToSecond, const CModelGeometryKeyform& sourceBefore,
                                    const CModelGeometryKeyform& sourceAfter, const QMap<QString, float>& base,
                                    Result& result, QString* error = nullptr);
    static bool PrepareSynchronization(const CModelDocument& document, const CGeometryObjectPairs::Pair& pair,
                                       bool firstToSecond, int sourceKey, const QMap<QString, float>& base,
                                       SynchronizationContext& result, QString* error = nullptr);
    static bool Synchronize(const SynchronizationContext& context, const CModelGeometryKeyform& sourceAfter,
                            Result& result, QString* error = nullptr);
    static bool PrepareRotationCorrespondence(const CModelDocument& document, const CGeometryObjectPairs::Pair& input,
                                              int firstKey, int secondKey, const QMap<QString, float>& base,
                                              CGeometryObjectPairs::Pair& result, QString* error = nullptr);
};
}

#endif
