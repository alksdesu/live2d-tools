#include "cpsdfiletask.h"
#include "cpsdwritelist.h"
#include "std/cexportcontrol.h"
#include "ui/cexporttaskrunner.h"

bool CPsdFileTask::Execute(const QString& path, const QSize& canvas, bool cubismCompatible,
                           const Next& next, CExportControl* control, QString* error, const Accepted& onAccepted)
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
                accepted = !item.rect.isEmpty() && !item.image.isNull() && item.image.size() == item.rect.size() &&
                    item.emptyReason == CPsdExportCoverage::Reason::None &&
                    writer.AddLayer(item.name, item.rect, item.image, item.opacity,
                        item.blendMode, false, item.premultiplied,
                        item.psdLayerId ? std::optional<quint32>(item.psdLayerId) : std::nullopt);
                break;
            case CPsdExportItem::Type::EmptyLayer:
                accepted = item.rect == QRect() && item.image.isNull() &&
                    (item.emptyReason == CPsdExportCoverage::Reason::NoVertices ||
                     item.emptyReason == CPsdExportCoverage::Reason::NoTriangles ||
                     item.emptyReason == CPsdExportCoverage::Reason::DegenerateBounds ||
                     item.emptyReason == CPsdExportCoverage::Reason::OutsideCanvas) &&
                    writer.AddEmptyLayer(item.name, item.opacity, item.blendMode, false,
                        item.psdLayerId ? std::optional<quint32>(item.psdLayerId) : std::nullopt);
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
            if (onAccepted && (item.type == CPsdExportItem::Type::Layer || item.type == CPsdExportItem::Type::EmptyLayer) &&
                !onAccepted(item, detail)) return false;
        }
    }, control, error);
}
