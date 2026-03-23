#ifndef CART_MESHES_H
#define CART_MESHES_H
#include <QString>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CArt_meshes
{
public:
    class Offset{
    public:
        int runtime_space_0;
        int runtime_space_1;
        int runtime_space_2;
        int runtime_space_3;
        int ids;
        int keyform_binding_sources_indices;
        int keyform_sources_begin_indices;
        int keyform_sources_counts;
        int is_visible;
        int is_enabled;
        int parent_part_indices;
        int parent_deformer_indices;
        int texture_nos;
        int drawable_flags;
        int vertex_counts;
        int uv_sources_begin_indices;
        int position_index_sources_begin_indices;
        int position_index_sources_counts;
        int drawable_mask_sources_begin_indices;
        int drawable_mask_sources_counts;

        Offset(CReader& bytes) {
            runtime_space_0 = bytes.readInt();
            runtime_space_1= bytes.readInt();
            runtime_space_2= bytes.readInt();
            runtime_space_3= bytes.readInt();
            ids= bytes.readInt();
            keyform_binding_sources_indices= bytes.readInt();
            keyform_sources_begin_indices= bytes.readInt();
            keyform_sources_counts= bytes.readInt();
            is_visible= bytes.readInt();
            is_enabled= bytes.readInt();
            parent_part_indices= bytes.readInt();
            parent_deformer_indices= bytes.readInt();
            texture_nos= bytes.readInt();
            drawable_flags= bytes.readInt();
            vertex_counts= bytes.readInt();
            uv_sources_begin_indices= bytes.readInt();
            position_index_sources_begin_indices= bytes.readInt();
            position_index_sources_counts= bytes.readInt();
            drawable_mask_sources_begin_indices= bytes.readInt();
            drawable_mask_sources_counts= bytes.readInt();
        }
        int keyform_color_sources_begin_indices;
        void Set_keyform_color_sources_begin_indices(CReader& bytes) {
            keyform_color_sources_begin_indices = bytes.readInt();

        }
    };

public:

    CArt_meshes(CReader& bytes,const Offset& artMeshes, int i) {
        bytes.seek(artMeshes.runtime_space_0 + i*0x4);
        runtime_space_0 = bytes.readInt();

        bytes.seek(artMeshes.runtime_space_1 + i*0x4);
        runtime_space_1 = bytes.readInt();

        bytes.seek(artMeshes.runtime_space_2 + i*0x4);
        runtime_space_2 = bytes.readInt();

        bytes.seek(artMeshes.runtime_space_3 + i*0x4);
        runtime_space_3 = bytes.readInt();

        bytes.seek(artMeshes.ids + i*0x40);
        ids = bytes.readString();

        bytes.seek(artMeshes.keyform_binding_sources_indices + i*0x4);
        keyform_binding_sources_indices = bytes.readInt();

        bytes.seek(artMeshes.keyform_sources_begin_indices + i*0x4);
        keyform_sources_begin_indices = bytes.readInt();

        bytes.seek(artMeshes.keyform_sources_counts + i*0x4);
        keyform_sources_counts = bytes.readInt();

        bytes.seek(artMeshes.is_visible + i*0x4);
        is_visible = bytes.readBoolean();

        bytes.seek(artMeshes.is_enabled + i*0x4);
        is_enabled = bytes.readBoolean();

        bytes.seek(artMeshes.parent_part_indices + i*0x4);
        parent_part_indices = bytes.readInt();


        bytes.seek(artMeshes.parent_deformer_indices + i*0x4);
        parent_deformer_indices = bytes.readInt();

        bytes.seek(artMeshes.texture_nos + i*0x4);
        texture_nos = bytes.readInt();

        bytes.seek(artMeshes.drawable_flags + i*0x4);
        drawable_flags = bytes.readInt();

        bytes.seek(artMeshes.vertex_counts + i*0x4);
        vertex_counts = bytes.readInt();

        bytes.seek(artMeshes.uv_sources_begin_indices + i*0x4);
        uv_sources_begin_indices = bytes.readInt();

        bytes.seek(artMeshes.position_index_sources_begin_indices + i*0x4);
        position_index_sources_begin_indices = bytes.readInt();

        bytes.seek(artMeshes.position_index_sources_counts + i*0x4);
        position_index_sources_counts = bytes.readInt();

        bytes.seek(artMeshes.drawable_mask_sources_begin_indices + i*0x4);
        drawable_mask_sources_begin_indices = bytes.readInt();


        bytes.seek(artMeshes.drawable_mask_sources_counts + i*0x4);
        drawable_mask_sources_counts = bytes.readInt();
    }

    int runtime_space_0;
    int runtime_space_1;
    int runtime_space_2;
    int runtime_space_3;
    QString ids;//名称
    int keyform_binding_sources_indices;//未知
    int keyform_sources_begin_indices;//Art_mesh_keyforms 表索引
    int keyform_sources_counts;//Art_mesh_keyforms  数量
    bool is_visible;//是否显示
    bool is_enabled;//位置
    int parent_part_indices;//Parts 表索引
    int parent_deformer_indices;//Deformers 表索引
    int texture_nos;//texture_00 或者  texture_01等图
    int drawable_flags;//画的标志信息
    int vertex_counts;//Uvs 表中顶点数量
    int uv_sources_begin_indices;//Uvs 表 开始索引
    int position_index_sources_begin_indices;//Position_indices  表索引位置
    int position_index_sources_counts;//Position_indices  表索引数量
    int drawable_mask_sources_begin_indices;//Drawable_masks  表开始位置
    int drawable_mask_sources_counts;//Drawable_masks 表数量





};

}


#endif // CART_MESHES_H
