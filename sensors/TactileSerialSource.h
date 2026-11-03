#pragma once
#include "TactileFrame.h"
#include "LatestSensorFrameBuffer.h"
#include <QThread>
#include <QPointer>
#include <QByteArray>
#include <functional>
#include <memory>

int runTactileSerialTests(const QString &path);
int runTactileVisualizationTests(const QString &path);

namespace tactile {
struct SerialSettings {
    QString port;
    int baud = 921600, slave = 1, type = 0, timeoutMs = 250, intervalMs = 10;
    bool verifyCrc = true;
    bool valid() const;
};
struct SerialSample {
    SerialSettings settings;
    Frame frame;
    QByteArray tx, rx;
    QVector<quint16> registers;
    QString error;
    bool portOpen = false, accepted = false, crcOk = false, verifyCrc = true, rangeOk = true;
    quint64 requests = 0, acceptedCount = 0, crcErrors = 0, timeouts = 0, protocolErrors = 0;
    qint64 receivedMs = 0;
    quint64 revision = 0;
};
quint16 tactileCrc(const QByteArray &bytes);
QByteArray tactileReadRequest(int slave);
// The entire RTU response is checked; no truncation, zero fill, or CRC repair.
SerialSample decodeTactileResponse(const QByteArray &, const SerialSettings &);
Frame serialFrame(const SerialSample &, const SerialSettings &, const QUuid &, quint64 generation,
                  quint64 sequence, qint64 monotonicNs);

// The conflicting manual addresses were tested: 0x465D persisted across power
// cycles; 0x466F returned exception 3. UI uses Decimal18013; the other candidate
// remains an explicit diagnostic choice, never an automatic fallback.
enum class SaveCandidate { None = 0, Decimal18013 = 1, Hex466F = 2 };
struct DeviceConfigResult {
    int requestedAddress = 0, configuredAddress = -1, configuredBaud = 0, respondingAddress = -1;
    int requestedBaud = 0, originalBaud = 0;
    SaveCandidate candidate = SaveCandidate::None;
    bool readOk = false, writeAttempted = false, writeAcknowledged = false;
    bool addressVerified = false, saveAttempted = false, saveAcknowledged = false;
    bool baudVerified = false;
    QString error;
    QVector<QByteArray> requests, responses;
};
using ConfigExchange = std::function<QByteArray(const QByteArray &)>;
// currentAddress may be the vendor public address 0 on a single-device bus.
// newAddress == 0 retains the stored ID; address writes only accept 1-127.
DeviceConfigResult configureTactileDevice(int currentAddress, int newAddress,
    SaveCandidate candidate, const ConfigExchange &exchange, int currentBaud = 0, int newBaud = 0);

struct SerialWorkerState;
// A worker owns QSerialPort and shared session state, independently of this GUI
// object. Even a driver blocked in close() cannot block stop() or destruction.
class TactileSerialSource final : public QObject {
    Q_OBJECT
public:
    explicit TactileSerialSource(QObject *parent = nullptr);
    ~TactileSerialSource() override { stop(); }
    // Refuse a new session until the previous worker has released the port.
    bool begin(const SerialSettings &settings,bool probeFirst=false);
    void stop();
    bool isRunning() const;
    bool stopping() const;
    // Bounded joining is only for diagnostics/tests; never called by the UI.
    bool wait(unsigned long timeoutMs = 2000);
    void setVerifyCrc(bool value);
    void setPaused(bool value);
    bool setIntervalMs(int value);
    bool takeLatest(SerialSample &sample);
    bool requestDeviceConfig(int newAddress = 0, SaveCandidate candidate = SaveCandidate::None, int newBaud = 0);
    bool configBusy() const;
    bool takeConfigResult(DeviceConfigResult &result);
    QUuid sessionId() const { return m_session; }
    quint64 generation() const { return m_generation; }
signals:
    void started();
    void finished();
private:
    friend int ::runTactileSerialTests(const QString &path);
    friend int ::runTactileVisualizationTests(const QString &path);
    // Offline regressions substitute replies while retaining the real worker,
    // decoder, pacing and shutdown path. Empty in the normal application.
    ConfigExchange m_exchangeForTests;
    static void run(const std::shared_ptr<SerialWorkerState> &state);
    std::shared_ptr<SerialWorkerState> m_state;
    QPointer<QThread> m_worker;
    QUuid m_session;
    quint64 m_generation = 0;
};
}
