#include "psdbatchresult.h"

bool IsPsdBatchTerminal(PsdBatchFileState state)
{
    return state == PsdBatchFileState::Succeeded || state == PsdBatchFileState::Failed || state == PsdBatchFileState::NotRun;
}

bool IsPsdBatchResultValid(const PsdBatchFileResult& result)
{
    return result.index >= 0 && result.elapsedMs >= 0 &&
           result.state >= PsdBatchFileState::Pending && result.state <= PsdBatchFileState::NotRun &&
           result.stage >= PsdExportStage::Queued && result.stage <= PsdExportStage::Done;
}

QString PsdBatchStateText(PsdBatchFileState state)
{
    switch (state) {
    case PsdBatchFileState::Pending: return QStringLiteral("等待");
    case PsdBatchFileState::Running: return QStringLiteral("进行中");
    case PsdBatchFileState::Succeeded: return QStringLiteral("成功");
    case PsdBatchFileState::Failed: return QStringLiteral("失败");
    case PsdBatchFileState::NotRun: return QStringLiteral("未执行");
    }
    return {};
}

QString PsdBatchStateKey(PsdBatchFileState state)
{
    switch (state) {
    case PsdBatchFileState::Pending: return QStringLiteral("pending");
    case PsdBatchFileState::Running: return QStringLiteral("running");
    case PsdBatchFileState::Succeeded: return QStringLiteral("succeeded");
    case PsdBatchFileState::Failed: return QStringLiteral("failed");
    case PsdBatchFileState::NotRun: return QStringLiteral("not_run");
    }
    return {};
}

QString PsdExportStageText(PsdExportStage stage)
{
    switch (stage) {
    case PsdExportStage::Queued: return QStringLiteral("等待开始");
    case PsdExportStage::Preflight: return QStringLiteral("检查模型");
    case PsdExportStage::Preparing: return QStringLiteral("准备导出");
    case PsdExportStage::Exporting: return QStringLiteral("导出图层");
    case PsdExportStage::Restoring: return QStringLiteral("恢复预览");
    case PsdExportStage::Done: return QStringLiteral("完成");
    }
    return {};
}

QString PsdExportStageKey(PsdExportStage stage)
{
    switch (stage) {
    case PsdExportStage::Queued: return QStringLiteral("queued");
    case PsdExportStage::Preflight: return QStringLiteral("preflight");
    case PsdExportStage::Preparing: return QStringLiteral("preparing");
    case PsdExportStage::Exporting: return QStringLiteral("exporting");
    case PsdExportStage::Restoring: return QStringLiteral("restoring");
    case PsdExportStage::Done: return QStringLiteral("done");
    }
    return {};
}
