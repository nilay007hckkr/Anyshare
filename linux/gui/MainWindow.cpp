#include "MainWindow.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QImageReader>
#include <QProcess>
#include <QScrollArea>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <cstdio>
#include <future>

#include "Handovers.hpp"
#include "JsonHandover.hpp"
#include "LinuxCommon.hpp"
#include "NetworkEngine.hpp"
#include "Protocol.hpp"
#include "SocketStream.hpp"
#include "TransferSession.hpp"
#include "Widgets.hpp"

#ifdef ANYSHARE_HAVE_BLUEZ
#include "BluetoothRfcomm.hpp"
#endif
#ifdef ANYSHARE_HAVE_LIBNFC
#include "Pn532NfcHandover.hpp"
#endif

using namespace anyshare;

namespace {

const char* kDemoHandover = "SIMULATED tap (demo mode, no NFC)";

QString q(const std::string& s) { return QString::fromStdString(s); }

// Forwards engine callbacks (worker thread) to the GUI thread, throttled.
class RelayObserver : public IProgressObserver {
public:
    struct Hooks {
        std::function<void(int percent, const QString& bytes, const QString& rate, const QString& eta)> progress;
        std::function<void(const QString& line)> log;
        std::function<void(const QString& name, const QString& path)> fileSaved;   // optional (receiver)
        std::function<void()> started;                                             // optional: data starts flowing
    };

    RelayObserver(QString verb, Hooks hooks) : verb_(std::move(verb)), hooks_(std::move(hooks)) {}

    void onProgress(int) override {}

    void onSessionStarted(std::size_t count, uint64_t total) override {
        start_ = std::chrono::steady_clock::now();
        if (hooks_.started) hooks_.started();
        hooks_.log(QString("%1 %2 file(s), %3").arg(verb_).arg(count).arg(q(host::humanBytes(total))));
    }

    void onBytesTransferred(uint64_t done, uint64_t total) override {
        auto now = std::chrono::steady_clock::now();
        if (done != total && now - lastUpdate_ < std::chrono::milliseconds(100)) return;
        lastUpdate_ = now;
        double secs = std::chrono::duration<double>(now - start_).count();
        uint64_t rate = secs > 0 ? static_cast<uint64_t>(done / secs) : 0;
        int percent = total ? static_cast<int>(done * 100 / total) : 100;
        QString eta = "done";
        if (done < total) {
            long left = rate > 0 ? static_cast<long>((total - done) / rate) : -1;
            eta = left < 0 ? "estimating..." : left < 60 ? QString("%1 s left").arg(left)
                                                         : QString("%1 min left").arg((left + 30) / 60);
        }
        hooks_.progress(percent, file_ + "  ·  " + q(host::humanBytes(done) + " / " + host::humanBytes(total)),
                        q(host::humanBytes(rate)) + "/s", eta);
        if (percent / 25 != lastQuarter_) {   // terminal: a line every 25 %
            lastQuarter_ = percent / 25;
            std::printf("  %s %3d%%  (%s/s)\n", verb_.toLower().toUtf8().constData(), percent, host::humanBytes(rate).c_str());
            std::fflush(stdout);
        }
    }

    void onFileStarted(std::size_t index, const std::string& name, uint64_t) override {
        file_ = QString("%1 (file %2)").arg(q(name)).arg(index + 1);
    }

    void onFileCompleted(std::size_t, const std::string& name, const std::string& path, bool verified) override {
        if (hooks_.fileSaved && !path.empty()) hooks_.fileSaved(q(name), q(path));
        hooks_.log(QString("%1 %2%3").arg(verified ? "[verified]" : "[unverified]", q(name),
                                         path.empty() ? QString() : " -> " + q(path)));
    }

    void onError(const std::string& message) override { hooks_.log("Problem: " + q(message)); }

private:
    QString verb_;
    Hooks hooks_;
    QString file_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastUpdate_{};
    int lastQuarter_ = -1;
};

}  // namespace

// ============================================================================
// Construction
// ============================================================================
MainWindow::MainWindow(const GuiOptions& options, QWidget* parent)
    : QMainWindow(parent), options_(options) {
#ifdef ANYSHARE_HAVE_BLUEZ
    try {
        bt::localAdapterAddress();
        bluetoothUsable_ = true;
    } catch (const std::exception& e) {
        log("Bluetooth adapter: none (" + q(e.what()) + ")", true);
    }
#endif
    int saved = settings_.value("mode", static_cast<int>(TransferMode::HardwareNfc)).toInt();
    if (saved >= 0 && saved <= static_cast<int>(TransferMode::DemoTap)) mode_ = static_cast<TransferMode>(saved);

    buildUi();
    applyMode();
    log("AnyShare desktop started. Engine log is also printed in this terminal.", true);
    if (options_.probeNfc) probeNfcAsync();
    connectDisplayAsync(options_.displayPort);
    if (options_.startReceiving) startReceiving();
}

MainWindow::~MainWindow() {
    dropDisplay();   // stops the reader thread before anything it could post to goes away
    onCancelPressed();
    if (sendThread_.joinable()) sendThread_.join();
    if (pullThread_.joinable()) pullThread_.join();
    receiveCancel_.cancel();
    if (receiveThread_.joinable()) receiveThread_.join();
    if (receiveThreadBt_.joinable()) receiveThreadBt_.join();
    if (probeThread_.joinable()) probeThread_.join();
    if (displayThread_.joinable()) displayThread_.join();
    if (discoverThread_.joinable()) discoverThread_.join();
    if (nearbyThread_.joinable()) nearbyThread_.join();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (sending_ || receiving_ || pulling_) log("Closing: stopping transfers...", true);
    onCancelPressed();
    receiveCancel_.cancel();
    event->accept();
}

