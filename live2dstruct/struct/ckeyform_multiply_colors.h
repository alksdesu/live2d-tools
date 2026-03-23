#ifndef CKEYFORM_MULTIPLY_COLORS_H
#define CKEYFORM_MULTIPLY_COLORS_H
#include "live2dstruct/util/cfilereader.h"

namespace live2dstruct {
class CKeyform_multiply_colors
{
public:

    class Offset{
    public:
        int r;
        int g;
        int b;
        Offset(CReader& bytes) {
            r = bytes.readInt();//占8个字节
            g = bytes.readInt();
            b = bytes.readInt();
        }


    };

    CKeyform_multiply_colors();
    float r;
    float g;
    float b;

    CKeyform_multiply_colors(CReader& bytes, Offset drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.r + i*4);
        r = bytes.readFloat();

        bytes.seek(drawOrderGroups.g + i*4);
        g = bytes.readFloat();

        bytes.seek(drawOrderGroups.b + i*4);
        b = bytes.readFloat();


    }



};
}
#endif // CKEYFORM_MULTIPLY_COLORS_H
