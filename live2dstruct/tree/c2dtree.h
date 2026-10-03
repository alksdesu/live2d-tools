#ifndef C2DTREE_H
#define C2DTREE_H

#include <QList>
#include "c2dtreenode.h"
#include "live2dstruct/csectiontable.h"
#include "cparameternode.h"
#include "crotationnode.h"
#include "cwarpnode.h"

#include "cartmeshnode.h"
#include "cpartsnode.h"
#include <QDebug>
#include "live2dstruct/coor/cwarpcoortype.h"
#include "live2dstruct/coor/crotationcoortype.h"
#include "live2dstruct/cblendshapeposeevaluator.h"
namespace live2dstruct {
class C2DTree
{
public:
    C2DTree(const CSectionTable& sectionTable);
    ~C2DTree();
    C2DTree(const C2DTree&) = delete;
    C2DTree& operator=(const C2DTree&) = delete;

    QList<C2DTreeNode*>& getArtTree() {
        return artTree;
    }
    QList<C2DTreeNode*>& getParametersTree() {
        return parametersTree;
    }
    const QList<C2DTreeNode*>& getParametersTree() const{
        return parametersTree;
    }

    QList<C2DTreeNode*>& getDeformRoot() {
        return deformRoot;
    }

    const QList<C2DTreeNode*>& getDeformRoot() const {
        return deformRoot;
    }


    QList<C2DTreeNode*>& getPartRoot() {
        return partRoot;
    }
    const QList<C2DTreeNode*>& getPartRoot()const {
        return partRoot;
    }
    void static Test();
    std::vector<C2DTreeNode*> GetWarpKeys();
    void SetParaValue(const QString& name,float value);
    void SetPoseOverrides(const CBlendShapePoseEvaluator::Overrides& overrides) { m_poseOverrides = overrides; }

private:
    void OnUpdate();
    void AddEdge(C2DTreeNode* father,C2DTreeNode* child);
    void InitDeformRoot(const CSectionTable& sectionTable);
    template <typename T> T* Own(T* node)
    {
        std::unique_ptr<T> owner(node);
        m_nodes.push_back(std::move(owner));
        return node;
    }
    std::vector<std::unique_ptr<C2DTreeNode>> m_nodes;
    CBlendShapePoseEvaluator::Overrides m_poseOverrides;
    std::vector<C2DTreeNode*> m_displayDeformers;
    QList<C2DTreeNode*> deformRoot;//树结构
    QList<C2DTreeNode*> partRoot;//表示一体,一起移动,一起
    QList<C2DTreeNode*> artTree;
    QList<C2DTreeNode*> parametersTree;

};
}
#endif // C2DTREE_H
