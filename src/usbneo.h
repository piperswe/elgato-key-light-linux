#pragma once

// USB HID transport for the Elgato Key Light Neo.
//
// The Neo speaks the same GET/PUT /elgato/* text-and-JSON API as the networked
// lights, tunnelled over USB HID in fixed 512-byte frames:
//
//   [0x02][frame_idx][total_frames][0x03][payload_len u16 LE][payload<=505][0x03][pad]
//
// Protocol reverse-engineered by Zameer Manji:
// https://zameermanji.com/blog/2026/3/4/elgato-key-light-neo-usb-protocol/
//
// The framing codec is exposed as free functions so it can be unit-tested
// without hardware. UsbNeoWorker owns the blocking hidapi I/O and lives on a
// dedicated thread; it serialises all exchanges by construction (a single
// thread), which is why the Python per-path lock is unnecessary here.

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include "keylight.h"

struct hid_device_;

namespace elg {

constexpr unsigned short kNeoVid = 0x0FD9;
constexpr unsigned short kNeoPid = 0x00A0;

constexpr int kFrameSize = 512;
constexpr int kMaxPayload = 505;  // 512 - 6-byte header - 1-byte terminator
constexpr int kReadTimeoutMs = 100;
constexpr int kReadDeadlineMs = 2000;

// Split payload into 512-byte frames, each prefixed with a 0x00 report id for
// hidapi (so each written buffer is 513 bytes).
QList<QByteArray> buildFrames(const QByteArray& payload);

struct ParsedFrame {
    bool ok = false;
    int index = 0;
    int total = 0;
    QByteArray payload;
};
ParsedFrame parseFrame(const QByteArray& frame);

// Extract the JSON object from a response, tolerating any status prefix.
bool decodeResponse(const QString& text, QJsonObject& out);

class UsbNeoWorker : public QObject {
    Q_OBJECT
public:
    explicit UsbNeoWorker(QObject* parent = nullptr);
    ~UsbNeoWorker() override;

public slots:
    void enumerate();
    void probeInfo(const QByteArray& path);
    void request(const QString& id, const QByteArray& path, elg::RequestKind kind,
                 const QJsonObject& body);

signals:
    void enumerated(const QList<QByteArray>& paths);
    void infoReady(const QByteArray& path, const QJsonObject& info);
    void infoFailed(const QByteArray& path);
    void requestDone(const QString& id, elg::RequestKind kind,
                     const QJsonObject& data);
    void requestFailed(const QString& id, elg::RequestKind kind);

private:
    // Perform "<method> <endpoint> [<json>]" and return the decoded reply.
    bool usbRequest(const QByteArray& path, const QByteArray& method,
                    const QByteArray& endpoint, const QJsonObject* body,
                    QJsonObject& out);
    bool exchange(const QByteArray& path, const QByteArray& request,
                  QString& response);
    bool readResponse(hid_device_* dev, QString& out);
};

}  // namespace elg
