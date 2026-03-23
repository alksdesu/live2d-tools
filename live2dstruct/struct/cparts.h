#ifndef CPARTS_H
#define CPARTS_H
#include <QString>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CParts
{
public:
    class Offset{
    public:
        int runtime_space_0;
        int ids;
        int keyform_binding_sources_indices;
        int keyform_sources_begin_indices;
        int keyform_sources_counts;
        int is_visible;
        int is_enabled;
        int parent_part_indices;
        Offset(CReader& reader){
            runtime_space_0 =reader.readInt();
            ids=reader.readInt();
            keyform_binding_sources_indices=reader.readInt();
            keyform_sources_begin_indices=reader.readInt();
            keyform_sources_counts=reader.readInt();
            is_visible=reader.readInt();
            is_enabled=reader.readInt();
            parent_part_indices=reader.readInt();
        }
    };
    int runtime_space_0;
    QString ids;
    int keyform_binding_sources_indices;//绑定的参数位置
    int keyform_sources_begin_indices;//开始所以
    int keyform_sources_counts;//每个part绘制层级
    bool is_visible;//是否显示
    bool is_enabled;//是否能够
    int parent_part_indices;//父part
    CParts(CReader& reader,const  Offset& offset, int index){
        reader.seek(offset.ids + index*0x40);
        ids = reader.readString();
        reader.seek(offset.keyform_binding_sources_indices + index*4);
        keyform_binding_sources_indices =reader.readInt();

        reader.seek(offset.keyform_sources_begin_indices + index*4);
        keyform_sources_begin_indices =reader.readInt();

        reader.seek(offset.keyform_sources_counts + index*4);
        keyform_sources_counts =reader.readInt();

        reader.seek(offset.is_visible + index*4);
        is_visible =reader.readBoolean();

        reader.seek(offset.is_enabled + index*4);
        is_enabled = reader.readBoolean();

        reader.seek(offset.parent_part_indices + index*4);
        parent_part_indices = reader.readInt();
    }


};
}
#endif // CPARTS_H
