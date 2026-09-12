#ifndef CPHYSICSPANEL_H
#define CPHYSICSPANEL_H

#include <QDockWidget>
#include <QTableWidget>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QJsonObject>
#include <QJsonArray>

class CPhysicsPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CPhysicsPanel(QWidget *parent = nullptr);
    void LoadPhysics(const QString& physicsPath);
    bool SavePhysics(const QString& path = QString());
    bool HasUnsavedChanges() const { return m_dirty; }

signals:
    void GravityChanged(float x, float y);
    void WindChanged(float x, float y);
    void PhysicsConfigChanged();
    void PhysicsSaved(const QString& path);
    // 选择物理组后发射输入参数列表，用于鼠标拖拽驱动
    void PhysicsDragParamsChanged(const QStringList& paramIds, const QStringList& types);

private:
    void showGroupDetail(int row);
    void emitDragParams(int row);
    void updateInputCell(int row, int column);
    void updateOutputCell(int row, int column);
    void updateVertexCell(int row, int column);
    void updateForceDocument();

    QLabel* m_fpsLabel;
    QTableWidget* m_groupTable;
    QCheckBox* m_dragModeCb;

    // 详情区域
    QTableWidget* m_inputTable;
    QTableWidget* m_outputTable;
    QTableWidget* m_vertexTable;

    // 重力/风力滑条
    QSlider* m_gravityX;
    QSlider* m_gravityY;
    QSlider* m_windX;
    QSlider* m_windY;
    QLabel* m_gravityXLabel;
    QLabel* m_gravityYLabel;
    QLabel* m_windXLabel;
    QLabel* m_windYLabel;

    // 解析后的数据
    QJsonArray m_physicsSettings;
    QJsonArray m_physicsDictionary;
    QJsonObject m_physicsDocument;
    QString m_physicsPath;
    bool m_updating = false;
    bool m_dirty = false;
};

#endif // CPHYSICSPANEL_H
