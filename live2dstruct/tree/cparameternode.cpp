#include "cparameternode.h"
namespace live2dstruct {
CParameterNode::CParameterNode(const QString& name) :C2DTreeNode(name){

    type = 0;
    m_IsShow =true;

}

CParameterNode::~CParameterNode()
{

    //qDebug("CParameterNode::~CParameterNode()");
}

void CParameterNode::InitParameterValue(const CSectionTable &table, const CParameters &para,int offset)
{
    if(offset == 0){
        m_IsShow = true;
    }else{
        m_IsShow = false;
    }
    m_Maxkeys.clear();
    m_keys.clear();
    default_values = para.default_values;
    max_values = para.max_values;
    min_values = para.min_values;
    decimal_places = para.decimal_places;
    int indices = para.parameter_binding_sources_begin_indices;
    int counts = para.parameter_binding_sources_counts;
    curValue = default_values;
    if(counts>0){
        int keyindices = table.parameter_bindings[indices+offset]->keys_sources_begin_indices;
        int keycounts = table.parameter_bindings[indices+offset]->keys_sources_counts;
        for(int j = 0 ; j < keycounts;j++){
            m_keys.push_back(table.keys->GetData(j+keyindices));
        }
        std::sort(m_keys.begin(), m_keys.end());

    }






    int parameter_types = para.parameter_types;

    if(parameter_types == 1){//该死的融合变形器
        type = parameter_types;
        int indices = para.keys_sources_begin_indices;
        int counts = para.keys_sources_counts;
        for(int i = 0 ;i < counts;i++){
            m_keys.push_back(table.keys->GetData(indices+i));
        }
        std::sort(m_keys.begin(), m_keys.end());
    }


}
}
