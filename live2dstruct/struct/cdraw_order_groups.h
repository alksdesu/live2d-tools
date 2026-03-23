#ifndef CDRAW_ORDER_GROUPS_H
#define CDRAW_ORDER_GROUPS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CDraw_order_groups
{
public:
    class Offset{
    public:
        int object_sources_begin_indices;
        int object_sources_counts;
        int object_sources_total_counts;
        int maximum_draw_orders;
        int minimum_draw_orders;

        Offset(CReader& bytes) {
            object_sources_begin_indices=bytes.readInt();
            object_sources_counts=bytes.readInt();
            object_sources_total_counts=bytes.readInt();
            maximum_draw_orders=bytes.readInt();
            minimum_draw_orders=bytes.readInt();
        }
    };


    int object_sources_begin_indices;//开始
    int object_sources_counts;
    int object_sources_total_counts;//总数量
    int maximum_draw_orders;//最大位置信息
    int minimum_draw_orders;//最小位置信息
    CDraw_order_groups(CReader& bytes,const Offset& drawOrderGroups, int i) {
        bytes.seek(drawOrderGroups.object_sources_begin_indices + i*4);
        object_sources_begin_indices = bytes.readInt();

        bytes.seek(drawOrderGroups.object_sources_counts + i*4);
        object_sources_counts = bytes.readInt();

        bytes.seek(drawOrderGroups.object_sources_total_counts + i*4);
        object_sources_total_counts = bytes.readInt();

        bytes.seek(drawOrderGroups.maximum_draw_orders + i*4);
        maximum_draw_orders = bytes.readInt();

        bytes.seek(drawOrderGroups.minimum_draw_orders + i*4);
        minimum_draw_orders = bytes.readInt();

    }



};
}
#endif // CDRAW_ORDER_GROUPS_H
