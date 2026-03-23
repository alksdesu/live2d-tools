#ifndef CCANVAS_INFO_H
#define CCANVAS_INFO_H
#include <cstdint>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CCanvas_info
{

public:
    class Offset{
    public:
        int CanvasOffset;

        Offset(CReader& bytes) {
            CanvasOffset = bytes.readInt();
        }
    };
    CCanvas_info(CReader& bytes,const Offset& canvasInfo) {
        bytes.seek(canvasInfo.CanvasOffset);
        pixels_per_unit =bytes.readFloat();
        origin_x=bytes.readFloat();
        origin_y=bytes.readFloat();
        width=bytes.readFloat();
        height=bytes.readFloat();

    }
    float pixels_per_unit;
    float origin_x;//仿射变换矩阵位置信息
    float origin_y;
    float width;
    float height;
    bool reverse_y_coordinate;
    uint8_t* padding;


};
}
#endif // CCANVAS_INFO_H
