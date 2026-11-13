#include <codecairn/Document.h>
#include <codecairn/FileIO.h>
#include <codecairn/WorkspacePaths.h>
#include <QThread>
#include <algorithm>
#include <limits>
#include <utility>

namespace codecairn {

Document::Document(const QString &path, const QByteArray &bytes, bool hasUtf8Bom, QObject *parent)
    : QObject(parent), path_(path.startsWith(":/") ? path : workspacePath(path)), bytes_(bytes) {
    setSavedContents(bytes, hasUtf8Bom);
}

void Document::assertOwnerThread() const {
    Q_ASSERT(QThread::currentThread() == thread());
}

const QByteArray &Document::contents() const {
    assertOwnerThread();
    if (!bytesCurrent_) {
        if (provider_)
            bytes_ = provider_();
        bytesCurrent_ = true;
    }
    return bytes_;
}

const QByteArray &Document::contentHash() const {
    const auto &bytes = contents();
    if (!hashCurrent_) {
        hash_ = sourceContentHash(bytes);
        hashCurrent_ = true;
    }
    return hash_;
}

SourceSnapshot Document::snapshot() const {
    const auto &hash = contentHash();
    return {path_, bytes_, hash, revision_};
}

SourcePosition Document::position(int byteOffset) const {
    auto current = snapshot();
    const auto length = std::min<qsizetype>(current.bytes.size(), std::numeric_limits<int>::max());
    const auto bounded = std::clamp(byteOffset, 0, static_cast<int>(length));
    return {std::move(current), bounded};
}

bool Document::modified() const {
    return contentHash() != savedContentHash_;
}

void Document::setPath(QString path) {
    assertOwnerThread();
    path = path.startsWith(":/") ? path : workspacePath(path);
    if (path_ == path)
        return;
    path_ = std::move(path);
    contentChanged();
}

void Document::updateContents(const QByteArray &bytes) {
    assertOwnerThread();
    bytes_ = bytes;
    bytesCurrent_ = true;
    hashCurrent_ = false;
    ++revision_;
    emit contentsChanged();
}

void Document::contentChanged() {
    assertOwnerThread();
    bytesCurrent_ = !provider_;
    hashCurrent_ = false;
    ++revision_;
    emit contentsChanged();
}

void Document::bindContents(std::function<QByteArray()> provider) {
    assertOwnerThread();
    if (!provider && provider_)
        contents();
    provider_ = std::move(provider);
    bytesCurrent_ = !provider_;
    hashCurrent_ = false;
}

void Document::setSavedContents(const QByteArray &bytes, bool hasUtf8Bom) {
    assertOwnerThread();
    savedBytes_ = bytes;
    savedContentHash_ = sourceContentHash(bytes);
    hasUtf8Bom_ = hasUtf8Bom;
    lastReportedExternalHash_.clear();
}

Document::ExternalChange Document::externalChange(const QByteArray &hash, bool fileExists,
                                                  bool modified) {
    assertOwnerThread();
    if (!fileExists)
        return ExternalChange::Removed;
    if (hash.isEmpty() || hash == savedContentHash_)
        return ExternalChange::None;
    if (modified) {
        if (lastReportedExternalHash_ == hash)
            return ExternalChange::None;
        lastReportedExternalHash_ = hash;
        return ExternalChange::UnsavedBuffer;
    }
    return ExternalChange::Reload;
}

}
