#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#ifndef APPELLATE_CA4_RULE54B_ROOT
#error "APPELLATE_CA4_RULE54B_ROOT must name content/ca4-rule54b"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

namespace engine = appellate::engine;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace storage = appellate::storage;

using model::PackId;
using model::PackRevision;
using model::ResourceKind;
using packs::PackArchive;
using packs::PackCatalog;
using packs::PackGraphState;
using packs::PackReader;
using packs::PackValidationScope;
using packs::ValidatedResource;
using namespace std::chrono_literals;

// Final release values live in one deliberately isolated block. The release auditor replaces
// only the placeholders once manifest/review/evidence assembly has frozen.
namespace release_pins {
constexpr auto root_digest = "7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728";
constexpr auto manifest_digest = "dcfac3e4cb8d60fe41843c15732d802b9dd5d689b6de7395f4f86339676dfa49";
constexpr auto archive_digest = "10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819";
constexpr std::uint64_t archive_byte_size = 3'974'147;
constexpr auto realism_review_digest =
    "e16caac5226fdb26fb8acead14ef0a0bfd4d569af5ba84b9da65389e5fb0c905";
constexpr auto evidence_closure_digest =
    "445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c";
} // namespace release_pins

constexpr auto pack_id = "us.ca4.rule54b.asterglen";
constexpr auto v1_root_digest = "ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424";
constexpr auto v1_archive_digest =
    "ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227";
constexpr std::uint64_t v1_archive_byte_size = 729'511;
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto case_digest = "c9356a9984a79260f685b197ce06f868be4f6dab097411782f19b9297ead743e";
constexpr auto record_digest = "429603e0b7b49ff25e8a444a411b9c257cfbf0009fbfe1fc103ae8ac80e52f84";
constexpr auto workflow_digest = "949626bcbe0046bbabf615f62df8376a4fcfa463e30fc9dd5c322d56d4428f21";
constexpr auto actual_bank_digest =
    "93b7156fff0a0bb811c99f4dd6cb0f94da7899d8d0412ea2183c9a97ec793742";
constexpr auto counterfactual_bank_digest =
    "88477e0fb4db65b1a73c8357a4c1556a9baf6562f2c4f0ed492673cef5336468";
constexpr auto realism_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";

struct TraceExpected final {
    std::string_view file_name;
    std::string_view trace_id;
    std::string_view evidence_id;
    int command_count{};
    int event_count{};
    std::string_view file_sha256;
    std::string_view journal_sha256;
    std::string_view digest;
    std::string_view terminal_stage_id;
    std::optional<std::string_view> disposition_plan_id;
};

struct DocketCount final {
    int documents{};
    int pages{};

    friend bool operator==(const DocketCount&, const DocketCount&) = default;
};

