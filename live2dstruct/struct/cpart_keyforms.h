#ifndef CPART_KEYFORMS_H
#define CPART_KEYFORMS_H
#include "live2dstruct/util/cfilereader.h"


namespace live2dstruct {
class CPart_keyforms
{
public:
    class Offset{
    public:
        int draw_orders;

        Offset(CReader& bytes) {
            draw_orders = bytes.readInt();
        }
    };
    float draw_orders;
    CPart_keyforms(CReader& bytes,const Offset& partKeyforms, int i) {
        bytes.seek(partKeyforms.draw_orders + i*4);
        draw_orders = bytes.readFloat();
    }

};
}
#endif // CPART_KEYFORMS_H
