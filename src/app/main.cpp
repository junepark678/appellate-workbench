#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Appellate Workbench"));
    QCoreApplication::setApplicationName(QStringLiteral("Appellate Workbench"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    const auto pack_directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{};
    appellate::ui::MainWindow window(pack_directory);
    window.show();
    return application.exec();
}
