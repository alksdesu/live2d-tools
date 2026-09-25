#ifndef CMODELTREEPANEL_H
#define CMODELTREEPANEL_H

#include <QDockWidget>
#include <QTreeWidget>
#include <QTabWidget>
#include <QMap>
#include <QLineEdit>

class CCidJson;

class CModelTreePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CModelTreePanel(QWidget *parent = nullptr);
    void BuildTree(const QString& mocPath, CCidJson* cid);
    QMap<QString, bool> VisibilityState() const;
    QMap<QString, bool> LockedState() const;

public slots:
    void HighlightMesh(const QString& artName);
    void ApplyVisibilityState(const QMap<QString, bool>& state);
    void ApplyLockedState(const QMap<QString, bool>& state);
    void SetVisibility(const QString& name, const QString& type, bool visible);

signals:
    void ObjectSelected(const QString& name, const QString& type);
    void ObjectsSelected(const QStringList& names, const QStringList& types);
    void MeshSelected(const QString& artName);
    void WarpSelected(const QString& artName);
    void ShowDeformers(const QStringList& names);
    void ShowDeformerMeshes(const QStringList& artMeshNames);
    void DeformerDragParamsChanged(const QStringList& paramIds, const QList<float>& minVals, const QList<float>& maxVals, const QList<float>& defaultVals);
    void VisibilityChanged(const QString& name, const QString& type, bool visible);
    void ObjectLockChanged(const QString& name, const QString& type, bool locked);

private:
    void setupPartTree(QTreeWidget* tree);
    void setupDeformerTree(QTreeWidget* tree);
    void onPartItemChanged(QTreeWidgetItem* item, int column);
    void onDeformerItemChanged(QTreeWidgetItem* item, int column);
    void setChildrenChecked(QTreeWidgetItem* item, Qt::CheckState state);
    void setItemLocked(QTreeWidgetItem* item, bool locked);

    // 从节点收集所有子变形器名称
    QStringList collectDeformerNames(QTreeWidgetItem* item);

    QTabWidget* m_tabWidget;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_partTree;
    QTreeWidget* m_deformerTree;
    // 变形器 → 其下所有 ArtMesh 名称（含子变形器的）
    QMap<QString, QStringList> m_deformerToArtMeshes;
    // 变形器 → 绑定的参数信息
    struct DeformerParamInfo {
        QStringList paramIds;
        QList<float> minVals, maxVals, defaultVals;
    };
    QMap<QString, DeformerParamInfo> m_deformerToParams;
    bool m_updatingChecks = false;
};

#endif // CMODELTREEPANEL_H
