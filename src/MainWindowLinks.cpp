#include "MainWindow.h"
#include "LanguageRegistry.h"
#include <QCryptographicHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QStatusBar>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
QString identifier(const QByteArray &source, int position) {
    const auto word = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 128 || c == '_' || c == '$' || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (position < 0 || position >= source.size() || !word(source[position]))
        return {};
    int start = position, end = position;
    while (start > 0 && word(source[start - 1]))
        --start;
    while (end < source.size() && word(source[end]))
        ++end;
    return QString::fromUtf8(source.mid(start, end - start));
}
}

void MainWindow::connectLanguageService(LanguageServerClient *service) {
    connect(service, &LanguageServerClient::locationsReady, this, &MainWindow::handleLocations);
    if (service == cppLanguageService_)
        return;
    connect(service, &LanguageServerClient::definitionReady, this, &MainWindow::handleDefinitions);
    connect(service, &LanguageServerClient::status, this,
            [this](const QString &message) { statusBar()->showMessage(message, 8000); });
    connect(service, &LanguageServerClient::ready, this, [this] {
        if (contextFollowsEditor_)
            contextTimer_->start();
    });
}

LanguageServerClient *MainWindow::languageService(const QString &path, const QByteArray &source) {
    return workspaceServices_->languageService(projectPath_, path, source);
}

void MainWindow::requestHover(CodeEditor *editor, int position) {
    if (position < 0 && hoverEditor_ && hoverEditor_ != editor)
        return;
    if (hoverEditor_)
        hoverEditor_->setLinkAvailable(hoverPosition_, false);
    hoverToken_ = ++linkGeneration_;
    hoverEditor_ = editor;
    hoverPosition_ = position;
    hoverTimer_->stop();
    if (position >= 0)
        hoverTimer_->start();
}

void MainWindow::jumpToDefinition(CodeEditor *editor, int position) {
    lookupLink(editor, position, true);
}

void MainWindow::lookupLink(CodeEditor *editor, int position, bool activate) {
    if (!editor)
        return;
    auto *document = currentDocument();
    const bool context = editor == contextPreviewEditor_;
    if ((!context && (!document || document->editor != editor)) ||
        (context && (contextPath_.isEmpty() || contextHistoryIndex_ < 0)))
        return;
    const auto bytes = context ? contextHistory_[contextHistoryIndex_].bytes : snapshot(document);
    const auto path = context ? contextPath_ : document->path;
    const auto name = identifier(bytes, position);
    if (name.isEmpty()) {
        if (activate)
            statusBar()->showMessage("Select an identifier to look up.", 5000);
        return;
    }
    const auto token = ++linkGeneration_;
    const auto hash = context ? QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                              : document->snapshotHash;
    int version = context ? 0 : static_cast<int>(document->revision);
    if (context) {
        if (auto *live = findDocument(path)) {
            snapshot(live);
            if (live->snapshotHash != hash) {
                if (activate)
                    reportError("Context is an older snapshot. Refresh it before following links.");
                return;
            }
            version = static_cast<int>(live->revision);
        } else {
            const auto disk = readSourceFile(path);
            if (!disk.error.isEmpty() ||
                QCryptographicHash::hash(disk.bytes, QCryptographicHash::Sha256).toHex() != hash) {
                if (activate)
                    reportError(disk.error.isEmpty()
                                    ? "Context source changed; refresh the preview."
                                    : disk.error);
                return;
            }
        }
    }
    if (activate) {
        jumpToken_ = token;
        jumpEditor_ = editor;
        jumpPath_ = path;
        jumpHash_ = hash;
        jumpName_ = name;
    } else {
        hoverToken_ = token;
        hoverPath_ = path;
        hoverHash_ = hash;
        hoverName_ = name;
    }
    if (auto *service = languageService(path, bytes))
        service->locations(path, bytes, version, position, token,
                           activate ? "navigation" : "hover");
    else
        fallbackLink(token, activate ? "navigation" : "hover");
}

