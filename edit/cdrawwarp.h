#ifndef CDRAWWARP_H
#define CDRAWWARP_H
#include "live2dstruct/keyform/cwarpnodekeyform.h"
#include <vector>
#include <QMatrix4x4>
#include <QVector2D>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <live2dstruct/tree/cwarpnode.h>
#include "cdrawobject.h"


using namespace live2dstruct;
class CDrawWarp : public CDrawObject
{
public:
    CDrawWarp();
    virtual ~CDrawWarp();
    void InitTest(int i,CWarpNode* node);
    void Init(CWarpNode* node);
    void UpdatePositions(CWarpNode* node);
    void SetColor(QVector3D color){
        this->color = color;
    }
    virtual void Draw(const QMatrix4x4 &mv);
    virtual void InitOpenGl();
    virtual void SetDraw(bool flag);

private:
    int rows;//5 ， 5
    int columns;//一行一行排列
    CWarpNodeKeyform curWarp;//画点是直接画的，
    std::vector<short> vetIndex;
    void ClaVetIndex();
    bool m_IsDraw;
    QOpenGLShaderProgram* shader;
    QVector3D color;
};

#endif // CDRAWWARP_H
