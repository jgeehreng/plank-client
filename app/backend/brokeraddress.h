#pragma once

#include <QString>
#include <QUrl>

// Uppercut: turns what a person types into a broker connection's address
// into the broker's base URL. Passwords cross this connection, so a broker on
// another computer is always HTTPS; plain HTTP is only for a broker on this
// computer.
//
//   nyc-plank.example.com          -> https://nyc-plank.example.com:8443
//   nyc-plank.example.com:443      -> https://nyc-plank.example.com:443
//   https://nyc-plank.example.com  -> https://nyc-plank.example.com:443
//   127.0.0.1 or localhost         -> http://127.0.0.1:8765
namespace BrokerAddress {

constexpr int DefaultHttpsPort = 8443;
constexpr int DefaultLoopbackPort = 8765;

inline bool isLoopbackHost(const QString& host)
{
    const QString lower = host.toLower();
    return lower == QLatin1String("127.0.0.1") || lower == QLatin1String("localhost") ||
            lower == QLatin1String("::1");
}

inline bool validHostName(const QString& host)
{
    if (host.isEmpty() || host.size() > 253) {
        return false;
    }
    for (const QChar c : host) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                (u >= '0' && u <= '9') || u == '.' || u == '-' || u == ':';
        if (!ok) {
            return false;
        }
    }
    return !host.startsWith(QLatin1Char('.')) && !host.startsWith(QLatin1Char('-'));
}

// Returns an invalid QUrl and sets *error when the address can't be used.
inline QUrl normalize(const QString& input, QString* error)
{
    QString text = input.trimmed();
    if (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    if (text.isEmpty()) {
        if (error) *error = QStringLiteral("Enter the broker's address.");
        return QUrl();
    }
    const bool hasScheme = text.contains(QStringLiteral("://"));
    QUrl url(hasScheme ? text : QStringLiteral("x://") + text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
            !(url.path().isEmpty() || url.path() == QLatin1String("/")) ||
            url.hasQuery() || url.hasFragment() || !validHostName(url.host())) {
        if (error) *error = QStringLiteral("That isn't a valid broker address.");
        return QUrl();
    }
    const bool loopback = isLoopbackHost(url.host());
    QString scheme = url.scheme().toLower();
    if (!hasScheme) {
        scheme = loopback ? QStringLiteral("http") : QStringLiteral("https");
    }
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
        if (error) *error = QStringLiteral("That isn't a valid broker address.");
        return QUrl();
    }
    if (scheme == QLatin1String("http") && !loopback) {
        if (error) *error = QStringLiteral("A broker on another computer must use https.");
        return QUrl();
    }
    int port = url.port(-1);
    if (port == -1) {
        if (!hasScheme) {
            port = loopback ? DefaultLoopbackPort : DefaultHttpsPort;
        } else {
            port = scheme == QLatin1String("https") ? 443 : DefaultLoopbackPort;
        }
    }
    if (port < 1 || port > 65535) {
        if (error) *error = QStringLiteral("That isn't a valid broker address.");
        return QUrl();
    }
    QUrl result;
    result.setScheme(scheme);
    result.setHost(loopback && url.host().toLower() == QLatin1String("localhost") ?
                       QStringLiteral("127.0.0.1") : url.host().toLower());
    result.setPort(port);
    if (error) error->clear();
    return result;
}

// How a saved address is shown: the host, plus the port when it isn't the default.
inline QString display(const QUrl& url)
{
    const bool defaultPort = (url.scheme() == QLatin1String("https") && url.port() == DefaultHttpsPort) ||
            (url.scheme() == QLatin1String("http") && url.port() == DefaultLoopbackPort);
    return defaultPort ? url.host() : QStringLiteral("%1:%2").arg(url.host()).arg(url.port());
}

}
