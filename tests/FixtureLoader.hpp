#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "RsmTypes.hpp"

// Loads a MyRSM "/" JSON payload (tests/fixtures/*.json) into SnapshotRows.
//
// Strict on types so a fixture can never be read differently here and by the
// Python reference: SensorApp/Class/Name/Unit/Value must be strings (or
// missing -> ""), SensorUpdateTime an integer (or missing -> 0), and every
// element an object. Returns false with *error set otherwise.
bool loadFixture(const QByteArray& json, QList<SnapshotRow>* rows, int* rawCount, QString* error);
