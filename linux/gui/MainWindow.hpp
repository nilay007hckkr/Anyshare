#pragma once

// AnyShare desktop app (Qt Widgets). Same engine as the CLI; all network and
// NFC work runs on worker threads, and every event is also logged to the
// terminal the app was started from.
#include <QMainWindow>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ByteStream.hpp"
#include "Esp32Bridge.hpp"
#include "LanDiscovery.hpp"
#include "INfcHandover.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QTimer;
class QToolButton;
class ReceiveBeacon;
class TransferStage;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class NetworkEngine;
class SecretCorner;
class QMenu;
class TapCard;

// Same three modes as the Android app.
enum class TransferMode { HardwareNfc, BluetoothFallback, DemoTap };

struct GuiOptions {
    bool probeNfc = true;          // look for a PN532 at start-up (tests turn this off)
    bool autoAccept = false;       // accept offers without asking (--yes; tests)
    bool startReceiving = false;   // --receive
    QString receiveDir;            // empty = saved setting or ~/Downloads/AnyShare
    int port = -1;                 // -1 = saved setting or 8080; 0 = any free port
    QString displayPort = "auto";  // ESP32 OLED bridge: "auto", a device path, or "" = off
    int discoveryPort = anyshare::discovery::kPort;   // UDP port for finding receivers by name
    QStringList discoveryExtraTargets;                 // e.g. "127.0.0.1" in tests
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(const GuiOptions& options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    // --- actions (also used by tests) ---------------------------------------
    TransferMode mode() const { return mode_; }
    void setMode(TransferMode mode);
    void setFiles(const QStringList& paths);
    void sendTo(const QString& address, const QString& foundName = {});   // HOST:PORT or BT address
    std::vector<anyshare::discovery::Peer> discoverNow(int timeoutMs);    // blocking; receivers by name
    QString deviceName() const;
    void setDemoPartner(const QString& address);
    void simulateTap();                           // demo mode: same as clicking the tap card
    bool startReceiving();
    void stopReceiving();
    bool isReceiving() const { return receiving_; }
    bool isSending() const { return sending_; }
    bool isPulling() const { return pulling_; }
    uint16_t listeningPort() const { return listeningPort_; }

    // --- exposed for tests ---------------------------------------------------
    SecretCorner* secretCorner() const { return secretCorner_; }
    QLabel* nfcStatusLabel() const { return nfcStatus_; }
    QLabel* transportLabel() const { return transportLabel_; }
    QLabel* statusLabel() const { return status_; }
    QLabel* demoBadge() const { return demoBadge_; }
    TapCard* tapCard() const { return tapCard_; }
    void setNfcReaderForTest(bool ready, const QString& name);
    bool displayConnected() const { return display_ && display_->connected(); }
    QListWidget* nearbyList() const { return nearbyList_; }
    QTabWidget* tabs() const { return tabs_; }
    TransferStage* transferStage() const { return stage_; }
    void refreshNearby();   // background search; results fill the "Nearby devices" list
    bool nearbySearching() const { return nearbySearching_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;   // drop files anywhere to send them
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void applyMode();
    void showSecretMenu();
    void onSendPressed();
    void onCancelPressed();
    void chooseFiles();
    // "Send to" dialog: receivers found by name (live) + a manual address field.
    bool askForAddress(const QString& title, QString& address, QString* foundName = nullptr);
    void startSend(std::unique_ptr<INfcHandover> handover, const QString& howLabel);
    void finishSend(bool ok, const QString& message, const QString& transport);
    void probeNfcAsync();
    void onNfcProbed(bool ready, const QString& detail);
    void connectDisplayAsync(const QString& port);
    void refreshNfcLine();
    // A card or phone tapped on the ESP32 reader. A phone in receive mode is a
    // real NFC handover: with files selected, the transfer starts right away.
    void onNfcEvent(const anyshare::bridge::NfcEvent& event);
    // A phone tapped with files to send (payload role "send"): connect to it and fetch them.
    // This is how phone -> computer works with a reader that cannot act as an NFC card.
    void startPull(const ConnectionInfo& info, const QString& phoneName);
    void finishPull(bool ok, const QString& message, const QString& transport, const std::string& token);
    void writeToNfcCard();                    // Send tab: "NFC card" entry
    void saveCardFile(const QString& name, const QByteArray& data);
    void dropDisplay();
    bool askAcceptOffer(const QString& sender, const QStringList& lines, const QString& total);
    void setBusy(bool busy);
    void log(const QString& line, bool terminalOnly = false);
    TransferMode effectiveMode() const;
    QString modeTitle(TransferMode mode) const;
    void post(std::function<void()> fn);   // run on the GUI thread
    QString receiveDir() const;

    // One colour language for the app, the OLED and the ESP32 LED.
    enum class Activity { Idle, Sending, Receiving, Done, Failed };
    void setActivity(Activity activity, const QString& text);
    void showProgress(int percent, const QString& bytes, const QString& rate, const QString& eta);
    void onNearbyClicked(QListWidgetItem* item);
    void addRecent(const QString& name, const QString& path);
    bool peerLooksLikeComputer(const QString& nameOrHost) const;   // from the nearby-devices list
    void setNextPeer(const QString& name, bool isComputer);           // shown by the transfer animation
    // Double-click on a received file: text and images open in a preview inside the app;
    // anything else goes to the system's default app (on WSL: the Windows one).
    void openReceivedFile(const QString& path);
    bool openExternally(const QString& path);
    void applyStyle();

    GuiOptions options_;
    QSettings settings_;
    TransferMode mode_ = TransferMode::HardwareNfc;
    bool bluetoothUsable_ = false;
    bool nfcReady_ = false;
    QString nfcDeviceName_;
    QStringList files_;

    // widgets
    SecretCorner* secretCorner_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QLabel* demoBadge_ = nullptr;
    TapCard* tapCard_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* dirLabel_ = nullptr;
    QSpinBox* portBox_ = nullptr;
    QComboBox* receiveTransport_ = nullptr;
    QPushButton* receiveButton_ = nullptr;
    QLabel* receiveInfo_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* speed_ = nullptr;
    QLabel* transportLabel_ = nullptr;
    QLabel* nfcStatus_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QWidget* logBox_ = nullptr;
    QToolButton* detailsButton_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* filesSummary_ = nullptr;
    QLabel* filesEmpty_ = nullptr;
    QListWidget* nearbyList_ = nullptr;
    QLabel* nearbyHint_ = nullptr;
    QPushButton* refreshNearbyButton_ = nullptr;
    QTimer* nearbyTimer_ = nullptr;
    ReceiveBeacon* beacon_ = nullptr;
    QListWidget* recentList_ = nullptr;
    QLabel* statePill_ = nullptr;
    QLabel* currentFile_ = nullptr;
    TransferStage* stage_ = nullptr;
    QString nextPeerName_;
    bool nextPeerIsComputer_ = false;
    QMenu* secretMenu_ = nullptr;

    // sending
    std::thread sendThread_;
    std::atomic<bool> sending_{false};
    CancelToken sendCancel_;
    std::mutex engineMutex_;
    NetworkEngine* engine_ = nullptr;

    // fetching from a phone that tapped with files (pull)
    std::thread pullThread_;
    std::atomic<bool> pulling_{false};
    CancelToken pullCancel_;
    std::string lastPulledToken_;   // the ESP32 reads a phone several times per tap: fetch once

    // receiving
    std::thread receiveThread_;     // Wi-Fi (TCP) listener
    std::thread receiveThreadBt_;   // Bluetooth (RFCOMM) listener, when an adapter is present
    bool receiving_ = false;
    CancelToken receiveCancel_;
    uint16_t listeningPort_ = 0;
    int receiveGeneration_ = 0;   // ignores late UI updates from an earlier session
    std::unique_ptr<anyshare::discovery::Responder> responder_;   // lets senders find us by name
    std::thread discoverThread_;   // the "Send to" dialog's search
    std::thread nearbyThread_;     // the Send tab's automatic search
    bool nearbySearching_ = false;

    std::thread probeThread_;
    bool probing_ = false;

    // ESP32 OLED bridge (optional); worker threads hold their own shared_ptr copy.
    std::shared_ptr<anyshare::bridge::Esp32Bridge> display_;
    std::thread displayThread_;
    bool displayConnecting_ = false;
    QString lastTap_;
    std::string servedHandover_;
    bool cardWritePending_ = false;   // what the ESP32 hands to phones while we receive (card mode)
};
