#ifndef CMODELTREEPANEL_H
#define CMODELTREEPANEL_H

#include <QDockWidget>
#include <QMap>
#include <QModelIndex>
#include <QStringList>

class CCidJson;
class QTabWidget;
class QLineEdit;
class QPushButton;
class CModelHierarchyModel;
class CModelHierarchyView;

class CModelTreePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CModelTreePanel(QWidget* parent = nullptr);
    void BuildTree(const QString& mocPath, CCidJson* cid);
    bool BuildTreeBytes(const QByteArray& bytes, CCidJson* cid, QString* error = nullptr);
    void SetDeformerActionsEnabled(bool create, bool remove);
    void HighlightObjects(const QStringList& ids, const QStringList& types);
    QMap<QString, bool> VisibilityState() const;
    QMap<QString, bool> LockedState() const;

public slots:
    void HighlightMesh(const QString& artName);
    void HighlightObject(const QString& id, const QString& type);
    void ApplyVisibilityState(const QMap<QString, bool>& state);
    void ApplyLockedState(const QMap<QString, bool>& state);
    void SetVisibility(const QString& name, const QString& type, bool visible);

signals:
    void CreateDeformerRequested();
    void RemoveDeformerRequested();
    void ObjectSelected(const QString& name, const QString& type);
    void ObjectsSelected(const QStringList& names, const QStringList& types);
    void MeshSelected(const QString& artName);
    void WarpSelected(const QString& artName);
    void ShowDeformers(const QStringList& names);
    void ShowDeformerMeshes(const QStringList& artMeshNames);
    void DeformerDragParamsChanged(const QStringList& paramIds, const QList<float>& minVals,
                                   const QList<float>& maxVals, const QList<float>& defaultVals);
    void VisibilityChanged(const QString& name, const QString& type, bool visible);
    void VisibilityBatchChanged(const QMap<QString, bool>& values);
    void ObjectLockChanged(const QString& name, const QString& type, bool locked);
    void LocksBatchChanged(const QMap<QString, bool>& values);

private:
    void InstallInteractions(CModelHierarchyView* view, CModelHierarchyModel* model, bool deformerTab);
    void PublishVisibility(const QMap<QString, bool>& requested);
    void PublishLocks(const QMap<QString, bool>& requested);
    void EmitSelection(CModelHierarchyView* view);
    void SelectObjects(const QModelIndex& part, const QModelIndex& deformer,
                       const QString& id, const QString& type);
    void ShowContextMenu(CModelHierarchyView* view, const QPoint& position);

    QTabWidget* m_tabWidget = nullptr;
    QLineEdit* m_search = nullptr;
    QPushButton* m_createDeformer = nullptr;
    QPushButton* m_removeDeformer = nullptr;
    CModelHierarchyView* m_partTree = nullptr;
    CModelHierarchyView* m_deformerTree = nullptr;
    CModelHierarchyModel* m_partModel = nullptr;
    CModelHierarchyModel* m_deformerModel = nullptr;
};

#endif
