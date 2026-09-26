#include "cmodelresourcepath.h"
#include <QDir>

QString CModelResourcePath::Resolve(const QString& modelDirectory, const QString& reference)
{
    if (reference.isEmpty() || reference.contains(QChar::Null) || modelDirectory.contains(QChar::Null)) return {};
    return QDir::cleanPath(QDir(modelDirectory).absoluteFilePath(reference));
}

QString CModelResourcePath::Identity(const QString& modelDirectory, const QString& reference)
{
    const QString path = Resolve(modelDirectory, reference);
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QDir(modelDirectory).relativeFilePath(path));
}
