#include "findfasterwidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("FindFaster"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    a.setWindowIcon(QIcon(QStringLiteral(":/icons/FindFaster.ico")));
    FindFasterWidget w;
    w.show();
    return a.exec();
}
