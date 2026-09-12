#ifndef CMODELDOCUMENT_H
#define CMODELDOCUMENT_H

#include <QSize>
#include <QString>
#include <QVector>

namespace live2dstruct {

struct CModelDiagnostic
{
    enum Severity { Warning, Error };
    Severity severity = Error;
    QString code;
    QString message;
    int index = -1;
};

struct CModelParameter
{
    QString id;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    int decimalPlaces = 0;
    bool repeat = false;
    int type = 0;
};

struct CModelKeyform
{
    QVector<float> positions;
    float opacity = 1.0f;
    float drawOrder = 0.0f;
};

struct CModelPart
{
    QString id;
    int parentIndex = -1;
    bool visible = true;
    bool enabled = true;
};

struct CModelDeformer
{
    enum Type { Warp, Rotation };
    QString id;
    Type type = Warp;
    int parentPartIndex = -1;
    int parentDeformerIndex = -1;
    bool visible = true;
    bool enabled = true;
    int rows = 0;
    int columns = 0;
};

struct CModelDrawable
{
    QString id;
    int parentPartIndex = -1;
    int parentDeformerIndex = -1;
    int textureIndex = -1;
    int flags = 0;
    int blendMode = 0;
    bool culling = true;
    int vertexCount = 0;
    bool visible = true;
    bool enabled = true;
    float opacity = 1.0f;
    float drawOrder = 0.0f;
    QVector<float> uvs;
    QVector<quint16> indices;
    QVector<int> maskIndices;
    QVector<CModelKeyform> keyforms;
};

class CModelDocument
{
public:
    bool Load(const QString& mocPath, QString* error = nullptr);
    void Clear();

    bool IsLoaded() const { return m_loaded; }
    const QString& SourcePath() const { return m_sourcePath; }
    const QSize& CanvasPixels() const { return m_canvasPixels; }
    const QVector<CModelParameter>& Parameters() const { return m_parameters; }
    const QVector<CModelPart>& Parts() const { return m_parts; }
    const QVector<CModelDeformer>& Deformers() const { return m_deformers; }
    const QVector<CModelDrawable>& Drawables() const { return m_drawables; }

    QVector<CModelDiagnostic> Validate() const;

private:
    bool m_loaded = false;
    QString m_sourcePath;
    QSize m_canvasPixels;
    QVector<CModelParameter> m_parameters;
    QVector<CModelPart> m_parts;
    QVector<CModelDeformer> m_deformers;
    QVector<CModelDrawable> m_drawables;
};

}

#endif
