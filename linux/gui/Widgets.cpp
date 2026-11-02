#include "Widgets.hpp"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMap>
#include <QPixmap>

#include <algorithm>
#include <cmath>

SecretCorner::SecretCorner(QWidget* parent) : QWidget(parent) {
    setFixedSize(56, 44);
    setObjectName("secretCorner");
    clock_.start();
}

void SecretCorner::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::RightButton) {
        clicks_.clear();
        return;
    }
    qint64 now = clock_.elapsed();
    clicks_.push_back(now);
    while (!clicks_.empty() && now - clicks_.front() > kWindowMs) clicks_.pop_front();
    if (clicks_.size() >= 3) {
        clicks_.clear();
        if (onTriggered_) onTriggered_();
    }
}

void SecretCorner::contextMenuEvent(QContextMenuEvent* event) {
    event->accept();   // no context menu: right-clicks are counted instead
}

TapCard::TapCard(QWidget* parent) : QWidget(parent) {
    setObjectName("tapCard");
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(120);
    animation_.setDuration(1100);
    animation_.setStartValue(0.0);
    animation_.setKeyValueAt(0.45, 1.0);
    animation_.setKeyValueAt(0.7, 1.0);
    animation_.setEndValue(0.0);
    animation_.setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        phase_ = v.toDouble();
        update();
    });
    QObject::connect(&animation_, &QVariantAnimation::finished, this, [this] {
        if (onTapped_) onTapped_();
    });
}

void TapCard::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}

void TapCard::playTap() {
    if (animation_.state() == QAbstractAnimation::Running) return;
    animation_.start();
}

void TapCard::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) playTap();
}

void TapCard::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF card = rect().adjusted(1, 1, -1, -1);
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.setBrush(palette().color(QPalette::Base));
    p.drawRoundedRect(card, 12, 12);

    const double phoneW = 34, phoneH = 58, gapApart = 120;
    double gap = gapApart * (1.0 - phase_) + 4;
    double cx = card.center().x(), top = card.top() + 12;
    QRectF left(cx - gap / 2 - phoneW, top, phoneW, phoneH);
    QRectF right(cx + gap / 2, top, phoneW, phoneH);

    if (phase_ > 0.85) {   // contact ripple
        QColor ripple(33, 150, 243, static_cast<int>(255 * (phase_ - 0.85) / 0.15 * 0.5));
        p.setPen(QPen(ripple, 3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, top + phoneH / 2), 30, 30);
    }
    p.setPen(QPen(palette().color(QPalette::Text), 2));
    p.setBrush(QColor(33, 150, 243));
    p.drawRoundedRect(left, 6, 6);
    p.setBrush(QColor(0, 150, 136));
    p.drawRoundedRect(right, 6, 6);

    p.setPen(palette().color(QPalette::Text));
    QRectF textArea(card.left() + 8, top + phoneH + 6, card.width() - 16, card.bottom() - top - phoneH - 8);
    p.drawText(textArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, caption_);
}

ReceiveBeacon::ReceiveBeacon(QWidget* parent) : QWidget(parent) {
    setObjectName("receiveBeacon");
    setMinimumSize(200, 180);
    pulse_.setDuration(1800);
    pulse_.setStartValue(0.0);
    pulse_.setEndValue(1.0);
    pulse_.setLoopCount(-1);
    QObject::connect(&pulse_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        phase_ = v.toDouble();
        update();
    });
}

void ReceiveBeacon::setName(const QString& name) {
    name_ = name;
    update();
}

void ReceiveBeacon::setActive(bool active) {
    active_ = active;
    if (active) pulse_.start();
    else pulse_.stop();
    phase_ = 0;
    update();
}

