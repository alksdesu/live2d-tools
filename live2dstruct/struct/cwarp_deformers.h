#ifndef CWARP_DEFORMERS_H
#define CWARP_DEFORMERS_H
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CWarp_deformers
{
public:
    class Offset{
    public:
        int keyform_binding_sources_indices;
        int keyform_sources_begin_indices;
        int keyform_sources_counts;
        int vertex_counts;
        int rows;
        int columns;
        int is_quad_source;

        int keyform_color_sources_begin_indices;
        Offset(CReader& bytes) {
            keyform_binding_sources_indices = bytes.readInt();
            keyform_sources_begin_indices =bytes.readInt();
            keyform_sources_counts =bytes.readInt();
            vertex_counts =bytes.readInt();
            rows = bytes.readInt();
            columns = bytes.readInt();
            //is_quad_source = bytes.readInt();
        }

        void Set_is_quad_source(CReader& bytes) {
            this->is_quad_source = bytes.readInt();
        }

        void Set_keyform_color_sources_begin_indices(CReader& bytes
                                                     ){
            this->keyform_color_sources_begin_indices = bytes.readInt();
        }
    };

    int keyform_binding_sources_indices;
    int keyform_sources_begin_indices;
    int keyform_sources_counts;
    int vertex_counts;
    int rows;
    int columns;
    bool is_quad_source;
    CWarp_deformers(CReader& bytes,const Offset& warpDeformers, int i) {
        bytes.seek(warpDeformers.keyform_binding_sources_indices + i*4);
        keyform_binding_sources_indices = bytes.readInt();

        bytes.seek(warpDeformers.keyform_sources_begin_indices + i*4);
        keyform_sources_begin_indices = bytes.readInt();

        bytes.seek(warpDeformers.keyform_sources_counts + i*4);
        keyform_sources_counts = bytes.readInt();


        bytes.seek(warpDeformers.vertex_counts + i*4);
        vertex_counts = bytes.readInt();

        bytes.seek(warpDeformers.rows + i*4);
        rows = bytes.readInt();

        bytes.seek(warpDeformers.columns + i*4);
        columns = bytes.readInt();

        bytes.seek(warpDeformers.is_quad_source + i*4);
        is_quad_source = bytes.readBoolean();


    }

};
}
#endif // CWARP_DEFORMERS_H
