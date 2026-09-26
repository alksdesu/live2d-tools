#ifndef CUBISMPARAMETERSTRUCTURE_HPP
#define CUBISMPARAMETERSTRUCTURE_HPP

#include <QMap>
#include <QMetaType>
#include <QString>
#include <optional>

struct ParameterRuntimeValue
{
    float current = 0.0f;
    std::optional<float> saved;
    int type = 0;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct ParameterStructureSeed
{
    enum class Mode { Defaults, Values };
    Mode mode = Mode::Defaults;
    ParameterRuntimeValue runtime;
    std::optional<ParameterRuntimeValue> editorEntry;
    std::optional<float> editorPose;
    std::optional<float> editorBase;
    std::optional<float> timelineBase;
};

using ParameterStructureSeeds = QMap<QString, ParameterStructureSeed>;
Q_DECLARE_METATYPE(ParameterStructureSeed)
Q_DECLARE_METATYPE(ParameterStructureSeeds)

#endif
