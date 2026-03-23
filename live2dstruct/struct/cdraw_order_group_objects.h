#ifndef CDRAW_ORDER_GROUP_OBJECTS_H
#define CDRAW_ORDER_GROUP_OBJECTS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CDraw_order_group_objects
{
public:
    class Offset{
    public:
        int types;
        int indices;
        int self_indices;

        Offset(CReader& bytes) {
            types = bytes.readInt();
            indices= bytes.readInt();
            self_indices= bytes.readInt();
        }
    };

    CDraw_order_group_objects(CReader& bytes,const Offset& drawOrderGroupObjects, int i) {
        bytes.seek(drawOrderGroupObjects.types + i*4);
        types = bytes.readInt();

        bytes.seek(drawOrderGroupObjects.indices + i*4);
        indices = bytes.readInt();

        bytes.seek(drawOrderGroupObjects.self_indices + i*4);
        self_indices = bytes.readInt();
    }
    int types;//图像类型
    int indices;//渲染顺序
    int self_indices;//位置
};
}

#endif // CDRAW_ORDER_GROUP_OBJECTS_H
