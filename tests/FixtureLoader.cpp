#include "FixtureLoader.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <limits>

namespace {

bool readString(const QJsonObject& obj, const char* key, QString* out, int index, QString* error)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (v.isUndefined()) {
        out->clear();
        return true;
    }
    if (!v.isString()) {
        *error = QStringLiteral("item %1: %2 must be a string").arg(index).arg(QLatin1String(key));
        return false;
    }
    *out = v.toString();
    return true;
}

} // namespace

bool loadFixture(const QByteArray& json, QList<SnapshotRow>* rows, int* rawCount, QString* error)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("JSON parse error: ") + parseError.errorString();
        return false;
    }
    if (!doc.isArray()) {
        *error = QStringLiteral("fixture is not a top-level JSON array");
        return false;
    }
    const QJsonArray arr = doc.array();
    rows->clear();
    *rawCount = int(arr.size());
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr.at(i).isObject()) {
            *error = QStringLiteral("item %1 is not an object").arg(i);
            return false;
        }
        const QJsonObject obj = arr.at(i).toObject();
        SnapshotRow row;
        if (!readString(obj, "SensorApp", &row.app, i, error)
            || !readString(obj, "SensorClass", &row.cls, i, error)
            || !readString(obj, "SensorName", &row.name, i, error)
            || !readString(obj, "SensorUnit", &row.unit, i, error)
            || !readString(obj, "SensorValue", &row.valueStr, i, error))
            return false;
        const QJsonValue t = obj.value(QLatin1String("SensorUpdateTime"));
        if (t.isUndefined()) {
            row.updateTime = 0;
        } else {
            constexpr qint64 sentinel = std::numeric_limits<qint64>::min();
            const qint64 iv = t.isDouble() ? t.toInteger(sentinel) : sentinel;
            if (iv == sentinel) {
                *error = QStringLiteral("item %1: SensorUpdateTime must be an integer").arg(i);
                return false;
            }
            row.updateTime = iv;
        }
        rows->append(row);
    }
    return true;
}
