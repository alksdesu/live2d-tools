#ifndef CDRAWMESH_H
#define CDRAWMESH_H
#include <QMatrix4x4>
#include <QOpenGLShaderProgram>
//绘制一个一块网格点
#include <vector>
#include "ctriangl.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QTransform>
#include <QOpenGLTexture>
#include <QPointF>
#include <QPointer>
#include <memory>
#include "opengl/cglmeshbuffer.h"



//改成指针方式
class CDrawMesh : public QOpenGLFunctions_3_3_Core
{
public:
    CDrawMesh(int ebosize,const short* vetebo,int uvsize,const float* vetuv,const float* vetpos);
    ~CDrawMesh();
    void Draw(const QMatrix4x4& projection);

    void MoveSelect(const QVector2D& vet);//移动的向量
    bool SetPositions(const QVector<float>& positions);
    bool SetIndices(const QVector<quint16>& indices);
    bool SelectNearestPoint(const QVector2D& position, float radius);
    void MoveSelected(const QVector2D& delta);
    QVector<float> Positions() const;
    void SetSelect(const QRectF& rect);
    void CancelSelect();
    const std::vector<CPoint*>& GetSelectPoint() const ;//保存被选择点的指针
    void AddPoint(const QVector2D& vet);
    void SetShow(bool draw){
        IsDraw = draw;
        IsOver= false;
    }
    void SetIsOver(bool draw){
        IsOver = draw;
    }
    bool ClickMesh(const QPointF& point);

    CTrianglMesh* GetMesh(){
        return mesh;
    };//三角形mesh网格
private:
    friend class EditorOverlayRenderTests;
    bool EnsureOpenGl();
    bool IsDraw = false;
    bool IsOver = false;
    QPointer<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLShaderProgram> shader;
    CGlMeshBuffer m_regularPoints;
    CGlMeshBuffer m_selectedPoints;
    std::vector<std::unique_ptr<CGlMeshBuffer>> m_lines;
    CTrianglMesh* mesh;//三角形mesh网格
    std::vector<CPoint*> m_vetSelectPoint;//保存所有被选择的点
};

#endif // CDRAWMESH_H
