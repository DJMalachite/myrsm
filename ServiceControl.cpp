#include "ServiceControl.hpp"

#include <QTextStream>

#include <windows.h>

#include <string>
#include <vector>

namespace service_control {

namespace {

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kNotElevated = 3;
constexpr int kNotInstalled = 4;

constexpr DWORD kWaitTimeoutMs = 30000;
constexpr DWORD kPollMs = 250;

const wchar_t kDisplayName[] = L"MyRsm (HWiNFO sensor exporter)";
const wchar_t kDescription[] =
    L"Serves HWiNFO shared-memory sensor readings as JSON (/), WebSocket and Prometheus metrics "
    L"(/metrics). Settings: %ProgramData%\\MyRsm\\myrsm.ini (restart the service after editing).";

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

QString errorText(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QStringLiteral("Unknown error");
    if (buf)
        LocalFree(buf);
    return QStringLiteral("%1 (error %2)").arg(text, QString::number(code));
}

int fail(const QString& what, DWORD code = GetLastError())
{
    err() << "myrsm service: " << what << ": " << errorText(code) << Qt::endl;
    return kFailed;
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE handle = nullptr) : h(handle) {}
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

int requireElevation(const char* command)
{
    err() << "myrsm service " << command
          << ": needs an elevated (Run as administrator) prompt." << Qt::endl;
    return kNotElevated;
}

std::wstring modulePath()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size())
            return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

QString stateName(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED: return QStringLiteral("STOPPED");
    case SERVICE_START_PENDING: return QStringLiteral("START_PENDING");
    case SERVICE_STOP_PENDING: return QStringLiteral("STOP_PENDING");
    case SERVICE_RUNNING: return QStringLiteral("RUNNING");
    case SERVICE_CONTINUE_PENDING: return QStringLiteral("CONTINUE_PENDING");
    case SERVICE_PAUSE_PENDING: return QStringLiteral("PAUSE_PENDING");
    case SERVICE_PAUSED: return QStringLiteral("PAUSED");
    default: return QStringLiteral("UNKNOWN(%1)").arg(state);
    }
}

bool queryStatus(SC_HANDLE svc, SERVICE_STATUS_PROCESS* st)
{
    DWORD needed = 0;
    return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(st), sizeof(*st), &needed);
}

QString exitCodes(const SERVICE_STATUS_PROCESS& st)
{
    return QStringLiteral("win32 exit code %1, service-specific exit code %2")
        .arg(QString::number(st.dwWin32ExitCode), QString::number(st.dwServiceSpecificExitCode));
}

// Waits until the service is STOPPED. Returns false on timeout / query error.
bool waitStopped(SC_HANDLE svc, SERVICE_STATUS_PROCESS* st)
{
    const ULONGLONG deadline = GetTickCount64() + kWaitTimeoutMs;
    for (;;) {
        if (!queryStatus(svc, st))
            return false;
        if (st->dwCurrentState == SERVICE_STOPPED)
            return true;
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(kPollMs);
    }
}

// Stops the service (if it isn't already stopped) and waits for STOPPED.
int stopAndWait(SC_HANDLE svc)
{
    SERVICE_STATUS_PROCESS st{};
    if (!queryStatus(svc, &st))
        return fail(QStringLiteral("cannot query the service"));
    if (st.dwCurrentState == SERVICE_STOPPED)
        return kOk;
    if (st.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS ss{};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
            const DWORD e = GetLastError();
            // Not active: already stopped. Cannot accept control: it's
            // starting or stopping — just wait for it to settle.
            if (e != ERROR_SERVICE_NOT_ACTIVE && e != ERROR_SERVICE_CANNOT_ACCEPT_CTRL)
                return fail(QStringLiteral("cannot stop the service"), e);
        }
    }
    if (!waitStopped(svc, &st)) {
        err() << "myrsm service: the service did not reach STOPPED within " << kWaitTimeoutMs / 1000
              << " s (state " << stateName(st.dwCurrentState) << ")." << Qt::endl;
        return kFailed;
    }
    return kOk;
}

QString configuredBinaryPath(SC_HANDLE svc)
{
    DWORD needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0)
        return {};
    std::vector<BYTE> buf(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (!QueryServiceConfigW(svc, cfg, needed, &needed))
        return {};
    return QString::fromWCharArray(cfg->lpBinaryPathName);
}

