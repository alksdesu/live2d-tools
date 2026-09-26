#ifndef CBLEND_SHAPE_TARGETS_H
#define CBLEND_SHAPE_TARGETS_H

#include "live2dstruct/util/cfilereader.h"

namespace live2dstruct {
class CBlend_shape_targets
{
public:
    struct Offset
    {
        int target_indices = 0;
        int blend_shape_keyform_binding_sources_begin_indices = 0;
        int blend_shape_keyform_binding_sources_counts = 0;

        Offset() = default;
        explicit Offset(CReader& bytes)
        {
            target_indices = bytes.readInt();
            blend_shape_keyform_binding_sources_begin_indices = bytes.readInt();
            blend_shape_keyform_binding_sources_counts = bytes.readInt();
        }
    };

    CBlend_shape_targets(CReader& bytes, const Offset& offsets, int index)
    {
        bytes.seek(offsets.target_indices + index * 4);
        target_indices = bytes.readInt();
        bytes.seek(offsets.blend_shape_keyform_binding_sources_begin_indices + index * 4);
        blend_shape_keyform_binding_sources_begin_indices = bytes.readInt();
        bytes.seek(offsets.blend_shape_keyform_binding_sources_counts + index * 4);
        blend_shape_keyform_binding_sources_counts = bytes.readInt();
    }

    int target_indices;
    int blend_shape_keyform_binding_sources_begin_indices;
    int blend_shape_keyform_binding_sources_counts;
};
}

#endif
