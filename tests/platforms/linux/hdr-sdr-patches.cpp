// Fullscreen, untagged SDR color patches for measuring the entire nested-KWin
// -> Wolf PQ capture path. Build with pkg-config Qt6Gui; needs no game/user data.
#include <QBackingStore>
#include <QGuiApplication>
#include <QPainter>
#include <QTimer>
#include <QWindow>

class Patches : public QWindow {
  QBackingStore store{this};
  void exposeEvent(QExposeEvent *) override {
    if (!isExposed()) return;
    store.resize(size());
    store.beginPaint(QRect(QPoint(), size()));
    QPainter painter(store.paintDevice());
    const QColor colors[] = {Qt::white, QColor(128, 128, 128), Qt::red,
                             Qt::green, Qt::blue, Qt::black};
    for (int i = 0; i < 6; ++i)
      painter.fillRect(QRect(i * width() / 6, 0,
                            (i + 1) * width() / 6 - i * width() / 6, height()), colors[i]);
    painter.end();
    store.endPaint();
    store.flush(QRect(QPoint(), size()));
  }
};

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  Patches patches;
  patches.setTitle("Wolf SDR color regression patches");
  patches.showFullScreen();
  QTimer::singleShot(15000, &app, &QGuiApplication::quit);
  return app.exec();
}
