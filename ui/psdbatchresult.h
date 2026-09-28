#ifndef PSDBATCHRESULT_H
#define PSDBATCHRESULT_H

#include <QMetaType>
#include <QString>
#include <QVector>
#include "std/cexportdiagnostics.h"
#include "std/cexportsourceinfo.h"

enum class PsdBatchFileState { Pending, Running, Succeeded, Failed, NotRun, Canceled };
enum class PsdExportStage { Queued, Preflight, Preparing, Exporting, Encoding, Writing, Restoring, Done };

struct PsdBatchFileResult
{
    int index = -1;
    QString sourcePath;
    QString outputPath;
    PsdBatchFileState state = PsdBatchFileState::Pending;
    PsdExportStage stage = PsdExportStage::Queued;
    QString error;
    qint64 elapsedMs = 0;
    bool outputCommitted = false;
    CExportDiagnostics diagnostics;
    CExportSourceInfo sourceInfo;

    bool operator==(const PsdBatchFileResult& other) const
    {
        return index == other.index && sourcePath == other.sourcePath && outputPath == other.outputPath &&
               state == other.state && stage == other.stage && error == other.error &&
               elapsedMs == other.elapsedMs && outputCommitted == other.outputCommitted && diagnostics == other.diagnostics &&
               sourceInfo == other.sourceInfo;
    }
    bool operator!=(const PsdBatchFileResult& other) const { return !(*this == other); }
};

using PsdBatchResults = QVector<PsdBatchFileResult>;

bool IsPsdBatchTerminal(PsdBatchFileState state);
bool IsPsdBatchResultValid(const PsdBatchFileResult& result);
bool IsPsdExportStageTransitionValid(PsdExportStage current, PsdExportStage next);
QString PsdBatchStateText(PsdBatchFileState state);
QString PsdBatchStateKey(PsdBatchFileState state);
QString PsdExportStageText(PsdExportStage stage);
QString PsdExportStageKey(PsdExportStage stage);

Q_DECLARE_METATYPE(PsdBatchFileResult)
Q_DECLARE_METATYPE(PsdBatchResults)
Q_DECLARE_METATYPE(PsdBatchFileState)
Q_DECLARE_METATYPE(PsdExportStage)

#endif
