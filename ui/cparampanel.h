#ifndef CPARAMPANEL_H
#define CPARAMPANEL_H

#include <QDockWidget>
#include <QScrollArea>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QMap>
#include <QSet>
#include <QMap>

class CCidJson;

class CParamPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CParamPanel(QWidget *parent = nullptr);
    void BuildParams(CCidJson* cid = nullptr, const QSet<QString>* blendShapeParams = nullptr);
    QMap<QString, float> CurrentValues() const;

signals:
    void ParamChanged(const QString& name, float value, bool isInit);
    void HistoryChanged(bool canUndo, bool canRedo);

public slots:
    void ResetAll();
    void SyncFromModel();
    void Undo();
    void Redo();
    void ClearHistory();
    void ApplyValues(const QMap<QString, float>& values);

private:
    struct ParamRow {
        QWidget* widget = nullptr;
        QSlider* slider;
        QDoubleSpinBox* spinBox;
        QLabel* valueLabel;
        float minVal;
        float maxVal;
        float defaultVal;
        QString paramId;
        int modelIndex = -1;
    };

    QScrollArea* m_scroll;
    QWidget* m_container;
    QLineEdit* m_search;
    QVector<ParamRow> m_rows;
    QSet<int> m_userDragging;
    QSet<int> m_userLocked; // 用户手动修改过的参数，不再被 sync 覆盖
    struct Edit {
        QString paramId;
        float before = 0.0f;
        float after = 0.0f;
    };
    QVector<Edit> m_history;
    int m_historyIndex = 0;
    QHash<QString, float> m_lastValues;
    QHash<int, float> m_dragStarts;
    bool m_replaying = false;

    void ApplyValue(int rowIndex, float value, bool record);
    void PushEdit(const QString& paramId, float before, float after);
    void EmitHistoryState();
};

#endif // CPARAMPANEL_H
