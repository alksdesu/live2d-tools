#include "ui/mainwindow.h"
#include "ui/theme.h"

#include <QApplication>
#include <QSettings>
#include <QFile>
#include <QMessageBox>
#include <QSurfaceFormat>

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication a(argc, argv);
    QSettings settings(QStringLiteral("live2d2psd"), QStringLiteral("live2d2psd"));
    a.setStyleSheet(Theme::globalStyleSheet(settings.value(QStringLiteral("darkTheme"), false).toBool()));

    MainWindow w;
    w.show();
    return a.exec();
}
