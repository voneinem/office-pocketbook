#include "mail_image_provider.h"

#include <QBuffer>
#include <QByteArray>
#include <QImageReader>

#include "pbo/log.h"

QImage MailImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    if (!store_) return QImage();
    const QString path = id.left(id.indexOf(QLatin1Char('?')) < 0 ? id.size() : id.indexOf(QLatin1Char('?')));
    const QStringList parts = path.split(QLatin1Char('/'));
    bool ok_message = false, ok_index = false;
    const qlonglong message = parts.value(0).toLongLong(&ok_message);
    const int index = parts.value(1).toInt(&ok_index);
    if (parts.size() != 2 || !ok_message || !ok_index) return QImage();
    const auto data = store_->image_data(message, index);
    if (!data) return QImage();

    QByteArray bytes(data->data(), static_cast<qsizetype>(data->size()));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (!original.isValid() || original.width() <= 0 || original.height() <= 0 || static_cast<qint64>(original.width()) * original.height() > 16 * 1000 * 1000) {
        pbo::log::event("image.undecodable", {pbo::log::count("size_known", original.isValid() ? 1 : 0)});
        return QImage();
    }
    if (size) *size = original;
    // decoded small when QML asks for a size (JPEG can do that cheaply); a picture is never made larger. A side that is not given (0) is free.
    {
        const int w = requestedSize.width(), h = requestedSize.height();
        QSize target = original;
        if (w > 0 && h > 0) target = original.scaled(QSize(w, h), Qt::KeepAspectRatio);
        else if (w > 0) target = original.scaled(QSize(w, 1 << 20), Qt::KeepAspectRatio);
        else if (h > 0) target = original.scaled(QSize(1 << 20, h), Qt::KeepAspectRatio);
        if (target.width() < original.width() || target.height() < original.height()) reader.setScaledSize(target.expandedTo(QSize(1, 1)));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        pbo::log::event("image.undecodable", {pbo::log::count("size_known", 1)});
        return QImage();
    }
    return image.convertToFormat(QImage::Format_Grayscale8);
}
