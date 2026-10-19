#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <array>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

static constexpr std::array sizes{16, 20, 24, 32, 40, 48, 64, 128, 256, 512};
static QByteArray digest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
}

#ifdef Q_OS_WIN
static int verifyExecutable(const QString& path) {
    HMODULE module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                    LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return 10;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON);
    const auto* bytes = resource ? static_cast<const char*>(LockResource(LoadResource(module, resource))) : nullptr;
    if (!bytes) { FreeLibrary(module); return 11; }
    QByteArray group(bytes, static_cast<qsizetype>(SizeofResource(module, resource)));
    QDataStream stream(group);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint16 reserved, type, count;
    stream >> reserved >> type >> count;
    bool ok = reserved == 0 && type == 1 && count == sizes.size() - 1;
    for (int index = 0; index < count; ++index) {
        quint8 width, height, colors, entryReserved;
        quint16 planes, bits, id;
        quint32 length;
        stream >> width >> height >> colors >> entryReserved >> planes >> bits >> length >> id;
        const int size = width ? width : 256;
        const HRSRC frame = FindResourceW(module, MAKEINTRESOURCEW(id), RT_ICON);
        const auto* data = frame ? static_cast<const char*>(LockResource(LoadResource(module, frame))) : nullptr;
        const QImage image = data ? QImage::fromData(reinterpret_cast<const uchar*>(data),
                                                     static_cast<int>(SizeofResource(module, frame)), "PNG") : QImage{};
        HICON icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
        const bool valid = index < static_cast<int>(sizes.size()) - 1 && size == sizes[index] &&
                           image.size() == QSize(size, size) && image.hasAlphaChannel() && icon;
        QTextStream(stdout) << "EXE_ICON size=" << size << " valid=" << valid << '\n';
        ok = valid && ok;
        if (icon) DestroyIcon(icon);
    }
    FreeLibrary(module);
    return ok ? 0 : 12;
}
#endif

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
#ifdef Q_OS_WIN
    if (args.size() == 3 && args[1] == "--verify-exe") return verifyExecutable(args[2]);
#endif
    if (args.size() != 4) return 2;
    const QImage master(args[1]);
    const QDir output(args[2]);
    const QString reportPath = args[3];
    if (master.isNull() || !master.hasAlphaChannel() || master.width() != master.height() ||
        master.pixelColor(0, 0).alpha() != 0 || master.pixelColor(master.width() - 1, master.height() - 1).alpha() != 0 ||
        !QDir().mkpath(output.absolutePath())) return 3;
    QJsonArray manifest;
    QList<QByteArray> frames;
    QImage preview(1000, 400, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor("#f1f6ff"));
    QPainter painter(&preview);
    painter.fillRect(0, 200, 1000, 200, QColor("#19243c"));
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    painter.setFont(QFont("Microsoft YaHei UI", 11));
    for (const auto size : sizes) {
        const QImage image = master.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString name = QString("peripheral-hub-%1.png").arg(size);
        const QString path = output.filePath(name);
        if (!image.save(path, "PNG")) return 4;
        manifest.append(QJsonObject{{"file", name}, {"size", size}, {"sha256", QString::fromLatin1(digest(path))}});
        if (size <= 256) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, "PNG")) return 5;
            frames.append(png);
        }
    }
    const std::array previewSizes{16, 20, 24, 32, 40, 48, 64, 128};
    for (int index = 0; index < static_cast<int>(previewSizes.size()); ++index) {
        const int size = previewSizes[index];
        const QImage image(output.filePath(QString("peripheral-hub-%1.png").arg(size)));
        for (const int row : {0, 1}) {
            const int x = 60 + index * 120;
            painter.drawImage(QPoint(x - size / 2, row * 200 + 95 - size / 2), image);
            painter.setPen(row == 0 ? QColor("#1b3155") : QColor("#e5edff"));
            painter.drawText(QRect(x - 50, row * 200 + 165, 100, 25), Qt::AlignCenter, QString::number(size) + " px");
        }
    }
    painter.end();
    if (!preview.save(reportPath, "PNG")) return 6;
    QFile icon(output.filePath("PeripheralHub.ico"));
    if (!icon.open(QIODevice::WriteOnly)) return 7;
    QDataStream stream(&icon);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(frames.size());
    quint32 offset = 6 + 16 * static_cast<quint32>(frames.size());
    for (int index = 0; index < frames.size(); ++index) {
        const auto size = static_cast<quint8>(sizes[index] == 256 ? 0 : sizes[index]);
        stream << size << size << quint8(0) << quint8(0) << quint16(1) << quint16(32)
               << quint32(frames[index].size()) << offset;
        offset += static_cast<quint32>(frames[index].size());
    }
    for (const auto& frame : frames) if (stream.writeRawData(frame.data(), static_cast<int>(frame.size())) != frame.size()) return 8;
    icon.close();
    QFile file(output.filePath("manifest.json"));
    if (!file.open(QIODevice::WriteOnly)) return 9;
    file.write(QJsonDocument(QJsonObject{{"sourceSha256", QString::fromLatin1(digest(args[1]))},
                                        {"generator", "imagegen; Qt PNG resize and ICO packaging"},
                                        {"icoSha256", QString::fromLatin1(digest(icon.fileName()))},
                                        {"frames", manifest}}).toJson());
    QTextStream(stdout) << "ICON_EXPORT=PASS PNG_FRAMES=" << sizes.size() << " ICO_FRAMES=" << frames.size()
                        << " SOURCE_ALPHA=1\n";
    return 0;
}
