#ifndef CPARAMETERS_H
#define CPARAMETERS_H
#include <QString>
#include "live2dstruct/util/cfilereader.h"
namespace live2dstruct {
class CParameters
{
public:
    class Offset{
    public:
        int runtime_space_0;
        int ids;
        int max_values;
        int min_values;
        int default_values;
        int is_repeat;
        int decimal_places;
        int parameter_binding_sources_begin_indices;
        int parameter_binding_sources_counts;

        Offset(CReader& bytes) {
            runtime_space_0 = bytes.readInt();
            ids= bytes.readInt();;
            max_values= bytes.readInt();;
            min_values= bytes.readInt();;
            default_values= bytes.readInt();;
            is_repeat= bytes.readInt();;
            decimal_places= bytes.readInt();;
            parameter_binding_sources_begin_indices= bytes.readInt();;
            parameter_binding_sources_counts= bytes.readInt();;
        }


        int parameter_types = 0;
        int extension_runtime_space_0 = 0;
        int keys_sources_begin_indices = 0;
        int keys_sources_counts = 0;
        int blend_shape_parameter_binding_sources_begin_indices = 0;
        int blend_shape_parameter_binding_sources_counts = 0;

        void InitParaExe(CReader& bytes){
            //新版本添加的参数
            parameter_types = bytes.readInt();
            blend_shape_parameter_binding_sources_begin_indices = bytes.readInt();
            blend_shape_parameter_binding_sources_counts = bytes.readInt();
        }
        void InitKeyForm(CReader& bytes){
            extension_runtime_space_0 = bytes.readInt();
            keys_sources_begin_indices = bytes.readInt();
            keys_sources_counts = bytes.readInt();
        }
    };


    int runtime_space_0;
    QString ids;
    float max_values;
    float min_values;
    float default_values;
    bool is_repeat;
    int decimal_places;
    int parameter_binding_sources_begin_indices;
    int parameter_binding_sources_counts;

    int parameter_types = 0;
    int extension_runtime_space_0 = 0;
    int keys_sources_begin_indices = 0;
    int keys_sources_counts = 0;
    int blend_shape_parameter_binding_sources_begin_indices = 0;
    int blend_shape_parameter_binding_sources_counts = 0;


    CParameters(CReader& bytes,const Offset& parameters, int i) {
        bytes.seek(parameters.runtime_space_0 + i*0x4);
        runtime_space_0 = bytes.readInt();

        bytes.seek(parameters.ids + i*0x40);
        ids = bytes.readString();

        bytes.seek(parameters.max_values + i*0x4);
        max_values = bytes.readFloat();
        bytes.seek(parameters.min_values + i*0x4);
        min_values = bytes.readFloat();
        bytes.seek(parameters.default_values + i*0x4);
        default_values = bytes.readFloat();

        bytes.seek(parameters.is_repeat + i*0x4);
        is_repeat = bytes.readBoolean();

        bytes.seek(parameters.decimal_places + i*0x4);
        decimal_places = bytes.readInt();

        bytes.seek(parameters.parameter_binding_sources_begin_indices + i*0x4);
        parameter_binding_sources_begin_indices = bytes.readInt();

        bytes.seek(parameters.parameter_binding_sources_counts + i*0x4);
        parameter_binding_sources_counts = bytes.readInt();




        const auto readExtension = [&bytes, i](int offset) {
            if (offset == 0) return 0;
            bytes.seek(offset + i * 4);
            return bytes.readInt();
        };
        parameter_types = readExtension(parameters.parameter_types);
        extension_runtime_space_0 = readExtension(parameters.extension_runtime_space_0);
        keys_sources_begin_indices = readExtension(parameters.keys_sources_begin_indices);
        keys_sources_counts = readExtension(parameters.keys_sources_counts);
        blend_shape_parameter_binding_sources_begin_indices = readExtension(parameters.blend_shape_parameter_binding_sources_begin_indices);
        blend_shape_parameter_binding_sources_counts = readExtension(parameters.blend_shape_parameter_binding_sources_counts);


    }




};
}
#endif // CPARAMETERS_H
