// Uppercut: what a person can type as a broker connection's address.
#include "../../app/backend/brokeraddress.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expect(const char* input, const char* want)
{
    QString error;
    const QUrl url = BrokerAddress::normalize(QString::fromUtf8(input), &error);
    const QString got = url.isValid() ? url.toString() : QStringLiteral("invalid");
    if (got != QString::fromUtf8(want)) {
        std::fprintf(stderr, "normalize(%s) = %s, want %s (%s)\n", input, qPrintable(got), want,
                     qPrintable(error));
        ++failures;
    }
    if (!url.isValid() && error.isEmpty()) {
        std::fprintf(stderr, "normalize(%s) gave no reason\n", input);
        ++failures;
    }
}

static void expectDisplay(const char* input, const char* want)
{
    QString error;
    const QString got = BrokerAddress::display(BrokerAddress::normalize(QString::fromUtf8(input), &error));
    if (got != QString::fromUtf8(want)) {
        std::fprintf(stderr, "display(%s) = %s, want %s\n", input, qPrintable(got), want);
        ++failures;
    }
}

static void expectWorkstation(const char* input, const char* want)
{
    QString error;
    const QString got = BrokerAddress::workstation(QString::fromUtf8(input), &error);
    const QString shown = got.isEmpty() ? QStringLiteral("invalid") : got;
    if (shown != QString::fromUtf8(want)) {
        std::fprintf(stderr, "workstation(%s) = %s, want %s (%s)\n", input, qPrintable(shown), want,
                     qPrintable(error));
        ++failures;
    }
    if (got.isEmpty() && error.isEmpty()) {
        std::fprintf(stderr, "workstation(%s) gave no reason\n", input);
        ++failures;
    }
}

int main()
{
    // A workstation reached directly: a name or address, optional port.
    expectWorkstation("nyc-lfx-003", "nyc-lfx-003");
    expectWorkstation("  NYC-LFX-003 ", "nyc-lfx-003");
    expectWorkstation("192.168.76.43", "192.168.76.43");
    expectWorkstation("192.168.76.43:28989", "192.168.76.43:28989");
    expectWorkstation("https://192.168.76.43", "invalid");
    expectWorkstation("nyc-lfx-003/path", "invalid");
    expectWorkstation("nyc lfx", "invalid");
    expectWorkstation("", "invalid");
    expectWorkstation("nyc-lfx-003:70000", "invalid");

    // A bare name is a broker on another computer: HTTPS on the default port.
    expect("nyc-plank.example.com", "https://nyc-plank.example.com:8443");
    expect("  NYC-Plank.Example.com/ ", "https://nyc-plank.example.com:8443");
    expect("nyc-plank.example.com:443", "https://nyc-plank.example.com:443");
    expect("https://nyc-plank.example.com", "https://nyc-plank.example.com:443");
    expect("https://nyc-plank.example.com:9443/", "https://nyc-plank.example.com:9443");
    expect("10.0.4.20", "https://10.0.4.20:8443");
    // This computer may use plain HTTP.
    expect("127.0.0.1", "http://127.0.0.1:8765");
    expect("localhost:9000", "http://127.0.0.1:9000");
    expect("http://127.0.0.1:8765", "http://127.0.0.1:8765");
    // Never plain HTTP to another computer, and nothing but an address.
    expect("http://nyc-plank.example.com", "invalid");
    expect("ftp://nyc-plank.example.com", "invalid");
    expect("https://user:pw@nyc-plank.example.com", "invalid");
    expect("https://nyc-plank.example.com/auth/login", "invalid");
    expect("nyc-plank.example.com?x=1", "invalid");
    expect("nyc plank", "invalid");
    expect("", "invalid");
    expect("nyc-plank.example.com:70000", "invalid");

    expectDisplay("nyc-plank.example.com", "nyc-plank.example.com");
    expectDisplay("nyc-plank.example.com:443", "nyc-plank.example.com:443");
    expectDisplay("127.0.0.1", "127.0.0.1");

    if (failures != 0) {
        std::fprintf(stderr, "broker address: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("broker address: pass");
    return EXIT_SUCCESS;
}
