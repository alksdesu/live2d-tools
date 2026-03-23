#ifndef CGLUE_H
#define CGLUE_H
#include <QString>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CGlue
{
public:
    class Offset{
    public:
        int runtime_space;
        int ids;
        int keyform_binding_sources_indices;
        int keyform_sources_begin_indices;
        int keyform_sources_counts;
        int art_mesh_indices_a;
        int art_mesh_indices_b;
        int glue_info_sources_begin_indices;
        int glue_info_sources_counts;

        Offset(CReader& bytes) {
            runtime_space =bytes.readInt();
            ids=bytes.readInt();
            keyform_binding_sources_indices=bytes.readInt();
            keyform_sources_begin_indices=bytes.readInt();
            keyform_sources_counts=bytes.readInt();
            art_mesh_indices_a=bytes.readInt();
            art_mesh_indices_b=bytes.readInt();
            glue_info_sources_begin_indices=bytes.readInt();
            glue_info_sources_counts=bytes.readInt();
        }
    };


public:
    int runtime_space;
    QString ids;//名称
    int keyform_binding_sources_indices;
    int keyform_sources_begin_indices;
    int keyform_sources_counts;
    int art_mesh_indices_a;
    int art_mesh_indices_b;
    int glue_info_sources_begin_indices;
    int glue_info_sources_counts;
    CGlue(CReader& bytes,const Offset& glue, int i) {
        bytes.seek(glue.runtime_space + i*4);
        runtime_space =bytes.readInt();

        bytes.seek(glue.ids + i*0x40);
        ids =bytes.readString();

        bytes.seek(glue.keyform_binding_sources_indices + i*0x4);
        keyform_binding_sources_indices =bytes.readInt();

        bytes.seek(glue.keyform_sources_begin_indices + i*0x4);
        keyform_sources_begin_indices =bytes.readInt();

        bytes.seek(glue.keyform_sources_counts + i*0x4);
        keyform_sources_counts =bytes.readInt();

        bytes.seek(glue.art_mesh_indices_a + i*0x4);
        art_mesh_indices_a =bytes.readInt();

        bytes.seek(glue.art_mesh_indices_b + i*0x4);
        art_mesh_indices_b =bytes.readInt();

        bytes.seek(glue.glue_info_sources_begin_indices + i*0x4);
        glue_info_sources_begin_indices =bytes.readInt();

        bytes.seek(glue.glue_info_sources_counts + i*0x4);
        glue_info_sources_counts =bytes.readInt();
    }


};
}
#endif // CGLUE_H
