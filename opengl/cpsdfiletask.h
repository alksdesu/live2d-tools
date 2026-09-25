#ifndef CPSDFILETASK_H
#define CPSDFILETASK_H

#include <QImage>
#include <QRect>
#include <QString>
#include <functional>

class CExportControl;

struct CPsdExportItem
{
    enum class Type { Progress, Layer, GroupStart, GroupEnd, Done };
    Type type = Type::Progress;
    QString name;
    QImage image;
    QRect rect;
    quint8 opacity = 255;
    quint32 blendMode = 0x6e6f726d;
    bool premultiplied = true;
};

class CPsdFileTask
{
public:
    using Next = std::function<bool(CPsdExportItem&, QString*)>;
    static bool Execute(const QString& path, const QSize& canvas, bool cubismCompatible,
                        const Next& next, CExportControl* control = nullptr, QString* error = nullptr);
};

#endif
