#ifndef CMATERIALEDITCONTROLLER_H
#define CMATERIALEDITCONTROLLER_H

#include "cmodeldocument.h"
#include <functional>
#include <memory>

namespace live2dstruct {
class CMaterialEditController
{
public:
    using Publisher = std::function<bool(const CModelDocument&, const CModelDocument&, const QSet<QString>&, QString*)>;
    CMaterialEditController();
    ~CMaterialEditController();
    CMaterialEditController(const CMaterialEditController&) = delete;
    CMaterialEditController& operator=(const CMaterialEditController&) = delete;
    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher);
    bool Begin(QString* error = nullptr);
    bool Preview(const CModelMaterialState& state, QString* error = nullptr);
    bool Commit(const CModelMaterialState& state, QString* error = nullptr);
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
