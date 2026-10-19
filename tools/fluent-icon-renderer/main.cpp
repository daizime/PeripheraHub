#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextStream>

// Development tool only. PeripheralHub still links just Core/Gui/Widgets.
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3)
        return 2;
    const QDir source(app.arguments().at(1));
    const QDir output(app.arguments().at(2));
    if (!QDir().mkpath(output.absolutePath()))
        return 3;
    const auto files = source.entryInfoList({"*.svg"}, QDir::Files, QDir::Name);
    if (files.isEmpty())
        return 4;
    for (const auto& file : files) {
        QSvgRenderer renderer(file.absoluteFilePath());
        if (!renderer.isValid())
            return 5;
        // 24 logical px at 4x. Also supplies crisp 16/20/24/28 px icons at high DPI.
        QImage image(96, 96, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        painter.end();
        if (!image.save(output.filePath(file.completeBaseName() + ".png")))
            return 6;
    }
    QTextStream(stdout) << "Rendered " << files.size() << " Fluent icons at 96 x 96.\n";
    return 0;
}
