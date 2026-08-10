#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/pack_id.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "record_workspace.hpp"

#include <QString>

#include <expected>
#include <vector>

namespace appellate::app {

enum class InstalledRecordErrorCode {
    RevisionMismatch,
    MissingCase,
    InvalidSelection,
    OrphanRecord,
    InvalidRecord,
    MissingBlob,
    AssetDigestMismatch,
    MaterializationFailure,
    AssetVerificationFailure,
    PdfLoadFailure,
    PageCountMismatch,
    WorkspaceFailure,
};

struct InstalledRecordError final {
    InstalledRecordErrorCode code{};
    QString message;

    friend bool operator==(const InstalledRecordError&, const InstalledRecordError&) = default;
};

struct InstalledRecordAsset final {
    packs::RuntimeRecordEntryId entry_id;
    model::BlobDescriptor descriptor;
    QString local_path;
    int page_count{};

    friend bool operator==(const InstalledRecordAsset&, const InstalledRecordAsset&) = default;
};

struct InstalledRecordLoad final {
    model::PackRevision revision;
    model::CaseId case_id;
    packs::RuntimeRecordId record_id;
    ui::RecordDefinition definition;
    std::vector<InstalledRecordAsset> assets;
};

// Bridges a runtime case to the native record workspace exclusively through the installed
// catalog. Authoring-directory paths are never accepted or resolved here.
class InstalledRecordController final {
  public:
    InstalledRecordController(packs::PackCatalog& catalog, ui::RecordWorkspace& workspace);

    InstalledRecordController(const InstalledRecordController&) = delete;
    InstalledRecordController& operator=(const InstalledRecordController&) = delete;
    InstalledRecordController(InstalledRecordController&&) = delete;
    InstalledRecordController& operator=(InstalledRecordController&&) = delete;
    ~InstalledRecordController() = default;

    [[nodiscard]] auto load(const packs::LoadedPack& loaded_pack,
                            const packs::RuntimePack& runtime_pack,
                            const model::CaseId& selected_case_id)
        -> std::expected<InstalledRecordLoad, InstalledRecordError>;

  private:
    packs::PackCatalog& catalog_;
    ui::RecordWorkspace& workspace_;
};

} // namespace appellate::app
