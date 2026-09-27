#ifndef CTEXTUREEDITCONTROLLER_H
#define CTEXTUREEDITCONTROLLER_H

#include "ctextureassets.h"
#include <QVector>
#include <functional>

namespace live2dstruct {
class CTextureEditController
{
public:
    using Catalog = CTextureAssets::Catalog;
    using State = CTextureAssets::State;
    using Publisher = std::function<bool(const State&, QString*)>;
    bool Initialize(const Catalog& sources, const State& state = {}, QString* error = nullptr);
    void SetPublisher(Publisher publisher);
    bool Replace(int slot, const QByteArray& png, QString* error = nullptr);
    bool Undo(QString* error = nullptr);
    bool Redo(QString* error = nullptr);
    bool PrepareRestore(const CTextureAssets::Draft& draft, CTextureEditController& result, QString* error = nullptr) const;
    void Swap(CTextureEditController& other) noexcept;
    void MarkSaved();
    const Catalog& Sources() const { return m_sources; }
    const State& Current() const { return m_state; }
    bool CanUndo() const { return !m_publishing && m_index > 0; }
    bool CanRedo() const { return !m_publishing && m_index < m_history.size(); }
    bool IsDirty() const { return m_state != m_saved; }
    quint64 Revision() const { return m_revision; }
    int LastChangedSlot() const { return m_lastSlot; }

private:
    struct Change { State before, after; int slot = -1; };
    bool Publish(CTextureEditController candidate, QString* error);
    Catalog m_sources;
    State m_state, m_saved;
    QVector<Change> m_history;
    Publisher m_publisher;
    int m_index = 0, m_lastSlot = -1;
    quint64 m_revision = 0;
    bool m_publishing = false;
};
}

#endif