int applyExtraConfig(SC_HANDLE svc)
{
    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<wchar_t*>(kDescription);
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description))
        return fail(QStringLiteral("cannot set the description"));

    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed))
        return fail(QStringLiteral("cannot enable delayed auto-start"));

    // Restart after 5 s, three times; reset the failure count after a day.
    SC_ACTION actions[3] = {
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 5000},
    };
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure))
        return fail(QStringLiteral("cannot set the recovery actions"));

    // Also apply the recovery actions when the service stops with an error
    // exit code (e.g. its port was taken), not only when the process dies.
    SERVICE_FAILURE_ACTIONS_FLAG flag{};
    flag.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag))
        return fail(QStringLiteral("cannot set the recovery actions flag"));
    return kOk;
}

int install()
{
    if (!isElevated())
        return requireElevation("install");
    const std::wstring exe = modulePath();
    if (exe.empty())
        return fail(QStringLiteral("cannot determine the path of myrsm.exe"));
    // Quoted, no arguments: settings come from myrsm.ini.
    const std::wstring binPath = L"\"" + exe + L"\"";

    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm)
        return fail(QStringLiteral("cannot open the service control manager"));

    ScHandle svc(CreateServiceW(scm.h, kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
                                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                binPath.c_str(), nullptr, nullptr, nullptr,
                                nullptr /* LocalSystem */, nullptr));
    bool created = true;
    QString previousPath;
    if (!svc) {
        const DWORD e = GetLastError();
        if (e != ERROR_SERVICE_EXISTS)
            return fail(QStringLiteral("cannot create the service"), e);
        created = false;
        svc.h = OpenServiceW(scm.h, kServiceName, SERVICE_ALL_ACCESS);
        if (!svc)
            return fail(QStringLiteral("cannot open the existing service"));
        previousPath = configuredBinaryPath(svc.h);
        // Stop whatever instance is running (possibly an old install
        // elsewhere) so the next start uses this binary.
        if (const int r = stopAndWait(svc.h); r != kOk)
            return r;
        if (!ChangeServiceConfigW(svc.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  binPath.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", L"",
                                  kDisplayName))
            return fail(QStringLiteral("cannot update the existing service"));
    }
    if (const int r = applyExtraConfig(svc.h); r != kOk)
        return r;

    const QString path = QString::fromStdWString(binPath);
    if (created) {
        out() << "Installed service MyRsm: " << path << Qt::endl;
    } else if (previousPath.compare(path, Qt::CaseInsensitive) != 0) {
        out() << "Updated service MyRsm: " << path << " (was " << previousPath << ")" << Qt::endl;
    } else {
        out() << "Updated service MyRsm: " << path << Qt::endl;
    }
    out() << "Start type: Automatic (Delayed Start), account LocalSystem, restart on failure." << Qt::endl;
    return kOk;
}

int uninstall()
{
    if (!isElevated())
        return requireElevation("uninstall");
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
        return fail(QStringLiteral("cannot open the service control manager"));
    ScHandle svc(OpenServiceW(scm.h, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
            out() << "Service MyRsm is not installed; nothing to do." << Qt::endl;
            return kOk;
        }
        return fail(QStringLiteral("cannot open the service"), e);
    }
    if (const int r = stopAndWait(svc.h); r != kOk)
        return r;
    if (!DeleteService(svc.h)) {
        const DWORD e = GetLastError();
        if (e != ERROR_SERVICE_MARKED_FOR_DELETE)
            return fail(QStringLiteral("cannot delete the service"), e);
    }
    out() << "Removed service MyRsm." << Qt::endl;
    return kOk;
}

int start()
{
    if (!isElevated())
        return requireElevation("start");
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
        return fail(QStringLiteral("cannot open the service control manager"));
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
            err() << "myrsm service start: service MyRsm is not installed." << Qt::endl;
            return kNotInstalled;
        }
        return fail(QStringLiteral("cannot open the service"), e);
    }
    if (!StartServiceW(svc.h, 0, nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING)
            return fail(QStringLiteral("cannot start the service"), e);
    }
    SERVICE_STATUS_PROCESS st{};
    const ULONGLONG deadline = GetTickCount64() + kWaitTimeoutMs;
    for (;;) {
        if (!queryStatus(svc.h, &st))
            return fail(QStringLiteral("cannot query the service"));
        if (st.dwCurrentState != SERVICE_START_PENDING)
            break;
        if (GetTickCount64() >= deadline) {
            err() << "myrsm service start: still START_PENDING after " << kWaitTimeoutMs / 1000 << " s."
                  << Qt::endl;
            return kFailed;
        }
        Sleep(kPollMs);
    }
    if (st.dwCurrentState != SERVICE_RUNNING) {
        err() << "myrsm service start: service is " << stateName(st.dwCurrentState) << " ("
              << exitCodes(st) << "). See the log in %ProgramData%\\MyRsm\\logs." << Qt::endl;
        return kFailed;
    }
    out() << "Service MyRsm is RUNNING (pid " << st.dwProcessId << ")." << Qt::endl;
    return kOk;
}

