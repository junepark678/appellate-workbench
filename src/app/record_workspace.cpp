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
#include <QTemporaryFile>
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

[[nodiscard]] std::optional<qsizetype> unicodeScalarCount(QStringView text) {
    qsizetype count = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto unit = text.at(index).unicode();
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 1 >= text.size()) {
                return std::nullopt;
            }
            const auto low = text.at(index + 1).unicode();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return std::nullopt;
            }
            ++index;
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return std::nullopt;
        }
        ++count;
    }
    return count;
}

[[nodiscard]] bool isBoundedText(QStringView text, qsizetype maximum, bool allow_empty = false) {
    const auto count = unicodeScalarCount(text);
    return count.has_value() && *count <= maximum && (allow_empty || *count > 0) &&
           !text.contains(QChar::Null);
}

[[nodiscard]] bool isLowercaseSha256(QStringView text) {
    return text.size() == 64 && std::ranges::all_of(text, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
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
        return QStringLiteral("Docket %1, entry %2, %3, filed %4 by %5, document %6, %7")
            .arg(row.entry.docket_label, row.entry.entry_label, row.entry.title,
                 row.entry.filed_on.toString(Qt::ISODate), row.entry.actor, row.document_title,
                 row.sealed ? QStringLiteral("sealed") : QStringLiteral("available"));
    }
    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (static_cast<Column>(index.column())) {
    case Column::Docket:
        return row.entry.docket_label;
    case Column::EntryLabel:
        return row.entry.entry_label;
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
    case Column::Docket:
        return QStringLiteral("Docket");
    case Column::EntryLabel:
        return QStringLiteral("No.");
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
            entry.docket_id,
            entry.docket_label,
            entry.entry_label,
            entry.filed_on.toString(Qt::ISODate),
            entry.title,
            entry.actor,
            entry.description,
            entry.document_id,
            entry.parent_entry_id,
            entry.relationship,
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
    const auto renderers =
        pdf_view_->findChildren<QPdfPageRenderer*>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto* renderer : renderers) {
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

    QSet<QString> docket_descriptor_ids;
    for (const auto& docket : definition.dockets) {
        static const QSet<QString> docket_types{
            QStringLiteral("district"), QStringLiteral("appellate"), QStringLiteral("agency"),
            QStringLiteral("original")};
        if (!isBoundedText(docket.id, 160) || !docket_types.contains(docket.type) ||
            !isBoundedText(docket.court_id, 160, true) || !isBoundedText(docket.court_ref, 240) ||
            !isBoundedText(docket.public_docket_number, 120) ||
            !isBoundedText(docket.caption, 512) || docket_descriptor_ids.contains(docket.id)) {
            return fail(
                RecordWorkspaceErrorCode::InvalidDefinition,
                QStringLiteral("Docket descriptors require unique IDs and complete metadata"));
        }
        docket_descriptor_ids.insert(docket.id);
    }

    QSet<QString> document_ids;
    QSet<QString> sealed_document_ids;
    QHash<QString, int> declared_page_counts;
    for (const auto& document : definition.documents) {
        if (!isBoundedText(document.id, 160) || !isBoundedText(document.title, 512) ||
            document.declared_page_count < 0 || document_ids.contains(document.id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Document IDs and titles must be unique and nonempty"));
        }
        document_ids.insert(document.id);
        if (document.sealed) {
            sealed_document_ids.insert(document.id);
        }
        declared_page_counts.insert(document.id, document.declared_page_count);
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
    QHash<QString, const RecordDocketEntry*> entries_by_id;
    for (const auto& entry : definition.docket) {
        if (!isBoundedText(entry.id, 160) || !entry.filed_on.isValid() ||
            !isBoundedText(entry.title, 512) || !isBoundedText(entry.actor, 240) ||
            !isBoundedText(entry.description, 4'096) ||
            !isBoundedText(entry.docket_id, 160, true) ||
            !isBoundedText(entry.docket_label, 120, true) ||
            !isBoundedText(entry.entry_label, 120, true) ||
            !isBoundedText(entry.parent_entry_id, 160, true) ||
            !isBoundedText(entry.relationship, 16, true) || entry.tags.size() > 32 ||
            docket_ids.contains(entry.id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Docket entries require unique IDs and complete metadata"));
        }
        if (std::ranges::any_of(entry.tags,
                                [](const QString& tag) { return !isBoundedText(tag, 64); })) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Docket entry tags exceed their supported bounds"));
        }
        docket_ids.insert(entry.id);
        entries_by_id.insert(entry.id, &entry);
        if (!entry.docket_id.isEmpty() && !docket_descriptor_ids.contains(entry.docket_id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Docket entry %1 references missing docket %2")
                            .arg(entry.id, entry.docket_id));
        }
        if (!document_ids.contains(entry.document_id)) {
            return fail(RecordWorkspaceErrorCode::MissingDocument,
                        QStringLiteral("Docket entry %1 references missing document %2")
                            .arg(entry.id, entry.document_id));
        }
        referenced_documents.insert(entry.document_id);
    }
    QHash<QString, QString> parent_by_entry;
    static const QSet<QString> relationships{
        QStringLiteral("attachment"), QStringLiteral("amendment"), QStringLiteral("supplement"),
        QStringLiteral("component")};
    for (const auto& entry : definition.docket) {
        if (entry.parent_entry_id.isEmpty() != entry.relationship.isEmpty()) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Parent IDs and relationships must be declared together"));
        }
        if (entry.parent_entry_id.isEmpty()) {
            continue;
        }
        const auto parent = entries_by_id.constFind(entry.parent_entry_id);
        if (parent == entries_by_id.constEnd() || entry.parent_entry_id == entry.id ||
            (*parent)->docket_id != entry.docket_id ||
            !relationships.contains(entry.relationship)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Parent links must resolve acyclically within one docket"));
        }
        if (definition.disclosure_policy.has_value() &&
            !sealed_document_ids.contains(entry.document_id) &&
            sealed_document_ids.contains((*parent)->document_id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("A public docket entry cannot have a sealed parent"));
        }
        parent_by_entry.insert(entry.id, entry.parent_entry_id);
    }
    QSet<QString> resolved_parent_chains;
    for (const auto& entry : definition.docket) {
        QSet<QString> chain;
        auto current = entry.id;
        while (parent_by_entry.contains(current) && !resolved_parent_chains.contains(current)) {
            if (chain.contains(current)) {
                return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                            QStringLiteral("Docket entry parent graph contains a cycle"));
            }
            chain.insert(current);
            current = parent_by_entry.value(current);
        }
        resolved_parent_chains.unite(chain);
    }
    for (const auto& document : definition.documents) {
        if (!referenced_documents.contains(document.id)) {
            return fail(
                RecordWorkspaceErrorCode::OrphanDocument,
                QStringLiteral("Document is not represented on the docket: %1").arg(document.id));
        }
    }

    QSet<QString> anchor_ids;
    QSet<QString> citation_labels;
    QHash<QString, const RecordPageAnchor*> anchors_by_id;
    for (const auto& anchor : definition.anchors) {
        if (!isBoundedText(anchor.id, 160) || !isBoundedText(anchor.document_id, 160) ||
            !isBoundedText(anchor.citation_label, 120, true) || anchor.page_index < 0 ||
            anchor_ids.contains(anchor.id) || document_ids.contains(anchor.id) ||
            docket_ids.contains(anchor.id) || !document_ids.contains(anchor.document_id) ||
            (declared_page_counts.value(anchor.document_id) > 0 &&
             anchor.page_index >= declared_page_counts.value(anchor.document_id)) ||
            (!anchor.citation_label.isEmpty() && citation_labels.contains(anchor.citation_label))) {
            return fail(RecordWorkspaceErrorCode::InvalidPageAnchor,
                        QStringLiteral("Record page anchor is invalid: %1").arg(anchor.id));
        }
        anchor_ids.insert(anchor.id);
        anchors_by_id.insert(anchor.id, &anchor);
        if (!anchor.citation_label.isEmpty()) {
            citation_labels.insert(anchor.citation_label);
        }
    }

    if (!definition.disclosure_policy.has_value()) {
        if (!definition.sealed_disclosures.empty()) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Sealed disclosures require an explicit policy"));
        }
        return {};
    }
    const auto& policy = *definition.disclosure_policy;
    if (!isBoundedText(policy.record_id, 160) || !isBoundedText(policy.policy_id, 160) ||
        policy.unauthorized_projection != QStringLiteral("public_counterparts_only") ||
        policy.authorized_projection != QStringLiteral("public_and_authorized_sealed") ||
        policy.sealed_asset_access != QStringLiteral("session_event_grant_required") ||
        definition.sealed_disclosures.empty()) {
        return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                    QStringLiteral("Record disclosure policy is incomplete or unsupported"));
    }

    QHash<QString, const RecordDocument*> documents_by_id;
    for (const auto& document : definition.documents) {
        documents_by_id.insert(document.id, &document);
    }
    QHash<QString, QString> docket_by_document;
    for (const auto& entry : definition.docket) {
        if (!docket_by_document.contains(entry.document_id)) {
            docket_by_document.insert(entry.document_id, entry.docket_id);
        } else if (docket_by_document.value(entry.document_id) != entry.docket_id) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("A disclosure document cannot span multiple dockets"));
        }
    }

    QSet<QString> sealed_documents;
    QSet<QString> disclosure_ids;
    QSet<QString> public_counterparts;
    QSet<QString> stable_anchor_ids;
    QSet<QString> mapped_sealed_anchors;
    QSet<QString> mapped_public_anchors;
    static const QSet<QString> requirement_kinds{QStringLiteral("motion"),
                                                 QStringLiteral("certificate"),
                                                 QStringLiteral("redacted_counterpart")};
    for (const auto& disclosure : definition.sealed_disclosures) {
        const auto sealed = documents_by_id.constFind(disclosure.sealed_document_id);
        if (!isBoundedText(disclosure.disclosure_id, 160) ||
            disclosure_ids.contains(disclosure.disclosure_id) ||
            stable_anchor_ids.contains(disclosure.disclosure_id) ||
            document_ids.contains(disclosure.disclosure_id) ||
            docket_ids.contains(disclosure.disclosure_id) ||
            anchor_ids.contains(disclosure.disclosure_id) ||
            !isBoundedText(disclosure.sealed_document_id, 160) ||
            !isBoundedText(disclosure.public_document_id, 160, true) ||
            !isBoundedText(disclosure.motion_document_id, 160, true) ||
            !isBoundedText(disclosure.certificate_document_id, 160, true) ||
            !isBoundedText(disclosure.authorization_authority_id, 160) ||
            sealed == documents_by_id.constEnd() || !(*sealed)->sealed ||
            sealed_documents.contains(disclosure.sealed_document_id) ||
            disclosure.required_items.size() > 3) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Sealed disclosure identity or authority is invalid"));
        }
        disclosure_ids.insert(disclosure.disclosure_id);
        sealed_documents.insert(disclosure.sealed_document_id);
        QSet<QString> requirements;
        for (const auto& requirement : disclosure.required_items) {
            if (!requirement_kinds.contains(requirement) || requirements.contains(requirement)) {
                return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                            QStringLiteral("Disclosure requirements must be typed and unique"));
            }
            requirements.insert(requirement);
        }

        const auto validatePublicDocument =
            [&](const QString& id, QStringView role) -> std::expected<void, RecordWorkspaceError> {
            if (id.isEmpty()) {
                return {};
            }
            const auto document = documents_by_id.constFind(id);
            if (document == documents_by_id.constEnd() || (*document)->sealed ||
                id == disclosure.sealed_document_id) {
                return fail(
                    RecordWorkspaceErrorCode::InvalidDefinition,
                    QStringLiteral("Disclosure %1 must resolve to a public document").arg(role));
            }
            return {};
        };
        if (const auto valid =
                validatePublicDocument(disclosure.public_document_id, u"counterpart");
            !valid) {
            return valid;
        }
        if (const auto valid = validatePublicDocument(disclosure.motion_document_id, u"motion");
            !valid) {
            return valid;
        }
        if (const auto valid =
                validatePublicDocument(disclosure.certificate_document_id, u"certificate");
            !valid) {
            return valid;
        }
        if (!disclosure.public_document_id.isEmpty() &&
            (public_counterparts.contains(disclosure.public_document_id) ||
             docket_by_document.value(disclosure.public_document_id) !=
                 docket_by_document.value(disclosure.sealed_document_id))) {
            return fail(
                RecordWorkspaceErrorCode::InvalidDefinition,
                QStringLiteral("Public counterparts must be one-to-one in the same docket"));
        }
        if (!disclosure.public_document_id.isEmpty()) {
            public_counterparts.insert(disclosure.public_document_id);
        }
        QSet<QString> support_documents;
        for (const auto& id : {disclosure.public_document_id, disclosure.motion_document_id,
                               disclosure.certificate_document_id}) {
            if (!id.isEmpty() && support_documents.contains(id)) {
                return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                            QStringLiteral("Disclosure support documents must be distinct"));
            }
            if (!id.isEmpty()) {
                support_documents.insert(id);
            }
        }
        if (disclosure.public_document_id.isEmpty() && !disclosure.anchor_mappings.empty()) {
            return fail(RecordWorkspaceErrorCode::InvalidPageAnchor,
                        QStringLiteral("Stable twin anchors require a public counterpart"));
        }
        for (const auto& mapping : disclosure.anchor_mappings) {
            const auto sealed_anchor = anchors_by_id.constFind(mapping.sealed_anchor_id);
            const auto public_anchor = anchors_by_id.constFind(mapping.public_anchor_id);
            if (!isBoundedText(mapping.stable_anchor_id, 160) ||
                !isBoundedText(mapping.sealed_anchor_id, 160) ||
                !isBoundedText(mapping.public_anchor_id, 160) ||
                stable_anchor_ids.contains(mapping.stable_anchor_id) ||
                anchor_ids.contains(mapping.stable_anchor_id) ||
                document_ids.contains(mapping.stable_anchor_id) ||
                docket_ids.contains(mapping.stable_anchor_id) ||
                mapped_sealed_anchors.contains(mapping.sealed_anchor_id) ||
                mapped_public_anchors.contains(mapping.public_anchor_id) ||
                sealed_anchor == anchors_by_id.constEnd() ||
                public_anchor == anchors_by_id.constEnd() ||
                (*sealed_anchor)->document_id != disclosure.sealed_document_id ||
                (*public_anchor)->document_id != disclosure.public_document_id) {
                return fail(
                    RecordWorkspaceErrorCode::InvalidPageAnchor,
                    QStringLiteral("Twin page anchors are duplicate, ambiguous, or orphaned"));
            }
            stable_anchor_ids.insert(mapping.stable_anchor_id);
            mapped_sealed_anchors.insert(mapping.sealed_anchor_id);
            mapped_public_anchors.insert(mapping.public_anchor_id);
        }
    }
    for (const auto& document : definition.documents) {
        if (document.sealed && !sealed_documents.contains(document.id)) {
            return fail(RecordWorkspaceErrorCode::InvalidDefinition,
                        QStringLiteral("Every sealed document requires one disclosure rule"));
        }
    }
    return {};
}

