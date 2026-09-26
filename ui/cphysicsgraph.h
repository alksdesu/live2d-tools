#ifndef CPHYSICSGRAPH_H
#define CPHYSICSGRAPH_H

#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>
#include "framework/Physics/CubismPhysicsObservation.hpp"

class CPhysicsGraphWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CPhysicsGraphWidget(QWidget* parent = nullptr);

    void SetGroup(const QJsonObject& group);
    void SetObservation(const Live2D::Cubism::Framework::PhysicsObservation& sample);
    void ClearObservation();
    void SetChannel(bool output, int row);
    static constexpr int HistoryCapacity = 240;
    int HistorySize() const { return m_history.size(); }
    bool HasObservation() const { return m_observation.sampled; }
    const QVector<QPointF>& History() const { return m_history; }
    int InputCount() const { return m_inputs.size(); }
    int OutputCount() const { return m_outputs.size(); }
    int VertexCount() const { return m_vertices.size(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QJsonArray m_inputs;
    QJsonArray m_outputs;
    QJsonArray m_vertices;
    Live2D::Cubism::Framework::PhysicsObservation m_observation;
    QVector<QPointF> m_history;
    bool m_outputChannel = true;
    int m_channelRow = -1;
};

#endif
