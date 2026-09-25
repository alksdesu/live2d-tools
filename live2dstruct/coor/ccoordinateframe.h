#ifndef CCOORDINATEFRAME_H
#define CCOORDINATEFRAME_H

#include "ccoortypeobject.h"
#include <memory>
#include <vector>

namespace live2dstruct {

class CCoordinateFrame
{
public:
    CCoordinateFrame(std::unique_ptr<CCoorTypeObject> transform,
                     std::shared_ptr<const CCoordinateFrame> parent = {});
    ~CCoordinateFrame();

    bool MapToCanvas(const std::vector<float>& source, std::vector<float>& destination) const;
    bool MapFromCanvas(const std::vector<float>& source, std::vector<float>& destination) const;

private:
    struct Link;
    std::shared_ptr<const Link> m_link;
};

}

#endif
