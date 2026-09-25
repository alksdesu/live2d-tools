#ifndef CWARPNODE_H
#define CWARPNODE_H

#include "c2dtreenode.h"
#include "cdeformernode.h"
#include <vector>
#include "live2dstruct/keyform/cwarpnodekeyform.h"
#include "live2dstruct/csectiontable.h"

namespace live2dstruct {
class CWarpNode : public CDeformerNode
{
public:
    CWarpNode(const QString& name);
    virtual ~CWarpNode();
    void InitWarp(const CWarp_deformers& warpDeformers,const  CSectionTable& sectionTable);
    //关键帧，根据参数计算关键帧
    CWarpNodeKeyform& GetCurKeyForm(){
        return curWarp;
    }
    std::vector<CWarpNodeKeyform>& GetKeyFormS(){
        return vetWarp;
    }
    void SetCurKeyForm(CWarpNodeKeyform& key){
        curWarp = key;
    }

    int GetColumns(){
        return columns;
    }
    int GetVertex_counts(){
        return vertex_counts;
    }
    int GetRows(){
        return rows;
    }
    bool GetIs_quad_source(){
        return is_quad_source;
    }

private:
    std::vector<CWarpNodeKeyform> vetWarp;
    CWarpNodeKeyform curWarp;

    int vertex_counts = 0;
    int rows = 0;
    int columns = 0;
    bool is_quad_source = true;
};
}

#endif // CWARPNODE_H
