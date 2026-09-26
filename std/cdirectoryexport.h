#ifndef CDIRECTORYEXPORT_H
#define CDIRECTORYEXPORT_H

#include <QByteArray>
#include <QStringList>
#include <QVector>

class CExportControl;

class CDirectoryExport
{
public:
    struct Entry {
        QString relativePath;
        QByteArray bytes;
    };
    struct Options {
        QStringList protectedPaths;
        qint64 maximumTotalBytes = 8LL * 1024 * 1024 * 1024;
        int maximumEntries = 10000;
    };
    struct Result {
        bool committed = false;
        QString directory;
        QString residualStaging;
    };
    static bool Publish(const QString& targetDirectory, const QVector<Entry>& entries, const Options& options,
                        Result& result, CExportControl* control = nullptr, QString* error = nullptr);
};

#endif
