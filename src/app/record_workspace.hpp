#pragma once

#include "appellate/model/record_access.hpp"

#include <QAbstractTableModel>
#include <QDate>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
class QPdfDocument;
class QPdfSearchModel;
class QPdfView;
class QPushButton;
class QSpinBox;
class QTableView;
class QTemporaryFile;

namespace appellate::ui {

struct RecordAssetLease final {
    QString file_path;
    // When present, owns the private verified snapshot for as long as the PDF
    // document is loaded. Public/legacy assets continue to use catalog paths.
    std::shared_ptr<QTemporaryFile> owned_snapshot;
};

using DeferredRecordAsset = std::function<std::expected<RecordAssetLease, QString>()>;

struct RecordDocument final {
    QString id;
    QString title;
    QString file_path;
    bool sealed{};
    QMap<QString, QString> metadata;
    int declared_page_count{};
    // Sealed pack assets use a resolver so validation/materialization and PDF
    // parsing cannot happen until the exact document is authorized and opened.
    DeferredRecordAsset deferred_asset;

    RecordDocument() = default;
    RecordDocument(QString id_value, QString title_value, QString file_path_value,
                   bool sealed_value, QMap<QString, QString> metadata_value, int page_count_value,
                   DeferredRecordAsset deferred_asset_value = {})
        : id(std::move(id_value)), title(std::move(title_value)),
          file_path(std::move(file_path_value)), sealed(sealed_value),
          metadata(std::move(metadata_value)), declared_page_count(page_count_value),
          deferred_asset(std::move(deferred_asset_value)) {}

    // Asset resolvers are execution capabilities, not authored record data.
    friend bool operator==(const RecordDocument& lhs, const RecordDocument& rhs) {
        return lhs.id == rhs.id && lhs.title == rhs.title && lhs.file_path == rhs.file_path &&
               lhs.sealed == rhs.sealed && lhs.metadata == rhs.metadata &&
               lhs.declared_page_count == rhs.declared_page_count;
    }
};

struct RecordDocketDescriptor final {
    QString id;
    QString type;
    QString court_id;
    QString court_ref;
    QString public_docket_number;
    QString caption;

    friend bool operator==(const RecordDocketDescriptor&, const RecordDocketDescriptor&) = default;
};

struct RecordDocketEntry final {
    QString id;
    QDate filed_on;
    QString title;
    QString actor;
    QString description;
    QString document_id;
    QStringList tags;
    QMap<QString, QString> metadata;
    QString docket_id;
    QString docket_label;
    QString entry_label;
    QString parent_entry_id;
    QString relationship;

    friend bool operator==(const RecordDocketEntry&, const RecordDocketEntry&) = default;
};

struct RecordPageAnchor final {
    QString id;
    QString document_id;
    int page_index{};
    QString citation_label;

    friend bool operator==(const RecordPageAnchor&, const RecordPageAnchor&) = default;
};

struct RecordTwinAnchor final {
    QString stable_anchor_id;
    QString sealed_anchor_id;
    QString public_anchor_id;

    friend bool operator==(const RecordTwinAnchor&, const RecordTwinAnchor&) = default;
};

struct RecordSealedDisclosure final {
    QString disclosure_id;
    QString sealed_document_id;
    QString public_document_id;
    QString motion_document_id;
    QString certificate_document_id;
    QString authorization_authority_id;
    QStringList required_items;
    std::vector<RecordTwinAnchor> anchor_mappings;

    friend bool operator==(const RecordSealedDisclosure&, const RecordSealedDisclosure&) = default;
};

struct RecordDisclosurePolicy final {
    QString record_id;
    QString policy_id;
    QString unauthorized_projection;
    QString authorized_projection;
    QString sealed_asset_access;

    friend bool operator==(const RecordDisclosurePolicy&, const RecordDisclosurePolicy&) = default;
};

struct RecordDefinition final {
    std::vector<RecordDocument> documents;
    std::vector<RecordDocketEntry> docket;
    std::vector<RecordPageAnchor> anchors;
    std::vector<RecordDocketDescriptor> dockets;
    std::optional<RecordDisclosurePolicy> disclosure_policy;
    std::vector<RecordSealedDisclosure> sealed_disclosures;

    RecordDefinition() = default;
    RecordDefinition(std::vector<RecordDocument> documents_value,
                     std::vector<RecordDocketEntry> docket_value,
                     std::vector<RecordPageAnchor> anchors_value,
                     std::vector<RecordDocketDescriptor> dockets_value,
                     std::optional<RecordDisclosurePolicy> policy_value = std::nullopt,
                     std::vector<RecordSealedDisclosure> disclosures_value = {})
        : documents(std::move(documents_value)), docket(std::move(docket_value)),
          anchors(std::move(anchors_value)), dockets(std::move(dockets_value)),
          disclosure_policy(std::move(policy_value)),
          sealed_disclosures(std::move(disclosures_value)) {}
};

enum class RecordWorkspaceErrorCode {
    InvalidDefinition,
    MissingDocument,
    OrphanDocument,
    MissingAsset,
    MissingDocketEntry,
    InvalidPageAnchor,
    SealedDocument,
    PdfLoadFailed,
    NoDocumentSelected,
    AccessProjectionMismatch,
};

struct RecordWorkspaceError final {
    RecordWorkspaceErrorCode code;
    QString message;

