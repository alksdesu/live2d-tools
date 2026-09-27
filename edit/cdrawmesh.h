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
#include <optional>
#include "opengl/cglmeshbuffer.h"



//改成指针方式
class CDrawMesh : public QOpenGLFunctions_3_3_Core
{
public:
    struct EditorState {
        int vertexCount = 0;
        QVector<int> selectedVertices;
        bool shown = false;
        bool hovered = false;
        std::optional<QVector<double>> paintedWeights;
    };
    CDrawMesh(int ebosize,const short* vetebo,int uvsize,const float* vetuv,const float* vetpos);
    ~CDrawMesh();
    void Draw(const QMatrix4x4& projection);

    void MoveSelect(const QVector2D& vet);//移动的向量
    bool SetPositions(const QVector<float>& positions);
    bool SetIndices(const QVector<quint16>& indices);
    bool SelectNearestPoint(const QVector2D& position, float radius);
    bool CanCopyEditorStateFrom(const CDrawMesh& source) const;
    bool CopyEditorStateFrom(const CDrawMesh& source);
    bool CaptureEditorState(EditorState& state, QString* error = nullptr) const;
    bool RestoreEditorState(const EditorState& state, QString* error = nullptr);
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
    bool IsShown() const { return IsDraw; }
    bool IsHovered() const { return IsOver; }
    bool ClickMesh(const QPointF& point);

    CTrianglMesh* GetMesh(){
        return mesh;
    };//三角形mesh网格
    const CTrianglMesh* GetMesh() const { return mesh; }
private:
    friend class CMeshMap;
    struct PreparedEditorState {
        std::vector<CPoint*> selected;
        bool shown = false;
        bool hovered = false;
        std::optional<QVector<double>> paintedWeights;
    };
    bool PrepareEditorState(const EditorState& state, PreparedEditorState& prepared, QString* error) const;
    void CommitEditorState(PreparedEditorState&& prepared) noexcept;
    friend class EditorOverlayRenderTests;
    bool EnsureOpenGl();
    void RefreshSelectedPoints() const;
    bool IsDraw = false;
    bool IsOver = false;
    QPointer<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLShaderProgram> shader;
    CGlMeshBuffer m_regularPoints;
    CGlMeshBuffer m_selectedPoints;
    std::vector<std::unique_ptr<CGlMeshBuffer>> m_lines;
    CTrianglMesh* mesh;//三角形mesh网格
    std::optional<QVector<double>> m_paintedWeights;
    mutable std::vector<CPoint*> m_vetSelectPoint;//保存所有被选择的点
};

#endif // CDRAWMESH_H
