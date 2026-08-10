#include "record_workspace.hpp"

#include <QAbstractItemView>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfPageRenderer>
#include <QPdfSearchModel>
#include <QPdfView>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace appellate::ui {
namespace {

[[nodiscard]] auto fail(RecordWorkspaceErrorCode code, QString message)
    -> std::unexpected<RecordWorkspaceError> {
    return std::unexpected(RecordWorkspaceError{code, std::move(message)});
}

[[nodiscard]] QString searchableMetadata(const QMap<QString, QString>& metadata) {
    QStringList parts;
    parts.reserve(metadata.size() * 2);
    for (auto iterator = metadata.constBegin(); iterator != metadata.constEnd(); ++iterator) {
        parts.push_back(iterator.key());
        parts.push_back(iterator.value());
    }
    return parts.join(u' ');
}

[[nodiscard]] QString pageStatus(int page_index, int page_count) {
    if (page_count <= 0 || page_index < 0) {
        return QStringLiteral("No document selected");
    }
    return QStringLiteral("Page %1 of %2").arg(page_index + 1).arg(page_count);
}

} // namespace

class DocketFilterProxyModel final : public QSortFilterProxyModel {
  public:
    explicit DocketFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {
        setDynamicSortFilter(true);
    }

    void setQuery(const QString& query) {
        beginFilterChange();
        terms_ = query.toCaseFolded().simplified().split(u' ', Qt::SkipEmptyParts);
        evaluation_count_ = 0;
        endFilterChange(Direction::Rows);
    }

    [[nodiscard]] qsizetype evaluationCount() const noexcept { return evaluation_count_; }

  protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row,
                                        const QModelIndex& source_parent) const override {
        ++evaluation_count_;
        if (terms_.isEmpty()) {
            return true;
        }
        const auto index = sourceModel()->index(source_row, 0, source_parent);
        const auto searchable = index.data(RecordDocketModel::SearchTextRole).toString();
        return std::ranges::all_of(
            terms_, [&searchable](const QString& term) { return searchable.contains(term); });
    }

  private:
    QStringList terms_;
    mutable qsizetype evaluation_count_{};
};

RecordDocketModel::RecordDocketModel(QObject* parent) : QAbstractTableModel(parent) {}

int RecordDocketModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int RecordDocketModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::Count);
}

QVariant RecordDocketModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];

    if (role == DocketIdRole) {
        return row.entry.id;
    }
    if (role == DocumentIdRole) {
        return row.entry.document_id;
    }
    if (role == SearchTextRole) {
        return row.search_text;
    }
    if (role == SealedRole) {
        return row.sealed;
    }
    if (role == Qt::ToolTipRole) {
        return row.entry.description;
    }
    if (role == Qt::AccessibleTextRole) {
        return QStringLiteral("%1, filed %2 by %3, document %4, %5")
            .arg(row.entry.title, row.entry.filed_on.toString(Qt::ISODate), row.entry.actor,
                 row.document_title,
                 row.sealed ? QStringLiteral("sealed") : QStringLiteral("available"));
    }
    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (static_cast<Column>(index.column())) {
    case Column::Filed:
        return row.entry.filed_on.toString(Qt::ISODate);
    case Column::Title:
        return row.entry.title;
    case Column::Actor:
        return row.entry.actor;
    case Column::Document:
        return row.document_title;
    case Column::Access:
        return row.sealed ? QStringLiteral("Sealed") : QStringLiteral("Available");
    case Column::Count:
        break;
    }
    return {};
}

QVariant RecordDocketModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (static_cast<Column>(section)) {
    case Column::Filed:
        return QStringLiteral("Filed");
    case Column::Title:
        return QStringLiteral("Entry");
    case Column::Actor:
        return QStringLiteral("Actor");
    case Column::Document:
        return QStringLiteral("Document");
    case Column::Access:
        return QStringLiteral("Access");
    case Column::Count:
        break;
    }
    return {};
}

