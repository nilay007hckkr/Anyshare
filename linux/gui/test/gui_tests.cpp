// Qt Test suite for the AnyShare desktop app. Runs headless:
//   QT_QPA_PLATFORM=offscreen ./build-linux/anyshare-gui-tests
// Set ANYSHARE_SCREENSHOT_DIR to also save screenshots of the window.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QListWidget>
#include <QMessageBox>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <random>
#include <thread>

#include "FakeEsp32.hpp"
#include "LanDiscovery.hpp"
#include "MainWindow.hpp"
#include "ConnectionInfoParser.hpp"
#include "NetworkEngine.hpp"
#include "SocketStream.hpp"
#include "TransferSession.hpp"
#include "Widgets.hpp"

using namespace anyshare;

namespace {

GuiOptions testOptions() {
    GuiOptions o;
    o.probeNfc = false;   // tests must not depend on attached hardware
    o.displayPort = "";   // no ESP32 bridge unless a test adds a fake one
    o.discoveryPort = 0;  // set per test; 0 = discovery answers nobody
    o.discoveryExtraTargets = QStringList{QStringLiteral("127.0.0.1")};
    return o;
}

QByteArray randomBytes(int size) {
    std::mt19937 rng(42);
    QByteArray data(size, '\0');
    for (char& c : data) c = static_cast<char>(rng());
    return data;
}

QString writeFile(const QString& dir, const QString& name, const QByteArray& data) {
    QFile f(dir + "/" + name);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    return f.fileName();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void rightClick(QWidget* w) { QTest::mouseClick(w, Qt::RightButton, Qt::NoModifier, QPoint(10, 10)); }

QMenu* openMenu(MainWindow& w) {
    QMenu* menu = w.findChild<QMenu*>("secretMenu");
    return menu && menu->isVisible() ? menu : nullptr;
}

QAction* actionContaining(QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions()) {
        if (a->text().contains(text)) return a;
    }
    return nullptr;
}

// Plain core-engine receiver on a background thread (the "other device").
class CoreReceiver {
public:
    // All interfaces, like a real receiver: discovery may report our LAN address, not 127.0.0.1.
    explicit CoreReceiver(const QString& dir) : listener_("0.0.0.0", 0) {
        options_.destinationDir = dir.toStdString();
        options_.acceptOffer = [](const protocol::Offer&) { return true; };
        thread_ = std::thread([this] {
            auto stream = listener_.accept(20000, &cancel_);
            if (stream) result_ = receiveFiles(*stream, options_, nullptr, &cancel_);
        });
    }
    ~CoreReceiver() {
        cancel_.cancel();
        if (thread_.joinable()) thread_.join();
    }
    uint16_t port() const { return listener_.port(); }
    ReceiveResult wait() {
        thread_.join();
        return result_;
    }

private:
    TcpListener listener_;
    ReceiveOptions options_;
    CancelToken cancel_;
    ReceiveResult result_;
    std::thread thread_;
};

// Clicks a button of the accept dialog as soon as it appears.
void answerAcceptDialog(const QString& button) {
    auto* timer = new QTimer();
    QObject::connect(timer, &QTimer::timeout, [timer, button] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || box->objectName() != "acceptDialog") return;
        for (QAbstractButton* b : box->buttons()) {
            if (b->text() == button) {
                timer->stop();
                timer->deleteLater();
                b->click();
                return;
            }
        }
    });
    timer->start(50);
}

void maybeScreenshot(QWidget& w, const QString& name) {
    QString dir = qEnvironmentVariable("ANYSHARE_SCREENSHOT_DIR");
    if (!dir.isEmpty()) w.grab().save(dir + "/" + name + ".png");
}

}  // namespace

class GuiTests : public QObject {
    Q_OBJECT

private slots:
    void init() { QSettings().clear(); }

