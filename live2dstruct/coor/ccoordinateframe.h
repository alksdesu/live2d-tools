#ifndef CCOORDINATEFRAME_H
#define CCOORDINATEFRAME_H

#include "ccoortypeobject.h"
#include <memory>
#include <QPointF>
#include <vector>

namespace live2dstruct {

class CRotationNodeKeyform;

class CCoordinateFrame
{
public:
    struct RotationBasis {
        QPointF origin;
        QPointF xAxis;
        QPointF yAxis;
    };
    struct ResolvedRotation {
        RotationBasis basis;
        double angle = 0;
        float signedScale = 1;
        double parentAngle = 0;
        float parentSignedScale = 1;
    };
    static bool ResolveRotation(float baseAngle, CRotationNodeKeyform& keyform,
                                const std::shared_ptr<const CCoordinateFrame>& parent,
                                ResolvedRotation& result);
    static bool CreateRotation(float baseAngle, CRotationNodeKeyform& keyform,
                               const std::shared_ptr<const CCoordinateFrame>& parent,
                               std::shared_ptr<const CCoordinateFrame>& result, RotationBasis& basis);
    static bool CreateWarp(int columns, int rows, const std::vector<float>& canvasPositions, bool quad,
                           const std::shared_ptr<const CCoordinateFrame>& parent,
                           std::shared_ptr<const CCoordinateFrame>& result);
    CCoordinateFrame(std::unique_ptr<CCoorTypeObject> transform,
                     std::shared_ptr<const CCoordinateFrame> parent = {});
    ~CCoordinateFrame();

    bool MapToCanvas(const std::vector<float>& source, std::vector<float>& destination) const;
    bool MapFromCanvas(const std::vector<float>& source, std::vector<float>& destination) const;

private:
    struct Link;
    std::shared_ptr<const Link> m_link;
    float m_totalScale = 1.0f;
    bool m_rotation = false;
};

}

#endif
