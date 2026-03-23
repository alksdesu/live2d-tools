#ifndef CMESH_H
#define CMESH_H
#include <QVector>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLTexture>
#include <QDebug>
class CMesh : public QOpenGLFunctions_3_3_Core
{
public:
    CMesh();
    ~CMesh();
    void setGlOwningContext(const QOpenGLContext *context);


    void SetConstPointEBO(float *uvVertex, float *positionVertex);
    void SetConstPoint(float* uvVertex,float* positionVertex,unsigned int* indices);
    void SetImage(const QString& name);
    void Draw();
    const unsigned int getVAO() const{
        return m_VAO;
    }
private:

    bool IsEbo;
    unsigned int m_VAO;
    unsigned int m_EBO;
    QVector<unsigned int> m_vboIdList;

    QOpenGLTexture* textureMaskFrame;
};

#endif // CMESH_H