    void secretMenuNeedsThreeQuickRightClicks() {
        MainWindow w(testOptions());
        w.show();
        maybeScreenshot(w, "main_window");
        SecretCorner* corner = w.secretCorner();

        rightClick(corner);
        rightClick(corner);
        QVERIFY(!openMenu(w));   // two clicks: nothing

        QTest::mouseClick(corner, Qt::LeftButton);   // a left click resets the count
        rightClick(corner);
        QVERIFY(!openMenu(w));

        QTest::qWait(SecretCorner::kWindowMs + 200);   // too slow: starts over
        rightClick(corner);
        rightClick(corner);
        QVERIFY(!openMenu(w));
        rightClick(corner);
        QMenu* menu = openMenu(w);
        QVERIFY(menu);
        QVERIFY(actionContaining(menu, "NFC-style demo"));
        QVERIFY(!actionContaining(menu, "Hardware NFC"));   // no reader: option is not offered
        menu->close();
    }

    void menuSwitchesToDemoMode() {
        MainWindow w(testOptions());
        w.show();
        QVERIFY(!w.demoBadge()->isVisible());
        for (int i = 0; i < 3; ++i) rightClick(w.secretCorner());
        QMenu* menu = openMenu(w);
        QVERIFY(menu);
        actionContaining(menu, "NFC-style demo")->trigger();
        QCOMPARE(w.mode(), TransferMode::DemoTap);
        QVERIFY(w.demoBadge()->isVisible());
        QVERIFY(w.demoBadge()->text().contains("SIMULATED"));
        QVERIFY(w.tapCard()->isVisible());
        maybeScreenshot(w, "demo_mode");
        QCOMPARE(QSettings().value("mode").toInt(), static_cast<int>(TransferMode::DemoTap));   // remembered
    }

    void noNfcTextWithoutReader() {
        MainWindow w(testOptions());
        w.show();
        QVERIFY(!w.nfcStatusLabel()->isVisible());
        for (QLabel* label : w.findChildren<QLabel*>()) {
            if (!label->isVisible()) continue;
            QString text = label->text().toLower();
            QVERIFY2(!text.contains("not supported") && !text.contains("unsupported") && !text.contains("unavailable"),
                     qPrintable(label->text()));
            QVERIFY2(!text.contains("nfc"), qPrintable("NFC mentioned without a reader: " + label->text()));
        }
        // A reader appears: the line is shown, and Hardware NFC becomes the active mode.
        w.setNfcReaderForTest(true, "pn532_uart:/dev/ttyUSB0");
        QVERIFY(w.nfcStatusLabel()->isVisible());
        QVERIFY(w.nfcStatusLabel()->text().contains("ready"));
        QVERIFY(w.findChild<QLabel*>("modeLabel")->text().contains("Hardware NFC"));
        for (int i = 0; i < 3; ++i) rightClick(w.secretCorner());
        QVERIFY(openMenu(w) && actionContaining(openMenu(w), "Hardware NFC"));
        openMenu(w)->close();
        w.setNfcReaderForTest(false, "unplugged");
        QVERIFY(!w.nfcStatusLabel()->isVisible());
    }

    void rejectsBadAddress() {
        MainWindow w(testOptions());
        QTemporaryDir src;
        w.setFiles({writeFile(src.path(), "a.bin", "abc")});
        w.sendTo("not-an-address");
        QVERIFY(!w.isSending());
        QVERIFY(w.statusLabel()->text().contains("HOST:PORT"));
    }

    void sendsFilesOverTcp() {
        QTemporaryDir src, dst;
        QByteArray big = randomBytes(3 * 1024 * 1024 + 17), small = "hello from the GUI";
        QStringList files = {writeFile(src.path(), "big.bin", big), writeFile(src.path(), "note.txt", small)};
        CoreReceiver receiver(dst.path());

        MainWindow w(testOptions());
        w.setFiles(files);
        w.sendTo(QString("127.0.0.1:%1").arg(receiver.port()));
        QVERIFY(w.isSending());
        QTRY_VERIFY_WITH_TIMEOUT(!w.isSending(), 30000);
        QVERIFY2(w.statusLabel()->text().startsWith("Sent"), qPrintable(w.statusLabel()->text()));
        QVERIFY(w.transportLabel()->text().contains("manual address"));

        ReceiveResult r = receiver.wait();
        QVERIFY2(r.status == TransferStatus::Success, toString(r.status));
        QCOMPARE(readFile(dst.path() + "/big.bin"), big);
        QCOMPARE(readFile(dst.path() + "/note.txt"), small);
    }

