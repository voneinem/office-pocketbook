#pragma once
// Hands the pictures of a mail to QML: Image { source: "image://mailimg/<messageId>/<index>" }. The bytes come from the store (Store::image_data); nothing is
// fetched here. The picture is decoded with a size check (a picture of more than 16 megapixels is refused), made gray (the panel is gray) and, when QML
// asks for a size, decoded at that size. An unknown or not loaded picture, and one that cannot be decoded, gives a null image.

#include <QImage>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>

#include "pbo/store.h"

class MailImageProvider : public QQuickImageProvider {
public:
    explicit MailImageProvider(pbo::Store* store) : QQuickImageProvider(QQuickImageProvider::Image), store_(store) {}

    // `id` is "<messageId>/<index>", optionally followed by "?..." (a cache key that changes when the picture changes).
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    pbo::Store* store_;
};
