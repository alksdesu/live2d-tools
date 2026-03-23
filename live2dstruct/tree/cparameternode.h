#ifndef CPARAMETERNODE_H
#define CPARAMETERNODE_H
#include "c2dtreenode.h"
#include <QString>
#include <vector>
#include "live2dstruct/tree/c2dtreenode.h"
#include "live2dstruct/csectiontable.h"
namespace live2dstruct {
class CParameterNode : public C2DTreeNode
{
public:

    CParameterNode(const QString& name);
    virtual ~CParameterNode();
    const float GetMaxValue() const{
        return max_values;
    }
    const float GetMinValue() const {
        return min_values;
    }
    const float GetDefaultValue() const{
        return default_values;
    }
    const std::vector<float>& GetKeys() const{
        return m_keys;
    }
    void InitParameterValue(const CSectionTable& table,const CParameters& para,int offset);


    const float GetCurValue() const{
        return curValue;

    }
    void SetCurValue(float value) {
        curValue = value;
        for(int i = 0 ;i < vetNext.size();i++){
            vetNext[i]->curValue = value;
        }
    }
    int GetType(){
        return type;
    }
    bool GetShow(){
        return m_IsShow;
    }
    void SetShow(bool show){
        m_IsShow = show;
    }
    const std::vector<float>&  GetMaxKey(){
        if(m_Maxkeys.empty()){
            Copy(m_Maxkeys,m_keys);
            for(int i = 0 ;i < vetNext.size();i++){
                Copy(m_Maxkeys,vetNext[i]->GetKeys());
            }
        }
        return m_Maxkeys;
    }
    void AddParaNext(CParameterNode* next){
        vetNext.push_back(next);
    }
    void Copy(std::vector<float>& src,const std::vector<float>& des){
        for(int i = 0 ;i < des.size();i++){
            src.push_back(des[i]);
        }
    }
    int GetManyPara(){
        return vetNext.size();
    }
private:
    bool m_IsShow;
    std::vector<float> m_Maxkeys;//参数值
    std::vector<CParameterNode*> vetNext;
    std::vector<float> m_keys;//参数值
    float max_values;
    float min_values;
    float default_values;
    int decimal_places;//小数点后几位
    float curValue;
    float type;
};
}

#endif // CPARAMETERNODE_H