void ReceiveBeacon::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPointF c = rect().center();
    double core = std::min(width(), height()) * 0.28;
    QColor accent = active_ ? QColor(ui::kAccent) : QColor(ui::kIdle);

    if (active_) {   // two rings expanding and fading out
        for (int i = 0; i < 2; ++i) {
            double t = std::fmod(phase_ + i * 0.5, 1.0);
            QColor ring = accent;
            ring.setAlphaF(0.45 * (1.0 - t));
            p.setPen(QPen(ring, 3));
            p.setBrush(Qt::NoBrush);
            double r = core + t * core * 0.75;
            p.drawEllipse(c, r, r);
        }
    }
    QColor fill = accent;
    fill.setAlphaF(active_ ? 1.0 : 0.25);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(c, core, core);

    // A small laptop glyph and the name inside the circle.
    p.setPen(QPen(active_ ? Qt::white : QColor(ui::kIdle), 2.5));
    p.setBrush(Qt::NoBrush);
    QRectF screen(c.x() - core * 0.38, c.y() - core * 0.55, core * 0.76, core * 0.46);
    p.drawRoundedRect(screen, 3, 3);
    p.drawLine(QPointF(c.x() - core * 0.5, screen.bottom() + 5), QPointF(c.x() + core * 0.5, screen.bottom() + 5));
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.05);
    p.setFont(f);
    p.setPen(active_ ? Qt::white : QColor("#374151"));
    QRectF text(c.x() - core * 0.95, c.y() + core * 0.05, core * 1.9, core * 0.6);
    p.drawText(text, Qt::AlignCenter, QFontMetrics(f).elidedText(name_, Qt::ElideRight, static_cast<int>(text.width())));
}

namespace {

QPixmap badgeBase(const QColor& colour) {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = colour;
    bg.setAlphaF(0.15);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 14, 14);
    return pm;
}

}  // namespace

QIcon deviceIcon(const QString& os) {
    QColor colour(ui::kAccent);
    QPixmap pm = badgeBase(colour);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour, 3.5));
    p.setBrush(Qt::NoBrush);
    if (os == "linux") {   // laptop: screen + base
        p.drawRoundedRect(QRectF(15, 17, 34, 22), 3, 3);
        p.drawLine(QPointF(10, 46), QPointF(54, 46));
    } else {               // phone
        p.drawRoundedRect(QRectF(22, 12, 20, 40), 4, 4);
        p.drawPoint(QPointF(32, 46));
    }
    p.end();
    return QIcon(pm);
}

QIcon fileTypeIcon(const QString& typeLabel) {
    static const QMap<QString, QPair<QString, QString>> styles = {
        {"Image", {"IMG", "#0EA5E9"}},  {"Video", {"VID", "#E11D48"}},     {"Audio", {"AUD", "#F59E0B"}},
        {"PDF", {"PDF", "#DC2626"}},    {"Document", {"DOC", "#2563EB"}},  {"Spreadsheet", {"XLS", "#16A34A"}},
        {"Slides", {"PPT", "#EA580C"}}, {"Text", {"TXT", "#6B7280"}},      {"Archive", {"ZIP", "#7C3AED"}},
        {"Android app", {"APK", "#22C55E"}},
    };
    auto style = styles.value(typeLabel, {"FILE", "#6B7280"});
    QColor colour(style.second);
    QPixmap pm = badgeBase(colour);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f;
    f.setBold(true);
    f.setPixelSize(style.first.size() > 3 ? 13 : 16);
    p.setFont(f);
    p.setPen(colour);
    p.drawText(QRectF(0, 0, 64, 64), Qt::AlignCenter, style.first);
    p.end();
    return QIcon(pm);
}

TransferStage::TransferStage(QWidget* parent) : QWidget(parent) {
    setObjectName("transferStage");
    setMinimumSize(280, 88);
    flow_.setDuration(1200);
    flow_.setStartValue(0.0);
    flow_.setEndValue(1.0);
    flow_.setLoopCount(-1);
    QObject::connect(&flow_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        phase_ = v.toDouble();
        update();
    });
}

void TransferStage::setPeer(const QString& name, bool isComputer) {
    peerName_ = name.isEmpty() ? QString("Other device") : name;
    peerIsComputer_ = isComputer;
    update();
}

