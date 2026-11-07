#include "main_window.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("D2ARM");
    QCoreApplication::setApplicationName("D2ARM Linux");
    QCoreApplication::setApplicationVersion(QStringLiteral(D2_VERSION));
    QIcon::setThemeName(QStringLiteral("hicolor"));

    MainWindow window;
    window.show();
    return app.exec();
}
