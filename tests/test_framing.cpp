// Unit tests for the USB HID framing codec and unit conversions. No hardware
// required. Run via `meson test`.

#include <cstdio>

#include <QByteArray>
#include <QJsonObject>

#include "../src/keylight.h"
#include "../src/usbneo.h"

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++g_failures;                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace elg;

static void testSingleFrameLayout() {
    // A short request fits in one 513-byte buffer (0x00 report id + 512 frame).
    const QByteArray req = "GET /elgato/lights";
    const QList<QByteArray> frames = buildFrames(req);
    CHECK(frames.size() == 1);
    const QByteArray& f = frames.first();
    CHECK(f.size() == kFrameSize + 1);
    CHECK(static_cast<unsigned char>(f[0]) == 0x00);  // report id
    CHECK(static_cast<unsigned char>(f[1]) == 0x02);  // magic
    CHECK(static_cast<unsigned char>(f[2]) == 0x00);  // frame idx
    CHECK(static_cast<unsigned char>(f[3]) == 0x01);  // total frames
    CHECK(static_cast<unsigned char>(f[4]) == 0x03);  // request marker
    const int len = static_cast<unsigned char>(f[5]) |
                    (static_cast<unsigned char>(f[6]) << 8);
    CHECK(len == req.size());
    // Payload immediately follows the 6-byte header, then a 0x03 terminator.
    CHECK(f.mid(7, req.size()) == req);
    CHECK(static_cast<unsigned char>(f[7 + req.size()]) == 0x03);
}

static void testRoundTrip() {
    // A payload larger than one chunk must split, and parseFrame must reassemble
    // the original bytes in index order.
    QByteArray payload;
    for (int i = 0; i < kMaxPayload * 2 + 37; ++i) {
        payload.append(char('A' + (i % 26)));
    }
    const QList<QByteArray> frames = buildFrames(payload);
    CHECK(frames.size() == 3);

    QByteArray reassembled;
    int total = -1;
    for (const QByteArray& frame : frames) {
        const ParsedFrame p = parseFrame(frame);
        CHECK(p.ok);
        total = p.total;
        CHECK(p.index >= 0 && p.index < total);
    }
    CHECK(total == 3);
    // Join in index order.
    for (int idx = 0; idx < total; ++idx) {
        for (const QByteArray& frame : frames) {
            const ParsedFrame p = parseFrame(frame);
            if (p.index == idx) {
                reassembled += p.payload;
                break;
            }
        }
    }
    CHECK(reassembled == payload);
}

static void testParseToleratesStrippedReportId() {
    // Some hidapi backends drop the leading report-id byte on read. parseFrame
    // must accept a frame whose first byte is already the 0x02 magic.
    QByteArray frame = buildFrames("hi").first();
    CHECK(static_cast<unsigned char>(frame[0]) == 0x00);
    QByteArray stripped = frame.mid(1);  // drop report id
    const ParsedFrame p = parseFrame(stripped);
    CHECK(p.ok);
    CHECK(p.payload == QByteArray("hi"));
}

static void testParseRejectsGarbage() {
    CHECK(!parseFrame(QByteArray("\x01\x02\x03", 3)).ok);  // too short / bad magic
    CHECK(!parseFrame(QByteArray()).ok);
    // total == 0 and index >= total must be rejected (they would otherwise let
    // readResponse terminate early or misassemble).
    QByteArray zeroTotal("\x02\x00\x00\x03\x00\x00", 6);  // idx 0, total 0
    CHECK(!parseFrame(zeroTotal).ok);
    QByteArray badIdx("\x02\x02\x01\x03\x00\x00", 6);  // idx 2, total 1
    CHECK(!parseFrame(badIdx).ok);
}

static void testDecodeResponse() {
    QJsonObject out;
    // Responses carry a status byte / prefix before the JSON body.
    CHECK(decodeResponse(QStringLiteral("\x00 {\"numberOfLights\":1}"), out));
    CHECK(out.value(QStringLiteral("numberOfLights")).toInt() == 1);
    CHECK(!decodeResponse(QStringLiteral("no json here"), out));
}

static void testConversions() {
    // kelvin = round(1e6 / mired / 50) * 50.
    CHECK(miredToKelvin(200) == 5000);
    CHECK(miredToKelvin(kMiredMin) == 7000);  // 143 mireds
    CHECK(kelvinLabel(200) == QStringLiteral("5000 K"));
    CHECK(clampInt(150, 0, 100) == 100);
    CHECK(clampInt(-5, 0, 100) == 0);
}

static void testPutBody() {
    KeyLight light;
    light.numLights = 2;
    LightState state;
    state.on = true;
    state.brightness = 42;
    state.temperature = 250;
    const QJsonObject body = buildLightsBody(light, state);
    CHECK(body.value(QStringLiteral("numberOfLights")).toInt() == 2);
    const auto arr = body.value(QStringLiteral("lights")).toArray();
    CHECK(arr.size() == 2);  // state repeated numberOfLights times
    CHECK(arr.first().toObject().value(QStringLiteral("on")).toInt() == 1);
    CHECK(arr.first().toObject().value(QStringLiteral("brightness")).toInt() == 42);

    // With bad/missing device data (numLights 0) the body must stay internally
    // consistent: numberOfLights matches the (clamped) array length.
    KeyLight zero;
    zero.numLights = 0;
    const QJsonObject zeroBody = buildLightsBody(zero, state);
    CHECK(zeroBody.value(QStringLiteral("numberOfLights")).toInt() ==
          zeroBody.value(QStringLiteral("lights")).toArray().size());
}

int main() {
    testSingleFrameLayout();
    testRoundTrip();
    testParseToleratesStrippedReportId();
    testParseRejectsGarbage();
    testDecodeResponse();
    testConversions();
    testPutBody();
    if (g_failures == 0) {
        printf("all framing tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
