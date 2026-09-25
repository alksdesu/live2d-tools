#include "cpsdfiletask.h"
#include "cpsdwritelist.h"
#include "std/cexportcontrol.h"
#include "ui/cexporttaskrunner.h"

bool CPsdFileTask::Execute(const QString& path, const QSize& canvas, bool cubismCompatible,
                           const Next& next, CExportControl* control, QString* error)
{
    if (!next) {
        if (error) *error = QStringLiteral("PSD 导出缺少图层来源");
        return false;
    }
    return CExportTaskRunner::Run([&](const CExportTaskRunner::GuiCall& gui, QString* detail) {
        CPsdWriteList writer;
        writer.SetControl(control);
        if (!writer.CreatePsd(path, canvas.width(), canvas.height(), cubismCompatible)) {
            if (detail) *detail = control ? control->Error() : QStringLiteral("无法创建 PSD 文档");
            return false;
        }
        for (;;) {
            if (control && !control->Checkpoint()) { if (detail) *detail = control->Error(); return false; }
            CPsdExportItem item;
            if (!gui([&](QString* guiError) { return next(item, guiError); }, detail)) return false;
            bool accepted = true;
            switch (item.type) {
            case CPsdExportItem::Type::Progress:
                continue;
            case CPsdExportItem::Type::Done:
                return writer.WritePsd();
            case CPsdExportItem::Type::Layer:
                if (!item.rect.isEmpty())
                    accepted = writer.AddLayer(item.name, item.rect, item.image, item.opacity,
                                               item.blendMode, false, item.premultiplied);
                break;
            case CPsdExportItem::Type::GroupStart:
                accepted = writer.AddGroupStart(item.name);
                break;
            case CPsdExportItem::Type::GroupEnd:
                accepted = writer.AddGroupEnd();
                break;
            }
            if (!accepted) {
                if (detail) *detail = control && !control->Error().isEmpty() ? control->Error() : QStringLiteral("PSD 图层编码失败");
                return false;
            }
        }
    }, control, error);
}
