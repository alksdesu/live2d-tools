#ifndef CWARPGRIDPREVIEW_H
#define CWARPGRIDPREVIEW_H

#include <QByteArray>
#include <QLineF>
#include <QMap>
#include <QSizeF>
#include <QStringList>
#include <QVector>

class CExportControl;

class CWarpGridPreview
{
public:
    struct Limits {
        qint64 maximumMocBytes = 128 * 1024 * 1024;
        qint64 maximumMemoryBytes = 512 * 1024 * 1024;
        qsizetype maximumVertices = 2000000;
        qsizetype maximumIndices = 6000000;
        qsizetype maximumSegments = 2000000;
        qint64 maximumWork = 128 * 1024 * 1024;
        int maximumObjects = 100000;
    };
    struct Request {
        QByteArray beforeMoc;
        QByteArray afterMoc;
        QMap<QString, float> parameters;
        QMap<QString, float> partOpacities;
        Limits limits;
    };
    struct Result {
        QVector<QLineF> beforeContours;
        QVector<QLineF> afterContours;
        double maximumDisplacementPixels = 0;
        qsizetype changedVertices = 0;
        QStringList changedDrawables;
        bool closedMeshEdges = false;
        QSizeF canvasPixels;
    };
    static bool Build(const Request& request, Result& result, QString* error = nullptr,
                      CExportControl* control = nullptr);
};

#endif
