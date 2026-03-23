#ifndef CKEYFORM_SCREEN_COLORS_H
#define CKEYFORM_SCREEN_COLORS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CKeyform_screen_colors
{
public:
    class Offset{
    public:
        int r;
        int g;
        int b;
        Offset(CReader& bytes) {
            r = bytes.readInt();
            g = bytes.readInt();
            b = bytes.readInt();
        }

    };


    CKeyform_screen_colors();

    CKeyform_screen_colors(CReader& bytes,const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.r + i*4);
        r = bytes.readFloat();

        bytes.seek(drawOrderGroups.g + i*4);
        g = bytes.readFloat();

        bytes.seek(drawOrderGroups.b + i*4);
        b = bytes.readFloat();


    }
    float r;
    float g;
    float b;

};
}
#endif // CKEYFORM_SCREEN_COLORS_H
