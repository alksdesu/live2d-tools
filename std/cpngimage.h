#ifndef CPNGIMAGE_H
#define CPNGIMAGE_H

#include <QByteArray>
#include <QSize>
#include <QString>
class QImage;

class CPngImage
{
public:
    static bool Size(const QByteArray& png, QSize& result, QString* error = nullptr);
    static bool Decode(const QByteArray& png, QImage& result, QString* error = nullptr);
};

#endif
