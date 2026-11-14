#include "SymbolCatalogModel.h"
#include "SymbolBrowser.h"
#include "UiAppearance.h"
#include <codecairn/Indexing.h>

#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QtConcurrentRun>

namespace {
constexpr int PageSize = 128;
}

QString SymbolCatalogModel::Request::key() const {
    return category + ':' + QString::number(offset);
}

SymbolCatalogModel::SymbolCatalogModel(bool categories, QObject *parent)
    : QAbstractItemModel(parent), grouped_(categories), job_(this) {
    pool_.setMaxThreadCount(1);
    connect(&job_, &QFutureWatcher<codecairn::SymbolPage>::finished, this, [this] {
        busy_ = false;
        if (running_.generation == generation_)
            queued_.remove(running_.key());
        const auto page = job_.result();
        if (running_.generation == generation_ && !page.cancelled) {
            if (!page.error.isEmpty()) {
                failedPages_.insert(running_.key(), page.error);
                const int group = categories_.indexOf(running_.category);
                const auto parentIndex = grouped_ && group >= 0 ? index(group, 0) : QModelIndex{};
                emit dataChanged(index(running_.offset, 0, parentIndex),
                                 index(qMin(running_.offset + PageSize, rowCount(parentIndex)) - 1,
                                       1, parentIndex));
                emit failed(page.error);
            } else {
                pages_.insert(running_.key(), new QVector<codecairn::IndexedSymbol>(page.symbols));
                const int group = categories_.indexOf(running_.category);
                const auto parentIndex = grouped_ && group >= 0 ? index(group, 0) : QModelIndex{};
                if (!page.symbols.isEmpty() && (!grouped_ || group >= 0) &&
                    running_.offset < rowCount(parentIndex))
                    emit dataChanged(
                        index(running_.offset, 0, parentIndex),
                        index(qMin(running_.offset + static_cast<int>(page.symbols.size()),
                                   rowCount(parentIndex)) -
                                  1,
                              1, parentIndex));
                emit rowsReady();
            }
        }
        startNext();
    });
}

SymbolCatalogModel::~SymbolCatalogModel() {
    if (cancelled_)
        cancelled_->store(true);
    job_.waitForFinished();
}

void SymbolCatalogModel::clear() {
    if (cancelled_)
        cancelled_->store(true);
    {
        const QSignalBlocker block(&job_);
        job_.waitForFinished();
    }
    busy_ = false;
    setCatalog({}, {}, {});
}

void SymbolCatalogModel::setCatalog(const QString &root, const codecairn::SymbolCatalogQuery &query,
                                    const codecairn::SymbolPage &summary) {
    bool sameOverlays = query_.overlays.size() == query.overlays.size();
    for (qsizetype i = 0; sameOverlays && i < query.overlays.size(); ++i)
        sameOverlays =
            query_.overlays[i].path == query.overlays[i].path &&
            query_.overlays[i].hash == query.overlays[i].hash &&
            query_.overlays[i].analysis.symbols.size() == query.overlays[i].analysis.symbols.size();
    const bool contextChanged = !sameOverlays || root_ != root ||
                                query_.projectFile != query.projectFile ||
                                query_.text != query.text || query_.category != query.category ||
                                query_.exact != query.exact;
    const bool structural =
        contextChanged || counts_ != summary.categories || total_ != summary.total;
    if (!structural)
        return;
    beginResetModel();
    ++generation_;
    if (cancelled_)
        cancelled_->store(true);
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    pages_.clear();
    pending_.clear();
    queued_.clear();
    failedPages_.clear();
    root_ = root;
    query_ = query;
    counts_ = summary.categories;
    categories_ = symbolCategoryOrder();
    for (auto it = counts_.cbegin(); it != counts_.cend(); ++it)
        if (!categories_.contains(it.key()))
            categories_.append(it.key());
    categories_.removeIf([&](const auto &name) { return counts_.value(name) == 0; });
    total_ = summary.total;
    if (!grouped_ && !summary.symbols.isEmpty())
        pages_.insert(Request{{}, 0, generation_}.key(),
                      new QVector<codecairn::IndexedSymbol>(summary.symbols.mid(0, PageSize)));
    endResetModel();
}

void SymbolCatalogModel::invalidatePages() {
    ++generation_;
    if (cancelled_)
        cancelled_->store(true);
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    pages_.clear();
    pending_.clear();
    queued_.clear();
    failedPages_.clear();
    if (!grouped_ && total_ > 0) {
        emit dataChanged(index(0, 0), index(total_ - 1, 1));
    } else {
        for (int group = 0; group < categories_.size(); ++group) {
            const auto parentIndex = index(group, 0);
            emit dataChanged(index(0, 0, parentIndex),
                             index(rowCount(parentIndex) - 1, 1, parentIndex));
        }
    }
}

QModelIndex SymbolCatalogModel::index(int row, int column, const QModelIndex &parent) const {
    if (column < 0 || column >= 2 || row < 0 || row >= rowCount(parent))
        return {};
    return createIndex(row, column,
                       grouped_ && parent.isValid() ? quintptr(parent.row() + 1) : quintptr(0));
}

