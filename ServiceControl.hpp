#pragma once

#include <QStringList>

// `myrsm.exe service <install|uninstall|start|stop|status>`: manages the MyRsm
// Windows service through the SCM API so installers and scripts don't need
// sc.exe.
//
// Exit codes:
//   0  success (stop/uninstall of a service that isn't running/installed is a success)
//   1  the operation failed (message on stderr)
//   2  usage error
//   3  not elevated (install/uninstall/start/stop need an Administrator prompt)
//   4  service not installed (status, start)
namespace service_control {

inline constexpr wchar_t kServiceName[] = L"MyRsm";

int run(const QStringList& args);

} // namespace service_control