void MainWindow::applyStyle() {
    // Light theme with fixed colours, so it looks the same on every desktop theme.
    setStyleSheet(QString(R"(
        QMainWindow, QWidget#central { background: #F4F6FB; }
        QWidget { color: #1F2430; font-size: 10pt; }
        QFrame[card="true"] { background: #FFFFFF; border: 1px solid #E3E7EF; border-radius: 14px; }
        QLabel[role="h1"] { font-size: 20pt; font-weight: 700; }
        QLabel[role="h2"] { font-size: 12pt; font-weight: 700; }
        QLabel[role="muted"] { color: #6B7280; }
        QLabel[role="small"] { color: #6B7280; font-size: 9pt; }
        QPushButton { padding: 7px 14px; border-radius: 9px; border: 1px solid #D5DAE3; background: #FFFFFF; }
        QPushButton:hover { background: #F0F2F7; }
        QPushButton:disabled { color: #9CA3AF; background: #F3F4F6; }
        QPushButton[primary="true"] { background: %1; color: white; border: none; font-weight: 600; padding: 9px 18px; }
        QPushButton[primary="true"]:hover { background: #4338CA; }
        QPushButton[primary="true"]:disabled { background: #A5B4FC; }
        QTabWidget::pane { border: none; }
        QTabBar { qproperty-drawBase: 0; }
        QTabBar::tab { padding: 8px 26px; min-width: 80px; margin-right: 8px; border-radius: 10px; background: #E8EBF3;
                       color: #374151; font-weight: 600; }
        QTabBar::tab:selected { background: %1; color: white; }
        QListWidget { border: none; background: transparent; outline: none; }
        QListWidget::item { padding: 6px; border-radius: 10px; }
        QListWidget::item:hover { background: #F3F4FA; }
        QListWidget::item:selected { background: #E0E7FF; color: #1F2430; }
        QPlainTextEdit { background: #FBFCFE; border: 1px solid #E3E7EF; border-radius: 10px;
                         font-family: monospace; font-size: 9pt; }
        QToolButton { border: none; color: #6B7280; }
        QSpinBox, QComboBox { padding: 4px 8px; border: 1px solid #D5DAE3; border-radius: 8px; background: white; }
    )").arg(ui::kAccent));
}

namespace {

QFrame* card() {
    auto* f = new QFrame();
    f->setProperty("card", true);
    return f;
}

QLabel* label(const QString& text, const char* role) {
    auto* l = new QLabel(text);
    l->setProperty("role", role);
    return l;
}

}  // namespace

void MainWindow::buildUi() {
    setWindowTitle("AnyShare");
    resize(940, 720);
    setMinimumSize(760, 600);
    setAcceptDrops(true);
    applyStyle();
    auto* central = new QWidget(this);
    central->setObjectName("central");
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(22, 16, 22, 16);
    root->setSpacing(12);
    setCentralWidget(central);

    // Header: name, this computer, mode; the hidden corner on the far right.
    auto* header = new QHBoxLayout();
    auto* titleBox = new QVBoxLayout();
    titleBox->setSpacing(2);
    titleBox->addWidget(label("AnyShare", "h1"));
    subtitle_ = label("", "muted");
    titleBox->addWidget(subtitle_);
    modeLabel_ = label("", "small");
    modeLabel_->setObjectName("modeLabel");
    titleBox->addWidget(modeLabel_);
    header->addLayout(titleBox, 1);
    secretCorner_ = new SecretCorner();
    secretCorner_->setOnTriggered([this] { showSecretMenu(); });
    header->addWidget(secretCorner_, 0, Qt::AlignTop | Qt::AlignRight);
    root->addLayout(header);

    demoBadge_ = new QLabel();
    demoBadge_->setObjectName("demoBadge");
    demoBadge_->setStyleSheet("QLabel { background: #FFF3E0; color: #B45309; border: 1px solid #FCD34D;"
                              " border-radius: 8px; padding: 6px 10px; font-weight: 600; }");
    root->addWidget(demoBadge_);
    tapCard_ = new TapCard();
    tapCard_->setOnTapped([this] {
        QString partner = settings_.value("demoPartner").toString();
        if (files_.isEmpty()) {
            status_->setText("Add files first, then tap.");
            return;
        }
        QString name;
        if (partner.isEmpty()) {
            if (!askForAddress("Demo partner", partner, &name)) return;
            settings_.setValue("demoPartnerName", name);
        }
        setDemoPartner(partner);
        QString partnerName = settings_.value("demoPartnerName").toString();
        setNextPeer(partnerName.isEmpty() ? partner : partnerName,
                    peerLooksLikeComputer(partnerName) || peerLooksLikeComputer(partner.section(':', 0, 0)));
        try {
            ConnectionInfo target = host::parseTarget(partner.toStdString());
            startSend(std::make_unique<ManualHandover>(target, kDemoHandover), kDemoHandover);
        } catch (const std::exception& e) {
            status_->setText("Demo partner address is invalid: " + q(e.what()));
        }
    });
    root->addWidget(tapCard_);

    tabs_ = new QTabWidget();
    tabs_->setObjectName("mainTabs");
    tabs_->setDocumentMode(true);
    root->addWidget(tabs_, 1);

    // ---- Send tab: files on the left, nearby devices on the right.
    auto* sendPage = new QWidget();
    auto* sendRow = new QHBoxLayout(sendPage);
    sendRow->setContentsMargins(0, 10, 0, 0);
    sendRow->setSpacing(12);

    auto* filesCard = card();
    auto* filesLayout = new QVBoxLayout(filesCard);
    filesLayout->setContentsMargins(16, 14, 16, 14);
    auto* filesHead = new QHBoxLayout();
    filesHead->addWidget(label("Files", "h2"));
    filesHead->addStretch();
    filesSummary_ = label("", "muted");
    filesHead->addWidget(filesSummary_);
    filesLayout->addLayout(filesHead);
    fileList_ = new QListWidget();
    fileList_->setObjectName("fileList");
    fileList_->setIconSize(QSize(32, 32));
    filesLayout->addWidget(fileList_, 1);
    filesEmpty_ = label("Drop files here\nor click Add files", "muted");
    filesEmpty_->setAlignment(Qt::AlignCenter);
    filesEmpty_->setMinimumHeight(120);
    filesEmpty_->setStyleSheet("QLabel { border: 2px dashed #CBD2E1; border-radius: 12px; font-size: 11pt; }");
    filesLayout->addWidget(filesEmpty_, 1);
    auto* fileButtons = new QHBoxLayout();
    addButton_ = new QPushButton("Add files");
    addButton_->setProperty("primary", true);
    clearButton_ = new QPushButton("Clear");
    fileButtons->addWidget(addButton_);
    fileButtons->addWidget(clearButton_);
    fileButtons->addStretch();
    filesLayout->addLayout(fileButtons);
    connect(addButton_, &QPushButton::clicked, this, [this] { chooseFiles(); });
    connect(clearButton_, &QPushButton::clicked, this, [this] { setFiles({}); });
    connect(fileList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (sending_) return;
        QStringList rest = files_;
        rest.removeAll(item->data(Qt::UserRole).toString());
        setFiles(rest);
    });
    sendRow->addWidget(filesCard, 1);

    auto* nearbyCard = card();
    auto* nearbyLayout = new QVBoxLayout(nearbyCard);
    nearbyLayout->setContentsMargins(16, 14, 16, 14);
    auto* nearbyHead = new QHBoxLayout();
    nearbyHead->addWidget(label("Nearby devices", "h2"));
    nearbyHead->addStretch();
    refreshNearbyButton_ = new QPushButton("Refresh");
    nearbyHead->addWidget(refreshNearbyButton_);
    nearbyLayout->addLayout(nearbyHead);
    nearbyList_ = new QListWidget();
    nearbyList_->setObjectName("nearbyList");
    nearbyList_->setIconSize(QSize(36, 36));
    nearbyLayout->addWidget(nearbyList_, 1);
    nearbyHint_ = label("Looking for devices in Receive mode on this network...", "muted");
    nearbyHint_->setWordWrap(true);
    nearbyLayout->addWidget(nearbyHint_);
    sendButton_ = new QPushButton("Send to an address...");
    sendButton_->setObjectName("sendButton");
    nearbyLayout->addWidget(sendButton_);
    connect(refreshNearbyButton_, &QPushButton::clicked, this, [this] { refreshNearby(); });
    connect(nearbyList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { onNearbyClicked(item); });
    connect(sendButton_, &QPushButton::clicked, this, [this] { onSendPressed(); });
    sendRow->addWidget(nearbyCard, 1);
    tabs_->addTab(sendPage, "Send");

    // ---- Receive tab: beacon and switch on the left, recent files on the right.
    auto* receivePage = new QWidget();
    auto* receiveRow = new QHBoxLayout(receivePage);
    receiveRow->setContentsMargins(0, 10, 0, 0);
    receiveRow->setSpacing(12);

    auto* beaconCard = card();
    auto* beaconLayout = new QVBoxLayout(beaconCard);
    beaconLayout->setContentsMargins(16, 14, 16, 14);
    beacon_ = new ReceiveBeacon();
    beaconLayout->addWidget(beacon_, 1);
    receiveInfo_ = label("Not visible. Start receiving so phones and PCs can find this computer.", "muted");
    receiveInfo_->setWordWrap(true);
    receiveInfo_->setAlignment(Qt::AlignCenter);
    receiveInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    beaconLayout->addWidget(receiveInfo_);
    receiveButton_ = new QPushButton("Start receiving");
    receiveButton_->setObjectName("receiveButton");
    receiveButton_->setProperty("primary", true);
    connect(receiveButton_, &QPushButton::clicked, this, [this] {
        if (receiving_) stopReceiving();
        else startReceiving();
    });
    beaconLayout->addWidget(receiveButton_);
    auto* advanced = new QHBoxLayout();
    advanced->addWidget(label("Port", "small"));
    portBox_ = new QSpinBox();
    portBox_->setRange(0, 65535);
    portBox_->setValue(options_.port >= 0 ? options_.port : settings_.value("port", 8080).toInt());
    advanced->addWidget(portBox_);
    receiveTransport_ = new QComboBox();
    if (bluetoothUsable_) receiveTransport_->addItem("Wi-Fi + Bluetooth", "both");
    receiveTransport_->addItem("Wi-Fi only", "tcp");
    receiveTransport_->setVisible(bluetoothUsable_);
    advanced->addWidget(receiveTransport_);
    advanced->addStretch();
    beaconLayout->addLayout(advanced);
    receiveRow->addWidget(beaconCard, 1);

    auto* recentCard = card();
    auto* recentLayout = new QVBoxLayout(recentCard);
    recentLayout->setContentsMargins(16, 14, 16, 14);
    recentLayout->addWidget(label("Received files", "h2"));
    recentList_ = new QListWidget();
    recentList_->setObjectName("recentList");
    recentList_->setIconSize(QSize(32, 32));
    connect(recentList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        openReceivedFile(item->data(Qt::UserRole).toString());
    });
    recentLayout->addWidget(recentList_, 1);
    dirLabel_ = label("", "small");
    dirLabel_->setWordWrap(true);
    dirLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recentLayout->addWidget(dirLabel_);
    auto* dirButtons = new QHBoxLayout();
    auto* openDir = new QPushButton("Open folder");
    auto* changeDir = new QPushButton("Change folder...");
    dirButtons->addWidget(openDir);
    dirButtons->addWidget(changeDir);
    dirButtons->addStretch();
    recentLayout->addLayout(dirButtons);
    connect(changeDir, &QPushButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, "Save received files to", receiveDir());
        if (dir.isEmpty()) return;
        settings_.setValue("receiveDir", dir);
        options_.receiveDir.clear();
        dirLabel_->setText("Saving to " + receiveDir());
    });
    connect(openDir, &QPushButton::clicked, this, [this] {
        QDir().mkpath(receiveDir());
        if (!openExternally(receiveDir())) status_->setText("No file manager available. The folder is " + receiveDir());
    });
    dirLabel_->setText("Saving to " + receiveDir());
    receiveRow->addWidget(recentCard, 1);
    tabs_->addTab(receivePage, "Receive");

    // ---- Transfer panel (always visible).
    auto* transferCard = card();
    auto* transferRow = new QHBoxLayout(transferCard);
    transferRow->setContentsMargins(12, 10, 16, 10);
    stage_ = new TransferStage();
    stage_->setFixedWidth(300);
    transferRow->addWidget(stage_);
    auto* transferLayout = new QVBoxLayout();
    transferRow->addLayout(transferLayout, 1);
    transferLayout->setSpacing(6);
    auto* transferHead = new QHBoxLayout();
    statePill_ = new QLabel();
    statePill_->setObjectName("statePill");
    transferHead->addWidget(statePill_);
    status_ = new QLabel("Ready");
    status_->setObjectName("status");
    status_->setWordWrap(true);
    transferHead->addWidget(status_, 1);
    speed_ = label("", "muted");
    transferHead->addWidget(speed_);
    cancelButton_ = new QPushButton("Cancel");
    cancelButton_->setVisible(false);
    connect(cancelButton_, &QPushButton::clicked, this, [this] { onCancelPressed(); });
    transferHead->addWidget(cancelButton_);
    transferLayout->addLayout(transferHead);
    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(10);
    transferLayout->addWidget(progress_);
    currentFile_ = label("", "small");
    transferLayout->addWidget(currentFile_);
    transportLabel_ = label("Transport: idle", "small");
    transportLabel_->setObjectName("transport");
    transportLabel_->setWordWrap(true);
    transferLayout->addWidget(transportLabel_);
    nfcStatus_ = label("", "small");
    nfcStatus_->setObjectName("nfcStatus");
    nfcStatus_->setVisible(false);   // shown only when a reader is actually present
    transferLayout->addWidget(nfcStatus_);
    root->addWidget(transferCard);

    // ---- Details (activity log), collapsed by default.
    detailsButton_ = new QToolButton();
    detailsButton_->setText("Show details");
    detailsButton_->setCheckable(true);
    root->addWidget(detailsButton_, 0, Qt::AlignLeft);
    logBox_ = new QWidget();
    auto* logLayout = new QVBoxLayout(logBox_);
    logLayout->setContentsMargins(0, 0, 0, 0);
    log_ = new QPlainTextEdit();
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setMinimumHeight(120);
    logLayout->addWidget(log_);
    logBox_->setVisible(false);
    root->addWidget(logBox_);
    connect(detailsButton_, &QToolButton::toggled, this, [this](bool on) {
        logBox_->setVisible(on);
        detailsButton_->setText(on ? "Hide details" : "Show details");
    });

    // Nearby devices refresh by themselves while the Send tab is open.
    nearbyTimer_ = new QTimer(this);
    nearbyTimer_->setInterval(4000);
    connect(nearbyTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && tabs_->currentIndex() == 0 && !sending_) refreshNearby();
    });
    nearbyTimer_->start();
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) refreshNearby();
    });

    subtitle_->setText("This computer: " + deviceName());
    beacon_->setName(deviceName());
    setFiles({});
    setActivity(Activity::Idle, "Ready");
}

// ============================================================================
// Look: activity colours, progress, drag and drop, nearby devices
// ============================================================================
void MainWindow::setActivity(Activity activity, const QString& text) {
    const char* colour = ui::kIdle;
    QString name = "Idle";
    switch (activity) {
        case Activity::Idle: break;
        case Activity::Sending: colour = ui::kSending; name = "Sending"; break;
        case Activity::Receiving: colour = ui::kReceiving; name = "Receiving"; break;
        case Activity::Done: colour = ui::kDone; name = "Done"; break;
        case Activity::Failed: colour = ui::kError; name = "Failed"; break;
    }
    statePill_->setText(name);
    statePill_->setStyleSheet(QString("QLabel { background: %1; color: white; border-radius: 9px; padding: 3px 10px;"
                                      " font-weight: 700; }").arg(colour));
    progress_->setStyleSheet(QString("QProgressBar { border: none; background: #E8EBF3; border-radius: 5px; }"
                                     "QProgressBar::chunk { border-radius: 5px; background: %1; }")
                                 .arg(activity == Activity::Idle ? ui::kAccent : colour));
    if (!text.isEmpty()) status_->setText(text);
}

void MainWindow::showProgress(int percent, const QString& bytes, const QString& rate, const QString& eta) {
    progress_->setValue(percent);
    speed_->setText(QString("%1%  ·  %2  ·  %3").arg(percent).arg(rate, eta));
    currentFile_->setText(bytes);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && !sending_) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList all = files_;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && !all.contains(url.toLocalFile())) all << url.toLocalFile();
    }
    setFiles(all);
    tabs_->setCurrentIndex(0);
    event->acceptProposedAction();
}

void MainWindow::refreshNearby() {
    if (nearbySearching_) return;
    if (nearbyThread_.joinable()) nearbyThread_.join();
    nearbySearching_ = true;
    refreshNearbyButton_->setEnabled(false);
    nearbyThread_ = std::thread([this] {
        auto peers = discoverNow(1200);
        post([this, peers] {
            if (nearbyThread_.joinable()) nearbyThread_.join();
            nearbySearching_ = false;
            refreshNearbyButton_->setEnabled(true);
            QString selected = nearbyList_->currentItem() ? nearbyList_->currentItem()->data(Qt::UserRole).toString() : QString();
            nearbyList_->clear();
            if (displayConnected()) {   // demo without an NFC phone: write onto a MIFARE Classic 1K card
                auto* card = new QListWidgetItem(fileTypeIcon("Archive"),
                                                 "NFC card (tap on the reader)\nMIFARE Classic 1K · up to ~700 bytes");
                card->setData(Qt::UserRole, QString("nfc-card"));
                card->setToolTip("Writes one small file or a note onto the next card tapped on the ESP32 reader");
                nearbyList_->addItem(card);
            }
            for (const auto& p : peers) {
                if (receiving_ && q(p.name) == deviceName() && p.port == listeningPort_) continue;   // ourselves
                QString kind = p.os == "linux" ? "Computer" : p.os == "android" ? "Phone" : q(p.os);
                auto* item = new QListWidgetItem(deviceIcon(q(p.os)),
                                                 QString("%1\n%2 · %3").arg(q(p.name), kind, q(p.host)));
                item->setData(Qt::UserRole, QString("%1:%2").arg(q(p.host)).arg(p.port));
                item->setData(Qt::UserRole + 1, q(p.name));
                item->setData(Qt::UserRole + 2, q(p.os));
                item->setToolTip(QString("%1:%2").arg(q(p.host)).arg(p.port));
                nearbyList_->addItem(item);
                if (item->data(Qt::UserRole).toString() == selected) nearbyList_->setCurrentItem(item);
            }
            nearbyHint_->setText(nearbyList_->count() == 0
                                     ? "No device found yet. On the other device open AnyShare and tap Receive (same Wi-Fi)."
                                     : (files_.isEmpty() ? "Add files, then click a device to send."
                                                         : "Click a device to send the files."));
        });
    });
}

void MainWindow::onNearbyClicked(QListWidgetItem* item) {
    if (!item || sending_) return;
    QString address = item->data(Qt::UserRole).toString(), name = item->data(Qt::UserRole + 1).toString();
    if (address == "nfc-card") {
        writeToNfcCard();
        return;
    }
    if (effectiveMode() == TransferMode::DemoTap) {
        settings_.setValue("demoPartnerName", name);
        setDemoPartner(address);
        status_->setText("Demo partner: " + name + ". Click the tap card to simulate the NFC tap.");
        return;
    }
    if (files_.isEmpty()) {
        status_->setText("Add files first, then click " + name + ".");
        return;
    }
    sendTo(address, name);
}

bool MainWindow::peerLooksLikeComputer(const QString& nameOrHost) const {
    for (int i = 0; nearbyList_ && i < nearbyList_->count(); ++i) {
        QListWidgetItem* it = nearbyList_->item(i);
        QString host = it->data(Qt::UserRole).toString().section(':', 0, 0);
        if (it->data(Qt::UserRole + 1).toString() == nameOrHost || host == nameOrHost) {
            return it->data(Qt::UserRole + 2).toString() == "linux";
        }
    }
    return false;   // unknown: most AnyShare peers are phones
}

void MainWindow::setNextPeer(const QString& name, bool isComputer) {
    nextPeerName_ = name;
    nextPeerIsComputer_ = isComputer;
}

bool MainWindow::openExternally(const QString& path) {
    if (host::runningUnderWsl()) {   // WSL usually has no Linux file opener: use Windows' default app
        QProcess wslpath;
        wslpath.start("wslpath", {"-w", path});
        if (wslpath.waitForFinished(2000) && wslpath.exitCode() == 0) {
            QString windowsPath = QString::fromUtf8(wslpath.readAllStandardOutput()).trimmed();
            if (!windowsPath.isEmpty() && QProcess::startDetached("explorer.exe", {windowsPath})) return true;
        }
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::openReceivedFile(const QString& path) {
    QFileInfo info(path);
    if (!info.isFile()) {
        status_->setText("File not found (moved or deleted?): " + path);
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName("previewDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(info.fileName());
    dialog->resize(560, 440);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(label(QString("%1 · %2 · %3").arg(info.fileName(), q(bridge::fileTypeLabel(info.fileName().toStdString())),
                                                        q(host::humanBytes(static_cast<uint64_t>(info.size())))), "muted"));

    QImageReader image(path);
    QFile file(path);
    QByteArray head;
    if (file.open(QIODevice::ReadOnly)) head = file.read(256 * 1024);
    bool looksText = !head.contains('\0') && QString::fromUtf8(head).toUtf8() == head;
    if (image.canRead()) {
        auto* view = new QLabel();
        view->setObjectName("previewImage");
        QPixmap pm = QPixmap::fromImage(image.read());
        view->setPixmap(pm.scaled(520, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        view->setAlignment(Qt::AlignCenter);
        layout->addWidget(view, 1);
    } else if (looksText) {
        auto* text = new QPlainTextEdit(QString::fromUtf8(head));
        text->setObjectName("previewText");
        text->setReadOnly(true);
        layout->addWidget(text, 1);
    } else {   // video, audio, archives...: hand over to a real app
        dialog->deleteLater();
        if (!openExternally(path)) status_->setText("No app here can open " + info.fileName() + ". It is saved at " + path);
        return;
    }
    auto* buttons = new QHBoxLayout();
    auto* external = new QPushButton("Open with app");
    auto* folder = new QPushButton("Open folder");
    auto* close = new QPushButton("Close");
    close->setProperty("primary", true);
    buttons->addWidget(external);
    buttons->addWidget(folder);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(external, &QPushButton::clicked, dialog, [this, path] {
        if (!openExternally(path)) status_->setText("No app here can open this file. It is saved at " + path);
    });
    connect(folder, &QPushButton::clicked, dialog, [this, info] {
        if (!openExternally(info.absolutePath())) status_->setText("No file manager available. The folder is " + info.absolutePath());
    });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    dialog->show();
}

void MainWindow::addRecent(const QString& name, const QString& path) {
    auto* item = new QListWidgetItem(fileTypeIcon(q(bridge::fileTypeLabel(name.toStdString()))),
                                     name + "\n" + QDateTime::currentDateTime().toString("HH:mm") + " · " +
                                         q(host::humanBytes(static_cast<uint64_t>(QFileInfo(path).size()))));
    item->setData(Qt::UserRole, path);
    item->setToolTip(path + "\n(double-click to open)");
    recentList_->insertItem(0, item);
}

// ============================================================================
// Modes and the hidden menu
// ============================================================================
TransferMode MainWindow::effectiveMode() const {
    // Without a reader, Hardware NFC cannot work: run the manual mode instead.
    if (mode_ == TransferMode::HardwareNfc && !nfcReady_) return TransferMode::BluetoothFallback;
    return mode_;
}

QString MainWindow::modeTitle(TransferMode mode) const {
    switch (mode) {
        case TransferMode::HardwareNfc: return "Hardware NFC";
        case TransferMode::BluetoothFallback: return bluetoothUsable_ ? "Bluetooth fallback" : "Wi-Fi";
        case TransferMode::DemoTap: return "NFC-style demo";
    }
    return {};
}

void MainWindow::setMode(TransferMode mode) {
    mode_ = mode;
    settings_.setValue("mode", static_cast<int>(mode));
    applyMode();
}

void MainWindow::applyMode() {
    TransferMode eff = effectiveMode();
    QString summary;
    switch (eff) {
        case TransferMode::HardwareNfc:
            summary = "tap a phone on the NFC reader, payload via Wi-Fi / Bluetooth";
            break;
        case TransferMode::BluetoothFallback:
            summary = bluetoothUsable_ ? "pick a nearby device, a Bluetooth device or type an address"
                                       : "pick a nearby device or type an address";
            break;
        case TransferMode::DemoTap:
            summary = bluetoothUsable_ ? "SIMULATED tap · real Bluetooth / Wi-Fi transfer" : "SIMULATED tap · real Wi-Fi transfer";
            break;
    }
    modeLabel_->setText("Mode: " + modeTitle(eff) + " · " + summary);
    bool demo = eff == TransferMode::DemoTap;
    demoBadge_->setText("SIMULATED tap — no NFC hardware is used. The file transfer itself is real.");
    demoBadge_->setVisible(demo);
    tapCard_->setVisible(demo);
    QString partner = settings_.value("demoPartner").toString();
    QString partnerName = settings_.value("demoPartnerName").toString();
    tapCard_->setCaption(partner.isEmpty() ? "Click to simulate the tap (you will be asked for the partner)"
                                           : "Click to simulate the tap  →  " + (partnerName.isEmpty() ? partner : partnerName));
    sendButton_->setText(eff == TransferMode::HardwareNfc ? "Send by NFC tap (hold phone on reader)"
                         : eff == TransferMode::DemoTap    ? "Choose demo partner..."
                                                           : "Send to an address...");
    log("Mode: " + modeTitle(eff), true);
}

void MainWindow::showSecretMenu() {
    delete secretMenu_;
    secretMenu_ = new QMenu(this);
    secretMenu_->setObjectName("secretMenu");
    auto* group = new QActionGroup(secretMenu_);
    auto addMode = [&](TransferMode m, const QString& text) {
        QAction* action = secretMenu_->addAction(text);
        action->setCheckable(true);
        action->setChecked(effectiveMode() == m);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this, [this, m] { setMode(m); });
    };
    secretMenu_->addSection("Transfer mode");
    if (nfcReady_) addMode(TransferMode::HardwareNfc, "Hardware NFC");
    addMode(TransferMode::BluetoothFallback, modeTitle(TransferMode::BluetoothFallback));
    addMode(TransferMode::DemoTap, "NFC-style demo (simulated tap)");
    secretMenu_->addSeparator();
    connect(secretMenu_->addAction("Demo partner address..."), &QAction::triggered, this, [this] {
        QString partner = settings_.value("demoPartner").toString(), name;
        if (!askForAddress("Demo partner", partner, &name)) return;
        settings_.setValue("demoPartnerName", name);
        setDemoPartner(partner);
    });
#ifdef ANYSHARE_HAVE_LIBNFC
    connect(secretMenu_->addAction("NFC reader device..."), &QAction::triggered, this, [this] {
        bool ok = false;
        QString dev = QInputDialog::getText(this, "NFC reader",
                                            "libnfc connection string (empty = auto-detect)\n"
                                            "e.g. pn532_uart:/dev/ttyUSB0",
                                            QLineEdit::Normal, settings_.value("nfcDevice").toString(), &ok);
        if (!ok) return;
        settings_.setValue("nfcDevice", dev.trimmed());
        probeNfcAsync();
    });
    connect(secretMenu_->addAction("Re-check NFC reader"), &QAction::triggered, this, [this] {
        status_->setText("Looking for an NFC reader...");
        probeNfcAsync();
    });
#endif
    connect(secretMenu_->addAction("This computer's name..."), &QAction::triggered, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, "Device name", "Name that phones and PCs see in their Send list:",
                                             QLineEdit::Normal, deviceName(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        settings_.setValue("deviceName", name.trimmed());
        subtitle_->setText("This computer: " + deviceName());
        beacon_->setName(deviceName());
    });
    connect(secretMenu_->addAction("ESP32 display port..."), &QAction::triggered, this, [this] {
        bool ok = false;
        QString port = QInputDialog::getText(this, "ESP32 display",
                                             "Serial port of the ESP32 NFC bridge\n"
                                             "auto = find it, off = do not use it, or e.g. /dev/ttyUSB0",
                                             QLineEdit::Normal, settings_.value("displayPort", "auto").toString(), &ok);
        if (!ok) return;
        port = port.trimmed();
        settings_.setValue("displayPort", port);
        dropDisplay();
        refreshNfcLine();
        if (port != "off") connectDisplayAsync(port);
    });
    secretMenu_->popup(secretCorner_->mapToGlobal(QPoint(secretCorner_->width(), secretCorner_->height())));
}

void MainWindow::setDemoPartner(const QString& address) {
    settings_.setValue("demoPartner", address.trimmed());
    applyMode();
}

// ============================================================================
// NFC reader detection
// ============================================================================
void MainWindow::setNfcReaderForTest(bool ready, const QString& name) { onNfcProbed(ready, name); }

void MainWindow::probeNfcAsync() {
#ifdef ANYSHARE_HAVE_LIBNFC
    if (probing_) return;
    if (probeThread_.joinable()) probeThread_.join();
    probing_ = true;
    std::string device = settings_.value("nfcDevice").toString().toStdString();
    probeThread_ = std::thread([this, device] {
        bool ok = false;
        QString detail;
        try {
            std::string report = Pn532NfcHandover::probe(device);
            auto opened = report.find("Opened: ");
            ok = opened != std::string::npos && report.find("FAILED") == std::string::npos;
            detail = ok ? q(report.substr(opened + 8, report.find('\n', opened) - opened - 8)) : q(report);
        } catch (const std::exception& e) {
            detail = q(e.what());
        }
        post([this, ok, detail] {
            if (probeThread_.joinable()) probeThread_.join();
            probing_ = false;
            onNfcProbed(ok, detail);
        });
    });
#endif
}

void MainWindow::onNfcProbed(bool ready, const QString& detail) {
    bool wasReady = nfcReady_;
    nfcReady_ = ready;
    nfcDeviceName_ = ready ? detail : QString();
    refreshNfcLine();
    if (ready) {
        log("NFC reader found: " + detail);
    } else {
        log("NFC reader: none (" + detail + ")", true);   // terminal only
        if (status_->text().startsWith("Looking for an NFC reader")) {
            status_->setText("No NFC reader found. Check the PN532 wiring and DIP switches (HOW_TO_RUN.md, section 7).");
        }
    }
    if (wasReady != ready) applyMode();
}

// ============================================================================
// Sending
// ============================================================================
void MainWindow::chooseFiles() {
    QString start = settings_.value("lastFileDir", QDir::homePath()).toString();
    QStringList picked = QFileDialog::getOpenFileNames(this, "Choose files to send", start);
    if (picked.isEmpty()) return;
    settings_.setValue("lastFileDir", QFileInfo(picked.first()).absolutePath());
    QStringList all = files_;
    for (const QString& p : picked) {
        if (!all.contains(p)) all << p;
    }
    setFiles(all);
}

void MainWindow::setFiles(const QStringList& paths) {
    files_.clear();
    fileList_->clear();
    qint64 total = 0;
    for (const QString& p : paths) {
        QFileInfo info(p);
        if (!info.isFile()) {
            log("Skipped (not a readable file): " + p);
            continue;
        }
        files_ << info.absoluteFilePath();
        total += info.size();
        QString type = q(bridge::fileTypeLabel(info.fileName().toStdString()));
        auto* item = new QListWidgetItem(fileTypeIcon(type), QString("%1\n%2 · %3").arg(info.fileName(), type,
                                                                                         q(host::humanBytes(info.size()))));
        item->setToolTip(info.absoluteFilePath() + "\n(double-click to remove)");
        item->setData(Qt::UserRole, info.absoluteFilePath());
        fileList_->addItem(item);
    }
    bool empty = files_.isEmpty();
    fileList_->setVisible(!empty);
    filesEmpty_->setVisible(empty);
    QString summary = QString("%1 file(s) · %2").arg(files_.size()).arg(q(host::humanBytes(static_cast<uint64_t>(total))));
    filesSummary_->setText(empty ? QString() : summary);
    clearButton_->setEnabled(!empty && !sending_);
    if (!sending_) status_->setText(empty ? "Ready" : summary + " selected");
    if (nearbyHint_ && nearbyList_->count() > 0) {
        nearbyHint_->setText(empty ? "Add files, then click a device to send." : "Click a device to send the files.");
    }
}

QString MainWindow::deviceName() const {
    return settings_.value("deviceName", q(host::hostName())).toString();
}

std::vector<discovery::Peer> MainWindow::discoverNow(int timeoutMs) {
    std::vector<std::string> extra;
    for (const QString& t : options_.discoveryExtraTargets) extra.push_back(t.toStdString());
    return discovery::discover(timeoutMs, static_cast<uint16_t>(options_.discoveryPort), extra);
}

bool MainWindow::askForAddress(const QString& title, QString& address, QString* foundName) {
    QDialog dialog(this);
    dialog.setObjectName("targetDialog");
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Nearby AnyShare devices (in Receive mode, same network):"));
    auto* list = new QListWidget();
    list->setObjectName("peerList");
    layout->addWidget(list);
    auto* again = new QPushButton("Search again");
    layout->addWidget(again, 0, Qt::AlignLeft);
    layout->addWidget(new QLabel(bluetoothUsable_ ? "Or type an address (IP:PORT, or a Bluetooth address):"
                                                  : "Or type an address (IP:PORT):"));
    auto* edit = new QLineEdit(address.isEmpty() ? settings_.value("lastTarget").toString() : address);
    edit->setObjectName("addressEdit");
    edit->setPlaceholderText("192.168.1.5:8080");
    layout->addWidget(edit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(title == "Send to" ? "Send" : "Save");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem* item) {
        if (item->data(Qt::UserRole).isValid()) dialog.accept();
    });

    // Search in the background; results appear while the dialog is open.
    QPointer<QListWidget> guard(list);
    auto search = [this, guard, again] {
        if (discoverThread_.joinable()) return;   // one search at a time
        guard->clear();
        guard->addItem("Searching...");
        again->setEnabled(false);
        discoverThread_ = std::thread([this, guard, again] {
            auto peers = discoverNow(1500);
            post([this, guard, again, peers] {
                if (discoverThread_.joinable()) discoverThread_.join();
                if (!guard) return;   // dialog already closed
                guard->clear();
                for (const auto& p : peers) {
                    auto* item = new QListWidgetItem(QString("%1    %2:%3  (%4)").arg(q(p.name), q(p.host)).arg(p.port).arg(q(p.os)));
                    item->setData(Qt::UserRole, QString("%1:%2").arg(q(p.host)).arg(p.port));
                    item->setData(Qt::UserRole + 1, q(p.name));
                    guard->addItem(item);
                }
                if (peers.empty()) guard->addItem("No AnyShare device found. Is it in Receive mode on this network?");
                else guard->setCurrentRow(0);
                again->setEnabled(true);
            });
        });
    };
    connect(again, &QPushButton::clicked, &dialog, search);
    search();

    bool ok = dialog.exec() == QDialog::Accepted;
    if (discoverThread_.joinable()) discoverThread_.join();   // the posted result then finds the list gone
    if (!ok) return false;
    QListWidgetItem* picked = list->currentItem();
    QString text = edit->text().trimmed();
    if (picked && picked->data(Qt::UserRole).isValid() && (edit->text() == settings_.value("lastTarget").toString() || text.isEmpty())) {
        text = picked->data(Qt::UserRole).toString();   // a found device wins unless an address was typed
        if (foundName) *foundName = picked->data(Qt::UserRole + 1).toString();
    } else if (foundName) {
        foundName->clear();
    }
    if (text.isEmpty()) return false;
    try {
        host::parseTarget(text.toStdString());
    } catch (const std::exception& e) {
        status_->setText(q(e.what()));
        return false;
    }
    address = text.trimmed();
    return true;
}

void MainWindow::onSendPressed() {
    if (sending_) return;
    if (files_.isEmpty()) {
        chooseFiles();
        if (files_.isEmpty()) return;
    }
    switch (effectiveMode()) {
        case TransferMode::HardwareNfc: {
#ifdef ANYSHARE_HAVE_LIBNFC
            auto nfc = std::make_unique<Pn532NfcHandover>(settings_.value("nfcDevice").toString().toStdString(), 30);
            nfc->setCancelToken(&sendCancel_);
            nfcStatus_->setText("NFC: waiting for a phone tap on the reader (30 s)");
            status_->setText("Hold the receiving phone (AnyShare → Receive) on the NFC reader...");
            startSend(std::move(nfc), "physical NFC tap");
#endif
            break;
        }
        case TransferMode::BluetoothFallback: {
            QString address, name;
            if (askForAddress("Send to", address, &name)) sendTo(address, name);
            break;
        }
        case TransferMode::DemoTap:
            if (settings_.value("demoPartner").toString().isEmpty()) {
                QString partner, name;
                if (!askForAddress("Demo partner", partner, &name)) return;
                settings_.setValue("demoPartnerName", name);
                setDemoPartner(partner);
            }
            status_->setText("Demo mode: click the tap card to simulate the NFC tap");
            break;
    }
}

void MainWindow::sendTo(const QString& address, const QString& foundName) {
    ConnectionInfo target;
    try {
        target = host::parseTarget(address.toStdString());
    } catch (const std::exception& e) {
        status_->setText(q(e.what()));
        return;
    }
    if (target.transport == "rfcomm" && !bluetoothUsable_) {
        status_->setText("This computer has no usable Bluetooth adapter. Enter the receiver's Wi-Fi address (IP:PORT).");
        return;
    }
    settings_.setValue("lastTarget", address.trimmed());
    QString host = address.trimmed().section(':', 0, 0);
    setNextPeer(foundName.isEmpty() ? address.trimmed() : foundName,
                peerLooksLikeComputer(foundName.isEmpty() ? host : foundName) || peerLooksLikeComputer(host));
    std::string how = target.transport == "rfcomm" ? "Bluetooth fallback, manual address (no NFC)"
                      : foundName.isEmpty()            ? "manual address (no NFC)"
                                                       : "Wi-Fi discovery: '" + foundName.toStdString() + "' (no NFC)";
    startSend(std::make_unique<ManualHandover>(target, how), q(how));
}

void MainWindow::simulateTap() { tapCard_->playTap(); }

void MainWindow::startSend(std::unique_ptr<INfcHandover> handover, const QString& howLabel) {
    if (sending_ || files_.isEmpty()) return;
    if (sendThread_.joinable()) sendThread_.join();
    sendCancel_.reset();
    sending_ = true;
    setBusy(true);
    progress_->setValue(0);
    speed_->clear();
    currentFile_->clear();
    stage_->setPeer(nextPeerName_.isEmpty() ? QString("Other device") : nextPeerName_, nextPeerIsComputer_);
    stage_->reset();
    nextPeerName_.clear();
    nextPeerIsComputer_ = false;
    setActivity(Activity::Sending, "Connecting...");
    transportLabel_->setText("Handover: " + howLabel);
    log("Sending " + QString::number(files_.size()) + " file(s); handover: " + howLabel);

    std::vector<std::string> paths;
    for (const QString& f : files_) paths.push_back(f.toStdString());
    bool bluetooth = bluetoothUsable_;
    std::string senderName = host::hostName();
    std::shared_ptr<bridge::Esp32Bridge> display = display_;

    sendThread_ = std::thread([this, handover = std::move(handover), paths, bluetooth, senderName, display]() mutable {
        auto logLater = [this](const QString& line) { post([this, line] { log(line); }); };
        ConnectionInfo info;
        try {
            info = handover->performHandover();
        } catch (const std::exception& e) {
            QString message = "Handover failed (" + q(handover->describe()) + "): " + q(e.what());
            if (display) display->showDone(false, "Handover failed: " + std::string(e.what()));
            post([this, message] { finishSend(false, message, {}); });
            return;
        }
        QString how = q(handover->describe());
        // Wi-Fi is much faster than RFCOMM. With Bluetooth here, the stream factory tries the offered
        // Wi-Fi address for 3 s and then Bluetooth (the devices may not share a network); without
        // Bluetooth, Wi-Fi is the only way.
        if (host::hasWifiAlternative(info) && !bluetooth) info.transport = "tcp";
        if (info.transport == "rfcomm" && !bluetooth) {
            post([this] { finishSend(false, "The receiver offered only Bluetooth, and this computer has no usable Bluetooth adapter. "
                                            "Put the phone on the same Wi-Fi as this computer and tap again.", {}); });
            return;
        }
        // A phone that is not on Wi-Fi offers only its Bluetooth *name*; that works only if it is
        // paired with (or discoverable to) this computer. Say so, and do not wait a whole minute.
        const bool bluetoothNameOnly = info.transport == "rfcomm" && info.btAddress.empty() && info.ip.empty();
        if (bluetoothNameOnly) {
            logLater("The phone offered only Bluetooth ('" + q(info.deviceName) +
                     "'), no Wi-Fi address: it is probably not on this Wi-Fi.");
        }
        info.files.clear();
        for (const auto& p : paths) info.files.push_back({p, "", 0, ""});
        QString bluetoothTarget = "Bluetooth " + q(info.btAddress.empty() ? info.deviceName : info.btAddress);
        QString target = info.transport == "tcp" ? q(info.ip + ":" + std::to_string(info.port))
                         : host::hasWifiAlternative(info)
                             ? "Wi-Fi " + q(info.ip + ":" + std::to_string(info.port)) + ", else " + bluetoothTarget
                             : bluetoothTarget;
        post([this, how, target] { transportLabel_->setText("Handover: " + how + " · Target: " + target); });
        logLater("Connecting to " + target + " ...");

        RelayObserver observer("Sending", {
            [this](int percent, const QString& bytes, const QString& rate, const QString& eta) {
                post([this, percent, bytes, rate, eta] { showProgress(percent, bytes, rate, eta); });
            },
            logLater,
            nullptr,
            [this] { post([this] { stage_->start(true); }); },
        });
        NetworkEngine engine;
        engine.setSenderName(senderName);
        // A phone found by name over Bluetooth answers within seconds or not at all: do not wait a minute.
        if (info.transport == "rfcomm" && info.btAddress.empty()) engine.setConnectTimeoutSeconds(30);
#ifdef ANYSHARE_HAVE_BLUEZ
        engine.setStreamFactory(bt::makeStreamFactory());
#endif
        engine.addObserver(&observer);
        std::unique_ptr<bridge::BridgeObserver> shown;
        if (display) {
            shown = std::make_unique<bridge::BridgeObserver>(*display, true);
            engine.addObserver(shown.get());
        }
        {
            std::lock_guard<std::mutex> lock(engineMutex_);
            engine_ = &engine;
        }
        if (!sendCancel_.isCancelled()) engine.connectToHost(info);
        {
            std::lock_guard<std::mutex> lock(engineMutex_);
            engine_ = nullptr;
        }
        bool ok = engine.lastTransferSucceeded() && !sendCancel_.isCancelled();
        QString message = sendCancel_.isCancelled() && !engine.lastTransferSucceeded() ? "Cancelled" : q(engine.lastMessage());
        if (!ok && bluetoothNameOnly && !sendCancel_.isCancelled()) {
            message += ". The phone offered only Bluetooth: connect it to the same Wi-Fi as this computer "
                       "(recommended) or pair it with this computer, then tap again.";
        }
        QString transport = q(engine.lastTransport());
        post([this, ok, message, transport, how] {
            finishSend(ok, message, transport.isEmpty() ? QString() : "Transport: " + transport + " · Handover: " + how);
        });
    });
}

void MainWindow::finishSend(bool ok, const QString& message, const QString& transport) {
    if (sendThread_.joinable()) sendThread_.join();
    sending_ = false;
    setBusy(false);
    if (!transport.isEmpty()) transportLabel_->setText(transport);
    if (ok) progress_->setValue(100);
    stage_->finish(ok);
    setActivity(ok ? Activity::Done : Activity::Failed, (ok ? "Sent: " : "Send failed: ") + message);
    log((ok ? "DONE: " : "FAILED: ") + message);
    refreshNfcLine();
}

// The NFC line appears only when NFC hardware is actually connected.
void MainWindow::refreshNfcLine() {
    QStringList parts;
    if (nfcReady_) parts << "NFC: reader ready (" + nfcDeviceName_ + ")";
    if (displayConnected()) {
        parts << (receiving_ && !servedHandover_.empty()
                      ? "NFC: ESP32 module is acting as this computer's card (phones tap it to send here)"
                      : "NFC reader: ESP32 + Si512 on " + q(display_->port()) +
                            " (tap a card, or a phone in Receive or \"Send by NFC tap\")");
    }
    if (!lastTap_.isEmpty() && !parts.isEmpty()) parts << "Last tap: " + lastTap_;
    nfcStatus_->setText(parts.join("  ·  "));
    nfcStatus_->setVisible(!parts.isEmpty());
}

void MainWindow::writeToNfcCard() {
    if (!displayConnected()) {
        status_->setText("The ESP32 NFC reader is not connected.");
        return;
    }
    QString name;
    QByteArray data;
    if (files_.isEmpty()) {   // no file: type a short note instead
        bool ok = false;
        QString note = QInputDialog::getMultiLineText(this, "Write to NFC card",
                                                      "Note to store on the card (about 700 bytes):", QString(), &ok);
        if (!ok || note.isEmpty()) return;
        name = "note.txt";
        data = note.toUtf8();
    } else if (files_.size() == 1) {
        QFile f(files_.first());
        if (!f.open(QIODevice::ReadOnly)) {
            status_->setText("Cannot read " + files_.first());
            return;
        }
        name = QFileInfo(f).fileName();
        data = f.readAll();
    } else {
        setActivity(Activity::Failed, "An NFC card holds one small file. Select just one (or none, to type a note).");
        return;
    }
    std::size_t needed = static_cast<std::size_t>(data.size()) + std::min<int>(name.toUtf8().size(), 40);
    if (needed > bridge::Esp32Bridge::kCardCapacity) {
        setActivity(Activity::Failed, QString("Too large for an NFC card: %1 bytes (it holds about %2). Use a short text file.")
                                          .arg(data.size()).arg(bridge::Esp32Bridge::kCardCapacity));
        return;
    }
    display_->writeToCard(name.toStdString(), data.toStdString());
    cardWritePending_ = true;
    progress_->setValue(0);
    setActivity(Activity::Sending, QString("Tap a MIFARE Classic 1K card on the reader to write %1 (%2 bytes)...")
                                       .arg(name).arg(data.size()));
    transportLabel_->setText("Transport: NFC card write (ESP32 + Si512) · no phone involved");
    log("NFC card write armed: " + name + " (" + QString::number(data.size()) + " bytes)");
}

void MainWindow::saveCardFile(const QString& rawName, const QByteArray& data) {
    QString name = q(protocol::sanitizeFilename(rawName.toStdString()));
    QDir().mkpath(receiveDir());
    QFileInfo target(QDir(receiveDir()).filePath(name));
    // Tapping the same card again must not pile up copies: identical content already in the inbox?
    QStringList candidates = {target.fileName()};
    for (int n = 1; n < 100; ++n) {
        candidates << QString("%1 (%2)%3").arg(target.completeBaseName()).arg(n)
                          .arg(target.suffix().isEmpty() ? "" : "." + target.suffix());
    }
    for (const QString& c : candidates) {
        QFile existing(QDir(receiveDir()).filePath(c));
        if (!existing.exists()) continue;
        if (existing.open(QIODevice::ReadOnly) && existing.size() == data.size() && existing.readAll() == data) {
            setActivity(Activity::Done, "Already received from this card: " + c + " (same content, not saved again)");
            return;
        }
    }
    for (int n = 1; target.exists(); ++n) {   // never overwrite, like network transfers
        target.setFile(QDir(receiveDir()).filePath(QString("%1 (%2)%3").arg(target.completeBaseName().section(" (", 0, 0))
                                                         .arg(n).arg(target.suffix().isEmpty() ? "" : "." + target.suffix())));
    }
    QFile f(target.absoluteFilePath());
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        setActivity(Activity::Failed, "Cannot save the card's file to " + receiveDir());
        return;
    }
    f.close();
    addRecent(target.fileName(), target.absoluteFilePath());
    progress_->setValue(100);
    setActivity(Activity::Done, QString("Received from NFC card: %1 (%2 bytes)").arg(target.fileName()).arg(data.size()));
    transportLabel_->setText("Transport: NFC card read (ESP32 + Si512) · no phone involved");
    log("Saved from NFC card: " + target.absoluteFilePath());
}

void MainWindow::dropDisplay() {
    if (!display_) return;
    display_->setOnEvent(nullptr);
    display_.reset();
}

void MainWindow::onNfcEvent(const bridge::NfcEvent& e) {
    QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    switch (e.kind) {
        case bridge::NfcEvent::Kind::Card: {
            QString type = q(bridge::cardTypeFromSak(e.sak));
            lastTap_ = QString("%1 card %2 (%3)").arg(time, q(e.uid), type);
            log("NFC card tapped: ID " + q(e.uid) + " · " + type);
            if (!sending_) status_->setText("NFC card tapped · ID " + q(e.uid) + " · " + type);
            break;
        }
        case bridge::NfcEvent::Kind::Phone: {
            QString name = "phone";
            ConnectionInfo info;
            try {
                info = parseConnectionInfo(e.payload);
                if (!info.deviceName.empty()) name = q(info.deviceName);
            } catch (const std::exception& ex) {
                lastTap_ = time + " phone \"" + name + "\" (unreadable payload)";
                log("NFC phone tap with an invalid payload: " + q(ex.what()));
                status_->setText("NFC tap: the phone sent an invalid payload (" + q(ex.what()) + ")");
                break;
            }
            lastTap_ = time + " phone \"" + name + "\"";
            if (info.role == "send") {   // the phone has files: fetch them (one fetch per tap)
                if (pulling_ || sending_ || info.sessionToken == lastPulledToken_) break;
                log("NFC phone tapped: \"" + name + "\" wants to send (payload " + QString::number(e.payload.size()) + " bytes)");
                startPull(info, name);
                break;
            }
            log("NFC phone tapped: \"" + name + "\" (payload " + QString::number(e.payload.size()) + " bytes)");
            if (sending_ || pulling_) break;   // one transfer at a time
            if (files_.isEmpty()) {
                tabs_->setCurrentIndex(0);
                status_->setText("NFC: \"" + name + "\" tapped. Add files, then tap the phone again to send.");
                break;
            }
            const char* how = "physical NFC tap (ESP32 + Si512 reader)";
            setNextPeer(name, false);   // a phone in receive mode answered the tap
            startSend(std::make_unique<JsonHandover>(e.payload, how), how);
            break;
        }
        case bridge::NfcEvent::Kind::WriteProgress:
            if (cardWritePending_) progress_->setValue(e.sak);
            return;
        case bridge::NfcEvent::Kind::Written:
            cardWritePending_ = false;
            progress_->setValue(100);
            lastTap_ = time + " card " + q(e.uid) + " written";
            setActivity(Activity::Done, "Written to NFC card " + q(e.uid) + ": " + q(e.message) +
                                            ". Tap it again (in Receive mode) to read it back.");
            log("NFC card " + q(e.uid) + " now holds " + q(e.message));
            break;
        case bridge::NfcEvent::Kind::WriteFailed:
            cardWritePending_ = false;
            setActivity(Activity::Failed, "NFC card write failed: " + q(e.message));
            log("NFC card write failed: " + q(e.message));
            break;
        case bridge::NfcEvent::Kind::CardFile: {
            lastTap_ = time + " card " + q(e.uid) + " holds " + q(e.message);
            QByteArray bytes(e.data.data(), static_cast<int>(e.data.size()));
            log("NFC card " + q(e.uid) + " holds " + q(e.message) + " (" + QString::number(bytes.size()) + " bytes)");
            if (receiving_) {
                saveCardFile(q(e.message), bytes);
            } else if (!sending_) {
                status_->setText("NFC card " + q(e.uid) + " holds \"" + q(e.message) + "\" (" + QString::number(bytes.size()) +
                                 " bytes). Start receiving to save it.");
            }
            break;
        }
        case bridge::NfcEvent::Kind::Restarted:   // e.g. reflashed or replugged: it forgot card mode
            log("ESP32 restarted", true);
            if (receiving_ && !servedHandover_.empty() && displayConnected()) display_->serveHandover(servedHandover_);
            return;
        case bridge::NfcEvent::Kind::Served:
            lastTap_ = time + " a phone read this computer's address";
            log("NFC: a phone tapped the ESP32 reader and read this computer's address");
            if (!sending_) status_->setText("NFC: a phone tapped the reader. Waiting for its files...");
            break;
        case bridge::NfcEvent::Kind::Error:
            lastTap_ = time + " failed";
            log("NFC tap failed: " + q(e.message));
            if (!sending_) status_->setText("NFC tap failed: " + q(e.message));
            break;
    }
    refreshNfcLine();
}

void MainWindow::startPull(const ConnectionInfo& info, const QString& phoneName) {
    if (pulling_ || sending_) return;
    if (pullThread_.joinable()) pullThread_.join();
    QString dir = receiveDir();
    if (!QDir().mkpath(dir)) {
        setActivity(Activity::Failed, "Cannot create " + dir);
        return;
    }
    pullCancel_.reset();
    pullCancel_.setOnCancel(nullptr);
    pulling_ = true;
    setBusy(true);
    progress_->setValue(0);
    speed_->clear();
    currentFile_->clear();
    stage_->setPeer(phoneName, false);
    stage_->reset();
    setActivity(Activity::Receiving, "\"" + phoneName + "\" tapped with files: connecting to it...");
    transportLabel_->setText("Handover: physical NFC tap (ESP32 + Si512 reader) · the phone sends, this computer fetches");

    const bool bluetooth = bluetoothUsable_;
    const std::string me = deviceName().toStdString();
    std::shared_ptr<bridge::Esp32Bridge> display = display_;
    pullThread_ = std::thread([this, info, dir, bluetooth, me, display, phoneName] {
        auto logLater = [this](const QString& line) { post([this, line] { log(line); }); };
        const std::string token = info.sessionToken;
        std::unique_ptr<IByteStream> stream;
        QStringList failures;
        // Wi-Fi first (much faster). When the phone also offers Bluetooth, give up on Wi-Fi quickly:
        // the two may simply not share a network.
        const bool bluetoothAlternative = bluetooth && info.transport == "rfcomm";
        if (!info.ip.empty() && info.port != 0 && !pullCancel_.isCancelled()) {
            logLater(QString("Connecting to %1:%2 over Wi-Fi ...").arg(q(info.ip)).arg(info.port));
            try {
                stream = connectTcp(info.ip, info.port, bluetoothAlternative ? 3000 : 10000);
            } catch (const std::exception& e) {
                failures << "Wi-Fi: " + q(e.what());
            }
        }
#ifdef ANYSHARE_HAVE_BLUEZ
        if (!stream && bluetoothAlternative && !pullCancel_.isCancelled()) {
            logLater("Connecting to \"" + phoneName + "\" over Bluetooth ...");
            try {
                stream = bt::makeStreamFactory(0)(info, 20000);   // Wi-Fi was just tried above
            } catch (const std::exception& e) {
                failures << "Bluetooth: " + q(e.what());
            }
        }
#endif
        if (!stream) {
            QString message = pullCancel_.isCancelled() ? QString("Cancelled")
                              : failures.isEmpty()
                                  ? "The phone offered no address this computer can reach (no shared Wi-Fi, no Bluetooth)"
                                  : "Could not reach the phone: " + failures.join("; ");
            if (info.transport == "rfcomm" && !bluetooth && !pullCancel_.isCancelled()) {
                message += ". This computer has no usable Bluetooth: put both on the same Wi-Fi.";
            }
            if (display) display->showDone(false, "Could not reach the phone");
            post([this, message, token] { finishPull(false, message, {}, token); });
            return;
        }
        const QString link = q(stream->describe());
        logLater("Connected to the phone via " + link + "; fetching its files");
        IByteStream* raw = stream.get();
        pullCancel_.setOnCancel([raw] { raw->abort(); });
        RelayObserver observer("Receiving", {
            [this](int percent, const QString& bytes, const QString& rate, const QString& eta) {
                post([this, percent, bytes, rate, eta] { showProgress(percent, bytes, rate, eta); });
            },
            logLater,
            [this](const QString& name, const QString& path) { post([this, name, path] { addRecent(name, path); }); },
            [this] { post([this] { stage_->start(false); }); },
        });
        std::unique_ptr<bridge::BridgeObserver> shown;
        if (display) shown = std::make_unique<bridge::BridgeObserver>(*display, false);
        bridge::TeeObserver both(&observer, shown.get());
        ReceiveOptions options;
        options.destinationDir = dir.toStdString();   // no acceptOffer: only the tap token authorises
        ReceiveResult result = receiveFilesByPull(*stream, token, me, options, &both, &pullCancel_);
        pullCancel_.setOnCancel(nullptr);
        const bool ok = result.status == TransferStatus::Success;
        QString message = ok ? QString("Received %1 file(s) from '%2'").arg(result.files.size()).arg(q(result.senderName))
                        : pullCancel_.isCancelled() ? QString("Cancelled")
                                                    : QString("Receive %1: %2").arg(q(toString(result.status)), q(result.message));
        QString transport = "Transport: " + link + " · fetched after an NFC tap (phone authorised by its tap token)";
        post([this, ok, message, transport, token] { finishPull(ok, message, transport, token); });
    });
}

void MainWindow::finishPull(bool ok, const QString& message, const QString& transport, const std::string& token) {
    if (pullThread_.joinable()) pullThread_.join();
    pulling_ = false;
    if (ok) lastPulledToken_ = token;   // later reads of the same tap are ignored; a failed one may be retried
    setBusy(false);
    if (!transport.isEmpty()) transportLabel_->setText(transport);
    if (ok) progress_->setValue(100);
    if (stage_->running() || ok) stage_->finish(ok);
    setActivity(ok ? Activity::Done : Activity::Failed, message + (!ok && message != "Cancelled" ? ". Tap the phone again to retry." : ""));
    log((ok ? "DONE: " : "FAILED: ") + message);
    refreshNfcLine();
}

// Finds the ESP32 bridge in the background (the handshake can take ~3 s per port).
void MainWindow::connectDisplayAsync(const QString& port) {
    QString chosen = port == "auto" ? settings_.value("displayPort", "auto").toString() : port;
    if (chosen.isEmpty() || chosen == "off" || displayConnecting_) return;
    if (displayThread_.joinable()) displayThread_.join();
    displayConnecting_ = true;
    std::string path = chosen.toStdString();
    displayThread_ = std::thread([this, path] {
        std::shared_ptr<bridge::Esp32Bridge> found;
        QString problem;
        try {
            found = bridge::Esp32Bridge::open(path);
        } catch (const std::exception& e) {
            problem = q(e.what());
        }
        post([this, found, problem] {
            if (displayThread_.joinable()) displayThread_.join();
            displayConnecting_ = false;
            display_ = found;
            if (found) {
                log("ESP32 NFC reader + display connected on " + q(found->port()));
                found->setOnEvent([this](const bridge::NfcEvent& e) { post([this, e] { onNfcEvent(e); }); });
                if (receiving_ && !servedHandover_.empty()) found->serveHandover(servedHandover_);
            }
            else log("ESP32 display: none (" + problem + ")", true);   // terminal only
            refreshNfcLine();
        });
    });
}

void MainWindow::onCancelPressed() {
    sendCancel_.cancel();
    pullCancel_.cancel();
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (engine_) engine_->cancel();
}

void MainWindow::setBusy(bool busy) {
    sendButton_->setEnabled(!busy);
    addButton_->setEnabled(!busy);
    clearButton_->setEnabled(!busy);
    cancelButton_->setVisible(busy);
}

// ============================================================================
// Receiving
// ============================================================================
QString MainWindow::receiveDir() const {
    if (!options_.receiveDir.isEmpty()) return options_.receiveDir;
    QString fallback = QDir::homePath() + "/Downloads/AnyShare";
    return settings_.value("receiveDir", fallback).toString();
}

bool MainWindow::startReceiving() {
    if (receiving_) return true;
    if (receiveThread_.joinable()) receiveThread_.join();
    if (receiveThreadBt_.joinable()) receiveThreadBt_.join();
    QString dir = receiveDir();
    if (!QDir().mkpath(dir)) {
        status_->setText("Cannot create " + dir);
        return false;
    }
    // Wi-Fi always; Bluetooth too when there is an adapter, so phones without a shared
    // network can still send (NFC tap or paired-device list). Both listen at once.
    bool withBluetooth = bluetoothUsable_ && receiveTransport_->currentData().toString() == "both";
    (void)withBluetooth;   // used only with BlueZ
    using AcceptFn = std::function<std::unique_ptr<IByteStream>(CancelToken*)>;
    AcceptFn acceptNext, acceptNextBt;
    QString where;
    std::string token = protocol::randomHex(8);
    try {
        {
            auto listener = std::make_shared<TcpListener>("0.0.0.0", static_cast<uint16_t>(portBox_->value()));
            listeningPort_ = listener->port();
            acceptNext = [listener](CancelToken* cancel) -> std::unique_ptr<IByteStream> { return listener->accept(-1, cancel); };
            std::string ip = host::firstLanIpv4();
            responder_ = std::make_unique<discovery::Responder>(deviceName().toStdString(), listeningPort_, "linux",
                                                                static_cast<uint16_t>(options_.discoveryPort));
            where = responder_->running() ? "Ready. Phones and PCs on this network see \"" + deviceName() + "\"."
                                          : "Ready (not findable by name: " + q(responder_->error()) + ").";
            if (!responder_->running()) log("Not discoverable by name: " + q(responder_->error()));
            where += QString("\nAddress: %1:%2").arg(q(ip.empty() ? "127.0.0.1" : ip)).arg(listeningPort_);
            ConnectionInfo self;   // what an NFC tap on the ESP32 module hands to a phone
            self.transport = "tcp";
            self.ip = ip.empty() ? "127.0.0.1" : ip;
            self.port = listeningPort_;
            self.deviceName = deviceName().toStdString();
            self.sessionToken = token;
#ifdef ANYSHARE_HAVE_BLUEZ
            if (withBluetooth) {
                try {   // a Bluetooth problem must not stop Wi-Fi receiving
                    auto bt = std::make_shared<bt::RfcommListener>(0);
                    acceptNextBt = [bt](CancelToken* cancel) -> std::unique_ptr<IByteStream> { return bt->accept(-1, cancel); };
                    self.transport = "rfcomm";   // the tap offers Bluetooth *and* the Wi-Fi address
                    self.btAddress = bt::localAdapterAddress();
                    self.rfcommChannel = bt->channel();
                    where += QString("\nBluetooth: %1 (channel %2)").arg(q(self.btAddress)).arg(bt->channel());
                    if (!bt->sdpRegistered()) {
                        where += "\nPhones can only send over Bluetooth after an NFC tap until bluetoothd runs with --compat";
                        log("Bluetooth SDP record not registered: " + q(bt->sdpError()), true);
                    }
                } catch (const std::exception& e) {
                    log("Bluetooth receiving not available: " + q(e.what()));
                }
            }
#endif
            servedHandover_ = connectionInfoToJson(self);
            if (host::runningUnderWsl() && ip.rfind("172.", 0) == 0) {
                where += "\n(WSL NAT mode: phones cannot reach this address; use WSL mirrored networking)";
            }
        }
    } catch (const std::exception& e) {
        status_->setText("Cannot start receiving: " + q(e.what()));
        log("Cannot start receiving: " + q(e.what()));
        return false;
    }
    settings_.setValue("port", portBox_->value());

    receiveCancel_.reset();
    receiveCancel_.setOnCancel(nullptr);
    receiving_ = true;
    receiveButton_->setText("Stop receiving");
    beacon_->setActive(true);
    portBox_->setEnabled(false);
    receiveTransport_->setEnabled(false);
    if (displayConnected() && !servedHandover_.empty()) {
        display_->serveHandover(servedHandover_);
        where += "\nNFC: phones can also tap the ESP32 reader to send here.";
    }
    receiveInfo_->setText(where);
    refreshNfcLine();
    status_->setText("Receive mode: waiting for a sender");
    log("Receiving into " + dir + " · " + QString(where).replace('\n', ' '));

    ReceiveOptions options;
    options.destinationDir = dir.toStdString();
    options.expectedToken = token;   // phones that read it from the ESP32 tap are accepted at once
    bool autoAccept = options_.autoAccept;
    options.acceptOffer = [this, autoAccept](const protocol::Offer& offer) {
        QStringList lines;
        for (std::size_t i = 0; i < offer.files.size() && i < 20; ++i) {
            lines << QString("%1  (%2)").arg(q(protocol::sanitizeFilename(offer.files[i].name)),
                                              q(host::humanBytes(offer.files[i].size)));
        }
        if (offer.files.size() > 20) lines << QString("... and %1 more").arg(offer.files.size() - 20);
        QString sender = q(offer.senderName), total = q(host::humanBytes(offer.totalBytes()));
        post([this, sender, total] {
            log("Incoming offer from '" + sender + "' (" + total + ")");
            stage_->setPeer(sender, peerLooksLikeComputer(sender));
            stage_->reset();
        });
        if (autoAccept) return true;
        auto answer = std::make_shared<std::promise<bool>>();
        std::future<bool> decided = answer->get_future();
        post([this, answer, sender, lines, total] { answer->set_value(askAcceptOffer(sender, lines, total)); });
        while (decided.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (receiveCancel_.isCancelled()) return false;
        }
        return decided.get();
    };

    int generation = ++receiveGeneration_;
    std::shared_ptr<bridge::Esp32Bridge> display = display_;
    auto oneAtATime = std::make_shared<std::mutex>();   // Wi-Fi and Bluetooth: one transfer at a time
    // primary = the Wi-Fi loop: when it ends, receiving has ended. The Bluetooth loop only logs.
    auto loop = [this, options, generation, display, oneAtATime](AcceptFn acceptNext, bool primary) {
        auto logLater = [this](const QString& line) { post([this, line] { log(line); }); };
        QString endMessage = "Stopped receiving";
        while (!receiveCancel_.isCancelled()) {
            std::unique_ptr<IByteStream> stream;
            try {
                stream = acceptNext(&receiveCancel_);
            } catch (const std::exception& e) {
                endMessage = "Receiver stopped: " + q(e.what());
                break;
            }
            if (!stream) break;
            std::lock_guard<std::mutex> session(*oneAtATime);
            QString peer = q(stream->describe());
            logLater("Connection from " + peer);
            post([this] {
                progress_->setValue(0);
                currentFile_->clear();
                setActivity(Activity::Receiving, "Receiving...");
            });
            IByteStream* raw = stream.get();
            receiveCancel_.setOnCancel([raw] { raw->abort(); });
            RelayObserver observer("Receiving", {
                [this](int percent, const QString& bytes, const QString& rate, const QString& eta) {
                    post([this, percent, bytes, rate, eta] { showProgress(percent, bytes, rate, eta); });
                },
                logLater,
                [this](const QString& name, const QString& path) { post([this, name, path] { addRecent(name, path); }); },
                [this] { post([this] { stage_->start(false); }); },
            });
            std::unique_ptr<bridge::BridgeObserver> shown;
            if (display) shown = std::make_unique<bridge::BridgeObserver>(*display, false);
            bridge::TeeObserver both(&observer, shown.get());
            ReceiveResult result = receiveFiles(*stream, options, &both, &receiveCancel_);
            receiveCancel_.setOnCancel(nullptr);
            bool ok = result.status == TransferStatus::Success;
            QString summary = ok ? QString("Received %1 file(s) from '%2'").arg(result.files.size()).arg(q(result.senderName))
                                 : QString("Receive %1: %2").arg(q(toString(result.status)), q(result.message));
            QString auth = result.tokenMatched ? "tap token" : "manual confirmation";
            post([this, ok, summary, peer, auth] {
                if (ok) progress_->setValue(100);
                if (stage_->running() || ok) stage_->finish(ok);
                setActivity(ok ? Activity::Done : Activity::Failed, summary + (receiving_ ? "  ·  still waiting for more" : ""));
                transportLabel_->setText("Transport: " + peer + " · sender authorised by " + auth);
                log((ok ? "DONE: " : "FAILED: ") + summary);
            });
        }
        if (!primary) {
            if (!receiveCancel_.isCancelled()) logLater("Bluetooth receiving stopped: " + endMessage);
            return;
        }
        post([this, endMessage, generation] {
            if (!receiving_ || generation != receiveGeneration_) return;   // stopReceiving() already updated the UI
            receiving_ = false;
            servedHandover_.clear();
            if (displayConnected()) display_->stopServing();
            refreshNfcLine();
            receiveButton_->setText("Start receiving");
            beacon_->setActive(false);
            portBox_->setEnabled(true);
            receiveTransport_->setEnabled(true);
            receiveInfo_->setText(endMessage);
            log(endMessage);
        });
    };
    receiveThread_ = std::thread(loop, acceptNext, true);
    if (acceptNextBt) receiveThreadBt_ = std::thread(loop, acceptNextBt, false);
    return true;
}

void MainWindow::stopReceiving() {
    receiveCancel_.cancel();
    if (receiveThread_.joinable()) receiveThread_.join();
    if (receiveThreadBt_.joinable()) receiveThreadBt_.join();
    responder_.reset();
    if (!receiving_) return;
    servedHandover_.clear();
    if (displayConnected()) display_->stopServing();
    refreshNfcLine();
    receiving_ = false;
    receiveButton_->setText("Start receiving");
    beacon_->setActive(false);
    portBox_->setEnabled(true);
    receiveTransport_->setEnabled(true);
    receiveInfo_->setText("Not visible. Start receiving so phones and PCs can find this computer.");
    setActivity(Activity::Idle, "Ready");
    log("Stopped receiving");
}

bool MainWindow::askAcceptOffer(const QString& sender, const QStringList& lines, const QString& total) {
    QMessageBox box(QMessageBox::Question, "Incoming files",
                    QString("'%1' wants to send you %2:\n\n%3\n\nNo NFC tap was used; accept only if you expect these files.")
                        .arg(sender, total, lines.join('\n')),
                    QMessageBox::NoButton, this);
    box.setObjectName("acceptDialog");
    QPushButton* accept = box.addButton("Accept", QMessageBox::AcceptRole);
    box.addButton("Decline", QMessageBox::RejectRole);
    box.exec();
    bool yes = box.clickedButton() == accept;
    log(yes ? "Offer accepted" : "Offer declined");
    return yes;
}

// ============================================================================
// Helpers
// ============================================================================
void MainWindow::post(std::function<void()> fn) {
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

void MainWindow::log(const QString& line, bool terminalOnly) {
    QString stamped = QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + line;
    std::printf("%s\n", stamped.toUtf8().constData());
    std::fflush(stdout);
    if (!terminalOnly && log_) log_->appendPlainText(stamped);
}
