#include "cfileoption.h"
#include <QDir>

CFileOption::CFileOption() {}
QString CFileOption::m_CurFilePath = QDir::homePath();
