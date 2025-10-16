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

    MyRsm(QObject* parent = nullptr);
    void poll();
    QJsonArray toJson(QList<std::pair<QString, QString>> filters = {});
signals:
    void newData();
private:
    QSharedMemory m_shm;
    QTimer m_timer;
    HWiNFO_SENSORS_SHARED_MEM2 m_data;
    QList<Sensor> m_sensors;
    QList<ReadingData> m_readings;
    qint64 m_lastPollTime = 0;
};