std::expected<void, RecordWorkspaceError> RecordWorkspace::setRecord(RecordDefinition definition) {
    if (const auto valid = validate(definition); !valid) {
        last_error_ = valid.error();
        status_label_->setText(valid.error().message);
        return std::unexpected(valid.error());
    }

    full_definition_ = std::move(definition);
    authorized_document_ids_.clear();
    access_projection_.reset();
    docket_filter_->clear();
    document_search_->clear();
    clearLoadedDocument();
    rebuildDisclosureProjection();
    clearError();
    return {};
}

bool RecordWorkspace::applyRecordAccessProjection(model::RecordAccessProjection projection) {
    return applyAccessProjection(std::move(projection)).has_value();
}

std::expected<void, RecordWorkspaceError>
RecordWorkspace::setAccessProjectionForTest(model::RecordAccessProjection projection) {
    return applyAccessProjection(std::move(projection));
}

std::expected<void, RecordWorkspaceError>
RecordWorkspace::applyAccessProjection(model::RecordAccessProjection projection) {
    if (!full_definition_.disclosure_policy.has_value()) {
        return recordError(RecordWorkspaceErrorCode::AccessProjectionMismatch,
                           QStringLiteral("This record has no session access policy"));
    }
    const auto& policy = *full_definition_.disclosure_policy;
    const auto zero_head = projection.head_digest == std::string(64, '0');
    if (QString::fromUtf8(projection.record_id) != policy.record_id ||
        QString::fromUtf8(projection.policy_id) != policy.policy_id ||
        projection.session_id.empty() ||
        !isLowercaseSha256(QString::fromLatin1(projection.head_digest)) ||
        (projection.through_sequence == 0) != zero_head ||
        (access_projection_.has_value() &&
         (access_projection_->session_id != projection.session_id ||
          projection.through_sequence < access_projection_->through_sequence ||
          (projection.through_sequence == access_projection_->through_sequence &&
           projection != *access_projection_)))) {
        return recordError(RecordWorkspaceErrorCode::AccessProjectionMismatch,
                           QStringLiteral("Session access projection does not match this record"));
    }
    QSet<QString> permitted;
    for (const auto& disclosure : full_definition_.sealed_disclosures) {
        permitted.insert(disclosure.sealed_document_id);
    }
    QSet<QString> authorized;
    for (const auto& document_id : projection.authorized_document_ids) {
        const auto id = QString::fromUtf8(document_id);
        if (!permitted.contains(id) || authorized.contains(id)) {
            return recordError(
                RecordWorkspaceErrorCode::AccessProjectionMismatch,
                QStringLiteral("Session access projection contains an invalid grant"));
        }
        authorized.insert(id);
    }

    if (!current_document_id_.isEmpty()) {
        const auto loaded = std::ranges::find(full_definition_.documents, current_document_id_,
                                              &RecordDocument::id);
        if (loaded != full_definition_.documents.end() && loaded->sealed &&
            !authorized.contains(loaded->id)) {
            clearLoadedDocument();
        }
    }
    authorized_document_ids_ = std::move(authorized);
    access_projection_ = std::move(projection);
    rebuildDisclosureProjection();
    clearError();
    return {};
}

