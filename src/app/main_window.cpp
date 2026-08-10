#include "main_window.hpp"

#include "appellate/packs/pack_reader.hpp"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace appellate::ui {

MainWindow::MainWindow(const QString& pack_directory, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Appellate Workbench"));
    resize(900, 600);

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);

    auto* heading = new QLabel(QStringLiteral("Appellate Workbench"), container);
    auto heading_font = heading->font();
    heading_font.setPointSize(20);
    heading_font.setBold(true);
    heading->setFont(heading_font);

    auto* boundary = new QLabel(
        QStringLiteral("Local-first simulation. Declarative content packs. No application server."),
        container);
    boundary->setWordWrap(true);

    pack_status_ = new QLabel(QStringLiteral("No content pack loaded."), container);
    profile_status_ = new QLabel(
        QStringLiteral("Pass a pack directory as the first command-line argument."), container);
    profile_status_->setWordWrap(true);

    layout->addWidget(heading);
    layout->addWidget(boundary);
    layout->addSpacing(24);
    layout->addWidget(pack_status_);
    layout->addWidget(profile_status_);
    layout->addStretch();

    setCentralWidget(container);

    if (!pack_directory.isEmpty()) {
        loadPack(pack_directory);
    }
}

void MainWindow::loadPack(const QString& directory) {
    const auto loaded = packs::PackReader::readDirectory(directory);
    if (!loaded) {
        pack_status_->setText(QStringLiteral("Pack rejected."));
        profile_status_->setText(loaded.error().message);
        return;
    }

    const auto& revision = loaded->revision;
    pack_status_->setText(QStringLiteral("Loaded %1 @ %2")
                              .arg(QString::fromStdString(revision.id.value),
                                   QString::fromStdString(revision.version)));

    if (loaded->judge_profiles.empty()) {
        profile_status_->setText(QStringLiteral("No bench profiles in this pack."));
        return;
    }

    profile_status_->setText(
        QStringLiteral("Bench profile: %1 (fictional/composite)")
            .arg(QString::fromStdString(loaded->judge_profiles.front().display_name)));
}

} // namespace appellate::ui
