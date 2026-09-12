#include "ui/mainwindow.h"
#include "ui/theme.h"

#include <QApplication>
#include <QSettings>
#include <QFile>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    a.setStyleSheet(Theme::globalStyleSheet(settings.value(QStringLiteral("darkTheme"), false).toBool()));

    MainWindow w;
    w.show();
    return a.exec();
}