Qt::ItemFlags RecordDocketModel::flags(const QModelIndex& index) const {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

void RecordDocketModel::setRecordData(const RecordDefinition& definition) {
    QHash<QString, RecordDocument> documents;
    documents.reserve(static_cast<qsizetype>(definition.documents.size()));
    for (const auto& document : definition.documents) {
        documents.insert(document.id, document);
    }

    std::vector<Row> rows;
    rows.reserve(definition.docket.size());
    for (const auto& entry : definition.docket) {
        const auto document = documents.value(entry.document_id);
        const QStringList search_parts{
            entry.id,
            entry.filed_on.toString(Qt::ISODate),
            entry.title,
            entry.actor,
            entry.description,
            entry.document_id,
            entry.tags.join(u' '),
            searchableMetadata(entry.metadata),
            document.title,
            searchableMetadata(document.metadata),
            document.sealed ? QStringLiteral("sealed") : QStringLiteral("available"),
        };
        rows.push_back(
            Row{entry, document.title, document.sealed, search_parts.join(u' ').toCaseFolded()});
    }

    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const RecordDocketEntry* RecordDocketModel::entryAt(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(row)].entry;
}

int RecordDocketModel::indexOf(QStringView docket_id) const {
    const auto found = std::ranges::find(
        rows_, docket_id, [](const Row& row) -> QStringView { return row.entry.id; });
    return found == rows_.end() ? -1 : static_cast<int>(std::distance(rows_.begin(), found));
}

RecordWorkspace::RecordWorkspace(QWidget* parent) : QWidget(parent) {
    docket_model_ = new RecordDocketModel(this);
    filter_model_ = new DocketFilterProxyModel(this);
    filter_model_->setSourceModel(docket_model_);

    docket_filter_ = new QLineEdit(this);
    docket_filter_->setObjectName(QStringLiteral("docketFilter"));
    docket_filter_->setPlaceholderText(QStringLiteral("Filter docket and record metadata"));
    docket_filter_->setAccessibleName(QStringLiteral("Filter docket and record metadata"));
    docket_filter_->setClearButtonEnabled(true);

    docket_view_ = new QTableView(this);
    docket_view_->setObjectName(QStringLiteral("docketView"));
    docket_view_->setAccessibleName(QStringLiteral("Docket entries"));
    docket_view_->setModel(filter_model_);
    docket_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    docket_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    docket_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    docket_view_->setFocusPolicy(Qt::StrongFocus);
    docket_view_->horizontalHeader()->setStretchLastSection(true);
    docket_view_->horizontalHeader()->setSectionResizeMode(
        static_cast<int>(RecordDocketModel::Column::Title), QHeaderView::Stretch);

    open_document_button_ = new QPushButton(QStringLiteral("Open document"), this);
    open_document_button_->setObjectName(QStringLiteral("openDocument"));
    open_document_button_->setAccessibleName(QStringLiteral("Open selected docket document"));
    open_document_button_->setFocusPolicy(Qt::StrongFocus);

    pdf_document_ = new QPdfDocument(this);
    pdf_search_model_ = new QPdfSearchModel(this);
    pdf_search_model_->setDocument(pdf_document_);
    pdf_view_ = new QPdfView(this);
    pdf_view_->setObjectName(QStringLiteral("pdfView"));
    pdf_view_->setAccessibleName(QStringLiteral("Record PDF document"));
    pdf_view_->setFocusPolicy(Qt::StrongFocus);
    pdf_view_->setDocument(pdf_document_);
    pdf_view_->setSearchModel(pdf_search_model_);
    pdf_view_->setPageMode(QPdfView::PageMode::MultiPage);
    pdf_view_->setZoomMode(QPdfView::ZoomMode::FitToWidth);

    document_search_ = new QLineEdit(this);
    document_search_->setObjectName(QStringLiteral("documentSearch"));
    document_search_->setPlaceholderText(QStringLiteral("Search within document"));
    document_search_->setAccessibleName(QStringLiteral("Search text within the record document"));
    document_search_->setClearButtonEnabled(true);

    next_search_result_button_ = new QPushButton(QStringLiteral("Next match"), this);
    next_search_result_button_->setObjectName(QStringLiteral("nextSearchResult"));
    next_search_result_button_->setAccessibleName(
        QStringLiteral("Go to next document search match"));
    next_search_result_button_->setFocusPolicy(Qt::StrongFocus);
    next_search_result_button_->setEnabled(false);

    previous_page_button_ = new QPushButton(QStringLiteral("Previous"), this);
    previous_page_button_->setObjectName(QStringLiteral("previousPage"));
    previous_page_button_->setAccessibleName(QStringLiteral("Previous record page"));
    previous_page_button_->setFocusPolicy(Qt::StrongFocus);
    previous_page_button_->setShortcut(QKeySequence(QStringLiteral("Alt+Left")));

    next_page_button_ = new QPushButton(QStringLiteral("Next"), this);
    next_page_button_->setObjectName(QStringLiteral("nextPage"));
    next_page_button_->setAccessibleName(QStringLiteral("Next record page"));
    next_page_button_->setFocusPolicy(Qt::StrongFocus);
    next_page_button_->setShortcut(QKeySequence(QStringLiteral("Alt+Right")));

    page_spin_box_ = new QSpinBox(this);
    page_spin_box_->setObjectName(QStringLiteral("pageNumber"));
    page_spin_box_->setAccessibleName(QStringLiteral("Record page number"));
    page_spin_box_->setRange(1, 1);
    page_spin_box_->setEnabled(false);

    page_count_label_ = new QLabel(QStringLiteral("of 0"), this);
    page_count_label_->setAccessibleName(QStringLiteral("Record page count"));
    status_label_ = new QLabel(QStringLiteral("No document selected"), this);
    status_label_->setObjectName(QStringLiteral("recordStatus"));
    status_label_->setAccessibleName(QStringLiteral("Record workspace status"));

    auto* left_layout = new QVBoxLayout;
    left_layout->addWidget(docket_filter_);
    left_layout->addWidget(docket_view_);
    left_layout->addWidget(open_document_button_);
    auto* left = new QWidget(this);
    left->setLayout(left_layout);

    auto* search_layout = new QHBoxLayout;
    search_layout->addWidget(document_search_);
    search_layout->addWidget(next_search_result_button_);
    auto* page_layout = new QHBoxLayout;
    page_layout->addWidget(previous_page_button_);
    page_layout->addWidget(next_page_button_);
    page_layout->addStretch();
    page_layout->addWidget(page_spin_box_);
    page_layout->addWidget(page_count_label_);

    auto* right_layout = new QVBoxLayout;
    right_layout->addLayout(search_layout);
    right_layout->addWidget(pdf_view_);
    right_layout->addLayout(page_layout);
    right_layout->addWidget(status_label_);
    auto* right = new QWidget(this);
    right->setLayout(right_layout);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(splitter);
    setLayout(layout);

    connect(docket_filter_, &QLineEdit::textChanged, this,
            [this](const QString& query) { filter_model_->setQuery(query); });
    connect(document_search_, &QLineEdit::textChanged, pdf_search_model_,
            &QPdfSearchModel::setSearchString);
    connect(open_document_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(openSelectedDocketEntry()); });
    connect(docket_view_, &QTableView::activated, this,
            [this](const QModelIndex&) { static_cast<void>(openSelectedDocketEntry()); });
    connect(previous_page_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(goToPage(currentPageIndex() - 1)); });
    connect(next_page_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(goToPage(currentPageIndex() + 1)); });
    connect(page_spin_box_, &QSpinBox::valueChanged, this,
            [this](int page) { static_cast<void>(goToPage(page - 1)); });
    connect(pdf_view_->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this,
            [this](int page) { updatePageControls(page); });
    connect(pdf_search_model_, &QPdfSearchModel::countChanged, this,
            [this] { next_search_result_button_->setEnabled(pdf_search_model_->count() > 0); });
    connect(next_search_result_button_, &QPushButton::clicked, this, [this] {
        const auto count = pdf_search_model_->count();
        if (count <= 0) {
            return;
        }
        const auto next = (pdf_view_->currentSearchResultIndex() + 1) % count;
        pdf_view_->setCurrentSearchResultIndex(next);
        pdf_view_->pageNavigator()->jump(pdf_search_model_->resultAtIndex(next));
    });

    auto* focus_filter = new QShortcut(QKeySequence::Find, this);
    focus_filter->setContext(Qt::WidgetWithChildrenShortcut);
    connect(focus_filter, &QShortcut::activated, docket_filter_, [this] {
        docket_filter_->setFocus(Qt::ShortcutFocusReason);
        docket_filter_->selectAll();
    });
    auto* focus_document_search = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    focus_document_search->setContext(Qt::WidgetWithChildrenShortcut);
    connect(focus_document_search, &QShortcut::activated, document_search_, [this] {
        document_search_->setFocus(Qt::ShortcutFocusReason);
        document_search_->selectAll();
    });

    updatePageControls(-1);
}

