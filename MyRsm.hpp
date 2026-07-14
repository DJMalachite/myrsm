#include <QObject>
#include <QSharedMemory>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include "hwisenssm2.h"

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

    explicit MyRsm(int pollIntervalMs = 250, QObject* parent = nullptr);
    ~MyRsm();
    void poll();
    QJsonArray toJson(QList<std::pair<QString, QString>> filters = {});
signals:
    void newData();
private:
    QSharedMemory m_shm;
    QTimer m_timer;
    QList<Sensor> m_sensors;
    QList<ReadingData> m_readings;
    qint64 m_lastPollTime = 0;
    HANDLE m_hwinfoMutex = nullptr;
    bool m_attachFailWarned = false;
    bool m_wasDead = false;
};
