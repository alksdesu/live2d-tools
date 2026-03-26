#ifndef CBLENDSHAPEPANEL_H
#define CBLENDSHAPEPANEL_H

#include <QDockWidget>
#include <QTableWidget>
#include <QTreeWidget>

class CCidJson;

class CBlendShapePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CBlendShapePanel(QWidget *parent = nullptr);
    void BuildBlendShapes(const QString& mocPath, CCidJson* cid);

    // 获取所有 blend shape 关联的参数 ID
    const QSet<QString>& getBlendShapeParamIds() const { return m_blendShapeParamIds; }

signals:
    void ParamChanged(const QString& name, float value, bool isInit);

private:
    QTreeWidget* m_tree;
    QSet<QString> m_blendShapeParamIds;
};

#endif // CBLENDSHAPEPANEL_H