constexpr std::array expected_traces{
    TraceExpected{"actual-bare-certification-dismissal-mandate.json",
                  "ca4r54b.trace.actual-bare-certification-dismissal-mandate",
                  "ca4r54b.evidence.trace.actual-bare-certification-dismissal-mandate", 37, 40,
                  "f7b1043e81a114432a79048b1d6c478747954493c04b992093d7fb5f8c8438de",
                  "c4890a8fa7987ad85127d98209d878865abb1e6fbbbbc0504676666bd3232e5c",
                  "da2038d9c6d3cc486af66db69b4eeea17de497685856290a439f81bfc0efd715",
                  "ca4r54b.stage.terminated", "ca4r54b.disposition.authored-dismissal"},
    TraceExpected{"missing-separate-document-timing.json",
                  "ca4r54b.trace.missing-separate-document-timing",
                  "ca4r54b.evidence.trace.missing-separate-document-timing", 8, 8,
                  "ecc1c39d20a39cf40e3215a00caf0b9d9dcdea4de9e05b3cfcbe1bbede029d77",
                  "458efc43af7aa14b4584ab90be95e9d2c2efbad85f222a1ba1352928b1a2a81e",
                  "4f5ededc2f76c2e1b0275ad0078afb4cda0aac66312d3788f80fa3d25c334035",
                  "ca4r54b.stage.terminated", std::nullopt},
    TraceExpected{"stay-denied-later-of-mandate.json", "ca4r54b.trace.stay-denied-later-of-mandate",
                  "ca4r54b.evidence.trace.stay-denied-later-of-mandate", 34, 37,
                  "26d72b5287f040cefa6362a8c5dd2feadd160479d6a7a6f3baeeff1524fb1df5",
                  "a7d8a92931bbe3dd771fabc6ed2224033ef08c9e84b24ce52e7daf39797fc779",
                  "2fe3708e60a2b7536de9d6d02d8f15409ee54b8f38f4bc42c0384937bb2dad63",
                  "ca4r54b.stage.terminated", "ca4r54b.disposition.authored-dismissal"},
    TraceExpected{"stay-dissolved-mandate.json", "ca4r54b.trace.stay-dissolved-mandate",
                  "ca4r54b.evidence.trace.stay-dissolved-mandate", 34, 37,
                  "496a7b250f4174e28dba1a4c5f92c5929a73b4d41d36f5639d09ff9923a71c69",
                  "1b40724db3fff2d830f0ee7618ab9627c8acb3371ce6631b056b480b0f44c216",
                  "03cd98e55564b8b3402479310a55c7b1a40783b4d4170f72793d3e1f2875072f",
                  "ca4r54b.stage.terminated", "ca4r54b.disposition.authored-dismissal"},
    TraceExpected{"stay-granted-blocks-mandate.json", "ca4r54b.trace.stay-granted-blocks-mandate",
                  "ca4r54b.evidence.trace.stay-granted-blocks-mandate", 30, 33,
                  "e326ae49473449fdb08ca4e2471d7738a9e6c7c05d669d9f501320da211550f7",
                  "578943bdf85aa7314541c41c877ce1c3235233b71cb552e9cacf7d15340d8ce8",
                  "24c08217f63173689ef3539928c8ac902040d4284a0141767667199f60e62eda",
                  "ca4r54b.stage.mandate-stayed", "ca4r54b.disposition.authored-dismissal"},
    TraceExpected{"supported-certification-merits-mandate.json",
                  "ca4r54b.trace.supported-certification-merits-mandate",
                  "ca4r54b.evidence.trace.supported-certification-merits-mandate", 13, 13,
                  "fff98bfcba66a23668d64b0480a5bad62ca1f7c740f1786516408c1f2fbfdfef",
                  "5cb6371491b22c57a1eb76d9a1ceaa9a3073863868d3253d72d228a8cf5a4fbf",
                  "872ade365757742b6c4da3ef85e7f5f20f14a34552bd4dd84e197519ce10aa5c",
                  "ca4r54b.stage.terminated",
                  "ca4r54b.disposition.counterfactual-supported-certification"},
    TraceExpected{"timely-rehearing-denied-mandate.json",
                  "ca4r54b.trace.timely-rehearing-denied-mandate",
                  "ca4r54b.evidence.trace.timely-rehearing-denied-mandate", 35, 38,
                  "f5ae591e4131b5795be136417a2ca9dce64129c987006f04e20d7d7f03b8ceb2",
                  "ebd1b9386d9a0466d40395b973fd01e2ef427e211062af5ae14cdcf1f750488c",
                  "d8a89405b9b9ee418096db43c9bd953be758f23d1227c951ca190fd25292decd",
                  "ca4r54b.stage.terminated", "ca4r54b.disposition.authored-dismissal"},
    TraceExpected{"valid-certification-late-notice-dismissal.json",
                  "ca4r54b.trace.valid-certification-late-notice-dismissal",
                  "ca4r54b.evidence.trace.valid-certification-late-notice-dismissal", 14, 15,
                  "95d65eac615e47c3d9ed0c01868174b2d48d9052eff7fe5a25a3ed9e6c9a0c0b",
                  "32f97d133740e18fc8608174a657c2526be45dc86b7f13b3bfd987d9ab0a3e8b",
                  "cc7fb5e6e14cb3a2feda2a260b9552da028452407b442b076d6db6f39b6e865b",
                  "ca4r54b.stage.terminated", "ca4r54b.disposition.counterfactual-untimely-notice"},
};

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] QByteArray bytes(std::string_view value) {
    return QByteArray(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString text(std::string_view value) {
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, QByteArrayView bytes) {
    addUint64(hash, static_cast<std::uint64_t>(bytes.size()));
    hash.addData(bytes);
}

void addFrame(QCryptographicHash& hash, QStringView value) {
    const auto bytes = value.toUtf8();
    addFrame(hash, QByteArrayView(bytes));
}

[[nodiscard]] std::optional<QString> journalDigest(const QJsonArray& journal) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, static_cast<std::uint64_t>(journal.size()));
    for (const auto& entry_value : journal) {
        if (!entry_value.isObject()) {
            return std::nullopt;
        }
        const auto entry = entry_value.toObject();
        const auto command_encoded =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(command_encoded);
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        if (command.isEmpty() || command.toBase64() != command_encoded) {
            return std::nullopt;
        }
        addFrame(hash, QByteArrayView(command));
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event : events) {
            const auto event_encoded = event.toString().toLatin1();
            const auto event_bytes = QByteArray::fromBase64(event_encoded);
            if (event_bytes.isEmpty() || event_bytes.toBase64() != event_encoded) {
                return std::nullopt;
            }
            addFrame(hash, QByteArrayView(event_bytes));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, QStringLiteral("ca4r54b.case.asterglen"));
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInt()));
    addUint64(hash, static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInt()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operations = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operations.size()));
    for (const auto& operation : operations) {
        addFrame(hash, operation.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const ValidatedResource* findResource(const std::vector<ValidatedResource>& resources,
                                                    std::string_view id) {
    const auto found = std::ranges::find(resources, id, [](const auto& resource) {
        return std::string_view(resource.descriptor.id);
    });
    return found == resources.end() ? nullptr : &*found;
}

[[nodiscard]] QSet<QString> strings(const QJsonArray& values) {
    QSet<QString> result;
    for (const auto& value : values) {
        result.insert(value.toString());
    }
    return result;
}

[[nodiscard]] int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

[[nodiscard]] std::optional<std::string>
commandDocumentDigest(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> std::optional<std::string> {
            if constexpr (requires { concrete.document_sha256; }) {
                return concrete.document_sha256;
            }
            return std::nullopt;
        },
        command);
}

[[nodiscard]] std::string commandSessionId(const model::WorkflowCommand& command) {
    return std::visit([](const auto& concrete) { return concrete.header.session_id; }, command);
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, event);
}

