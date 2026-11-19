#include "MyRsm.hpp"
#include <QDebug>
#include <QJsonObject>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QDateTime>
#include <QElapsedTimer>

namespace {
// In-memory layout of the 4-char ASCII signature HWiNFO writes, assuming a
// little-endian (x86/x64) process — matches how HWiNFO itself packs it.
constexpr DWORD kSignatureDead = DWORD('D') | (DWORD('E') << 8) | (DWORD('A') << 16) | (DWORD('D') << 24);
constexpr DWORD kMutexWaitMs = 50;

double nowSeconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

// HWiNFO's shared-memory mutex, held only for the duration of one read.
//
// Never kept open between ticks and never created by us: an open handle keeps
// the named mutex alive after HWiNFO exits, and a restarted HWiNFO then
// refuses to publish shared memory until every holder has closed it (seen in
// practice: HWiNFO only came back once MyRsm was stopped). Opening it per
// tick costs one syscall every poll interval.
class HwinfoMutexLock {
public:
    enum class State { Locked, Unavailable, Timeout };

    HwinfoMutexLock()
    {
        m_handle = OpenMutexA(SYNCHRONIZE, FALSE, HWiNFO_SENSORS_SM2_MUTEX);
        if (!m_handle) {
            m_error = GetLastError();
            m_state = State::Unavailable;
            return;
        }
        const DWORD r = WaitForSingleObject(m_handle, kMutexWaitMs);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            m_state = State::Locked;
        } else {
            m_state = State::Timeout;
        }
    }
    ~HwinfoMutexLock()
    {
        if (m_state == State::Locked)
            ReleaseMutex(m_handle);
        if (m_handle)
            CloseHandle(m_handle);
    }
    HwinfoMutexLock(const HwinfoMutexLock&) = delete;
    HwinfoMutexLock& operator=(const HwinfoMutexLock&) = delete;

    State state() const { return m_state; }
    DWORD error() const { return m_error; }

private:
    HANDLE m_handle = nullptr;
    State m_state = State::Unavailable;
    DWORD m_error = 0;
};
}

MyRsm::MyRsm(int pollIntervalMs, double freshnessTimeoutS, QObject* parent)
    : QObject(parent)
    , m_shm(QNativeIpcKey(HWiNFO_SENSORS_MAP_FILE_NAME2))
    , m_freshnessTimeoutS(freshnessTimeoutS)
{
    poll();
    connect(&m_timer, &QTimer::timeout, this, &MyRsm::poll);
    m_timer.setInterval(pollIntervalMs);
    m_timer.start();
}

MyRsm::~MyRsm()
{
    if (m_shm.isAttached())
        m_shm.detach();
}

void MyRsm::releaseSharedMemory()
{
    if (m_shm.isAttached()) {
        m_shm.detach();
        m_reloadSensors = true;
    }
}