QModelIndex SymbolCatalogModel::parent(const QModelIndex &index) const {
    return grouped_ && index.isValid() && index.internalId() > 0
               ? createIndex(static_cast<int>(index.internalId()) - 1, 0, quintptr(0))
               : QModelIndex{};
}

int SymbolCatalogModel::rowCount(const QModelIndex &parent) const {
    if (!parent.isValid())
        return grouped_ ? static_cast<int>(categories_.size()) : total_;
    if (!grouped_ || parent.column() != 0 || parent.internalId() != 0 ||
        parent.row() >= categories_.size())
        return 0;
    return counts_.value(categories_[parent.row()]);
}

QString SymbolCatalogModel::category(const QModelIndex &index) const {
    if (!grouped_ || !index.isValid())
        return {};
    const int group =
        index.internalId() == 0 ? index.row() : static_cast<int>(index.internalId()) - 1;
    return group >= 0 && group < categories_.size() ? categories_[group] : QString{};
}

std::optional<codecairn::IndexedSymbol> SymbolCatalogModel::entry(const QModelIndex &index) const {
    if (!index.isValid() || (grouped_ && index.internalId() == 0))
        return {};
    const int offset = index.row() / PageSize * PageSize;
    const Request request{grouped_ ? category(index) : QString{}, offset, generation_};
    if (const auto *page = pages_.object(request.key())) {
        const int row = index.row() - offset;
        if (row < page->size())
            return page->at(row);
    } else {
        const_cast<SymbolCatalogModel *>(this)->request(index);
    }
    return {};
}

QVariant SymbolCatalogModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    if (grouped_ && index.internalId() == 0) {
        const auto name = category(index);
        if (role == Qt::DisplayRole && index.column() == 0)
            return QString("%1 (%2)").arg(name).arg(counts_.value(name));
        if (role == Qt::DecorationRole && index.column() == 0)
            return UiAppearance::icon(name);
        if (role == SymbolItemRole::Index || role == SymbolItemRole::Key)
            return name;
        return {};
    }
    if (role != Qt::DisplayRole && role != Qt::DecorationRole && role != Qt::ToolTipRole &&
        role != SymbolItemRole::Path && role != SymbolItemRole::Key)
        return {};
    const auto value = entry(index);
    if (!value) {
        const auto key = Request{grouped_ ? category(index) : QString{},
                                 index.row() / PageSize * PageSize, generation_}
                             .key();
        if (role == Qt::ToolTipRole && failedPages_.contains(key))
            return failedPages_.value(key);
        return role == Qt::DisplayRole && index.column() == 0
                   ? QVariant(failedPages_.contains(key) ? tr("Load failed; refresh to retry")
                                                         : tr("Loading..."))
                   : QVariant{};
    }
    const auto &symbol = value->symbol;
    if (role == Qt::DisplayRole)
        return index.column() == 0
                   ? (symbol.qualifiedName.isEmpty() ? symbol.name : symbol.qualifiedName)
                   : (grouped_ ? QFileInfo(value->path).fileName()
                               : QDir(root_).relativeFilePath(value->path));
    if (role == Qt::DecorationRole && index.column() == 0)
        return UiAppearance::icon(symbol.kind);
    if (role == SymbolItemRole::Path)
        return value->path;
    if (role == SymbolItemRole::Key)
        return value->path + ':' + QString::number(symbol.nameStart) + ':' + symbol.name;
    if (role == Qt::ToolTipRole)
        return QString("%1 - %2\n%3:%4\n%5%6")
            .arg(symbol.kind, symbol.definition ? "definition" : "declaration", value->path)
            .arg(symbol.line + 1)
            .arg(symbol.signature,
                 symbol.generated ? "\nCompiler-expanded definition at the macro invocation." : "");
    return {};
}

QVariant SymbolCatalogModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == 0 ? (grouped_ ? tr("Symbol Type") : tr("Symbol Name"))
                        : (grouped_ ? tr("File Name") : tr("File"));
}

void SymbolCatalogModel::request(const QModelIndex &index) {
    Request value{grouped_ ? category(index) : QString{}, index.row() / PageSize * PageSize,
                  generation_};
    if (queued_.contains(value.key()))
        return;
    if (failedPages_.contains(value.key()))
        return;
    if (pending_.size() >= 32) {
        queued_.remove(pending_.dequeue().key());
    }
    queued_.insert(value.key());
    pending_.enqueue(value);
    startNext();
}

void SymbolCatalogModel::startNext() {
    if (busy_ || pending_.isEmpty())
        return;
    running_ = pending_.dequeue();
    auto query = query_;
    if (grouped_) {
        query.text.clear();
        query.exact = false;
        query.category = running_.category;
    }
    query.offset = running_.offset;
    query.limit = PageSize;
    query.includeCounts = false;
    query.includeCategories = false;
    query.includeCategorySymbols = false;
    query.cancel = cancelled_;
    busy_ = true;
    job_.setFuture(QtConcurrent::run(
        &pool_, [root = root_, query] { return queryProjectSymbols(root, query); }));
}

int SymbolCatalogModel::cachedRows() const {
    int count = 0;
    for (const auto &key : pages_.keys())
        count += pages_.object(key)->size();
    return count;
}