void RecordWorkspace::rebuildDisclosureProjection() {
    RecordDefinition projected;
    projected.dockets = full_definition_.dockets;
    projected.disclosure_policy = full_definition_.disclosure_policy;
    projected.sealed_disclosures = full_definition_.sealed_disclosures;
    QSet<QString> visible_document_ids;
    for (const auto& document : full_definition_.documents) {
        const auto visible = !full_definition_.disclosure_policy.has_value() || !document.sealed ||
                             authorized_document_ids_.contains(document.id);
        if (visible) {
            projected.documents.push_back(document);
            visible_document_ids.insert(document.id);
        }
    }
    for (const auto& entry : full_definition_.docket) {
        if (visible_document_ids.contains(entry.document_id)) {
            projected.docket.push_back(entry);
        }
    }
    QHash<QString, RecordPageAnchor> authored_anchors;
    for (const auto& anchor : full_definition_.anchors) {
        authored_anchors.insert(anchor.id, anchor);
        if (visible_document_ids.contains(anchor.document_id)) {
            projected.anchors.push_back(anchor);
        }
    }
    if (full_definition_.disclosure_policy.has_value()) {
        for (const auto& disclosure : full_definition_.sealed_disclosures) {
            const auto use_sealed =
                authorized_document_ids_.contains(disclosure.sealed_document_id);
            for (const auto& mapping : disclosure.anchor_mappings) {
                const auto physical_id =
                    use_sealed ? mapping.sealed_anchor_id : mapping.public_anchor_id;
                const auto physical = authored_anchors.constFind(physical_id);
                if (physical != authored_anchors.constEnd() &&
                    visible_document_ids.contains(physical->document_id)) {
                    auto stable = *physical;
                    stable.id = mapping.stable_anchor_id;
                    projected.anchors.push_back(std::move(stable));
                }
            }
        }
    }

    docket_model_->setRecordData(projected);
    QHash<QString, RecordDocument> documents;
    for (auto& document : projected.documents) {
        documents.insert(document.id, std::move(document));
    }
    QHash<QString, RecordPageAnchor> anchors;
    for (auto& anchor : projected.anchors) {
        anchors.insert(anchor.id, std::move(anchor));
    }
    QHash<QString, QString> citation_anchors;
    for (auto anchor = anchors.constBegin(); anchor != anchors.constEnd(); ++anchor) {
        if (!anchor->citation_label.isEmpty()) {
            citation_anchors.insert(anchor->citation_label, anchor.key());
        }
    }
    documents_ = std::move(documents);
    anchors_ = std::move(anchors);
    citation_anchors_ = std::move(citation_anchors);
}

