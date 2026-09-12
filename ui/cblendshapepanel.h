#ifndef CBLENDSHAPEPANEL_H
#define CBLENDSHAPEPANEL_H

#include <QDockWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QMap>

class CCidJson;
class QSlider;
class QLabel;

class CBlendShapePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CBlendShapePanel(QWidget *parent = nullptr);
    void BuildBlendShapes(const QString& mocPath, CCidJson* cid);
    void SyncParameterValue(const QString& paramId, float value);

    // 获取所有 blend shape 关联的参数 ID
    const QSet<QString>& getBlendShapeParamIds() const { return m_blendShapeParamIds; }

signals:
    void ParamChanged(const QString& name, float value, bool isInit);

private:
    QTreeWidget* m_tree;
    QSet<QString> m_blendShapeParamIds;
    QMap<QString, QSlider*> m_paramSliders;
    QMap<QString, QLabel*> m_paramValueLabels;
    QMap<QString, QPair<float, float>> m_paramRanges;
};

#endif // CBLENDSHAPEPANEL_H
