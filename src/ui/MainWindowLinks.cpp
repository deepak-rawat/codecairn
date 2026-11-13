#include "MainWindow.h"
#include <codecairn/LanguageRegistry.h>
#include <codecairn/NavigationTarget.h>
#include <QCryptographicHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QStatusBar>

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
    const auto path = context ? contextPath_ : document->path();
    const auto name = sourceIdentifier(bytes, position);
    if (name.isEmpty()) {
        if (activate)
            statusBar()->showMessage("Select an identifier to look up.", 5000);
        return;
    }
    const auto token = ++linkGeneration_;
    const auto hash = context ? QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                              : document->snapshotHash();
    int version = context ? 0 : static_cast<int>(document->revision());
    if (context) {
        if (auto *live = findDocument(path)) {
            snapshot(live);
            if (live->snapshotHash() != hash) {
                if (activate)
                    reportError("Context is an older snapshot. Refresh it before following links.");
                return;
            }
            version = static_cast<int>(live->revision());
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
    codecairn::DefinitionQuery query;
    query.at = {{path, bytes, hash, static_cast<quint64>(version)}, position};
    auto &request = activate ? jumpRequest_ : hoverRequest_;
    auto &generation = activate ? jumpRequestGeneration_ : hoverRequestGeneration_;
    project_->cancel(request);
    generation = token;
    request = project_->findDefinitions(query);
}

void MainWindow::handleDefinitionResult(quint64 token, const QString &channel,
                                        const codecairn::DefinitionPage &result) {
    if (channel == "context") {
        displayContextDefinitions(token, result);
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
        if (document->snapshotHash() != hash) {
            if (activate)
                reportError("Source changed during lookup; select the symbol again.");
            return;
        }
    }
    if (!result.error.isEmpty()) {
        if (activate || !result.semantic)
            reportError(result.error);
        return;
    }
    if (!result.semantic) {
        DefinitionCandidates candidates;
        candidates.total = result.total;
        candidates.complete = result.complete;
        for (const auto &definition : result.definitions)
            if (definition.symbol)
                candidates.targets.append(*definition.symbol);
        if (!activate) {
            editor->setLinkAvailable(hoverPosition_, !candidates.targets.isEmpty());
            return;
        }
        if (candidates.targets.isEmpty()) {
            statusBar()->showMessage("No definition found for " + jumpName_, 6000);
            return;
        }
        int choice = 0;
        if (candidates.targets.size() > 1 || !candidates.complete) {
            QStringList names;
            for (const auto &entry : candidates.targets)
                names << entry.symbol.qualifiedName + " - " + entry.path + ':' +
                             QString::number(entry.symbol.line + 1);
            bool accepted = false;
            const auto selected = QInputDialog::getItem(
                this, "Choose syntax candidate",
                "Not compiler verified (first 100 matches at most)", names, 0, false, &accepted);
            if (!accepted || token != jumpToken_ || !jumpEditor_)
                return;
            choice = static_cast<int>(names.indexOf(selected));
            if (choice < 0)
                return;
        }
        const auto &entry = candidates.targets[choice];
        navigateTo({entry.path, entry.symbol.nameStart, entry.symbol.nameEnd, -1, entry.hash}, true,
                   true);
        return;
    }
    const auto &targets = result.definitions;
    if (targets.isEmpty())
        return;
    if (!activate) {
        editor->setLinkAvailable(hoverPosition_, true);
        return;
    }
    int choice = 0;
    if (targets.size() > 1) {
        QStringList names;
        for (const auto &target : targets)
            names << target.location.path + ':' + QString::number(target.location.line + 1);
        bool accepted = false;
        const auto chosen = QInputDialog::getItem(this, "Choose definition", "Semantic definitions",
                                                  names, 0, false, &accepted);
        if (!accepted || token != jumpToken_ || !jumpEditor_)
            return;
        choice = static_cast<int>(names.indexOf(chosen));
        if (choice < 0)
            return;
    }
    navigateTo(targets[choice].location, true, true);
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
