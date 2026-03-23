#ifndef CPARAMPANEL_H
#define CPARAMPANEL_H

#include <QDockWidget>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QMap>

class CParamPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CParamPanel(QWidget *parent = nullptr);
    void BuildParams();

signals:
    void ParamChanged(const QString& name, float value, bool isInit);

public slots:
    void ResetAll();

private:
    struct ParamRow {
        QSlider* slider;
        QLabel* valueLabel;
        float minVal;
        float maxVal;
        float defaultVal;
        QString paramId;
    };

    QScrollArea* m_scroll;
    QWidget* m_container;
    QVector<ParamRow> m_rows;
};

#endif // CPARAMPANEL_H
