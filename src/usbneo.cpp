#include "usbneo.h"

#include <algorithm>

#include <QElapsedTimer>
#include <QJsonDocument>

#include <hidapi/hidapi.h>

namespace elg {

QList<QByteArray> buildFrames(const QByteArray& payload) {
    QList<QByteArray> chunks;
    for (int i = 0; i < payload.size(); i += kMaxPayload) {
        chunks.append(payload.mid(i, kMaxPayload));
    }
    if (chunks.isEmpty()) {
        chunks.append(QByteArray());
    }

    const int total = chunks.size();
    QList<QByteArray> frames;
    for (int idx = 0; idx < total; ++idx) {
        const QByteArray& chunk = chunks[idx];
        QByteArray frame;
        frame.append(char(0x02));
        frame.append(char(idx));
        frame.append(char(total));
        frame.append(char(0x03));
        frame.append(char(chunk.size() & 0xFF));
        frame.append(char((chunk.size() >> 8) & 0xFF));
        frame.append(chunk);
        frame.append(char(0x03));
        if (frame.size() < kFrameSize) {
            frame.append(QByteArray(kFrameSize - frame.size(), '\0'));
        }
        frame.prepend(char(0x00));  // leading report-id byte for hidapi
        frames.append(frame);
    }
    return frames;
}

ParsedFrame parseFrame(const QByteArray& frame) {
    ParsedFrame result;
    QByteArray data = frame;
    // Depending on the hidapi backend a read may keep the leading report-id byte.
    if (data.size() > 1 && static_cast<unsigned char>(data[0]) == 0x00 &&
        static_cast<unsigned char>(data[1]) == 0x02) {
        data = data.mid(1);
    }
    // Requests carry 0x03 in byte 3; responses put a status byte there, so only
    // the 0x02 magic is a reliable header check.
    if (data.size() < 6 || static_cast<unsigned char>(data[0]) != 0x02) {
        return result;
    }
    result.index = static_cast<unsigned char>(data[1]);
    result.total = static_cast<unsigned char>(data[2]);
    // A frame index must fall within a positive frame count.
    if (result.total <= 0 || result.index < 0 || result.index >= result.total) {
        return result;
    }
    const int length = static_cast<unsigned char>(data[4]) |
                       (static_cast<unsigned char>(data[5]) << 8);
    result.payload = data.mid(6, length);
    if (result.payload.size() != length) {
        return result;  // truncated
    }
    result.ok = true;
    return result;
}

bool decodeResponse(const QString& text, QJsonObject& out) {
    const int start = text.indexOf(QLatin1Char('{'));
    if (start < 0) {
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(text.mid(start).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    out = doc.object();
    return true;
}

UsbNeoWorker::UsbNeoWorker(QObject* parent) : QObject(parent) {
    hid_init();
}

UsbNeoWorker::~UsbNeoWorker() {
    hid_exit();
}

void UsbNeoWorker::enumerate() {
    QList<QByteArray> paths;
    hid_device_info* devs = hid_enumerate(kNeoVid, kNeoPid);
    for (hid_device_info* cur = devs; cur; cur = cur->next) {
        if ((cur->interface_number == 0 || cur->interface_number == -1) &&
            cur->path) {
            paths.append(QByteArray(cur->path));
        }
    }
    hid_free_enumeration(devs);
    std::sort(paths.begin(), paths.end());
    emit enumerated(paths);
}

void UsbNeoWorker::probeInfo(const QByteArray& path) {
    QJsonObject info;
    if (usbRequest(path, "GET", kEpInfo, nullptr, info)) {
        emit infoReady(path, info);
    } else {
        emit infoFailed(path);
    }
}

void UsbNeoWorker::request(const QString& id, const QByteArray& path,
                           RequestKind kind, const QJsonObject& body) {
    QJsonObject out;
    bool ok = false;
    if (kind == RequestKind::Put) {
        ok = usbRequest(path, "PUT", kEpLights, &body, out);
    } else {  // State
        ok = usbRequest(path, "GET", kEpLights, nullptr, out);
    }
    if (ok) {
        emit requestDone(id, kind, out);
    } else {
        emit requestFailed(id, kind);
    }
}

bool UsbNeoWorker::usbRequest(const QByteArray& path, const QByteArray& method,
                              const QByteArray& endpoint, const QJsonObject* body,
                              QJsonObject& out) {
    QByteArray request = method + " " + endpoint;
    if (body) {
        request += " ";
        request += QJsonDocument(*body).toJson(QJsonDocument::Compact);
    }
    QString response;
    if (!exchange(path, request, response)) {
        return false;
    }
    return decodeResponse(response, out);
}

bool UsbNeoWorker::exchange(const QByteArray& path, const QByteArray& request,
                            QString& response) {
    hid_device* dev = hid_open_path(path.constData());
    if (!dev) {
        return false;
    }
    bool ok = true;
    const QList<QByteArray> frames = buildFrames(request);
    for (const QByteArray& frame : frames) {
        if (hid_write(dev, reinterpret_cast<const unsigned char*>(frame.constData()),
                      frame.size()) < 0) {
            ok = false;
            break;
        }
    }
    if (ok) {
        ok = readResponse(dev, response);
    }
    hid_close(dev);
    return ok;
}

bool UsbNeoWorker::readResponse(hid_device_* dev, QString& out) {
    QHash<int, QByteArray> frames;
    int total = -1;
    QElapsedTimer timer;
    timer.start();
    unsigned char buf[kFrameSize + 1];
    while (timer.elapsed() < kReadDeadlineMs) {
        const int n = hid_read_timeout(dev, buf, sizeof(buf), kReadTimeoutMs);
        if (n <= 0) {
            continue;
        }
        const ParsedFrame frame =
            parseFrame(QByteArray(reinterpret_cast<const char*>(buf), n));
        if (!frame.ok) {
            return false;
        }
        // Fix the frame count from the first frame; a later frame disagreeing
        // means the stream is out of sync, so fail rather than misassemble.
        if (total < 0) {
            total = frame.total;
        } else if (frame.total != total) {
            return false;
        }
        frames.insert(frame.index, frame.payload);
        if (static_cast<int>(frames.size()) >= total) {
            break;
        }
    }
    if (total < 0 || static_cast<int>(frames.size()) < total) {
        return false;
    }
    QByteArray joined;
    for (int i = 0; i < total; ++i) {
        joined += frames.value(i);
    }
    out = QString::fromUtf8(joined);
    return true;
}

}  // namespace elg
