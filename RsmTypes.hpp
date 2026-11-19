#pragma once

// Plain data types shared between MyRsm (Windows / shared-memory side) and the
// Windows-free PromExporter module. Keep this header free of <windows.h> so the
// exporter can be unit-tested and reused by the fixture renderer.

#include <QList>
#include <QString>
#include <QtGlobal>

// One reading as served on the "/" JSON route, before any Prometheus-side
// cleaning. The string fields are exactly what toJson() puts in SensorApp /
// SensorClass / SensorName / SensorUnit / SensorValue.
struct SnapshotRow {
    QString app;        // SensorApp (always "HWiNFO" from shared memory)
    QString cls;        // SensorClass  = sensor.nameUser
    QString name;       // SensorName   = reading.nameUser
    QString unit;       // SensorUnit   = reading.unit
    QString valueStr;   // SensorValue  = simplifyValue(reading.value)
    qint64 updateTime = 0; // SensorUpdateTime = HWiNFO poll_time
    // Only filled when getMinMaxAvg=1 was requested (JSON route only).
    QString valueMinStr;
    QString valueMaxStr;
    QString valueAvgStr;
};

// Health of the shared-memory poll loop (see README "Freshness").
struct PollHealth {
    double lastAttemptTs = 0.0;     // unix seconds of the last poll tick
    double lastSuccessTs = 0.0;     // unix seconds of the last tick that saw fresh data
    double lastPollDurationS = 0.0; // duration of the last tick
    qint64 lastPollTime = 0;        // HWiNFO poll_time seen at the last fresh tick
    quint64 successfulPolls = 0;
    quint64 failedPolls = 0;
    int rawItemsLast = 0;           // readings parsed by the last successful tick (0 after a failure)
    bool lastReadOk = false;        // last (non mutex-timeout) tick succeeded
    QString lastError;              // human-readable reason for the last failure, "" when OK
};