void MainWindow::handleLocations(quint64 token, const QString &channel,
                                 const QVector<LspLocation> &targets, const QString &error) {
    if (channel == "references") {
        if (token == referenceToken_)
            displayReferences(targets, error);
        return;
    }
    const bool activate = channel == "navigation";
    if (token != (activate ? jumpToken_ : hoverToken_))
        return;
    auto editor = activate ? jumpEditor_ : hoverEditor_;
    if (!editor || (!activate && !editor->linkGestureActive()))
        return;
    const auto path = activate ? jumpPath_ : hoverPath_;
    const auto hash = activate ? jumpHash_ : hoverHash_;
    if (auto *document = findDocument(path)) {
        snapshot(document);
        if (document->snapshotHash != hash) {
            if (activate)
                reportError("Source changed during lookup; select the symbol again.");
            return;
        }
    }
    if (targets.isEmpty()) {
        fallbackLink(token, channel);
        return;
    }
    if (!activate) {
        editor->setLinkAvailable(hoverPosition_, true);
        return;
    }
    int choice = 0;
    if (targets.size() > 1) {
        QStringList names;
        for (const auto &target : targets)
            names << target.path + ':' + QString::number(target.line + 1);
        bool accepted = false;
        const auto chosen = QInputDialog::getItem(this, "Choose definition", "Semantic definitions",
                                                  names, 0, false, &accepted);
        if (!accepted)
            return;
        choice = static_cast<int>(names.indexOf(chosen));
    }
    navigateDefinition(targets[choice]);
}

void MainWindow::fallbackLink(quint64 token, const QString &channel) {
    const bool activate = channel == "navigation";
    if (token != (activate ? jumpToken_ : hoverToken_))
        return;
    if (auto *document = findDocument(activate ? jumpPath_ : hoverPath_)) {
        snapshot(document);
        if (document->snapshotHash != (activate ? jumpHash_ : hoverHash_)) {
            if (activate)
                reportError("Source changed during lookup; select the symbol again.");
            return;
        }
    }
    SymbolQuery query;
    query.text = activate ? jumpName_ : hoverName_;
    query.exact = true;
    query.limit = RuntimeLimits::DefinitionCandidateLimit;
    query.overlays = liveOverlays();
    auto *job = new QFutureWatcher<SymbolPage>(this);
    connect(job, &QFutureWatcher<SymbolPage>::finished, this, [this, job, token, activate] {
        auto page = job->result();
        job->deleteLater();
        if (token != (activate ? jumpToken_ : hoverToken_))
            return;
        if (!page.error.isEmpty()) {
            reportError(page.error);
            return;
        }
        const bool definitions =
            std::any_of(page.symbols.cbegin(), page.symbols.cend(),
                        [](const auto &entry) { return entry.symbol.definition; });
        if (definitions)
            page.symbols.erase(
                std::remove_if(page.symbols.begin(), page.symbols.end(),
                               [](const auto &entry) { return !entry.symbol.definition; }),
                page.symbols.end());
        if (!activate) {
            if (hoverEditor_)
                hoverEditor_->setLinkAvailable(hoverPosition_, !page.symbols.isEmpty());
            return;
        }
        if (page.symbols.isEmpty()) {
            statusBar()->showMessage("No definition found for " + jumpName_, 6000);
            return;
        }
        int choice = 0;
        if (page.symbols.size() > 1 || page.total > RuntimeLimits::DefinitionCandidateLimit) {
            QStringList names;
            for (const auto &entry : page.symbols)
                names << entry.symbol.qualifiedName + " - " + entry.path + ':' +
                             QString::number(entry.symbol.line + 1);
            bool accepted = false;
            const auto selected = QInputDialog::getItem(
                this, "Choose syntax candidate",
                "Not compiler verified (first 100 matches at most)", names, 0, false, &accepted);
            if (!accepted)
                return;
            choice = static_cast<int>(names.indexOf(selected));
        }
        const auto &entry = page.symbols[choice];
        navigateTo({entry.path, entry.symbol.nameStart, entry.symbol.nameEnd, -1, entry.hash}, true,
                   true);
    });
    job->setFuture(QtConcurrent::run(
        [root = projectPath_, query] { return queryWorkspaceSymbols(root, query); }));
}

void MainWindow::navigateDefinition(const LspLocation &target) {
    NavigationLocation location;
    location.path = target.path;
    location.line = target.line;
    location.character = target.character;
    location.endLine = target.endLine;
    location.endCharacter = target.endCharacter;
    navigateTo(location, true, true);
}
