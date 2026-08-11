#pragma once

#include <QByteArray>
#include <QString>

#include <expected>

class QApplication;

namespace appellate::ui {

// Runs only when explicitly requested by the release verifier. The caller must place the process
// in an OS network namespace; this function exercises the installed executable's production
// local-session wiring and reports one compact JSON object on success.
[[nodiscard]] auto runOfflineSelfTest(QApplication& application, const QString& catalog_root,
                                      const QString& bundled_workflow_pack,
                                      const QString& imported_grounded_pack)
    -> std::expected<QByteArray, QString>;

} // namespace appellate::ui
