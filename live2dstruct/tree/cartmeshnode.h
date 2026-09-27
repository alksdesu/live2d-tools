#ifndef CARTMESHNODE_H
#define CARTMESHNODE_H
#include <QString>
#include "c2dtreenode.h"
#include <vector>
#include "live2dstruct/keyform/cartmeshkeyform.h"
#include "live2dstruct/csectiontable.h"

namespace live2dstruct {
class CArtMeshNode:public C2DTreeNode
{
public:
    CArtMeshNode(const QString& name);
    virtual ~CArtMeshNode();
    void InitArtMesh(CArt_meshes& art_meshes ,const  CSectionTable& sectionTable);
    CArtMeshKeyForm& GetCurKeyForm(){
        return curKey;
    }

    void SetCurMeshKey(CArtMeshKeyForm& key){
        curKey = key;
    }
    std::vector<CArtMeshKeyForm>& GetKeyFormS(){
        return meshKeys;
    }
private:
    CArtMeshKeyForm curKey;
    //mesh关键帧
    std::vector<CArtMeshKeyForm> meshKeys;//引用
    bool is_visible;
    bool is_enabled;
    int texture_nos;
    int drawable_flags;
    int vertex_counts;
    std::vector<float>  uv;//纹理坐标位置
    std::vector<quint16>  position;//构成的三角形顶点
};
}
#endif // CARTMESHNODE_H
