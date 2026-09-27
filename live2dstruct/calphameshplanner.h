#ifndef CALPHAMESHPLANNER_H
#define CALPHAMESHPLANNER_H

#include "cmocmeshstructurebuilder.h"
#include <QImage>
#include <QPointF>
#include <memory>

class CExportControl;

namespace live2dstruct {
class CAlphaMeshPlanner
{
public:
    enum class Density { Coarse, Standard, Fine };
    struct Settings
    {
        double boundarySpacing = 24;
        double interiorSpacing = 40;
        double margin = 2;
        int alphaThreshold = 0;
        bool operator==(const Settings& other) const;
    };
    struct Limits
    {
        qint64 maximumPixels = 64LL * 1024 * 1024;
        qint64 maximumWorkingBytes = 512LL * 1024 * 1024;
        qint64 maximumOperations = 256LL * 1024 * 1024;
        int maximumVertices = CMocMeshStructureBuilder::MaximumVertices;
        int maximumConstraints = 100000;
    };
    struct Input
    {
        QString artMeshId;
        QImage image;
        QVector<QPointF> pixelCoordinates;
        QVector<float> uvs;
        QVector<quint16> indices;
        QSize referenceSize;
        QVector<int> pinnedVertices;
        bool repeat = false;
        QByteArray sourceToken;
        Limits limits;
    };
    struct Contour
    {
        QVector<QPointF> points;
        bool hole = false;
    };
    struct Statistics
    {
        int oldVertices = 0;
        int newVertices = 0;
        int triangles = 0;
        int islands = 0;
        int holes = 0;
        int preservedPins = 0;
        qint64 scannedPixels = 0;
        qint64 operations = 0;
        double boundarySpacingPixels = 0;
        double interiorSpacingPixels = 0;
        double marginPixels = 0;
    };
    struct Plan
    {
        CMocMeshStructureBuilder::MeshEdit edit;
        QVector<QPointF> pixelCoordinates;
        QVector<Contour> contours;
        Statistics statistics;
        QStringList warnings;
        QByteArray sourceToken;

    private:
        struct Verification;
        std::shared_ptr<const Verification> m_verification;
        friend class CAlphaMeshPlanner;
    };
    static Settings Preset(Density density);
    static bool ValidateSettings(const Settings& settings, QString* error = nullptr);
    static bool Build(const Input& input, const Settings& settings, Plan& result, QString* error = nullptr,
        CExportControl* control = nullptr);
    static bool Validate(const Input& input, const Plan& plan, QString* error = nullptr);
};
}

#endif