    friend bool operator==(const RecordWorkspaceError&, const RecordWorkspaceError&) = default;
};

class RecordDocketModel final : public QAbstractTableModel {
  public:
    enum class Column {
        Docket,
        EntryLabel,
        Filed,
        Title,
        Actor,
        Document,
        Access,
        Count,
    };

    enum Role {
        DocketIdRole = Qt::UserRole + 1,
        DocumentIdRole,
        SearchTextRole,
        SealedRole,
    };

    explicit RecordDocketModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRecordData(const RecordDefinition& definition);
    [[nodiscard]] const RecordDocketEntry* entryAt(int row) const;
    [[nodiscard]] int indexOf(QStringView docket_id) const;

  private:
    struct Row final {
        RecordDocketEntry entry;
        QString document_title;
        bool sealed{};
        QString search_text;
    };

    std::vector<Row> rows_;
};

class DocketFilterProxyModel;

class RecordWorkspace final : public QWidget, public model::RecordAccessProjectionTarget {
  public:
    explicit RecordWorkspace(QWidget* parent = nullptr);
    ~RecordWorkspace() override;

    [[nodiscard]] auto setRecord(RecordDefinition definition)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto openDocketEntry(QStringView docket_id)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto openSelectedDocketEntry() -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto navigateToAnchor(QStringView anchor_id)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto navigateToCitation(QStringView citation_label)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto goToPage(int page_index) -> std::expected<void, RecordWorkspaceError>;

    void setDocketFilter(const QString& query);
    void setDocumentSearch(const QString& query);

    [[nodiscard]] qsizetype visibleDocketCount() const;
    [[nodiscard]] qsizetype filterEvaluationCount() const;
    [[nodiscard]] int loadedPageCount() const;
    [[nodiscard]] int currentPageIndex() const;
    [[nodiscard]] int documentSearchResultCount() const;
    [[nodiscard]] const QString& currentDocumentId() const noexcept;
    [[nodiscard]] const std::optional<RecordWorkspaceError>& lastError() const noexcept;
    [[nodiscard]] std::vector<model::RecordDisclosureDeficiency> disclosureDeficiencies() const;

    [[nodiscard]] QLineEdit* docketFilterEdit() const noexcept;
    [[nodiscard]] QLineEdit* documentSearchEdit() const noexcept;
    [[nodiscard]] QTableView* docketView() const noexcept;
    [[nodiscard]] QPdfView* pdfView() const noexcept;
    [[nodiscard]] QPushButton* openDocumentButton() const noexcept;
    [[nodiscard]] QPushButton* previousPageButton() const noexcept;
    [[nodiscard]] QPushButton* nextPageButton() const noexcept;
    [[nodiscard]] QSpinBox* pageSpinBox() const noexcept;

  private:
    friend class RecordWorkspaceTestAccess;

    [[nodiscard]] bool
    applyRecordAccessProjection(model::RecordAccessProjection projection) override;
    [[nodiscard]] auto setAccessProjectionForTest(model::RecordAccessProjection projection)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto applyAccessProjection(model::RecordAccessProjection projection)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto validate(const RecordDefinition& definition) const
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto openDocument(const RecordDocument& document, int page_index)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto recordError(RecordWorkspaceErrorCode code, QString message)
        -> std::unexpected<RecordWorkspaceError>;
    void clearError();
    void clearLoadedDocument();
    void rebuildDisclosureProjection();
    void updatePageControls(int page_index);
    void selectSourceRow(int source_row);

    RecordDocketModel* docket_model_{};
    DocketFilterProxyModel* filter_model_{};
    QLineEdit* docket_filter_{};
    QLineEdit* document_search_{};
    QTableView* docket_view_{};
    QPdfDocument* pdf_document_{};
    QPdfSearchModel* pdf_search_model_{};
    QPdfView* pdf_view_{};
    QPushButton* open_document_button_{};
    QPushButton* previous_page_button_{};
    QPushButton* next_page_button_{};
    QPushButton* next_search_result_button_{};
    QSpinBox* page_spin_box_{};
    QLabel* page_count_label_{};
    QLabel* status_label_{};

    QHash<QString, RecordDocument> documents_;
    QHash<QString, RecordPageAnchor> anchors_;
    QHash<QString, QString> citation_anchors_;
    RecordDefinition full_definition_;
    QSet<QString> authorized_document_ids_;
    std::optional<model::RecordAccessProjection> access_projection_;
    std::shared_ptr<QTemporaryFile> current_asset_snapshot_;
    QString current_document_id_;
    std::optional<RecordWorkspaceError> last_error_;
};

} // namespace appellate::ui
