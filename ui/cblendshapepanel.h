#ifndef CBLENDSHAPEPANEL_H
#define CBLENDSHAPEPANEL_H

#include <QDockWidget>
#include <QByteArray>
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
    bool BuildBlendShapesBytes(const QByteArray& bytes, CCidJson* cid, QString* error = nullptr,
                              const QByteArray& sourceDigest = {});
    void SetDocument(const live2dstruct::CModelDocument* document);
    void SyncParameterValue(const QString& paramId, float value);
    bool HasParameterGesture() const;
    const QSet<QString>& getBlendShapeParamIds() const;
    QByteArray SaveLayout() const;
    bool RestoreLayout(const QByteArray& state);

public slots:
    void RefreshParameterMetadata();
    void RefreshDeformerDisplayNames();
    void RefreshDisplayNames();
    void CancelParameterGesture();

signals:
    void ParamChanged(const QString& name, float value, bool isInit);
    void ParameterGestureStarted(const QString& id, float originalDisplayedValue);
    void ParameterGestureFinished();
    void ParameterGestureCanceled();
    void ParameterSelected(const QString& id);
    void ObjectSelected(const QString& id, const QString& type);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> CreateState();
    void ConnectState();
    void ShowParameterDetails(const QString& id);
    void ExpandDetails(QTreeWidgetItem* item);
    void ActivateItem(QTreeWidgetItem* item);
    void FilterParameters();
    void ScheduleLayout();
    void UpdateSplitterLayout();
    QString ParameterName(const QString& id) const;
    void BeginParameterGesture(QObject* slider, const QString& id);
    void EndParameterGesture(bool cancel);
    std::unique_ptr<State> m_state;
    const live2dstruct::CModelDocument* m_document = nullptr;
    bool m_rebuilding = false;
    bool m_manualSplit = false;
    bool m_applyingLayout = false;
    bool m_layoutQueued = false;
    double m_splitRatio = 0.5;
};

#endif
