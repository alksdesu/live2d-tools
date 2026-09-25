#ifndef CDEFORMERDRAG_H
#define CDEFORMERDRAG_H

#include <QList>
#include <QStringList>
#include <QVector>
#include <functional>

class CDeformerDrag
{
public:
    struct Parameter {
        QString id;
        float minimum;
        float maximum;
        float defaultValue;
    };
    using Vertices = QVector<QVector<float>>;
    using Evaluator = std::function<bool(const QVector<float>&, Vertices&, QString*)>;
    using Restorer = std::function<bool(QString*)>;

    bool SetParameters(const QStringList& ids, const QList<float>& minimums, const QList<float>& maximums,
                       const QList<float>& defaults, QString* error = nullptr);
    bool Apply(float viewX, float viewY, QString* error = nullptr);
    void Stop() noexcept;
    void Swap(CDeformerDrag& other) noexcept;
    bool IsActive() const noexcept { return m_active; }
    const QStringList& Ids() const noexcept { return m_ids; }
    const QList<float>& Values() const noexcept { return m_values; }
    static bool MapValue(float input, float minimum, float maximum, float& output);
    static bool Probe(const QVector<Parameter>& parameters, const QVector<float>& current, int drawableCount,
                      const QVector<int>& targets, const Evaluator& evaluate, const Restorer& restore,
                      QVector<int>& result, QString* error = nullptr);
    bool ProbeParameters(const QVector<Parameter>& parameters, const QVector<float>& current, int drawableCount,
                         const QVector<int>& targets, const Evaluator& evaluate, const Restorer& restore,
                         QString* error = nullptr);

private:
    QVector<Parameter> m_parameters;
    QStringList m_ids;
    QList<float> m_values;
    bool m_active = false;
};

#endif
