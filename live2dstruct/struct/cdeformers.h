#ifndef CDEFORMERS_H
#define CDEFORMERS_H
#include <QString>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CDeformers
{
public:

    class Offset{
    public:
        int runtime_space_0;
        int ids;
        int keyform_binding_sources_indices;
        int is_visible;
        int is_enabled;
        int parent_part_indices;
        int parent_deformer_indices;
        int types;
        int specific_sources_indices;//

        Offset(CReader& bytes) {
            runtime_space_0 =bytes.readInt();
            ids=bytes.readInt();
            keyform_binding_sources_indices=bytes.readInt();
            is_visible=bytes.readInt();
            is_enabled=bytes.readInt();
            parent_part_indices=bytes.readInt();
            parent_deformer_indices=bytes.readInt();
            types=bytes.readInt();
            specific_sources_indices=bytes.readInt();
        }
    };


    int runtime_space_0;
    QString ids;//名称
    int keyform_binding_sources_indices;//未知
    bool is_visible;//是否显示
    bool is_enabled;//是否能够
    int parent_part_indices;//Parts 表索引
    int parent_deformer_indices;//Deformers 表索引
    int types;//决定是Warp 或者 Rotation
    int specific_sources_indices;//未知
    CDeformers(CReader& bytes,const  Offset& deformers, int i) {
        bytes.seek(deformers.runtime_space_0+ i*0x4);
        runtime_space_0 = bytes.readInt();

        bytes.seek(deformers.ids + i*0x40);
        ids = bytes.readString();

        bytes.seek(deformers.keyform_binding_sources_indices+ i*0x4);
        keyform_binding_sources_indices = bytes.readInt();

        bytes.seek(deformers.is_visible+ i*0x4);
        is_visible = bytes.readBoolean();

        bytes.seek(deformers.is_enabled+ i*0x4);
        is_enabled = bytes.readBoolean();

        bytes.seek(deformers.parent_part_indices+ i*0x4);
        parent_part_indices = bytes.readInt();

        bytes.seek(deformers.parent_deformer_indices+ i*0x4);
        parent_deformer_indices = bytes.readInt();

        bytes.seek(deformers.types+ i*0x4);
        types = bytes.readInt();

        bytes.seek(deformers.specific_sources_indices+ i*0x4);
        specific_sources_indices = bytes.readInt();
    }


};
}

#endif // CDEFORMERS_H
