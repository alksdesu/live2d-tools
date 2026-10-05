#ifndef CGLUEEDITCONTROLLER_H
#define CGLUEEDITCONTROLLER_H

#include "cmocmeshstructurebuilder.h"
#include <QByteArray>
#include <QVector>
#include <functional>

namespace live2dstruct {
class CGlueEditController
{
public:
    using Snapshot = QVector<CMocMeshStructureBuilder::GlueSnapshot>;
    using Request = CMocMeshStructureBuilder::Request;
    using WeightEdit = CMocMeshStructureBuilder::GlueWeightEdit;
    using Side = CMocMeshStructureBuilder::GlueRetarget::Side;
    using Publisher = std::function<bool(const QByteArray&, const Request&, const QByteArray&, QString*)>;

    bool Initialize(const QByteArray& base, QString* error = nullptr);
    void Reset();
    void SetPublisher(Publisher publisher);
    bool SetWeight(int glueIndex, int pairIndex, Side side, float weight, QString* error = nullptr);
    bool Apply(const QVector<WeightEdit>& edits, QString* error = nullptr);
    bool Undo(QString* error = nullptr);
    bool Redo(QString* error = nullptr);
    void MarkSaved();

    const QByteArray& SourceBytes() const { return m_source; }
    const QByteArray& CurrentBytes() const { return m_state; }
    const Snapshot& CurrentGlues() const { return m_glues; }
    const Request& LastRequest() const { return m_lastRequest; }
    bool CanUndo() const { return !m_publishing && m_index > 0; }
    bool CanRedo() const { return !m_publishing && m_index < m_history.size(); }
    bool IsPublishing() const { return m_publishing; }
    bool IsDirty() const { return m_state != m_saved; }
    quint64 Revision() const { return m_revision; }

private:
    struct Change {
        QByteArray beforeBytes, afterBytes;
        Snapshot beforeGlues, afterGlues;
        Request forward, reverse;
    };
    bool Publish(CGlueEditController candidate, const Request& request, QString* error);
    void Swap(CGlueEditController& other) noexcept;
    QByteArray m_source, m_state, m_saved;
    Snapshot m_glues;
    QVector<Change> m_history;
    Publisher m_publisher;
    Request m_lastRequest;
    int m_index = 0;
    quint64 m_revision = 0;
    bool m_publishing = false;
};
}

#endif
