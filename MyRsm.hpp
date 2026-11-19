#pragma once

#include <QObject>
#include <QSharedMemory>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <utility>
#include "hwisenssm2.h"
#include "RsmTypes.hpp"

class MyRsm : public QObject
{
    Q_OBJECT
public:
    class Sensor {
        public:
            QString nameOrig;
            QString nameUser;
    };

    class ReadingData {
        public:
            unsigned int sensorIndex;
            QString nameOrig;
            QString nameUser;
            QString unit;
            double value;
            double valueMin;
            double valueMax;
            double valueAvg;
    };

    using SnapshotRow = ::SnapshotRow;
    using Filters = QList<std::pair<QString, QString>>;

    explicit MyRsm(int pollIntervalMs = 250, double freshnessTimeoutS = 20.0, QObject* parent = nullptr);
    ~MyRsm();
    void poll();

    // Readings after the "/" route's classRegex / nameRegex filters, with
    // values already formatted by simplifyValue(). Both the JSON route and
    // /metrics are rendered from this, so they cannot drift apart.
    QList<SnapshotRow> snapshot(const Filters& filters = {}) const;
    QJsonArray toJson(const Filters& filters = {}) const;
    PollHealth health() const { return m_health; }

    // Pure helpers behind snapshot()/toJson(); static so they can be tested
    // with a fixed reading list and no shared memory.
    static QList<SnapshotRow> buildSnapshot(const QList<Sensor>& sensors,
                                            const QList<ReadingData>& readings,
                                            qint64 pollTime,
                                            const Filters& filters);
    static QJsonArray snapshotToJson(const QList<SnapshotRow>& rows, bool includeMinMaxAvg);
    static QString simplifyValue(double value);
signals:
    void newData();
private:
    // Success: parsed with >=1 reading. NoReadings: parsed but empty (served on
    // "/" as before, but counted as a failed poll, like the connector did).
    enum class TickResult { Success, NoReadings, MutexTimeout, Failed };
    TickResult readSharedMemory(QString* error);
    // Detach from HWiNFO's mapping (re-attached on the next tick).
    void releaseSharedMemory();

    QSharedMemory m_shm;
    QTimer m_timer;
    QList<Sensor> m_sensors;
    QList<ReadingData> m_readings;
    qint64 m_lastPollTime = 0;
    // HWiNFO's mutex is opened per read, never held between ticks (see
    // HwinfoMutexLock in MyRsm.cpp).
    QString m_lastAttachError;
    bool m_everAttached = false;
    bool m_mutexWarned = false;
    bool m_wasDead = false;
    bool m_reloadSensors = true;
    bool m_hasFreshData = false;
    bool m_wasStale = false;
    double m_freshnessTimeoutS = 20.0;
    PollHealth m_health;
};
