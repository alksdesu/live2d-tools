#ifndef CMODELRESOURCEPATH_H
#define CMODELRESOURCEPATH_H

#include <QString>

class CModelResourcePath
{
public:
    static QString Resolve(const QString& modelDirectory, const QString& reference);
    static QString Identity(const QString& modelDirectory, const QString& reference);
};

#endif
