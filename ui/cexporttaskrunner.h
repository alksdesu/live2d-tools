#ifndef CEXPORTTASKRUNNER_H
#define CEXPORTTASKRUNNER_H

#include <QString>
#include <functional>

class CExportControl;

class CExportTaskRunner
{
public:
    using GuiCall = std::function<bool(const std::function<bool(QString*)>&, QString*)>;
    using Task = std::function<bool(const GuiCall&, QString*)>;

    static bool Run(const Task& task, CExportControl* control = nullptr, QString* error = nullptr);
};

#endif
