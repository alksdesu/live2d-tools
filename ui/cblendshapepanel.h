#ifndef CBLENDSHAPEPANEL_H
#define CBLENDSHAPEPANEL_H

#include <QDockWidget>
#include <QSet>
#include <memory>

class CCidJson;
class QTreeWidgetItem;
namespace live2dstruct { class CModelDocument; }

class CBlendShapePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CBlendShapePanel(QWidget* parent = nullptr);
    ~CBlendShapePanel() override;
    void BuildBlendShapes(const QString& mocPath, CCidJson* cid);
    bool BuildBlendShapesBytes(const QByteArray& bytes, CCidJson* cid, QString* error = nullptr);
    void SetDocument(const live2dstruct::CModelDocument* document);
    void SyncParameterValue(const QString& paramId, float value);
    const QSet<QString>& getBlendShapeParamIds() const;

public slots:
    void RefreshParameterMetadata();

signals:
    void ParamChanged(const QString& name, float value, bool isInit);
    void ParameterSelected(const QString& id);
    void ObjectSelected(const QString& id, const QString& type);

private:
    struct State;
    std::unique_ptr<State> CreateState();
    void ConnectState();
    void ShowParameterDetails(const QString& id);
    void ExpandDetails(QTreeWidgetItem* item);
    void ActivateItem(QTreeWidgetItem* item);
    void FilterParameters();
    QString ParameterName(const QString& id) const;
    std::unique_ptr<State> m_state;
    const live2dstruct::CModelDocument* m_document = nullptr;
    bool m_rebuilding = false;
};

#endif