MyRsm::TickResult MyRsm::readSharedMemory(QString* error)
{
    // Stay attached between polls instead of attaching/detaching every tick;
    // at 250ms that overhead adds up. Only re-attempt if we lost it or never
    // got it (e.g. HWiNFO wasn't running yet at startup, or we released it
    // because HWiNFO went away / the data went stale).
    if (!m_shm.isAttached()) {
        if (!m_shm.attach(QSharedMemory::ReadOnly)) {
            const QString reason = m_shm.errorString();
            *error = QStringLiteral("Unable to attach to shared memory: ") + reason;
            // Log every change of reason, not just the first failure.
            if (reason != m_lastAttachError) {
                qWarning() << "Unable to attach to HWiNFO shared memory:" << reason;
                m_lastAttachError = reason;
            }
            return TickResult::Failed;
        }
        if (!m_lastAttachError.isEmpty() || !m_everAttached)
            qInfo() << "Attached to HWiNFO shared memory.";
        m_lastAttachError.clear();
        m_everAttached = true;
        // A (re-)attach may be a different HWiNFO instance with a different
        // sensor list of the same length; re-read it.
        m_reloadSensors = true;
    }

    // HWiNFO documents this mutex as required around shared-memory access.
    const HwinfoMutexLock lock;
    switch (lock.state()) {
    case HwinfoMutexLock::State::Timeout:
        // HWiNFO is mid-write; skip this tick rather than risk a torn read.
        return TickResult::MutexTimeout;
    case HwinfoMutexLock::State::Unavailable:
        if (!m_mutexWarned) {
            qWarning() << "Unable to open the HWiNFO shared memory mutex (error" << lock.error()
                       << "); reads may be unsynchronized.";
            m_mutexWarned = true;
        }
        break;
    case HwinfoMutexLock::State::Locked:
        m_mutexWarned = false;
        break;
    }

    PHWiNFO_SENSORS_SHARED_MEM2 pHWiNFOMemory =
        (PHWiNFO_SENSORS_SHARED_MEM2) m_shm.data();

    if (pHWiNFOMemory->dwSignature == kSignatureDead) {
        *error = QStringLiteral("HWiNFO shared memory reports inactive (DEAD signature)");
        if (!m_wasDead) {
            qWarning() << "HWiNFO shared memory reports inactive; keeping last known readings.";
            m_wasDead = true;
        }
        // HWiNFO is shutting down: let go of its mapping right away so a
        // restarted HWiNFO starts from a clean slate. Re-attached next tick.
        releaseSharedMemory();
        return TickResult::Failed;
    }
    m_wasDead = false;

    m_lastPollTime = pHWiNFOMemory->poll_time;
    // loop through all available sensors
    if (m_reloadSensors || m_sensors.isEmpty() || m_sensors.size() != pHWiNFOMemory->dwNumSensorElements)
    {
        m_reloadSensors = false;
        m_sensors.clear();
        m_sensors.reserve(pHWiNFOMemory->dwNumSensorElements);
        for (DWORD dwSensor = 0; dwSensor < pHWiNFOMemory->dwNumSensorElements; dwSensor++)
        {
            PHWiNFO_SENSORS_SENSOR_ELEMENT sensor = (PHWiNFO_SENSORS_SENSOR_ELEMENT) ((BYTE*)pHWiNFOMemory +
                pHWiNFOMemory->dwOffsetOfSensorSection +
                (pHWiNFOMemory->dwSizeOfSensorElement * dwSensor));
            m_sensors.append(Sensor{
                QString::fromLocal8Bit((const char*)sensor->szSensorNameOrig, std::strlen(sensor->szSensorNameOrig)),
                QString::fromLocal8Bit((const char*)sensor->szSensorNameUser, std::strlen(sensor->szSensorNameUser))
            });
        }
    }
    // loop through all available readings
    m_readings.clear();
    m_readings.reserve(pHWiNFOMemory->dwNumReadingElements);
    for (DWORD dwReading = 0; dwReading < pHWiNFOMemory->dwNumReadingElements; dwReading++)
    {
        PHWiNFO_SENSORS_READING_ELEMENT reading = (PHWiNFO_SENSORS_READING_ELEMENT) ((BYTE*)pHWiNFOMemory +
            pHWiNFOMemory->dwOffsetOfReadingSection +
            (pHWiNFOMemory->dwSizeOfReadingElement * dwReading));
        m_readings.append(ReadingData{
            reading->dwSensorIndex,
            QString::fromLocal8Bit((const char*)reading->szLabelOrig, std::strlen(reading->szLabelOrig)),
            QString::fromLocal8Bit((const char*)reading->szLabelUser, std::strlen(reading->szLabelUser)),
            QString::fromLocal8Bit((const char*)reading->szUnit, std::strlen(reading->szUnit)),
            reading->Value,
            reading->ValueMin,
            reading->ValueMax,
            reading->ValueAvg
        });
    }

    // (The mutex is released when `lock` goes out of scope.)
    if (m_readings.isEmpty()) {
        *error = QStringLiteral("HWiNFO shared memory contains no readings");
        return TickResult::NoReadings;
    }
    return TickResult::Success;
}

void MyRsm::poll()
{
    QElapsedTimer timer;
    timer.start();
    QString error;
    const TickResult result = readSharedMemory(&error);
    const double now = nowSeconds();

    m_health.lastAttemptTs = now;
    m_health.lastPollDurationS = timer.nsecsElapsed() / 1e9;

    switch (result) {
    case TickResult::Success:
        ++m_health.successfulPolls;
        m_health.lastReadOk = true;
        m_health.lastError.clear();
        m_health.rawItemsLast = int(m_readings.size());
        // Only count the sample as fresh if HWiNFO has actually written a new
        // one since the last tick (or this is the very first read). A frozen
        // poll_time means HWiNFO stopped updating even if the mapping is alive.
        if (!m_hasFreshData || m_lastPollTime != m_health.lastPollTime) {
            m_hasFreshData = true;
            m_health.lastPollTime = m_lastPollTime;
            m_health.lastSuccessTs = now;
        }
        break;
    case TickResult::MutexTimeout:
        // A failed attempt, but on its own it doesn't change source_up or the
        // cached data: staleness only kicks in after the freshness window.
        ++m_health.failedPolls;
        break;
    case TickResult::NoReadings:
    case TickResult::Failed:
        ++m_health.failedPolls;
        m_health.lastReadOk = false;
        m_health.lastError = error;
        m_health.rawItemsLast = 0;
        break;
    }

    const bool stale = !m_hasFreshData || (now - m_health.lastSuccessTs) > m_freshnessTimeoutS;
    if (stale) {
        if (!m_wasStale) {
            qWarning() << "No fresh HWiNFO sample within" << m_freshnessTimeoutS
                       << "s; /metrics exports no sensor series until it recovers.";
            m_wasStale = true;
        }
        // Drop the mapping so the next tick re-attaches: picks up a restarted
        // HWiNFO and stops us keeping a dead instance's mapping alive.
        releaseSharedMemory();
    } else if (m_wasStale) {
        qInfo() << "HWiNFO data is fresh again.";
        m_wasStale = false;
    }

    if (result == TickResult::Success || result == TickResult::NoReadings)
        emit newData();
}


