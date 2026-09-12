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

    bool IsEbo = true;
    unsigned int m_VAO = 0;
    unsigned int m_EBO = 0;
    QVector<unsigned int> m_vboIdList;

    QOpenGLTexture* textureMaskFrame = nullptr;
};

#endif // CMESH_H
