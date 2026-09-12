#ifndef CDRAWORDER_H
#define CDRAWORDER_H

#include <QHash>
#include <QString>
#include <QVector>
#include "cmodeldocument.h"

namespace live2dstruct {

struct CDrawableOrder
{
    QString id;
    int drawOrder = 0;
    int renderOrder = 0;
};

QVector<int> ResolveDrawableOrder(const QVector<CDrawableOrder>& drawables,
                                  const QHash<QString, int>& overrides = {});

struct CLayerOrderItem
{
    enum Type { Layer, GroupStart, GroupEnd };
    Type type = Layer;
    QString id;
    int drawableIndex = -1;
};

QVector<CLayerOrderItem> BuildGroupedLayerOrder(const QVector<CModelPart>& parts,
                                              const QVector<CModelDrawable>& drawables,
                                              const QVector<int>& sortedIndices);

}

#endif