[[nodiscard]] model::WorkflowState initialState(const model::WorkflowDefinition& workflow,
                                                const model::WorkflowCommand& first_command) {
    model::WorkflowState state;
    state.session_id = commandSessionId(first_command);
    state.workflow_id = workflow.id;
    state.current_stage_id = workflow.initial_stage_id;
    return state;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QDir authoring_root(QStringLiteral(APPELLATE_CA4_RULE54B_ROOT));
    const QDir pack_root(authoring_root.filePath(QStringLiteral("pack-v0.2.0")));
    const QDir foundations_root(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    const QDir traces_root(authoring_root.filePath(QStringLiteral("traces/v0.2.0")));

    const PackRevision expected_root{PackId{pack_id}, "0.2.0", release_pins::root_digest};
    const PackRevision expected_v1{PackId{pack_id}, "0.1.0", v1_root_digest};
    const PackRevision expected_federal{PackId{"foundation.us-federal"}, "2025.12.01",
                                        federal_digest};
    const PackRevision expected_ca4{PackId{"foundation.us-ca4"}, "2026.03.23", ca4_digest};
    const PackRevision expected_bench{PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                                      bench_digest};

    const auto source =
        PackReader::readDirectory(pack_root.path(), PackValidationScope::ResolvedClosure);
    if (!source) {
        return fail(QStringLiteral("Asterglen 0.2 source pack: %1").arg(source.error().message));
    }
    const auto manifest_bytes = readAll(pack_root.filePath(QStringLiteral("manifest.json")));
    if (manifest_bytes.isEmpty() || sha256(manifest_bytes) != release_pins::manifest_digest ||
        source->revision != expected_root ||
        source->graph_state != PackGraphState::DeferredReferences ||
        source->dependencies.size() != std::size_t{3} ||
        source->required_capabilities.size() != std::size_t{16} ||
        source->resources.size() != std::size_t{9} || source->blobs.size() != std::size_t{75}) {
        return fail(QStringLiteral("Asterglen 0.2 release envelope/count contract mismatch"));
    }

    const std::array expected_dependencies{expected_federal, expected_ca4, expected_bench};
    for (const auto& expected : expected_dependencies) {
        if (std::ranges::find(source->dependencies, expected, [](const auto& dependency) {
                return dependency.revision;
            }) == source->dependencies.end()) {
            return fail(QStringLiteral("missing exact dependency: %1")
                            .arg(QString::fromStdString(expected.id.value)));
        }
    }
    QSet<QString> capability_ids;
    for (const auto& capability : source->required_capabilities) {
        capability_ids.insert(QStringLiteral("%1@%2")
                                  .arg(QString::fromStdString(capability.id))
                                  .arg(capability.version));
    }
    const QSet<QString> required_new_capabilities{
        QStringLiteral("workbench.pack.route-filing-bindings@1"),
        QStringLiteral("workbench.pack.alternative-event-date-deadlines@1"),
        QStringLiteral("workbench.pack.operation-legal-time-guards@1"),
        QStringLiteral("workbench.pack.realism-evidence@1"),
        QStringLiteral("workbench.pack.structured-disposition@1"),
    };
    if (!std::ranges::all_of(required_new_capabilities,
                             [&](const auto& id) { return capability_ids.contains(id); })) {
        return fail(QStringLiteral("Asterglen 0.2 capability closure is incomplete"));
    }

    for (const auto& resource : source->resources) {
        const auto bytes =
            readAll(pack_root.filePath(QString::fromStdString(resource.descriptor.path)));
        if (bytes.isEmpty() || sha256(bytes).toStdString() != resource.descriptor.sha256) {
            return fail(QStringLiteral("resource digest mismatch: %1")
                            .arg(QString::fromStdString(resource.descriptor.path)));
        }
    }
    for (const auto& blob : source->blobs) {
        const auto bytes = readAll(pack_root.filePath(QString::fromStdString(blob.path)));
        if (bytes.size() != static_cast<qsizetype>(blob.byte_size) ||
            sha256(bytes).toStdString() != blob.sha256) {
            return fail(QStringLiteral("blob identity mismatch: %1")
                            .arg(QString::fromStdString(blob.path)));
        }
    }

    const auto* case_resource = findResource(source->resources, "ca4r54b.case.asterglen");
    const auto* record_resource = findResource(source->resources, "ca4r54b.record.asterglen");
    const auto* workflow_resource =
        findResource(source->resources, "ca4r54b.workflow.civil-rule54b");
    const auto* actual_argument = findResource(source->resources, "ca4r54b.argument.actual-record");
    const auto* counterfactual_argument =
        findResource(source->resources, "ca4r54b.argument.counterfactual");
    const auto* review_resource =
        findResource(source->resources, "ca4r54b.review.authoring-2026-08-12");
    if (case_resource == nullptr || record_resource == nullptr || workflow_resource == nullptr ||
        actual_argument == nullptr || counterfactual_argument == nullptr ||
        review_resource == nullptr || case_resource->descriptor.kind != ResourceKind::Case ||
        record_resource->descriptor.kind != ResourceKind::Record ||
        workflow_resource->descriptor.kind != ResourceKind::Workflow ||
        case_resource->descriptor.sha256 != case_digest ||
        record_resource->descriptor.sha256 != record_digest ||
        workflow_resource->descriptor.sha256 != workflow_digest ||
        review_resource->descriptor.sha256 != release_pins::realism_review_digest) {
        return fail(QStringLiteral("Asterglen 0.2 required resource identity mismatch"));
    }

    const auto record_entries =
        record_resource->document.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record_resource->document.value(QStringLiteral("page_anchors")).toArray();
    const auto dockets = record_resource->document.value(QStringLiteral("dockets")).toArray();
    if (dockets.size() != 4 || record_entries.size() != 75 || anchors.size() != 377) {
        return fail(QStringLiteral("75-document/377-anchor record envelope mismatch"));
    }
    QHash<QString, DocketCount> docket_counts;
    QHash<QString, QJsonObject> entry_by_id;
    QHash<QString, QJsonObject> entry_by_sha;
    int searchable_pages = 0;
    for (const auto& value : record_entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("entry_id")).toString();
        const auto docket_id = entry.value(QStringLiteral("docket_id")).toString();
        const auto digest = entry.value(QStringLiteral("asset_sha256")).toString();
        const auto page_count = entry.value(QStringLiteral("page_count")).toInt();
        if (id.isEmpty() || docket_id.isEmpty() || digest.isEmpty() || page_count <= 0 ||
            entry_by_id.contains(id) || entry_by_sha.contains(digest) ||
            entry.value(QStringLiteral("sealed")).toBool(true)) {
            return fail(QStringLiteral("record entry is duplicate, sealed, or incomplete"));
        }
        entry_by_id.insert(id, entry);
        entry_by_sha.insert(digest, entry);
        auto count = docket_counts.value(docket_id);
        ++count.documents;
        count.pages += page_count;
        docket_counts.insert(docket_id, count);

        QPdfDocument pdf;
        const auto pdf_path =
            pack_root.filePath(entry.value(QStringLiteral("asset_path")).toString());
        if (pdf.load(pdf_path) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready || pdf.pageCount() != page_count) {
            return fail(QStringLiteral("record PDF cannot be loaded: %1").arg(id));
        }
        for (int page = 0; page < pdf.pageCount(); ++page) {
            if (pdf.getAllText(page).text().simplified().size() < 120) {
                return fail(QStringLiteral("record PDF page is not substantively searchable: %1/%2")
                                .arg(id)
                                .arg(page + 1));
            }
            ++searchable_pages;
        }
    }
    const QHash<QString, DocketCount> expected_docket_counts{
        {QStringLiteral("ca4r54b.docket.ndwv"), {37, 234}},
        {QStringLiteral("ca4r54b.docket.ca4-v2"), {13, 70}},
        {QStringLiteral("ca4r54b.docket.counterfactual-district"), {5, 16}},
        {QStringLiteral("ca4r54b.docket.counterfactual-appellate"), {20, 57}},
    };
    if (docket_counts != expected_docket_counts || searchable_pages != 377) {
        return fail(
            QStringLiteral("actual/counterfactual docket isolation or page totals drifted"));
    }
    for (int index = 0; index < anchors.size(); ++index) {
        const auto anchor = anchors.at(index).toObject();
        const bool joint_appendix = index < 234;
        const auto ordinal = joint_appendix ? index + 1 : index - 233;
        const auto prefix = joint_appendix ? QStringLiteral("JA") : QStringLiteral("PA");
        if (anchor.value(QStringLiteral("citation_label")).toString() !=
                QStringLiteral("%1%2").arg(prefix).arg(ordinal) ||
            !entry_by_id.contains(anchor.value(QStringLiteral("entry_id")).toString())) {
            return fail(QStringLiteral("JA1-JA234 / PA1-PA143 anchor continuity mismatch"));
        }
    }

    const auto actors = case_resource->document.value(QStringLiteral("actors")).toArray();
    const auto issues = case_resource->document.value(QStringLiteral("issues")).toArray();
    const auto plans = case_resource->document.value(QStringLiteral("disposition_plans")).toArray();
    if (actors.size() != 5 || issues.size() != 3 || plans.size() != 3 ||
        case_resource->document.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4r54b.disposition.authored-dismissal") ||
        case_resource->document.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4r54b.operation.issue-dismissal-judgment") ||
        std::ranges::any_of(actors, [](const auto& value) {
            return !value.toObject().value(QStringLiteral("synthetic")).toBool();
        })) {
        return fail(
            QStringLiteral("three-issue/three-disposition synthetic case contract mismatch"));
    }

    const auto check_bank = [&](const ValidatedResource& resource, QString expected_id,
                                QString expected_mode, QString expected_digest,
                                int expected_questions) -> std::optional<QString> {
        const auto document = resource.document;
        const auto permitted =
            strings(document.value(QStringLiteral("permitted_issue_ids")).toArray());
        const auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
        const auto bindings = bank.value(QStringLiteral("issue_topic_bindings")).toArray();
        const auto questions = bank.value(QStringLiteral("questions")).toArray();
        if (document.value(QStringLiteral("resource_id")).toString() != expected_id ||
            permitted.size() != 3 ||
            bank.value(QStringLiteral("mode")).toString() != expected_mode ||
            bank.value(QStringLiteral("grounding_digest")).toString() != expected_digest ||
            bindings.size() != 3 || questions.size() != expected_questions) {
            return QStringLiteral("argument bank envelope mismatch: %1").arg(expected_id);
        }
        QSet<QString> topic_pairs;
        for (const auto& binding_value : bindings) {
            const auto binding = binding_value.toObject();
            const auto issue_id = binding.value(QStringLiteral("issue_id")).toString();
            if (!permitted.contains(issue_id)) {
                return QStringLiteral("argument bank binds an unpermitted issue");
            }
            for (const auto& topic_value : binding.value(QStringLiteral("topic_ids")).toArray()) {
                topic_pairs.insert(issue_id + QLatin1Char('|') + topic_value.toString());
            }
        }
        QSet<QString> question_ids;
        for (const auto& question_value : questions) {
            const auto question = question_value.toObject();
            const auto id = question.value(QStringLiteral("question_id")).toString();
            const auto issue_id = question.value(QStringLiteral("issue_id")).toString();
            const auto pair =
                issue_id + QLatin1Char('|') + question.value(QStringLiteral("topic_id")).toString();
            if (id.isEmpty() || question_ids.contains(id) || !permitted.contains(issue_id) ||
                !topic_pairs.contains(pair) ||
                question.value(QStringLiteral("prompt")).toString().isEmpty() ||
                question.value(QStringLiteral("grounding")).toArray().isEmpty()) {
                return QStringLiteral("argument bank question is ungrounded or duplicated");
            }
            question_ids.insert(id);
        }
        return std::nullopt;
    };
    if (const auto error = check_bank(
            *actual_argument, QStringLiteral("ca4r54b.argument.actual-record"),
            QStringLiteral("actual_record"), QString::fromLatin1(actual_bank_digest), 15);
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            check_bank(*counterfactual_argument, QStringLiteral("ca4r54b.argument.counterfactual"),
                       QStringLiteral("counterfactual_training"),
                       QString::fromLatin1(counterfactual_bank_digest), 16);
        error.has_value()) {
        return fail(*error);
    }

    const auto stages = workflow_resource->document.value(QStringLiteral("stages")).toArray();
    const auto operations =
        workflow_resource->document.value(QStringLiteral("operations")).toArray();
    const auto routes =
        workflow_resource->document.value(QStringLiteral("filing_routes")).toArray();
    QSet<QString> workflow_operation_ids;
    QSet<QString> disposition_plan_ids;
    int filing_bindings = 0;
    int document_bindings = 0;
    int disposition_bindings = 0;
    int allowed_legal_times = 0;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto operation_id = operation.value(QStringLiteral("operation_id")).toString();
        const auto legal_times = operation.value(QStringLiteral("allowed_legal_times")).toArray();
        if (operation_id.isEmpty() || workflow_operation_ids.contains(operation_id) ||
            legal_times.isEmpty()) {
            return fail(QStringLiteral("workflow operation identity/LegalTime guard mismatch"));
        }
        workflow_operation_ids.insert(operation_id);
        allowed_legal_times += static_cast<int>(legal_times.size());
        const auto binding = operation.value(QStringLiteral("document_binding")).toObject();
        if (!binding.isEmpty()) {
            ++document_bindings;
            const auto entry =
                entry_by_id.value(binding.value(QStringLiteral("record_entry_id")).toString());
            if (entry.isEmpty() ||
                binding.value(QStringLiteral("document_sha256")).toString() !=
                    entry.value(QStringLiteral("asset_sha256")).toString() ||
                binding.value(QStringLiteral("expected_court_date")).toString() !=
                    entry.value(QStringLiteral("filed_on")).toString()) {
                return fail(QStringLiteral("operation document binding does not resolve exactly"));
            }
        }
        const auto plan_id = operation.value(QStringLiteral("disposition_plan_id")).toString();
        if (!plan_id.isEmpty()) {
            ++disposition_bindings;
            disposition_plan_ids.insert(plan_id);
        }
    }
    for (const auto& value : routes) {
        const auto route = value.toObject();
        const auto bindings = route.value(QStringLiteral("filing_bindings")).toArray();
        if (bindings.isEmpty() || route.contains(QStringLiteral("deficiency_operation_id")) ||
            route.contains(QStringLiteral("deficiency_deadline"))) {
            return fail(QStringLiteral("production filing route is open or deficiency-enabled"));
        }
        filing_bindings += static_cast<int>(bindings.size());
        for (const auto& binding_value : bindings) {
            const auto binding = binding_value.toObject();
            const auto entry =
                entry_by_id.value(binding.value(QStringLiteral("record_entry_id")).toString());
            const auto legal_time = binding.value(QStringLiteral("expected_legal_time")).toObject();
            if (entry.isEmpty() ||
                binding.value(QStringLiteral("document_sha256")).toString() !=
                    entry.value(QStringLiteral("asset_sha256")).toString() ||
                legal_time.value(QStringLiteral("court_date")).toString() !=
                    entry.value(QStringLiteral("filed_on")).toString()) {
                return fail(QStringLiteral("route filing binding does not resolve exactly"));
            }
        }
    }
    const QSet<QString> expected_disposition_plans{
        QStringLiteral("ca4r54b.disposition.authored-dismissal"),
        QStringLiteral("ca4r54b.disposition.counterfactual-supported-certification"),
        QStringLiteral("ca4r54b.disposition.counterfactual-untimely-notice"),
    };
    if (stages.size() != 13 || operations.size() != 81 || routes.size() != 11 ||
        workflow_operation_ids.size() != 81 || filing_bindings != 12 || document_bindings != 30 ||
        disposition_bindings != 3 || allowed_legal_times != 94 ||
        disposition_plan_ids != expected_disposition_plans) {
        return fail(
            QStringLiteral("13/81/11 workflow, 12/30/3 bindings, or 94 LegalTimes drifted"));
    }

    const auto review = review_resource->document;
    const auto dimensions = review.value(QStringLiteral("dimensions")).toObject();
    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    const auto embedded_traces = evidence.value(QStringLiteral("traces")).toArray();
    QHash<QString, QJsonObject> embedded_by_id;
    for (const auto& value : embedded_traces) {
        const auto trace = value.toObject();
        embedded_by_id.insert(trace.value(QStringLiteral("trace_id")).toString(), trace);
    }
    if (review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        dimensions.size() != 7 ||
        evidence.value(QStringLiteral("closure_digest")).toString() !=
            release_pins::evidence_closure_digest ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 75 ||
        embedded_traces.size() != 8 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 31) {
        return fail(QStringLiteral("level-2 independent-review evidence envelope mismatch"));
    }
    for (auto dimension = dimensions.constBegin(); dimension != dimensions.constEnd();
         ++dimension) {
        if (dimension.value().toInt() != 2) {
            return fail(QStringLiteral("realism review dimension is not level 2"));
        }
    }
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("procedural_law"), 47},     {QStringLiteral("deadlines_authority"), 29},
        {QStringLiteral("record_consistency"), 78}, {QStringLiteral("consequences"), 34},
        {QStringLiteral("oral_argument"), 17},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 113},
    };
    for (auto expected = expected_dimension_counts.constBegin();
         expected != expected_dimension_counts.constEnd(); ++expected) {
        if (dimension_evidence.value(expected.key()).toArray().size() != expected.value()) {
            return fail(QStringLiteral("realism dimension evidence count mismatch: %1")
                            .arg(expected.key()));
        }
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return fail(QStringLiteral("cannot create integration temporary directory"));
    }
    const auto catalog =
        PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    if (!catalog) {
        return fail(QStringLiteral("catalog open: %1").arg(catalog.error().message));
    }
    const auto install = [&](const QString& path, const QString& time) {
        return (*catalog)->installArchive(path, time);
    };
    const auto federal = install(foundations_root.filePath(QStringLiteral(
                                     "us-federal/foundation-us-federal-2025.12.01.awpack")),
                                 QStringLiteral("2026-08-12T00:00:00Z"));
    const auto ca4 = install(
        foundations_root.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
        QStringLiteral("2026-08-12T00:00:01Z"));
    const auto bench =
        install(foundations_root.filePath(QStringLiteral(
                    "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
                QStringLiteral("2026-08-12T00:00:02Z"));
    const auto v1_archive =
        authoring_root.filePath(QStringLiteral("us-ca4-rule54b-asterglen-0.1.0.awpack"));
    const auto v2_archive =
        authoring_root.filePath(QStringLiteral("us-ca4-rule54b-asterglen-0.2.0.awpack"));
    const auto v1_bytes = readAll(v1_archive);
    const auto v2_bytes = readAll(v2_archive);
    const auto installed_v1 = install(v1_archive, QStringLiteral("2026-08-12T00:00:03Z"));
    const auto installed_v2 = install(v2_archive, QStringLiteral("2026-08-12T00:00:04Z"));
    if (!federal || !ca4 || !bench || !installed_v1 || !installed_v2 ||
        federal->revision != expected_federal || ca4->revision != expected_ca4 ||
        bench->revision != expected_bench || installed_v1->revision != expected_v1 ||
        installed_v2->revision != expected_root ||
        static_cast<std::uint64_t>(v1_bytes.size()) != v1_archive_byte_size ||
        sha256(v1_bytes) != v1_archive_digest ||
        static_cast<std::uint64_t>(v2_bytes.size()) != release_pins::archive_byte_size ||
        sha256(v2_bytes) != release_pins::archive_digest) {
        return fail(QStringLiteral("v0.1/v0.2 co-installation or immutable archive bytes drifted"));
    }
    const auto listed = (*catalog)->list();
    if (!listed) {
        return fail(QStringLiteral("cannot list co-installed packs"));
    }
    const auto asterglen_versions = std::ranges::count_if(
        *listed, [](const auto& pack) { return pack.revision.id.value == pack_id; });
    const auto loaded_v1 = (*catalog)->load(PackId{pack_id}, "0.1.0");
    const auto loaded_v2 = (*catalog)->load(PackId{pack_id}, "0.2.0");
    const auto resolved_v2 = (*catalog)->loadResolved(expected_root);
    if (asterglen_versions != 2 || !loaded_v1 || !loaded_v2 || !resolved_v2 ||
        loaded_v1->revision != expected_v1 || loaded_v2->revision != expected_root ||
        loaded_v1->blobs.size() != std::size_t{18} || loaded_v2->blobs.size() != std::size_t{75}) {
        return fail(QStringLiteral("catalog did not preserve both exact Asterglen versions"));
    }
    const auto runtime_v1 = packs::loadRuntimePack(*loaded_v1);
    const auto runtime_v2 = packs::loadRuntimePack(*resolved_v2);
    if (!runtime_v1 || !runtime_v2 || runtime_v1->cases.size() != std::size_t{1} ||
        runtime_v2->cases.size() != std::size_t{1} ||
        runtime_v1->cases.front().record.docket_entries.size() != std::size_t{18} ||
        runtime_v1->cases.front().record.page_anchors.size() != std::size_t{47} ||
        runtime_v2->cases.front().record.docket_entries.size() != std::size_t{75} ||
        runtime_v2->cases.front().record.page_anchors.size() != std::size_t{377} ||
        runtime_v2->cases.front().argument_configurations.size() != std::size_t{2}) {
        return fail(QStringLiteral("runtime v0.1/v0.2 isolation contract mismatch"));
    }
    const auto& runtime_case = runtime_v2->cases.front();

    QSet<QString> executed_operation_ids;
    QSet<QString> rejected_operation_ids;
    int total_commands = 0;
    int total_events = 0;
    int rejection_count = 0;
    for (const auto& expected : expected_traces) {
        const auto trace_bytes = readAll(traces_root.filePath(text(expected.file_name)));
        const auto trace = QJsonDocument::fromJson(trace_bytes).object();
        const auto journal_json = trace.value(QStringLiteral("journal")).toArray();
        const auto operation_ids_json = trace.value(QStringLiteral("operation_ids")).toArray();
        const auto computed_journal_digest = journalDigest(journal_json);
        const auto computed_trace_digest = traceDigest(trace);
        if (trace.isEmpty() || sha256(trace_bytes) != bytes(expected.file_sha256) ||
            trace.value(QStringLiteral("trace_id")).toString() != text(expected.trace_id) ||
            trace.value(QStringLiteral("evidence_id")).toString() != text(expected.evidence_id) ||
            trace.value(QStringLiteral("workflow_id")).toString() !=
                QStringLiteral("ca4r54b.workflow.civil-rule54b") ||
            trace.value(QStringLiteral("engine_revision")).toString() !=
                QString::fromLatin1(realism_engine_revision) ||
            trace.value(QStringLiteral("command_count")).toInt() != expected.command_count ||
            trace.value(QStringLiteral("event_count")).toInt() != expected.event_count ||
            trace.value(QStringLiteral("journal_sha256")).toString() !=
                text(expected.journal_sha256) ||
            trace.value(QStringLiteral("digest")).toString() != text(expected.digest) ||
            trace.value(QStringLiteral("terminal_stage_id")).toString() !=
                text(expected.terminal_stage_id) ||
            computed_journal_digest != std::optional{text(expected.journal_sha256)} ||
            computed_trace_digest != text(expected.digest) ||
            embedded_by_id.value(text(expected.trace_id)) != trace) {
            return fail(QStringLiteral("canonical trace identity/digest mismatch: %1 "
                                       "file=%2 journal=%3 trace=%4 embedded=%5")
                            .arg(text(expected.file_name), QString::fromLatin1(sha256(trace_bytes)),
                                 computed_journal_digest.value_or(QStringLiteral("none")),
                                 computed_trace_digest,
                                 embedded_by_id.value(text(expected.trace_id)) == trace
                                     ? QStringLiteral("yes")
                                     : QStringLiteral("no")));
        }

        std::vector<model::WorkflowJournalEntry> journal;
        journal.reserve(static_cast<std::size_t>(journal_json.size()));
        QJsonArray decoded_operation_ids;
        std::vector<std::pair<std::size_t, model::WorkflowFilingId>> rejected_filings;
        std::size_t flat_event_index = 0;
        for (const auto& entry_value : journal_json) {
            const auto entry = entry_value.toObject();
            const auto command_bytes = QByteArray::fromBase64(
                entry.value(QStringLiteral("command_base64")).toString().toLatin1());
            const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
            if (!command || storage::encodeWorkflowCommand(*command) != command_bytes) {
                return fail(QStringLiteral("trace command codec is not canonical"));
            }
            if (const auto digest = commandDocumentDigest(*command); digest.has_value()) {
                const auto record_entry = entry_by_sha.value(QString::fromStdString(*digest));
                const auto court_date = std::visit(
                    [](const auto& concrete) {
                        const auto day = concrete.header.occurred_at.court_date.value;
                        return QStringLiteral("%1-%2-%3")
                            .arg(static_cast<int>(day.year()), 4, 10, QLatin1Char('0'))
                            .arg(static_cast<unsigned>(day.month()), 2, 10, QLatin1Char('0'))
                            .arg(static_cast<unsigned>(day.day()), 2, 10, QLatin1Char('0'));
                    },
                    *command);
                if (record_entry.isEmpty() ||
                    record_entry.value(QStringLiteral("filed_on")).toString() != court_date) {
                    return fail(QStringLiteral("trace command document/date does not resolve"));
                }
            }
            std::vector<model::WorkflowEvent> events;
            for (const auto& event_value : entry.value(QStringLiteral("events_base64")).toArray()) {
                const auto event_bytes = QByteArray::fromBase64(event_value.toString().toLatin1());
                const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
                if (!event || storage::encodeWorkflowEvent(*event) != event_bytes) {
                    return fail(QStringLiteral("trace event codec is not canonical"));
                }
                const auto operation_id =
                    QString::fromStdString(eventHeader(*event).operation_id.value);
                decoded_operation_ids.push_back(operation_id);
                executed_operation_ids.insert(operation_id);
                if (const auto* rejected = std::get_if<model::WorkflowFilingRejected>(&*event);
                    rejected != nullptr) {
                    if (rejected->reason !=
                        model::WorkflowFilingRejectionReason::NonconformingFiling) {
                        return fail(
                            QStringLiteral("production rejection is not NonconformingFiling"));
                    }
                    rejected_operation_ids.insert(operation_id);
                    rejected_filings.emplace_back(flat_event_index, rejected->filing_id);
                    ++rejection_count;
                }
                ++flat_event_index;
                events.push_back(*event);
            }
            journal.push_back(model::WorkflowJournalEntry{*command, std::move(events)});
        }
        if (decoded_operation_ids != operation_ids_json ||
            static_cast<int>(journal.size()) != expected.command_count ||
            static_cast<int>(flat_event_index) != expected.event_count) {
            return fail(QStringLiteral("trace event/operation count mismatch"));
        }
        std::vector<model::WorkflowEvent> flat_events;
        for (const auto& entry : journal) {
            flat_events.insert(flat_events.end(), entry.events.begin(), entry.events.end());
        }
        for (const auto& [index, filing_id] : rejected_filings) {
            const auto recovery = std::ranges::find_if(
                flat_events.begin() + static_cast<std::ptrdiff_t>(index + 1U), flat_events.end(),
                [&](const auto& event) {
                    const auto* accepted = std::get_if<model::WorkflowFilingAccepted>(&event);
                    return accepted != nullptr && accepted->filing_id == filing_id;
                });
            if (recovery == flat_events.end()) {
                return fail(
                    QStringLiteral("nonconforming bound filing did not recover with same ID"));
            }
        }

        const auto initial = initialState(runtime_case.workflow, journal.front().command);
        const auto first_replay = engine::replayWorkflow(runtime_case.workflow,
                                                         runtime_case.definition, initial, journal);
        const auto second_replay = engine::replayWorkflow(
            runtime_case.workflow, runtime_case.definition, initial, journal);
        if (!first_replay || !second_replay || *first_replay != *second_replay ||
            first_replay->current_stage_id.value != expected.terminal_stage_id) {
            return fail(QStringLiteral("trace did not replay deterministically: %1")
                            .arg(text(expected.file_name)));
        }
        if (expected.disposition_plan_id.has_value()) {
            if (!first_replay->judgment_disposition.has_value() ||
                std::get_if<model::DispositionPlan>(&*first_replay->judgment_disposition) ==
                    nullptr ||
                std::get<model::DispositionPlan>(*first_replay->judgment_disposition).id.value !=
                    *expected.disposition_plan_id) {
                return fail(QStringLiteral("trace reached the wrong structured disposition"));
            }
        } else if (first_replay->judgment_disposition.has_value()) {
            return fail(
                QStringLiteral("missing-separate-document trace unexpectedly issued judgment"));
        }

        auto event_tamper = journal;
        std::visit([](auto& event) { ++event.header.sequence; },
                   event_tamper.front().events.front());
        if (engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   event_tamper)) {
            return fail(QStringLiteral("trace sequence tamper replayed"));
        }
        auto legal_time_tamper = journal;
        std::visit([](auto& command) { command.header.occurred_at.instant += 1s; },
                   legal_time_tamper.front().command);
        if (engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   legal_time_tamper)) {
            return fail(QStringLiteral("trace LegalTime tamper replayed"));
        }
        auto document_tamper = journal;
        bool changed_document = false;
        for (auto& entry : document_tamper) {
            std::visit(
                [&](auto& command) {
                    if constexpr (requires { command.document_sha256; }) {
                        if (!changed_document && !command.document_sha256.empty()) {
                            command.document_sha256.front() =
                                command.document_sha256.front() == '0' ? '1' : '0';
                            changed_document = true;
                        }
                    }
                },
                entry.command);
            if (changed_document) {
                break;
            }
        }
        if (!changed_document ||
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   document_tamper)) {
            return fail(QStringLiteral("trace document-SHA tamper replayed"));
        }
        total_commands += expected.command_count;
        total_events += expected.event_count;
    }
    const QSet<QString> expected_rejection_operations{
        QStringLiteral("ca4r54b.operation.reject-initial-filing"),
        QStringLiteral("ca4r54b.operation.reject-notice"),
        QStringLiteral("ca4r54b.operation.reject-opening-brief"),
        QStringLiteral("ca4r54b.operation.reject-post-judgment-filing"),
        QStringLiteral("ca4r54b.operation.reject-reply-or-no-reply"),
        QStringLiteral("ca4r54b.operation.reject-response-brief"),
        QStringLiteral("ca4r54b.operation.reject-status-certification"),
    };
    if (total_commands != 205 || total_events != 221 || rejection_count != 7 ||
        rejected_operation_ids != expected_rejection_operations ||
        executed_operation_ids != workflow_operation_ids) {
        return fail(QStringLiteral("eight-trace 205/221/81 reject/recovery coverage mismatch"));
    }

    const auto archive_a =
        QDir(temporary.path()).filePath(QStringLiteral("asterglen-v02-a.awpack"));
    const auto archive_b =
        QDir(temporary.path()).filePath(QStringLiteral("asterglen-v02-b.awpack"));
    const auto exported_a = PackArchive::exportDirectory(pack_root.path(), archive_a, {},
                                                         PackValidationScope::ResolvedClosure);
    const auto exported_b = PackArchive::exportDirectory(pack_root.path(), archive_b, {},
                                                         PackValidationScope::ResolvedClosure);
    const auto archive_bytes = readAll(archive_a);
    if (!exported_a || !exported_b || *exported_a != expected_root ||
        *exported_b != expected_root || archive_bytes != readAll(archive_b) ||
        static_cast<std::uint64_t>(archive_bytes.size()) != release_pins::archive_byte_size ||
        sha256(archive_bytes) != release_pins::archive_digest) {
        return fail(QStringLiteral("Asterglen 0.2 archive export is not frozen/deterministic"));
    }

    std::cout << "Asterglen 0.2 integration passed: immutable 0.1 coexistence; 75 PDFs / 377 "
                 "anchors (JA1-JA234, PA1-PA143); 13/81/11 workflow with 12 route, 30 "
                 "document, 3 disposition bindings and 94 LegalTimes; two grounded banks; "
                 "8 traces / 205 commands / 221 events / 81 operations.\n";
    return 0;
}
