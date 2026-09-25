#ifndef CKEYFORMMAPPINGCONTEXT_H
#define CKEYFORMMAPPINGCONTEXT_H

#include <QMap>
#include <QString>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CModelDocument;

class CKeyformMappingContext
{
public:
    static constexpr double AbsoluteTolerance = 0.00002;
    static constexpr double RelativeTolerance = 0.000002;

    bool Build(const CModelDocument& document, const QString& artMeshId, int keyformIndex,
               const QMap<QString, float>& fullPose, QString* error = nullptr);
    bool ToModel(const QVector<float>& local, QVector<float>& world, QString* error = nullptr) const;
    bool FromModel(const QVector<float>& world, QVector<float>& local, QString* error = nullptr) const;
    bool MatchesRuntime(const QVector<float>& actualModelVertices, QString* error = nullptr) const;
    bool IsValid() const { return bool(m_state); }

private:
    struct State;
    std::shared_ptr<const State> m_state;
};
}

#endif
