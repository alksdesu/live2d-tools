#ifndef CPHYSICSGRAPH_H
#define CPHYSICSGRAPH_H

#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>

class CPhysicsGraphWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CPhysicsGraphWidget(QWidget* parent = nullptr);

    void SetGroup(const QJsonObject& group);
    int InputCount() const { return m_inputs.size(); }
    int OutputCount() const { return m_outputs.size(); }
    int VertexCount() const { return m_vertices.size(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QJsonArray m_inputs;
    QJsonArray m_outputs;
    QJsonArray m_vertices;
};

#endif
