#include "mainwindow.h"

#include <QApplication>
#include <QTimer>
#include <QTabWidget>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    const QStringList arguments = a.arguments();
    if (arguments.contains("--demo")) w.startPreview();
    if (arguments.contains("--spectrum"))
        w.findChild<QTabWidget *>()->setCurrentWidget(w.findChild<QWidget *>("spectrumPage"));
    if (arguments.contains("--send") || arguments.contains("--duplex") || !arguments.contains("--spectrum"))
        w.findChild<QTabWidget *>()->setCurrentWidget(w.findChild<QWidget *>("sendPage"));
    const int preview = arguments.indexOf("--preview");
    if (preview >= 0 && preview + 1 < arguments.size()) {
        w.startPreview();
        const QString path = arguments[preview + 1];
        QTimer::singleShot(1250, &w, [&w, path] {
            const bool saved = w.grab().save(path);
            QApplication::exit(saved ? 0 : 1);
        });
    }
    return a.exec();
}