QString MyRsm::simplifyValue(double value) {
    auto val = (qFuzzyCompare(value, static_cast<int>(value)) ? QString::number(static_cast<int>(value)) : QString::number(value, 'f'));
    if (val.contains('.')) {
        val.remove(QRegularExpression("0+$"));   // remove trailing zeros
        val.remove(QRegularExpression("\\.$"));   // remove trailing dot if all decimals were zeros
    }
    return val;
}

namespace {
QString getFromFilters(const MyRsm::Filters& filters, const QString& key) {
    for (const auto& f : filters) {
        if (f.first == key) {
            return f.second;
        }
    }
    return {};
}
}

QList<MyRsm::SnapshotRow> MyRsm::buildSnapshot(const QList<Sensor>& sensors,
                                               const QList<ReadingData>& readings,
                                               qint64 pollTime,
                                               const Filters& filters)
{
    // Compile each filter regex once per request instead of once per reading
    // (previously up to 2x per reading) — with a few hundred readings that
    // was the dominant cost once requests start coming in every 250ms.
    QRegularExpression classRe;
    bool hasClassFilter = false;
    if (auto pattern = getFromFilters(filters, "classRegex"); !pattern.isEmpty()) {
        classRe.setPattern(pattern);
        hasClassFilter = true;
    }
    QRegularExpression nameRe;
    bool hasNameFilter = false;
    if (auto pattern = getFromFilters(filters, "nameRegex"); !pattern.isEmpty()) {
        nameRe.setPattern(pattern);
        hasNameFilter = true;
    }
    const bool includeMinMaxAvg = getFromFilters(filters, "getMinMaxAvg") == "1";

    QList<SnapshotRow> rows;
    rows.reserve(readings.size());
    for (const ReadingData& rd : readings) {
        // Guard against a reading that points past the sensor list (would
        // otherwise be an out-of-bounds access).
        if (rd.sensorIndex >= static_cast<unsigned int>(sensors.size()))
            continue;
        const QString& sensorName = sensors[rd.sensorIndex].nameUser;
        if ((hasClassFilter && !classRe.match(sensorName).hasMatch()) ||
            (hasNameFilter && !nameRe.match(rd.nameUser).hasMatch()))
            continue;
        SnapshotRow row;
        row.app = QStringLiteral("HWiNFO");
        row.cls = sensorName;
        row.name = rd.nameUser;
        row.unit = rd.unit;
        row.valueStr = simplifyValue(rd.value);
        row.updateTime = pollTime;
        if (includeMinMaxAvg) {
            row.valueMinStr = simplifyValue(rd.valueMin);
            row.valueMaxStr = simplifyValue(rd.valueMax);
            row.valueAvgStr = simplifyValue(rd.valueAvg);
        }
        rows.append(row);
    }
    return rows;
}

QJsonArray MyRsm::snapshotToJson(const QList<SnapshotRow>& rows, bool includeMinMaxAvg)
{
    QJsonArray arr;
    for (const SnapshotRow& row : rows) {
        QJsonObject obj;
        obj["SensorApp"] = row.app;
        obj["SensorClass"] = row.cls;
        obj["SensorName"] = row.name;
        obj["SensorUnit"] = row.unit;
        obj["SensorValue"] = row.valueStr;
        if (includeMinMaxAvg) {
            obj["SensorValueMin"] = row.valueMinStr;
            obj["SensorValueMax"] = row.valueMaxStr;
            obj["SensorValueAvg"] = row.valueAvgStr;
        }
        obj["SensorUpdateTime"] = row.updateTime;
        arr.append(obj);
    }
    return arr;
}

QList<MyRsm::SnapshotRow> MyRsm::snapshot(const Filters& filters) const
{
    return buildSnapshot(m_sensors, m_readings, m_lastPollTime, filters);
}

QJsonArray MyRsm::toJson(const Filters& filters) const
{
    return snapshotToJson(snapshot(filters), getFromFilters(filters, "getMinMaxAvg") == "1");
}
