#ifndef CWARPCOORTYPE_H
#define CWARPCOORTYPE_H
#include "vector"
#include <QDebug>
#include <QRectF>
#include "cgridtransform.h"
#include "ccoortypeobject.h"
namespace live2dstruct {

class CWarpCoorType : public CCoorTypeObject
{
public:
    CWarpCoorType(int Col,int Row,std::vector<float>& positions,bool isQuadTransform);
    virtual ~CWarpCoorType();
    bool transformLocalToCanvas(float* src, float* des, int vetSize, int offset, int step) override;
    bool transformCanvasToLocal(float* src, float* des, int len) override;



    int GetCol();
    int GetRow();
    std::vector<float>& GetPositions();
    bool GetIsQuadTransform();
    static bool Test();
private:
    void transformLocalToCanvas_nation(float* src, float* des, int vetNum, int offset, int step, std::vector<float>& positions, int col, int row, bool IsQuad);
    int Col;
    int Row;
    std::vector<float> Positions;
    bool isQuadTransform;
};
}
#endif // CWARPCOORTYPE_H
