#pragma once

#include <QAbstractTableModel>
#include <QDate>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <expected>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QPdfDocument;
class QPdfSearchModel;
class QPdfView;
class QPushButton;
class QSpinBox;
class QTableView;

namespace appellate::ui {

struct RecordDocument final {
    QString id;
    QString title;
    QString file_path;
    bool sealed{};
    QMap<QString, QString> metadata;

    friend bool operator==(const RecordDocument&, const RecordDocument&) = default;
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

    friend bool operator==(const RecordDocketEntry&, const RecordDocketEntry&) = default;
};

struct RecordPageAnchor final {
    QString id;
    QString document_id;
    int page_index{};

    friend bool operator==(const RecordPageAnchor&, const RecordPageAnchor&) = default;
};

struct RecordDefinition final {
    std::vector<RecordDocument> documents;
    std::vector<RecordDocketEntry> docket;
    std::vector<RecordPageAnchor> anchors;
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
};

struct RecordWorkspaceError final {
    RecordWorkspaceErrorCode code;
    QString message;

    friend bool operator==(const RecordWorkspaceError&, const RecordWorkspaceError&) = default;
};

class RecordDocketModel final : public QAbstractTableModel {
  public:
    enum class Column {
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

class RecordWorkspace final : public QWidget {
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

    [[nodiscard]] QLineEdit* docketFilterEdit() const noexcept;
    [[nodiscard]] QLineEdit* documentSearchEdit() const noexcept;
    [[nodiscard]] QTableView* docketView() const noexcept;
    [[nodiscard]] QPdfView* pdfView() const noexcept;
    [[nodiscard]] QPushButton* openDocumentButton() const noexcept;
    [[nodiscard]] QPushButton* previousPageButton() const noexcept;
    [[nodiscard]] QPushButton* nextPageButton() const noexcept;
    [[nodiscard]] QSpinBox* pageSpinBox() const noexcept;

  private:
    [[nodiscard]] auto validate(const RecordDefinition& definition) const
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto openDocument(const RecordDocument& document, int page_index)
        -> std::expected<void, RecordWorkspaceError>;
    [[nodiscard]] auto recordError(RecordWorkspaceErrorCode code, QString message)
        -> std::unexpected<RecordWorkspaceError>;
    void clearError();
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
    QString current_document_id_;
    std::optional<RecordWorkspaceError> last_error_;
};

} // namespace appellate::ui
