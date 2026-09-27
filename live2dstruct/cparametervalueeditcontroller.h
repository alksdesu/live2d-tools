#ifndef CPARAMETERVALUEEDITCONTROLLER_H
#define CPARAMETERVALUEEDITCONTROLLER_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <functional>
#include <memory>

namespace live2dstruct {
class CModelDocument;

class CParameterValueEditController
{
public:
    enum class Phase { Single, Begin, Preview, Finish, Cancel };
    struct Request {
        Phase phase = Phase::Single;
        quint64 gestureId = 0;
        QMap<QString, float> drivers;
    };
    struct Publication {
        Phase phase = Phase::Single;
        quint64 gestureId = 0;
        QMap<QString, float> before;
        QMap<QString, float> after;
        bool changed = false;
        bool restoreContext = false;
    };
    struct Result {
        QMap<QString, float> acceptedValues;
        bool changed = false;
        bool historyChanged = false;
        bool restoreContext = false;
    };
    using Publisher = std::function<bool(const Publication&, QString*)>;
    using Expander = std::function<bool(const CModelDocument&, const QMap<QString, float>&,
                                        QMap<QString, float>&, QString*)>;
    using BaselineProvider = std::function<bool(QMap<QString, float>&, QString*)>;
    using ContextDigestProvider = std::function<QByteArray()>;

    CParameterValueEditController();
    ~CParameterValueEditController();
    CParameterValueEditController(const CParameterValueEditController&) = delete;
    CParameterValueEditController& operator=(const CParameterValueEditController&) = delete;
    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher);
    void SetExpander(Expander expander);
    void SetBaselineProvider(BaselineProvider provider);
    void SetContextDigestProvider(ContextDigestProvider provider);
    bool Apply(const Request& request, Result& result, QString* error = nullptr);
    bool IsActive() const;
    bool HasChanges() const;
    quint64 GestureId() const;
    void Reset();

private:
    struct State;
    std::shared_ptr<State> m_state;
};
}

#endif
