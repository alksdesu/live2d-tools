#ifndef CROTATIONNODE_H
#define CROTATIONNODE_H
#include "cdeformernode.h"
#include "live2dstruct/keyform/crotationnodekeyform.h"
#include <vector>
#include <optional>
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/struct/crotation_deformers.h"


namespace live2dstruct {
class CRotationNode : public CDeformerNode
{
public:
    CRotationNode(const QString& name);
    virtual ~CRotationNode();
    const std::optional<CCoordinateFrame::RotationBasis>& GetCanvasBasis() const { return m_canvasBasis; }
    void SetCanvasBasis(std::optional<CCoordinateFrame::RotationBasis> basis) { m_canvasBasis = std::move(basis); }
    void InitRotation(const CRotation_deformers& rdef,const  CSectionTable& sectionTable);
    std::vector<CRotationNodeKeyform>& GetKeyForms(){
        return list;
    }
    CRotationNodeKeyform& GetCurKeyForm(){
        return cur;
    }
    void SetCurKeyForm(CRotationNodeKeyform& keyForm){
        cur = keyForm;
    }

    float GetBaseAngles(){
        return base_angles;
    }

    std::vector<CRotationNodeKeyform>& GetKeyFormS(){
        return list;
    }


private:
    std::optional<CCoordinateFrame::RotationBasis> m_canvasBasis;
    std::vector<CRotationNodeKeyform> list;
    CRotationNodeKeyform cur;
    float base_angles = 0.0f;
};
}

#endif // CROTATIONNODE_H