    void demoTapSendsForReal() {
        QTemporaryDir src, dst;
        QByteArray data = randomBytes(256 * 1024);
        CoreReceiver receiver(dst.path());

        MainWindow w(testOptions());
        w.show();
        w.setMode(TransferMode::DemoTap);
        w.setDemoPartner(QString("127.0.0.1:%1").arg(receiver.port()));
        w.setFiles({writeFile(src.path(), "demo.bin", data)});
        w.simulateTap();
        QVERIFY(!w.isSending());   // nothing is sent until the tap animation finishes
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("Sen"), 30000);   // "Sent" / "Send failed"
        QTRY_VERIFY_WITH_TIMEOUT(!w.isSending(), 30000);
        QVERIFY2(w.statusLabel()->text().startsWith("Sent"), qPrintable(w.statusLabel()->text()));
        QVERIFY2(w.transportLabel()->text().contains("SIMULATED tap"), qPrintable(w.transportLabel()->text()));
        ReceiveResult r = receiver.wait();
        QVERIFY2(r.status == TransferStatus::Success, toString(r.status));
        QCOMPARE(readFile(dst.path() + "/demo.bin"), data);
    }

    void sendProgressReachesEsp32Display() {
        FakeEsp32 esp(true);
        QTemporaryDir src, dst;
        CoreReceiver receiver(dst.path());
        GuiOptions o = testOptions();
        o.displayPort = QString::fromStdString(esp.path());
        MainWindow w(o);
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(w.displayConnected(), 5000);
        QTRY_VERIFY(w.nfcStatusLabel()->isVisible());   // ESP32 present: the NFC line appears
        QVERIFY(w.nfcStatusLabel()->text().contains("ESP32"));

        w.setFiles({writeFile(src.path(), "IMG_0042.jpg", randomBytes(1024 * 1024))});
        w.sendTo(QString("127.0.0.1:%1").arg(receiver.port()));
        QTRY_VERIFY_WITH_TIMEOUT(!w.isSending(), 30000);
        QVERIFY2(receiver.wait().status == TransferStatus::Success, "transfer failed");
        QTRY_VERIFY_WITH_TIMEOUT(!esp.lines().empty() && esp.lines().back().rfind("DONE|", 0) == 0, 3000);
        bool sawProgress = false;
        for (const auto& l : esp.lines()) sawProgress |= l.find("XFER|SEND|") == 0 && l.find("|IMG_0042.jpg|Image|1/1") != std::string::npos;
        QVERIFY(sawProgress);
        QCOMPARE(QString::fromStdString(esp.lines().back()), QString("DONE|OK|Sent 1 file(s): IMG_0042.jpg"));
    }

    void sendToDeviceFoundByName() {
        QTemporaryDir src, dst;
        CoreReceiver receiver(dst.path());
        anyshare::discovery::Responder responder("Almas Phone", receiver.port(), "android", 0);
        QVERIFY(responder.running());
        GuiOptions o = testOptions();
        o.discoveryPort = responder.udpPort();
        MainWindow w(o);
        w.show();
        w.setFiles({writeFile(src.path(), "hello.txt", "found you")});

        // Pick the found device in the Send dialog, as a user would.
        QTimer::singleShot(0, [] {
            auto* timer = new QTimer();
            QObject::connect(timer, &QTimer::timeout, [timer] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog || dialog->objectName() != "targetDialog") return;
                auto* list = dialog->findChild<QListWidget*>("peerList");
                for (int i = 0; list && i < list->count(); ++i) {
                    if (list->item(i)->text().startsWith("Almas Phone")) {
                        timer->stop();
                        timer->deleteLater();
                        list->setCurrentRow(i);
                        dialog->accept();
                        return;
                    }
                }
            });
            timer->start(50);
        });
        w.setMode(TransferMode::BluetoothFallback);
        QTest::mouseClick(w.findChild<QPushButton*>("sendButton"), Qt::LeftButton);   // opens the dialog (modal)
        QTRY_VERIFY_WITH_TIMEOUT(!w.isSending(), 30000);
        QVERIFY2(w.statusLabel()->text().startsWith("Sent"), qPrintable(w.statusLabel()->text()));
        QVERIFY2(w.transportLabel()->text().contains("Wi-Fi discovery: 'Almas Phone'"), qPrintable(w.transportLabel()->text()));
        QVERIFY(receiver.wait().status == TransferStatus::Success);
        QCOMPARE(readFile(dst.path() + "/hello.txt"), QByteArray("found you"));
    }

    void nearbyListSendsOnClick() {
        QTemporaryDir src, dst;
        CoreReceiver receiver(dst.path());
        anyshare::discovery::Responder responder("Almas Phone", receiver.port(), "android", 0);
        GuiOptions o = testOptions();
        o.discoveryPort = responder.udpPort();
        MainWindow w(o);
        w.resize(940, 720);
        w.show();
        w.setFiles({writeFile(src.path(), "IMG_2042.jpg", randomBytes(300 * 1024)),
                    writeFile(src.path(), "notes.pdf", randomBytes(40 * 1024))});
        w.refreshNearby();
        QTRY_VERIFY_WITH_TIMEOUT(w.nearbyList()->count() == 1, 5000);
        QListWidgetItem* item = w.nearbyList()->item(0);
        QVERIFY(item->text().startsWith("Almas Phone\nPhone · "));
        maybeScreenshot(w, "send_tab");

        QTest::mouseClick(w.nearbyList()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.nearbyList()->visualItemRect(item).center());
        QTRY_VERIFY_WITH_TIMEOUT(!w.isSending() && w.statusLabel()->text().startsWith("Sen"), 30000);
        QVERIFY2(w.statusLabel()->text().startsWith("Sent"), qPrintable(w.statusLabel()->text()));
        QVERIFY(w.transportLabel()->text().contains("Wi-Fi discovery: 'Almas Phone'"));
        QVERIFY(receiver.wait().status == TransferStatus::Success);
        QCOMPARE(QFileInfo(dst.path() + "/IMG_2042.jpg").size(), qint64(300 * 1024));
        // The animation showed this computer -> the phone, and ended with a check.
        QCOMPARE(w.transferStage()->peerName(), QString("Almas Phone"));
        QVERIFY(!w.transferStage()->peerIsComputer());
        QVERIFY(w.transferStage()->sending());
        QVERIFY(!w.transferStage()->running());
        maybeScreenshot(w, "send_done");
    }

    void receiveTabShowsVisibility() {
        QTemporaryDir dst;
        GuiOptions o = testOptions();
        o.receiveDir = dst.path();
        o.port = 0;
        o.autoAccept = true;
        uint16_t udp;
        {
            anyshare::discovery::Responder probe("x", 1, "linux", 0);
            udp = probe.udpPort();
        }
        o.discoveryPort = udp;
        MainWindow w(o);
        w.resize(940, 720);
        w.show();
        w.tabs()->setCurrentIndex(1);
        QVERIFY(w.startReceiving());
        QVERIFY(w.findChild<QWidget*>("receiveBeacon")->isVisible());
        QVERIFY(w.findChild<QLabel*>("statePill")->text() == "Idle");

        // A real sender: the state turns to Receiving/Done and the file shows up under "Received files".
        ConnectionInfo info;
        info.ip = "127.0.0.1";
        info.port = w.listeningPort();
        QTemporaryDir src;
        info.files.push_back({writeFile(src.path(), "holiday.mp4", randomBytes(2 * 1024 * 1024)).toStdString(), "", 0, ""});
        std::atomic<bool> done{false};
        std::thread sender([&] {
            NetworkEngine engine;
            engine.setSenderName("Almas Phone");
            engine.connectToHost(info);
            done = true;
        });
        for (int waited = 0; !done.load() && waited < 30000; waited += 20) QTest::qWait(20);
        sender.join();
        auto* recent = w.findChild<QListWidget*>("recentList");
        QTRY_VERIFY_WITH_TIMEOUT(recent->count() == 1, 5000);
        QVERIFY(recent->item(0)->text().startsWith("holiday.mp4"));
        QTRY_COMPARE_WITH_TIMEOUT(w.findChild<QLabel*>("statePill")->text(), QString("Done"), 5000);
        QCOMPARE(w.transferStage()->peerName(), QString("Almas Phone"));   // receiving: phone -> this computer
        QVERIFY(!w.transferStage()->sending());
        maybeScreenshot(w, "receive_tab");
        w.stopReceiving();
    }

    void transferAnimationRuns() {   // the strip itself: packets while running, a mark when done
        TransferStage stage;
        stage.resize(300, 92);
        stage.setPeer("NILAY", true);
        stage.start(false);
        QVERIFY(stage.running());
        QTest::qWait(400);
        maybeScreenshot(stage, "stage_receiving");
        stage.setPeer("Almas Phone", false);
        stage.start(true);
        QTest::qWait(300);
        maybeScreenshot(stage, "stage_sending");
        stage.finish(true);
        QVERIFY(!stage.running());
        maybeScreenshot(stage, "stage_done");
    }

    void receiverIsFoundByName() {
        QTemporaryDir dst;
        GuiOptions o = testOptions();
        o.receiveDir = dst.path();
        o.port = 0;
        uint16_t udp;
        {
            anyshare::discovery::Responder probe("x", 1, "linux", 0);   // borrow a free UDP port
            udp = probe.udpPort();
        }
        o.discoveryPort = udp;
        MainWindow w(o);
        QVERIFY(w.startReceiving());
        auto peers = anyshare::discovery::discover(600, udp, {"127.0.0.1"});
        QCOMPARE(peers.size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(peers[0].name), w.deviceName());
        QCOMPARE(peers[0].port, w.listeningPort());
        w.stopReceiving();
        QVERIFY(anyshare::discovery::discover(400, udp, {"127.0.0.1"}).empty());   // gone after Stop
    }

    void nfcTapsShowAndSend() {
        FakeEsp32 esp(true);
        QTemporaryDir src, dst;
        CoreReceiver receiver(dst.path());
        GuiOptions o = testOptions();
        o.displayPort = QString::fromStdString(esp.path());
        MainWindow w(o);
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(w.displayConnected(), 5000);
        QVERIFY(w.nfcStatusLabel()->text().contains("NFC reader: ESP32"));

        // A plain card: the UI says so (ID + type), nothing is sent.
        esp.emitLine("TAG uid=7672F906 sak=0x08");
        esp.emitLine("CARD plain NFC card/tag, not a phone (the reader works)");
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().contains("7672F906"), 3000);
        QVERIFY(w.statusLabel()->text().contains("MIFARE Classic 1K"));
        QVERIFY(w.nfcStatusLabel()->text().contains("Last tap"));
        QVERIFY(!w.isSending());

        // A phone in receive mode without files selected: ask for files first.
        QString payload = QString("{\"v\":1,\"transport\":\"tcp\",\"name\":\"Almas Phone\",\"ip\":\"127.0.0.1\","
                                  "\"port\":%1,\"token\":\"0123456789abcdef\"}").arg(receiver.port());
        esp.emitLine("TAG uid=08A1B2C3 sak=0x20");
        esp.emitLine("PAYLOAD " + payload.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().contains("Add files"), 3000);
        QVERIFY(!w.isSending());

        // With files: the tap itself starts a real transfer, labelled as a physical NFC tap.
        w.setFiles({writeFile(src.path(), "tap.jpg", randomBytes(200 * 1024))});
        esp.emitLine("TAG uid=08A1B2C3 sak=0x20");
        esp.emitLine("PAYLOAD " + payload.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("Sen"), 30000);
        QVERIFY2(w.statusLabel()->text().startsWith("Sent"), qPrintable(w.statusLabel()->text()));
        QVERIFY2(w.transportLabel()->text().contains("physical NFC tap (ESP32 + Si512 reader)"),
                 qPrintable(w.transportLabel()->text()));
        QVERIFY(receiver.wait().status == TransferStatus::Success);

        // A failed tap is reported, not hidden.
        esp.emitLine("ERROR phone answered SW 6A82: open AnyShare and tap Receive first");
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("NFC tap failed"), 3000);
    }

    // Phone -> computer with a reader-only module: the phone publishes role "send" and listens,
    // the ESP32 reads it, and the computer connects and fetches the files (pull).
    void phoneTapWithFilesIsFetched() {
        FakeEsp32 esp(true);
        QTemporaryDir src, dst;
        GuiOptions o = testOptions();
        o.displayPort = QString::fromStdString(esp.path());
        o.receiveDir = dst.path();
        MainWindow w(o);   // not in Receive mode, and autoAccept off: the tap token alone authorises
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(w.displayConnected(), 5000);

        const std::string token = "fedcba9876543210";
        QByteArray photo = randomBytes(300 * 1024);
        std::vector<OutgoingFile> files{
            anyshare::outgoingFileFromPath(writeFile(src.path(), "holiday.jpg", photo).toStdString())};
        TcpListener phone("127.0.0.1", 0);
        std::atomic<int> sessions{0};
        std::atomic<bool> phoneOk{false};
        std::string receiverName;
        std::thread phoneThread([&] {
            auto s = phone.accept(20000);
            if (!s) return;
            ++sessions;
            SendOptions opts;
            opts.senderName = "Lakshya's M34";
            opts.token = token;
            PullSendResult r = sendFilesOnPull(*s, files, opts);
            receiverName = r.receiverName;
            phoneOk = r.status == TransferStatus::Success;
        });

        QString payload = QString("{\"v\":1,\"role\":\"send\",\"transport\":\"tcp\",\"name\":\"Lakshya's M34\","
                                  "\"ip\":\"127.0.0.1\",\"port\":%1,\"token\":\"%2\"}")
                              .arg(phone.port()).arg(QString::fromStdString(token));
        esp.emitLine("TAG uid=08A1B2C3 sak=0x20");
        esp.emitLine("PAYLOAD " + payload.toStdString());
        esp.emitLine("PAYLOAD " + payload.toStdString());   // held on the module: read again, fetched once
        QTRY_VERIFY_WITH_TIMEOUT(w.isPulling() || QFile::exists(dst.path() + "/holiday.jpg"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isPulling(), 30000);
        phoneThread.join();
        QVERIFY(phoneOk.load());
        QCOMPARE(sessions.load(), 1);
        QCOMPARE(QString::fromStdString(receiverName), w.deviceName());
        QFile got(dst.path() + "/holiday.jpg");
        QVERIFY(got.open(QIODevice::ReadOnly));
        QVERIFY(got.readAll() == photo);
        QVERIFY2(w.statusLabel()->text().startsWith("Received 1 file"), qPrintable(w.statusLabel()->text()));
        QVERIFY2(w.transportLabel()->text().contains("fetched after an NFC tap"), qPrintable(w.transportLabel()->text()));

        // The same tap read once more after it finished: nothing happens.
        esp.emitLine("PAYLOAD " + payload.toStdString());
        QTest::qWait(300);
        QVERIFY(!w.isPulling());

        // A phone that is gone: reported, and a new tap can retry.
        uint16_t closedPort;
        {
            TcpListener probe("127.0.0.1", 0);
            closedPort = probe.port();
        }
        QString gone = QString("{\"v\":1,\"role\":\"send\",\"transport\":\"tcp\",\"name\":\"Gone\",\"ip\":\"127.0.0.1\","
                               "\"port\":%1,\"token\":\"1111222233334444\"}").arg(closedPort);
        esp.emitLine("PAYLOAD " + gone.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().contains("Could not reach the phone"), 15000);
        QVERIFY(!w.isPulling());
    }

    void receivingServesHandoverToEsp32() {
        FakeEsp32 esp(true);
        QTemporaryDir src, dst;
        GuiOptions o = testOptions();
        o.displayPort = QString::fromStdString(esp.path());
        o.receiveDir = dst.path();
        o.port = 0;
        MainWindow w(o);   // autoAccept stays off: only the tap token may skip the question
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(w.displayConnected(), 5000);
        esp.clear();
        QVERIFY(w.startReceiving());

        // The module gets this laptop's handover (address + one-time token) for card mode.
        std::string served;
        for (int i = 0; i < 100 && served.empty(); ++i) {
            for (const auto& l : esp.lines()) if (l.rfind("CARD|{", 0) == 0) served = l.substr(5);
            QTest::qWait(20);
        }
        QVERIFY(!served.empty());
        ConnectionInfo info = anyshare::parseConnectionInfo(served);
        QCOMPARE(info.port, w.listeningPort());
        QVERIFY(info.sessionToken.size() >= 16);
        QVERIFY(w.nfcStatusLabel()->text().contains("acting as this computer's card"));

        // The firmware reports that a phone read it ...
        esp.emitLine("SERVED handover read by a phone");
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().contains("a phone tapped the reader"), 3000);

        // ... and that phone sends with the token: accepted without asking. (On a machine with a
        // Bluetooth adapter the handover says "rfcomm" with Wi-Fi as the alternative; this fake phone uses Wi-Fi.)
        info.transport = "tcp";
        info.ip = "127.0.0.1";
        info.files.push_back({writeFile(src.path(), "from_tap.jpg", randomBytes(100 * 1024)).toStdString(), "", 0, ""});
        std::atomic<bool> done{false}, ok{false};
        std::thread phone([&] {
            NetworkEngine engine;
            engine.setSenderName("Almas Phone");
            engine.connectToHost(info);
            ok = engine.lastTransferSucceeded();
            done = true;
        });
        for (int waited = 0; !done.load() && waited < 30000; waited += 20) QTest::qWait(20);
        phone.join();
        QVERIFY(ok.load());
        QVERIFY(QFile::exists(dst.path() + "/from_tap.jpg"));
        QVERIFY2(w.transportLabel()->text().contains("authorised by tap token"), qPrintable(w.transportLabel()->text()));

        esp.clear();
        w.stopReceiving();
        QTRY_VERIFY_WITH_TIMEOUT(!esp.lines().empty() && esp.lines().front() == "CARD|OFF", 3000);
    }

    void nfcCardWriteAndRead() {
        FakeEsp32 esp(true);
        QTemporaryDir src, dst;
        GuiOptions o = testOptions();
        o.displayPort = QString::fromStdString(esp.path());
        o.receiveDir = dst.path();
        o.port = 0;
        MainWindow w(o);
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(w.displayConnected(), 5000);

        // The Send tab offers the card; one small file goes to the firmware as hex.
        w.setFiles({writeFile(src.path(), "hello.txt", "Hello card")});
        w.refreshNearby();
        QTRY_VERIFY_WITH_TIMEOUT(w.nearbyList()->count() >= 1 && !w.nearbySearching(), 5000);
        QListWidgetItem* card = w.nearbyList()->item(0);
        QVERIFY(card->text().startsWith("NFC card"));
        esp.clear();
        QTest::mouseClick(w.nearbyList()->viewport(), Qt::LeftButton, Qt::NoModifier, w.nearbyList()->visualItemRect(card).center());
        QTRY_VERIFY_WITH_TIMEOUT(!esp.lines().empty() && esp.lines().back() == "WRITE|hello.txt|48656C6C6F2063617264", 3000);
        QVERIFY(w.statusLabel()->text().contains("Tap a MIFARE Classic 1K card"));
        esp.emitLine("TAG uid=7672F906 sak=0x08");
        esp.emitLine("WPROG 100");
        esp.emitLine("WRITTEN 10 bytes hello.txt");
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("Written to NFC card 7672F906"), 3000);
        QCOMPARE(w.findChild<QLabel*>("statePill")->text(), QString("Done"));

        // Too much for a card: refused before anything is sent.
        w.setFiles({writeFile(src.path(), "big.bin", randomBytes(4000))});
        esp.clear();
        w.refreshNearby();
        QTRY_VERIFY_WITH_TIMEOUT(w.nearbyList()->count() >= 1 && !w.nearbySearching(), 5000);
        card = w.nearbyList()->item(0);
        QTest::mouseClick(w.nearbyList()->viewport(), Qt::LeftButton, Qt::NoModifier, w.nearbyList()->visualItemRect(card).center());
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("Too large for an NFC card"), 3000);
        QVERIFY(esp.lines().empty() || esp.lines().back().rfind("WRITE|", 0) != 0);

        // Card -> laptop: in Receive mode a tapped card's file is saved (never overwriting).
        QVERIFY(w.startReceiving());
        esp.emitLine("TAG uid=7672F906 sak=0x08");
        esp.emitLine("CARDDATA hello.txt|48656C6C6F2063617264");
        QTRY_COMPARE_WITH_TIMEOUT(readFile(dst.path() + "/hello.txt"), QByteArray("Hello card"), 3000);
        esp.emitLine("CARDDATA hello.txt|48656C6C6F2063617264");   // same card tapped again: no copy
        QTRY_VERIFY_WITH_TIMEOUT(w.statusLabel()->text().startsWith("Already received from this card"), 3000);
        QVERIFY(!QFile::exists(dst.path() + "/hello (1).txt"));
        esp.emitLine("CARDDATA hello.txt|4E657720636F6E74656E74");   // new content, same name: kept side by side
        QTRY_COMPARE_WITH_TIMEOUT(readFile(dst.path() + "/hello (1).txt"), QByteArray("New content"), 3000);
        QCOMPARE(w.findChild<QListWidget*>("recentList")->count(), 2);

        // Double-click a received text file: an in-app preview shows it.
        auto* recent = w.findChild<QListWidget*>("recentList");
        w.tabs()->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(recent->isVisible() && recent->visualItemRect(recent->item(0)).height() > 0, 3000);
        QListWidgetItem* first = recent->item(0);
        emit recent->itemDoubleClicked(first);   // what a double-click on the row emits
        QDialog* preview = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((preview = w.findChild<QDialog*>("previewDialog")) != nullptr, 3000);
        auto* text = preview->findChild<QPlainTextEdit*>("previewText");
        QVERIFY(text);
        QCOMPARE(text->toPlainText(), QString("New content"));
        preview->close();
        w.stopReceiving();
    }

    void receivesAfterAcceptAndHonoursDecline() {
        QTemporaryDir src, dst;
        GuiOptions o = testOptions();
        o.receiveDir = dst.path();
        o.port = 0;
        MainWindow w(o);
        w.show();
        QVERIFY(w.startReceiving());
        QVERIFY(w.isReceiving());
        QVERIFY(w.listeningPort() != 0);

        auto sendOnce = [&](const QString& name, const QByteArray& data) {
            ConnectionInfo info;
            info.ip = "127.0.0.1";
            info.port = w.listeningPort();
            info.files.push_back({writeFile(src.path(), name, data).toStdString(), "", 0, ""});
            std::atomic<bool> done{false}, ok{false};
            std::thread sender([&] {
                NetworkEngine engine;
                engine.setSenderName("test-sender");
                engine.setConnectTimeoutSeconds(5);
                engine.connectToHost(info);
                ok = engine.lastTransferSucceeded();
                done = true;
            });
            for (int waited = 0; !done.load() && waited < 30000; waited += 20) QTest::qWait(20);
            sender.join();
            return ok.load();
        };

        QByteArray accepted = randomBytes(512 * 1024);
        answerAcceptDialog("Accept");
        QVERIFY(sendOnce("accepted.bin", accepted));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(dst.path() + "/accepted.bin"), accepted, 5000);

        answerAcceptDialog("Decline");
        QVERIFY(!sendOnce("declined.bin", "nope"));
        QVERIFY(!QFile::exists(dst.path() + "/declined.bin"));

        QVERIFY(w.isReceiving());   // keeps listening between transfers
        w.stopReceiving();
        QVERIFY(!w.isReceiving());
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("AnyShare-tests");   // keep the user's settings untouched
    QApplication::setApplicationName("AnyShare-tests");
    GuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "gui_tests.moc"
