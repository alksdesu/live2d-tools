#ifndef CMODELTREEPANEL_H
#define CMODELTREEPANEL_H

#include <QDockWidget>
#include <QTreeWidget>

class CModelTreePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CModelTreePanel(QWidget *parent = nullptr);
    void BuildTree();

public slots:
    void HighlightMesh(const QString& artName);

signals:
    void MeshSelected(const QString& artName);
    void WarpSelected(const QString& artName);

private:
    void BuildPartTree(QTreeWidgetItem* parentItem, void* node);
    QTreeWidget* m_tree;
    QMap<QString, QTreeWidgetItem*> m_nameToItem;
};

#endif // CMODELTREEPANEL_H