RecordWorkspace::~RecordWorkspace() {
    // QPdfDocument teardown can synchronously move the page navigator. Destroy the PDF objects
    // in dependency order while the derived object is still alive: leaving QPdfView to QWidget's
    // base destructor can invoke our navigator lambda after RecordWorkspace's lifetime has ended,
    // and its threaded page renderer is not guaranteed to have stopped before leak checking.
    QObject::disconnect(pdf_view_->pageNavigator(), nullptr, this, nullptr);
    QObject::disconnect(pdf_search_model_, nullptr, this, nullptr);

    // Qt 6.11's QPdfPageRendererPrivate destructor stops its worker thread but omits deleting the
    // QThread. Switching through the public API first performs the complete quit/wait/delete path.
    if (auto* renderer = pdf_view_->findChild<QPdfPageRenderer*>(); renderer != nullptr) {
        renderer->setRenderMode(QPdfPageRenderer::RenderMode::SingleThreaded);
    }
    delete pdf_view_;
    pdf_view_ = nullptr;
    delete pdf_search_model_;
    pdf_search_model_ = nullptr;
    pdf_document_->close();
    delete pdf_document_;
    pdf_document_ = nullptr;
}

std::expected<void, RecordWorkspaceError>
RecordWorkspace::validate(const RecordDefinition& definition) const {
    if (definition.documents.empty() || definition.docket.empty()) {
        return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                    QStringLiteral("A record requires documents and docket entries"));
    }

    QSet<QString> document_ids;
    for (const auto& document : definition.documents) {
        if (document.id.isEmpty() || document.title.isEmpty() ||
            document_ids.contains(document.id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Document IDs and titles must be unique and nonempty"));
        }
        document_ids.insert(document.id);
        if (!document.sealed) {
            const QFileInfo file(document.file_path);
            if (!file.isFile() || file.isSymLink()) {
                return fail(RecordWorkspaceErrorCode::MissingAsset,
                            QStringLiteral("Document asset is missing: %1").arg(document.id));
            }
        }
    }

    QSet<QString> docket_ids;
    QSet<QString> referenced_documents;
    for (const auto& entry : definition.docket) {
        if (entry.id.isEmpty() || !entry.filed_on.isValid() || entry.title.isEmpty() ||
            entry.actor.isEmpty() || docket_ids.contains(entry.id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Docket entries require unique IDs and complete metadata"));
        }
        docket_ids.insert(entry.id);
        if (!document_ids.contains(entry.document_id)) {
            return fail(RecordWorkspaceErrorCode::MissingDocument,
                        QStringLiteral("Docket entry %1 references missing document %2")
                            .arg(entry.id, entry.document_id));
        }
        referenced_documents.insert(entry.document_id);
    }
    for (const auto& document : definition.documents) {
        if (!referenced_documents.contains(document.id)) {
            return fail(
                RecordWorkspaceErrorCode::OrphanDocument,
                QStringLiteral("Document is not represented on the docket: %1").arg(document.id));
        }
    }

    QSet<QString> anchor_ids;
    for (const auto& anchor : definition.anchors) {
        if (anchor.id.isEmpty() || anchor.page_index < 0 || anchor_ids.contains(anchor.id) ||
            !document_ids.contains(anchor.document_id)) {
            return fail(RecordWorkspaceErrorCode::InvalidPageAnchor,
                        QStringLiteral("Record page anchor is invalid: %1").arg(anchor.id));
        }
        anchor_ids.insert(anchor.id);
    }
    return {};
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::setRecord(RecordDefinition definition) {
    if (const auto valid = validate(definition); !valid) {
        last_error_ = valid.error();
        status_label_->setText(valid.error().message);
        return std::unexpected(valid.error());
    }

    docket_model_->setRecordData(definition);
    QHash<QString, RecordDocument> documents;
    documents.reserve(static_cast<qsizetype>(definition.documents.size()));
    for (auto& document : definition.documents) {
        documents.insert(document.id, std::move(document));
    }
    QHash<QString, RecordPageAnchor> anchors;
    anchors.reserve(static_cast<qsizetype>(definition.anchors.size()));
    for (auto& anchor : definition.anchors) {
        anchors.insert(anchor.id, std::move(anchor));
    }

    documents_ = std::move(documents);
    anchors_ = std::move(anchors);
    docket_filter_->clear();
    document_search_->clear();
    pdf_document_->close();
    current_document_id_.clear();
    clearError();
    updatePageControls(-1);
    return {};
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::openDocketEntry(QStringView docket_id) {
    const auto source_row = docket_model_->indexOf(docket_id);
    if (source_row < 0) {
        return recordError(RecordWorkspaceErrorCode::MissingDocketEntry,
                           QStringLiteral("Docket entry does not exist"));
    }
    selectSourceRow(source_row);
    const auto* entry = docket_model_->entryAt(source_row);
    const auto document = documents_.constFind(entry->document_id);
    if (document == documents_.constEnd()) {
        return recordError(RecordWorkspaceErrorCode::MissingDocument,
                           QStringLiteral("Docket document is unavailable"));
    }
    return openDocument(document.value(), 0);
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::openSelectedDocketEntry() {
    const auto selected = docket_view_->currentIndex();
    if (!selected.isValid()) {
        return recordError(RecordWorkspaceErrorCode::MissingDocketEntry,
                           QStringLiteral("Select a docket entry first"));
    }
    const auto source = filter_model_->mapToSource(selected);
    const auto* entry = docket_model_->entryAt(source.row());
    if (entry == nullptr) {
        return recordError(RecordWorkspaceErrorCode::MissingDocketEntry,
                           QStringLiteral("Selected docket entry is unavailable"));
    }
    return openDocketEntry(entry->id);
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::navigateToAnchor(QStringView anchor_id) {
    const auto anchor = anchors_.constFind(anchor_id.toString());
    if (anchor == anchors_.constEnd()) {
        return recordError(RecordWorkspaceErrorCode::InvalidPageAnchor,
                           QStringLiteral("Record page anchor does not exist"));
    }
    const auto document = documents_.constFind(anchor->document_id);
    if (document == documents_.constEnd()) {
        return recordError(RecordWorkspaceErrorCode::MissingDocument,
                           QStringLiteral("Anchor document does not exist"));
    }
    return openDocument(document.value(), anchor->page_index);
}

std::expected<void, RecordWorkspaceError>
RecordWorkspace::openDocument(const RecordDocument& document, int page_index) {
    if (document.sealed) {
        return recordError(RecordWorkspaceErrorCode::SealedDocument,
                           QStringLiteral("This record item is sealed and cannot be opened"));
    }

    pdf_search_model_->setSearchString({});
    document_search_->clear();
    pdf_document_->close();
    const auto error = pdf_document_->load(document.file_path);
    if (error != QPdfDocument::Error::None ||
        pdf_document_->status() != QPdfDocument::Status::Ready || pdf_document_->pageCount() <= 0) {
        current_document_id_.clear();
        updatePageControls(-1);
        return recordError(RecordWorkspaceErrorCode::PdfLoadFailed,
                           QStringLiteral("Cannot load record PDF: %1").arg(document.title));
    }
    if (page_index < 0 || page_index >= pdf_document_->pageCount()) {
        current_document_id_.clear();
        pdf_document_->close();
        updatePageControls(-1);
        return recordError(RecordWorkspaceErrorCode::InvalidPageAnchor,
                           QStringLiteral("Record page anchor is outside the document"));
    }

    current_document_id_ = document.id;
    clearError();
    return goToPage(page_index);
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::goToPage(int page_index) {
    if (current_document_id_.isEmpty() || pdf_document_->status() != QPdfDocument::Status::Ready) {
        return recordError(RecordWorkspaceErrorCode::NoDocumentSelected,
                           QStringLiteral("Open a record document first"));
    }
    if (page_index < 0 || page_index >= pdf_document_->pageCount()) {
        return recordError(RecordWorkspaceErrorCode::InvalidPageAnchor,
                           QStringLiteral("Requested page is outside the document"));
    }

    pdf_view_->pageNavigator()->jump(page_index, QPointF{}, 0);
    clearError();
    updatePageControls(page_index);
    return {};
}

void RecordWorkspace::setDocketFilter(const QString& query) { docket_filter_->setText(query); }

void RecordWorkspace::setDocumentSearch(const QString& query) { document_search_->setText(query); }

qsizetype RecordWorkspace::visibleDocketCount() const { return filter_model_->rowCount(); }

qsizetype RecordWorkspace::filterEvaluationCount() const {
    return filter_model_->evaluationCount();
}

int RecordWorkspace::loadedPageCount() const { return pdf_document_->pageCount(); }

int RecordWorkspace::currentPageIndex() const {
    return current_document_id_.isEmpty() ? -1 : pdf_view_->pageNavigator()->currentPage();
}

int RecordWorkspace::documentSearchResultCount() const { return pdf_search_model_->count(); }

const QString& RecordWorkspace::currentDocumentId() const noexcept { return current_document_id_; }

const std::optional<RecordWorkspaceError>& RecordWorkspace::lastError() const noexcept {
    return last_error_;
}

QLineEdit* RecordWorkspace::docketFilterEdit() const noexcept { return docket_filter_; }

QLineEdit* RecordWorkspace::documentSearchEdit() const noexcept { return document_search_; }

QTableView* RecordWorkspace::docketView() const noexcept { return docket_view_; }

QPdfView* RecordWorkspace::pdfView() const noexcept { return pdf_view_; }

QPushButton* RecordWorkspace::openDocumentButton() const noexcept { return open_document_button_; }

QPushButton* RecordWorkspace::previousPageButton() const noexcept { return previous_page_button_; }

QPushButton* RecordWorkspace::nextPageButton() const noexcept { return next_page_button_; }

QSpinBox* RecordWorkspace::pageSpinBox() const noexcept { return page_spin_box_; }

std::unexpected<RecordWorkspaceError> RecordWorkspace::recordError(RecordWorkspaceErrorCode code,
                                                                   QString message) {
    last_error_ = RecordWorkspaceError{code, std::move(message)};
    status_label_->setText(last_error_->message);
    return std::unexpected(*last_error_);
}

void RecordWorkspace::clearError() {
    last_error_.reset();
    status_label_->setText(pageStatus(currentPageIndex(), loadedPageCount()));
}

void RecordWorkspace::updatePageControls(int page_index) {
    const auto count = loadedPageCount();
    const auto usable = count > 0 && page_index >= 0;
    const QSignalBlocker blocker(page_spin_box_);
    page_spin_box_->setEnabled(usable);
    page_spin_box_->setRange(1, std::max(1, count));
    page_spin_box_->setValue(usable ? page_index + 1 : 1);
    page_count_label_->setText(QStringLiteral("of %1").arg(count));
    previous_page_button_->setEnabled(usable && page_index > 0);
    next_page_button_->setEnabled(usable && page_index + 1 < count);
    if (!last_error_.has_value()) {
        status_label_->setText(pageStatus(page_index, count));
    }
}

void RecordWorkspace::selectSourceRow(int source_row) {
    const auto source = docket_model_->index(source_row, 0);
    const auto proxy = filter_model_->mapFromSource(source);
    if (!proxy.isValid()) {
        return;
    }
    docket_view_->setCurrentIndex(proxy);
    docket_view_->selectionModel()->select(proxy, QItemSelectionModel::ClearAndSelect |
                                                      QItemSelectionModel::Rows);
    docket_view_->scrollTo(proxy);
}

} // namespace appellate::ui
