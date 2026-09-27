#ifndef CPARAMETERSELECTIONQUERY_H
#define CPARAMETERSELECTIONQUERY_H

#include <QStringList>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CModelDocument;
class CSectionTable;

class CParameterSelectionQuery
{
public:
    struct Target { QString type, id; };
    struct Parameter {
        QString id;
        int type = 0;
        float minimum = 0, maximum = 1, defaultValue = 0;
    };
    struct Result {
        QStringList ordinary, blendDrivers, blendLimiters, all;
        QStringList diagnostics;
    };

    bool Build(const CModelDocument& document, QString* error = nullptr);
    bool Build(const CSectionTable& section, QString* error = nullptr);
    bool Query(const QVector<Target>& selection, Result& result, QString* error = nullptr) const;
    const QVector<Parameter>& Parameters() const;

private:
    struct State;
    std::shared_ptr<const State> m_state;
};
}

#endif
