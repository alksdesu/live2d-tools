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
//竟然在这里循环包含了。；；；；；；；；
#include "live2dstruct/keyform/clinearinterpolation.h"
namespace live2dstruct {
class C2DTree
{
public:
    C2DTree(const CSectionTable& sectionTable);
    ~C2DTree();

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
    void DeletPart(QList<C2DTreeNode*>& noded);
    void static Test();
    std::vector<C2DTreeNode*> GetWarpKeys();
    void SetParaValue(const QString& name,float value);

private:
    //设置每个变形器的位置
    void OnUpdate();
    void GetWarpKeys(QList<C2DTreeNode*>& list ,std::vector<C2DTreeNode*>& data);
    void AddEdge(C2DTreeNode* father,C2DTreeNode* child);
    void InitDeformRoot(const CSectionTable& sectionTable);
    void SetAllCurPoint(QList<C2DTreeNode*>& list );//设置当前模型point
    void SetCurFrame(QList<C2DTreeNode*>& list);//设置当前帧
    void SetChildPoint(QList<C2DTreeNode*>& list,CCoorTypeObject* type);
    //void SetChildWarp(QList<C2DTreeNode*>& list,CWarpCoorType& type);
    int GetKeyIndex(int size);
    QList<C2DTreeNode*> deformRoot;//树结构
    QList<C2DTreeNode*> partRoot;//表示一体,一起移动,一起
    QList<C2DTreeNode*> artTree;
    QList<C2DTreeNode*> parametersTree;

};
}
#endif // C2DTREE_H
