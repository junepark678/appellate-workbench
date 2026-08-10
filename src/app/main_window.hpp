#pragma once

#include <QMainWindow>

class QLabel;

namespace appellate::ui {

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(const QString& pack_directory = {}, QWidget* parent = nullptr);

  private:
    void loadPack(const QString& directory);

    QLabel* pack_status_{};
    QLabel* profile_status_{};
};

} // namespace appellate::ui
