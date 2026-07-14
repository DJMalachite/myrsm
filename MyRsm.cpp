#include "MyRsm.hpp"
#include <QDebug>
#include <QJsonObject>
#include <QCoreApplication>
#include <QRegularExpression>

namespace {
// In-memory layout of the 4-char ASCII signature HWiNFO writes, assuming a
// little-endian (x86/x64) process — matches how HWiNFO itself packs it.
constexpr DWORD kSignatureDead = DWORD('D') | (DWORD('E') << 8) | (DWORD('A') << 16) | (DWORD('D') << 24);
constexpr DWORD kMutexWaitMs = 50;
}

MyRsm::MyRsm(int pollIntervalMs, QObject* parent)
    : QObject(parent)
    , m_shm(QNativeIpcKey(HWiNFO_SENSORS_MAP_FILE_NAME2))
{
    // HWiNFO documents this mutex as required around shared-memory access.
    // Opened once and reused so each poll only pays for a wait/release, not a
    // fresh handle open every tick.
    m_hwinfoMutex = CreateMutexA(nullptr, FALSE, HWiNFO_SENSORS_SM2_MUTEX);
    if (!m_hwinfoMutex)
        m_hwinfoMutex = OpenMutexA(SYNCHRONIZE, FALSE, HWiNFO_SENSORS_SM2_MUTEX);
    if (!m_hwinfoMutex)
        qWarning() << "Unable to create/open the HWiNFO shared memory mutex; reads may be unsynchronized.";

    poll();
    connect(&m_timer, &QTimer::timeout, this, &MyRsm::poll);
    m_timer.setInterval(pollIntervalMs);
    m_timer.start();
}

MyRsm::~MyRsm()
{
    if (m_shm.isAttached())
        m_shm.detach();
    if (m_hwinfoMutex)
        CloseHandle(m_hwinfoMutex);
}

void MyRsm::poll()
{
    // Stay attached between polls instead of attaching/detaching every tick;
    // at 250ms that overhead adds up. Only re-attempt if we lost it or never
    // got it (e.g. HWiNFO wasn't running yet at startup).
    if (!m_shm.isAttached()) {
        if (!m_shm.attach(QSharedMemory::ReadOnly)) {
            if (!m_attachFailWarned) {
                qWarning() << "Unable to attach to shared memory:" << m_shm.errorString();
                m_attachFailWarned = true;
            }
            return;
        }
        m_attachFailWarned = false;
    }

    bool locked = false;
    if (m_hwinfoMutex) {
        DWORD waitResult = WaitForSingleObject(m_hwinfoMutex, kMutexWaitMs);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            // HWiNFO is mid-write; skip this tick rather than risk a torn read.
            return;
        }
        locked = true;
    }

    PHWiNFO_SENSORS_SHARED_MEM2 pHWiNFOMemory =
        (PHWiNFO_SENSORS_SHARED_MEM2) m_shm.data();

    if (pHWiNFOMemory->dwSignature == kSignatureDead) {
        if (locked)
            ReleaseMutex(m_hwinfoMutex);
        if (!m_wasDead) {
            qWarning() << "HWiNFO shared memory reports inactive; keeping last known readings.";
            m_wasDead = true;
        }
        return;
    }
    m_wasDead = false;

    m_lastPollTime = pHWiNFOMemory->poll_time;
    // loop through all available sensors
    if (m_sensors.isEmpty() || m_sensors.size() != pHWiNFOMemory->dwNumSensorElements)
    {
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

    if (locked)
        ReleaseMutex(m_hwinfoMutex);

    emit newData();
}


QString simplifyValue(double value) {
    auto val = (qFuzzyCompare(value, static_cast<int>(value)) ? QString::number(static_cast<int>(value)) : QString::number(value, 'f'));
    if (val.contains('.')) {
        val.remove(QRegularExpression("0+$"));   // remove trailing zeros
        val.remove(QRegularExpression("\\.$"));   // remove trailing dot if all decimals were zeros
    }
    return val;
}

QString getFromFilters(QList<std::pair<QString, QString>> filters, const QString& key) {
    for (const auto& f : filters) {
        if (f.first == key) {
            return f.second;
        }
    }
    return {};
}

QJsonArray MyRsm::toJson(QList<std::pair<QString, QString>> filters)
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

    QJsonArray arr;
    for (const ReadingData& rd : m_readings) {
        if ((hasClassFilter && !classRe.match(m_sensors[rd.sensorIndex].nameUser).hasMatch()) ||
            (hasNameFilter && !nameRe.match(rd.nameUser).hasMatch()))
            continue;
        QJsonObject obj;
        obj["SensorApp"] = "HWiNFO";
        obj["SensorClass"] = m_sensors[rd.sensorIndex].nameUser;
        obj["SensorName"] = rd.nameUser;
        obj["SensorUnit"] = rd.unit;
        obj["SensorValue"] = simplifyValue(rd.value);
        if (includeMinMaxAvg) {
            obj["SensorValueMin"] = simplifyValue(rd.valueMin);
            obj["SensorValueMax"] = simplifyValue(rd.valueMax);
            obj["SensorValueAvg"] = simplifyValue(rd.valueAvg);
        }
        obj["SensorUpdateTime"] = m_lastPollTime;
        arr.append(obj);
    }
    return arr;
}