void TransferStage::start(bool sending) {
    sending_ = sending;
    running_ = true;
    result_ = Result::None;
    flow_.start();
    update();
}

void TransferStage::finish(bool ok) {
    running_ = false;
    result_ = ok ? Result::Ok : Result::Failed;
    flow_.stop();
    update();
}

void TransferStage::reset() {
    running_ = false;
    result_ = Result::None;
    flow_.stop();
    update();
}

namespace {

// A device glyph centred on `c`: laptop (screen + base) or phone.
void drawDevice(QPainter& p, QPointF c, bool computer, const QColor& colour) {
    p.setPen(QPen(colour, 2.6));
    p.setBrush(Qt::NoBrush);
    if (computer) {
        p.drawRoundedRect(QRectF(c.x() - 17, c.y() - 15, 34, 22), 3, 3);
        p.drawLine(QPointF(c.x() - 23, c.y() + 11), QPointF(c.x() + 23, c.y() + 11));
    } else {
        p.drawRoundedRect(QRectF(c.x() - 10, c.y() - 19, 20, 36), 4, 4);
        p.drawPoint(QPointF(c.x(), c.y() + 12));
    }
}

}  // namespace

void TransferStage::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double iconY = height() * 0.40;
    const QPointF self(54, iconY), peer(width() - 54, iconY);
    QColor accent = !running_ && result_ == Result::None ? QColor(ui::kIdle)
                    : result_ == Result::Ok              ? QColor(ui::kDone)
                    : result_ == Result::Failed          ? QColor(ui::kError)
                    : sending_                           ? QColor(ui::kSending)
                                                         : QColor(ui::kReceiving);

    // Dashed track between the two devices.
    QColor track = accent;
    track.setAlphaF(0.35);
    p.setPen(QPen(track, 2, Qt::DashLine));
    p.drawLine(QPointF(self.x() + 34, iconY), QPointF(peer.x() - 34, iconY));

    drawDevice(p, self, true, QColor("#374151"));
    drawDevice(p, peer, peerIsComputer_, QColor("#374151"));

    // Names under the devices.
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    p.setPen(QColor("#6B7280"));
    QFontMetrics fm(f);
    p.drawText(QRectF(self.x() - 54, iconY + 24, 108, 18), Qt::AlignCenter, "This computer");
    p.drawText(QRectF(peer.x() - 54, iconY + 24, 108, 18), Qt::AlignCenter, fm.elidedText(peerName_, Qt::ElideRight, 104));

    // Packets: three small squares travelling from the sender to the receiver.
    double from = sending_ ? self.x() + 36 : peer.x() - 36;
    double to = sending_ ? peer.x() - 36 : self.x() + 36;
    if (running_) {
        for (int i = 0; i < 3; ++i) {
            double t = std::fmod(phase_ + i / 3.0, 1.0);
            double x = from + (to - from) * t;
            QColor packet = accent;
            packet.setAlphaF(std::min(1.0, std::min(t, 1.0 - t) * 5.0));   // fade in and out at the ends
            p.setPen(Qt::NoPen);
            p.setBrush(packet);
            p.drawRoundedRect(QRectF(x - 7, iconY - 7, 14, 14), 3, 3);
        }
    } else if (result_ != Result::None) {   // check or cross next to the receiver
        QPointF mark = sending_ ? QPointF(peer.x() - 30, iconY - 20) : QPointF(self.x() + 30, iconY - 20);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(mark, 10, 10);
        p.setPen(QPen(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap));
        if (result_ == Result::Ok) {
            p.drawLine(mark + QPointF(-4.5, 0), mark + QPointF(-1, 3.5));
            p.drawLine(mark + QPointF(-1, 3.5), mark + QPointF(5, -3.5));
        } else {
            p.drawLine(mark + QPointF(-4, -4), mark + QPointF(4, 4));
            p.drawLine(mark + QPointF(-4, 4), mark + QPointF(4, -4));
        }
    }
}