void RecordWorkspace::clearLoadedDocument() {
    pdf_search_model_->setSearchString({});
    document_search_->clear();
    pdf_document_->close();
    current_asset_snapshot_.reset();
    current_document_id_.clear();
    updatePageControls(-1);
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
RecordWorkspace::navigateToCitation(QStringView citation_label) {
    const auto anchor = citation_anchors_.constFind(citation_label.toString());
    if (anchor == citation_anchors_.constEnd()) {
        return recordError(RecordWorkspaceErrorCode::InvalidPageAnchor,
                           QStringLiteral("Record citation does not exist"));
    }
    return navigateToAnchor(*anchor);
}

std::expected<void, RecordWorkspaceError>
RecordWorkspace::openDocument(const RecordDocument& document, int page_index) {
    if (document.sealed && (!full_definition_.disclosure_policy.has_value() ||
                            !authorized_document_ids_.contains(document.id))) {
        return recordError(
            RecordWorkspaceErrorCode::SealedDocument,
            QStringLiteral("This record item is unavailable in the current session"));
    }

    QString source = document.file_path;
    std::shared_ptr<QTemporaryFile> authorized_snapshot;
    if (document.sealed && full_definition_.disclosure_policy.has_value() &&
        document.deferred_asset) {
        const auto resolved = document.deferred_asset();
        if (!resolved || resolved->file_path.isEmpty()) {
            clearLoadedDocument();
            return recordError(
                RecordWorkspaceErrorCode::PdfLoadFailed,
                QStringLiteral("Authorized sealed record asset failed local verification"));
        }
        source = resolved->file_path;
        authorized_snapshot = std::move(resolved->owned_snapshot);
    }
    if (source.isEmpty()) {
        clearLoadedDocument();
        return recordError(RecordWorkspaceErrorCode::PdfLoadFailed,
                           document.sealed
                               ? QStringLiteral("Authorized sealed record asset is unavailable")
                               : QStringLiteral("Record PDF asset is unavailable"));
    }

    pdf_search_model_->setSearchString({});
    document_search_->clear();
    pdf_document_->close();
    current_asset_snapshot_.reset();
    const auto error = pdf_document_->load(source);
    if (error != QPdfDocument::Error::None ||
        pdf_document_->status() != QPdfDocument::Status::Ready || pdf_document_->pageCount() <= 0) {
        current_document_id_.clear();
        updatePageControls(-1);
        return recordError(RecordWorkspaceErrorCode::PdfLoadFailed,
                           document.sealed
                               ? QStringLiteral("Authorized sealed record PDF cannot be loaded")
                               : QStringLiteral("Cannot load record PDF: %1").arg(document.title));
    }
    if (document.declared_page_count > 0 &&
        pdf_document_->pageCount() != document.declared_page_count) {
        current_document_id_.clear();
        pdf_document_->close();
        updatePageControls(-1);
        return recordError(
            RecordWorkspaceErrorCode::PdfLoadFailed,
            document.sealed ? QStringLiteral("Authorized sealed record PDF failed local validation")
                            : QStringLiteral("Record PDF page count differs from its declaration"));
    }
    if (page_index < 0 || page_index >= pdf_document_->pageCount()) {
        current_document_id_.clear();
        pdf_document_->close();
        updatePageControls(-1);
        return recordError(RecordWorkspaceErrorCode::InvalidPageAnchor,
                           QStringLiteral("Record page anchor is outside the document"));
    }

    current_document_id_ = document.id;
    current_asset_snapshot_ = std::move(authorized_snapshot);
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

std::vector<model::RecordDisclosureDeficiency> RecordWorkspace::disclosureDeficiencies() const {
    std::vector<model::RecordDisclosureDeficiency> deficiencies;
    for (const auto& disclosure : full_definition_.sealed_disclosures) {
        const auto appendIfMissing = [&](QStringView requirement, const QString& document_id,
                                         model::RecordDisclosureDeficiencyKind kind) {
            if (disclosure.required_items.contains(requirement) && document_id.isEmpty()) {
                deficiencies.push_back(model::RecordDisclosureDeficiency{
                    disclosure.disclosure_id.toUtf8().toStdString(), kind});
            }
        };
        appendIfMissing(u"motion", disclosure.motion_document_id,
                        model::RecordDisclosureDeficiencyKind::MissingPublicMotion);
        appendIfMissing(u"certificate", disclosure.certificate_document_id,
                        model::RecordDisclosureDeficiencyKind::MissingCertificate);
        appendIfMissing(u"redacted_counterpart", disclosure.public_document_id,
                        model::RecordDisclosureDeficiencyKind::MissingRedactedCounterpart);
    }
    return deficiencies;
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
