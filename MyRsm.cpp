#include "MyRsm.hpp"
#include <QDebug>
#include <QJsonObject>
#include <QCoreApplication>
#include <QRegularExpression>

MyRsm::MyRsm(QObject* parent)
    : QObject(parent)
    , m_shm(QNativeIpcKey(HWiNFO_SENSORS_MAP_FILE_NAME2))
{
    poll();
    connect(&m_timer, &QTimer::timeout, this, &MyRsm::poll);
    m_timer.setInterval(1000);
    m_timer.start();
}

void MyRsm::poll()
{
    if (!m_shm.attach(QSharedMemory::ReadOnly)) {
        qWarning() << "Unable to attach to shared memory:" << m_shm.errorString();
        return;
    }
    PHWiNFO_SENSORS_SHARED_MEM2 pHWiNFOMemory = 
        (PHWiNFO_SENSORS_SHARED_MEM2) m_shm.data();
    m_lastPollTime = pHWiNFOMemory->poll_time;
    // loop through all available sensors
    if (m_sensors.isEmpty() || m_sensors.size() != pHWiNFOMemory->dwNumSensorElements)
    {
        m_sensors.clear();
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
    m_shm.detach();
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

bool filter(QList<std::pair<QString, QString>> filters, const QString& key, const QString& value) {
    auto re = getFromFilters(filters, key);
    if (!re.isEmpty()) {
        QRegularExpression reClass(re);
        return reClass.match(value).hasMatch();
    }
    return true;
}

QJsonArray MyRsm::toJson(QList<std::pair<QString, QString>> filters)
{
    QJsonArray arr;
    for (const ReadingData& rd : m_readings) {
        if (!filter(filters, "classRegex", m_sensors[rd.sensorIndex].nameUser) ||
            !filter(filters, "nameRegex", rd.nameUser))
            continue;
        QJsonObject obj;
        obj["SensorApp"] = "HWiNFO";
        obj["SensorClass"] = m_sensors[rd.sensorIndex].nameUser;
        obj["SensorName"] = rd.nameUser;
        obj["SensorUnit"] = rd.unit;
        obj["SensorValue"] = simplifyValue(rd.value);
        if (getFromFilters(filters, "getMinMaxAvg") == "1") {
            obj["SensorValueMin"] = simplifyValue(rd.valueMin);
            obj["SensorValueMax"] = simplifyValue(rd.valueMax);
            obj["SensorValueAvg"] = simplifyValue(rd.valueAvg);
        }
        obj["SensorUpdateTime"] = m_lastPollTime;
        arr.append(obj);
    }
    return arr;
}
