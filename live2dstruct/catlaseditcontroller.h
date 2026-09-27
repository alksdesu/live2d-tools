#ifndef CATLASEDITCONTROLLER_H
#define CATLASEDITCONTROLLER_H

#include "cmodeldocument.h"
#include <functional>
#include <memory>

namespace live2dstruct {
class CAtlasEditController
{
public:
    using Publisher = std::function<bool(const CModelDocument&, const CModelDocument&, const QSet<QString>&, QString*)>;
    CAtlasEditController();
    ~CAtlasEditController();
    CAtlasEditController(const CAtlasEditController&) = delete;
    CAtlasEditController& operator=(const CAtlasEditController&) = delete;
    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher);
    bool Begin(QString* error = nullptr);
    bool Preview(const CModelAtlasState& state, QString* error = nullptr);
    bool Commit(const CModelAtlasState& state, QString* error = nullptr);
    bool Submit(const CModelAtlasState& state, QString* error = nullptr);
    bool Cancel(QString* error = nullptr);
    bool History(bool undo, CModelEdit& edit, QString* error = nullptr);
    bool IsActive() const;
    const CModelDocument* PreviewDocument() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
}

#endif
