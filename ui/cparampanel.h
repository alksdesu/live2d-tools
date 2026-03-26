#ifndef CPARAMPANEL_H
#define CPARAMPANEL_H

#include <QDockWidget>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QMap>
#include <QSet>

class CCidJson;

class CParamPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CParamPanel(QWidget *parent = nullptr);
    void BuildParams(CCidJson* cid = nullptr, const QSet<QString>* blendShapeParams = nullptr);

signals:
    void ParamChanged(const QString& name, float value, bool isInit);

public slots:
    void ResetAll();
    void SyncFromModel();

private:
    struct ParamRow {
        QSlider* slider;
        QLabel* valueLabel;
        float minVal;
        float maxVal;
        float defaultVal;
        QString paramId;
        int modelIndex = -1;
    };

    QScrollArea* m_scroll;
    QWidget* m_container;
    QVector<ParamRow> m_rows;
    QSet<int> m_userDragging;
    QSet<int> m_userLocked; // 用户手动修改过的参数，不再被 sync 覆盖
};

#endif // CPARAMPANEL_H
