    // anyshare-gui -- Qt desktop app for AnyShare on Linux.
// Run it from a terminal to see the engine log there as well.
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>

#include "MainWindow.hpp"

namespace {
std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit.store(true); }
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AnyShare");
    QApplication::setOrganizationName("AnyShare");

    QCommandLineParser parser;
    parser.setApplicationDescription("AnyShare desktop app");
    parser.addHelpOption();
    QCommandLineOption receive("receive", "Start receiving immediately.");
    QCommandLineOption yes("yes", "Accept incoming files without asking (unattended tests).");
    QCommandLineOption dir("dir", "Folder for received files.", "DIR");
    QCommandLineOption port("port", "TCP port to receive on (0 = any free port).", "N");
    QCommandLineOption noProbe("no-nfc-probe", "Do not look for an NFC reader at start-up.");
    QCommandLineOption esp32("esp32", "ESP32 OLED bridge: auto (default), off, or a port like /dev/ttyUSB0.", "PORT");
    parser.addOptions({receive, yes, dir, port, noProbe, esp32});
    parser.process(app);

    GuiOptions options;
    options.startReceiving = parser.isSet(receive);
    options.autoAccept = parser.isSet(yes);
    options.receiveDir = parser.value(dir);
    options.probeNfc = !parser.isSet(noProbe);
    if (parser.isSet(esp32)) options.displayPort = parser.value(esp32) == "off" ? QString() : parser.value(esp32);
    if (parser.isSet(port)) {
        bool ok = false;
        options.port = parser.value(port).toInt(&ok);
        if (!ok || options.port < 0 || options.port > 65535) {
            std::fprintf(stderr, "invalid --port\n");
            return 2;
        }
    }

    // Ctrl-C in the terminal closes the window cleanly (transfers are cancelled).
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QTimer quitPoll;
    QObject::connect(&quitPoll, &QTimer::timeout, &app, [] {
        if (g_quit.load()) QApplication::closeAllWindows();
    });
    quitPoll.start(200);

    MainWindow window(options);
    window.show();
    std::printf("AnyShare window is open. Logs appear here; close the window or press Ctrl-C to quit.\n");
    std::fflush(stdout);
    return app.exec();
}
