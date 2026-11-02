#pragma once

// Small custom widgets for the AnyShare desktop app. Callbacks instead of
// signals keep them free of moc.
#include <QElapsedTimer>
#include <QIcon>
#include <QVariantAnimation>
#include <QWidget>

#include <deque>
#include <functional>

// Invisible hot spot in the window's top-right corner. Three right-clicks in
// quick succession open the hidden mode menu (the desktop twin of the
// Android app's long-press on the top-right corner).
class SecretCorner : public QWidget {
public:
    static constexpr qint64 kWindowMs = 900;   // all three clicks within this time

    explicit SecretCorner(QWidget* parent = nullptr);
    void setOnTriggered(std::function<void()> callback) { onTriggered_ = std::move(callback); }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;   // one release per physical click
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    std::function<void()> onTriggered_;
    QElapsedTimer clock_;
    std::deque<qint64> clicks_;
};

// Receive screen centrepiece: this computer's name in a circle; rings pulse
// outwards while receiving (static and grey when not visible).
class ReceiveBeacon : public QWidget {
public:
    explicit ReceiveBeacon(QWidget* parent = nullptr);
    void setName(const QString& name);
    void setActive(bool active);
    bool isActive() const { return active_; }
    QSize sizeHint() const override { return {220, 200}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation pulse_;
    QString name_;
    bool active_ = false;
    double phase_ = 0;
};

// Transfer animation: this computer on the left, the peer (laptop or phone) on
// the right, packets flying from the sender to the receiver while running.
class TransferStage : public QWidget {
public:
    enum class Result { None, Ok, Failed };
    explicit TransferStage(QWidget* parent = nullptr);
    void setPeer(const QString& name, bool isComputer);
    void start(bool sending);            // packets fly: sending = this computer -> peer
    void finish(bool ok);                // stop; show a check or a cross on the receiver
    void reset();                        // idle: no packets, no result
    bool running() const { return running_; }
    bool sending() const { return sending_; }
    QString peerName() const { return peerName_; }
    bool peerIsComputer() const { return peerIsComputer_; }
    QSize sizeHint() const override { return {320, 92}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation flow_;
    QString peerName_ = "Other device";
    bool peerIsComputer_ = false;
    bool sending_ = true;
    bool running_ = false;
    Result result_ = Result::None;
    double phase_ = 0;
};

// Small drawn icons (no icon theme needed, same look on every desktop).
QIcon deviceIcon(const QString& os);            // "linux" -> laptop, anything else -> phone
QIcon fileTypeIcon(const QString& typeLabel);   // "Image", "Video", "PDF", ... (bridge::fileTypeLabel)

// Colours shared with the ESP32 status LED: sending purple, receiving red, done green, error amber.
namespace ui {
constexpr const char* kAccent = "#4F46E5";
constexpr const char* kSending = "#7C3AED";
constexpr const char* kReceiving = "#E11D48";
constexpr const char* kDone = "#10B981";
constexpr const char* kError = "#F59E0B";
constexpr const char* kIdle = "#6B7280";
}  // namespace ui

// "Tap" card for the NFC-style demo: two phones slide together when clicked.
// It only animates; the caller starts the (real) transfer when it finishes.
class TapCard : public QWidget {
public:
    explicit TapCard(QWidget* parent = nullptr);
    void setOnTapped(std::function<void()> callback) { onTapped_ = std::move(callback); }
    void setCaption(const QString& caption);
    void playTap();   // also used by tests and the keyboard shortcut

    QSize sizeHint() const override { return {360, 120}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    std::function<void()> onTapped_;
    QVariantAnimation animation_;
    QString caption_;
    double phase_ = 0;   // 0 = apart, 1 = touching
};
