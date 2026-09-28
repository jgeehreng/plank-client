#pragma once

#include "outputtopology.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

// Each media feature owns its version and configuration. This envelope stays
// stable when optional features are added. The QUIC ALPN remains authoritative.
namespace MacMediaFeatures {
inline QString transport() { return QStringLiteral("plank-native/2"); }
inline QJsonArray required() { return {"desktop", "audio", "input"}; }
inline QStringList names() { return {"desktop", "audio", "input", "clipboard", "microphone", "camera"}; }
inline bool cameraSupported()
{
#if defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}
inline bool encodingSupported(const QString& mode)
{
    return mode == QLatin1String("hevc-10-420-videotoolbox") ||
           mode == QLatin1String("hevc-10-444-videotoolbox");
}
inline QJsonObject profile(const QString& name, const QString& mode)
{
    if (name == QLatin1String("desktop")) return {{"schema_version", 1}, {"encoding_mode", mode}};
    if (name == QLatin1String("audio")) return {{"schema_version", 1}, {"codec", "opus"},
        {"sample_rate", 48000}, {"channels", 2}, {"packet_duration_ms", 5}};
    if (name == QLatin1String("input")) return {{"schema_version", 1}, {"keyboard", true},
        {"mouse", "absolute"}, {"pen", "normalized"}};
    if (name == QLatin1String("clipboard")) return {{"schema_version", 1}};
    if (name == QLatin1String("microphone")) return {{"schema_version", 2}, {"codec", "opus"},
        {"sample_rate", 48000}, {"channels", 2}, {"packet_duration_ms", 10}};
    if (name == QLatin1String("camera")) return {{"schema_version", 1},
        {"codecs", QJsonArray {"h264-annex-b", "mjpeg"}}};
    return {};
}
inline QJsonArray profiles(const QString& name, const QString& mode)
{
    QJsonArray result;
// Timestamped capture requires PipeWire 1.0.5, so builds for older
// distributions define PLANK_NO_TIMED_MICROPHONE and offer SDL capture only.
#if defined(Q_OS_LINUX) && !defined(PLANK_NO_TIMED_MICROPHONE)
    if (name == QLatin1String("microphone")) {
        auto timed = profile(name, mode);
        timed.insert("schema_version", 3); timed.insert("capture_clock", "monotonic-ns");
        result.append(timed);
    }
#endif
    result.append(profile(name, mode));
    return result;
}
inline bool containsProfile(const QJsonValue& value, const QJsonObject& expected)
{
    if (!value.isObject() || value.toObject().size() > 32 || expected.isEmpty()) return false;
    const auto object = value.toObject();
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (object.value(it.key()) != it.value()) return false;
    return true;
}
inline bool validRequired(const QJsonValue& value)
{
    if (!value.isArray() || value.toArray().size() > 16) return false;
    const auto array = value.toArray();
    QStringList seen;
    for (const auto& item : array) {
        if (!item.isString() || !names().contains(item.toString()) || seen.contains(item.toString())) return false;
        seen.append(item.toString());
    }
    for (const auto& name : required()) if (!array.contains(name)) return false;
    return true;
}
struct Agreement {
    int launchSchema = 0;
    QString encodingMode;
    QJsonObject features;
    QJsonArray requiredFeatures = required();
    bool enabled(const QString& name) const { return features.value(name).isObject(); }
};
inline QJsonObject offer(const QString& mode)
{
    if (!encodingSupported(mode)) return {};
    QJsonObject features;
    for (const auto& name : names()) {
        const bool enabled = (name != QLatin1String("clipboard") || NvOutputTopology::PlatformClipboardSyncFeature) &&
                             (name != QLatin1String("camera") || cameraSupported());
        features.insert(name, enabled ? profiles(name, mode) : QJsonArray {});
    }
    return {{"schema_version", 1}, {"transport", transport()}, {"required_features", required()}, {"features", features}};
}
inline bool select(const QJsonObject& response, const QString& mode, Agreement& result)
{
    result = {};
    if (!encodingSupported(mode) || response.size() > 16 ||
        response.value("schema_version") != QJsonValue(1) || response.value("launch_schema") != QJsonValue(7) ||
        response.value("transport") != transport() || !validRequired(response.value("required_features")) ||
        !response.value("features").isObject() || response.value("features").toObject().size() > 32) return false;
    const auto features = response.value("features").toObject();
    const auto offered = offer(mode).value("features").toObject();
    QJsonObject selected;
    for (const auto& name : names()) {
        const auto value = features.value(name);
        if ((value.isNull() || value.isUndefined()) && !response.value("required_features").toArray().contains(name)) {
            selected.insert(name, QJsonValue::Null);
        } else {
            QJsonObject match;
            for (const auto& choice : offered.value(name).toArray())
                if (containsProfile(value, choice.toObject())) { match = choice.toObject(); break; }
            if (match.isEmpty()) return false;
            selected.insert(name, match);
        }
    }
    result = {7, mode, selected, response.value("required_features").toArray()};
    return true;
}
inline Agreement legacy(int schema, const QString& mode)
{
    if (schema < 4 || schema > 6 || !encodingSupported(mode)) return {};
    Agreement result {schema, mode, {}};
    for (const auto& name : names()) {
        const bool enabled = (name != QLatin1String("clipboard") || NvOutputTopology::PlatformClipboardSyncFeature) &&
            (name != QLatin1String("microphone") || schema >= 5) &&
            (name != QLatin1String("camera") || (schema >= 6 && cameraSupported()));
        result.features.insert(name, enabled ? QJsonValue(profile(name, mode)) : QJsonValue(QJsonValue::Null));
    }
    return result;
}
// Only pinned server information and an explicit missing negotiation endpoint
// permit this finite bridge. Unknown releases never trigger launch retries.
inline int legacySchema(const QString& version)
{
    static const QRegularExpression known(QStringLiteral("^(1\\.0\\.(156|157)|1\\.1\\.(001|002|003))(?:-[a-z0-9][a-z0-9-]*)?$"));
    const auto match = known.match(version);
    if (!match.hasMatch()) return 0;
    const auto base = match.captured(1);
    if (base == QLatin1String("1.1.003")) return 6;
    if (base == QLatin1String("1.1.002")) return 5;
    return 4;
}
} // namespace MacMediaFeatures
