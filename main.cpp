#include "ui/mainwindow.h"
#include "ui/theme.h"

#include <QApplication>
#include <QFile>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setStyleSheet(Theme::globalStyleSheet());

    MainWindow w;
    w.show();
    return a.exec();
}