int stop()
{
    if (!isElevated())
        return requireElevation("stop");
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
        return fail(QStringLiteral("cannot open the service control manager"));
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
            out() << "Service MyRsm is not installed; nothing to stop." << Qt::endl;
            return kOk;
        }
        return fail(QStringLiteral("cannot open the service"), e);
    }
    if (const int r = stopAndWait(svc.h); r != kOk)
        return r;
    out() << "Service MyRsm is STOPPED." << Qt::endl;
    return kOk;
}

int status()
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
        return fail(QStringLiteral("cannot open the service control manager"));
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
            out() << "Service MyRsm: not installed" << Qt::endl;
            return kNotInstalled;
        }
        return fail(QStringLiteral("cannot open the service"), e);
    }
    SERVICE_STATUS_PROCESS st{};
    if (!queryStatus(svc.h, &st))
        return fail(QStringLiteral("cannot query the service"));

    QString startType = QStringLiteral("unknown");
    QString account;
    QString binary;
    DWORD needed = 0;
    QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed) {
        std::vector<BYTE> buf(needed);
        auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (QueryServiceConfigW(svc.h, cfg, needed, &needed)) {
            binary = QString::fromWCharArray(cfg->lpBinaryPathName);
            account = cfg->lpServiceStartName ? QString::fromWCharArray(cfg->lpServiceStartName) : QString();
            switch (cfg->dwStartType) {
            case SERVICE_AUTO_START: startType = QStringLiteral("Automatic"); break;
            case SERVICE_DEMAND_START: startType = QStringLiteral("Manual"); break;
            case SERVICE_DISABLED: startType = QStringLiteral("Disabled"); break;
            default: startType = QString::number(cfg->dwStartType); break;
            }
            if (cfg->dwStartType == SERVICE_AUTO_START) {
                DWORD n = 0;
                SERVICE_DELAYED_AUTO_START_INFO delayed{};
                if (QueryServiceConfig2W(svc.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                         reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &n)
                    && delayed.fDelayedAutostart)
                    startType += QStringLiteral(" (Delayed Start)");
            }
        }
    }

    out() << "Service:    MyRsm" << Qt::endl;
    out() << "State:      " << stateName(st.dwCurrentState);
    if (st.dwCurrentState != SERVICE_STOPPED)
        out() << " (pid " << st.dwProcessId << ")";
    out() << Qt::endl;
    out() << "Start type: " << startType << Qt::endl;
    out() << "Account:    " << account << Qt::endl;
    out() << "Binary:     " << binary << Qt::endl;
    if (st.dwCurrentState == SERVICE_STOPPED)
        out() << "Last exit:  " << exitCodes(st) << Qt::endl;
    return kOk;
}

int usage()
{
    err() << "usage: myrsm.exe service <install|uninstall|start|stop|status>\n"
             "  install    create (or update) the MyRsm service for this myrsm.exe\n"
             "  uninstall  stop and remove the service\n"
             "  start      start the service and wait until it is RUNNING\n"
             "  stop       stop the service and wait until it is STOPPED\n"
             "  status     show state, start type and binary path\n"
             "install/uninstall/start/stop need an elevated prompt.\n"
             "Exit codes: 0 ok, 1 failed, 2 usage, 3 not elevated, 4 not installed."
          << Qt::endl;
    return kUsage;
}

} // namespace

int run(const QStringList& args)
{
    if (args.size() != 1)
        return usage();
    const QString cmd = args.first().toLower();
    if (cmd == u"install") return install();
    if (cmd == u"uninstall") return uninstall();
    if (cmd == u"start") return start();
    if (cmd == u"stop") return stop();
    if (cmd == u"status") return status();
    return usage();
}

} // namespace service_control
