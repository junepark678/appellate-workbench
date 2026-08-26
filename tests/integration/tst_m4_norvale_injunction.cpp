#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/workflow_event.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPdfDocument>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#ifndef APPELLATE_M4_NORVALE_ROOT
#error "APPELLATE_M4_NORVALE_ROOT must name content/m4/norvale-injunction"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

namespace engine = appellate::engine;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

// Final immutable assembly values live in one deliberately isolated block.
namespace ReleasePins {
constexpr std::string_view trace_closure =
    "f85ff7fa0fb6952fad6cb666083165c0504dcc415c23537dac088f330230bede";
constexpr std::string_view manifest_sha256 =
    "e89b902b39d22bcc4f5b1aa407d754e665e1243d196dc4af3703816c355f46e4";
constexpr std::string_view realism_review_sha256 =
    "8870e5c8c10e956552f99c5069a2fbc6874402cc07e062bca38706ba565bc4e8";
constexpr std::string_view evidence_closure =
    "1170d682b46773d09b63b5dcfcd5b7c485c2f792881c94027b76550ef021d82c";
constexpr std::string_view root_revision =
    "a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f";
constexpr std::string_view archive_sha256 =
    "a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19";
constexpr std::uint64_t archive_byte_size = 4'744'009;
} // namespace ReleasePins

constexpr auto successor_plan_sha256 =
    "b950388515198f489b00857965c3db053421b811b7bd7675cf26b1ddf3100ea4";
constexpr auto source_closure_sha256 =
    "e85ec3ff6c0fefabec9b1884c1256af09ed431c0f1c19f0a0d7bafe8d1ba0780";
constexpr auto render_plan_sha256 =
    "24b1a198889301f3b294d2d865479f1821a9bb0ede4e5ae8965ea09b1efed773";
constexpr auto inventory_sha256 =
    "12de3cd55506d0d3d11f2140381e129013995db5f615c7ead987b1e2e8786470";
constexpr auto raw_pdf_closure_sha256 =
    "09478456855e46cd3bf4a8ba6abb44a14d38761f4be7b7dc8aa0d87cb28e47f1";
constexpr auto framed_pdf_closure_sha256 =
    "9f3ec6f5843e067562363883636b14867c1b9dd55447fd6744b291178d1ac448";
constexpr auto record_sha256 = "a25bb89f96b78bbf7b084b50c4327953ed0af602359e49460dd1e10ef48306c3";
constexpr auto workflow_sha256 = "1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a";
constexpr auto trace_plan_sha256 =
    "88c524a0c6e9db5f8dc946df76fe597d9b4e14d7f059f248dea47fe69e8d97a1";
constexpr auto actual_bank_grounding_sha256 =
    "a7b9d3f45093cd389d57fea1522b1c2ae80fac29bf5705dd5b24033694c6f4ea";
constexpr auto counterfactual_bank_grounding_sha256 =
    "5b5559db07537e94046dffc733b8f3f104f09533ff9ffede190f4e1758260ecb";

struct FrozenResource final {
    std::string_view candidate;
    std::string_view promoted;
    std::string_view sha256;
    std::string_view kind;
    std::string_view id;
};

constexpr std::array frozen_resources{
    FrozenResource{"argument-actual.candidate.json", "argument-actual.json",
                   "b99581558b2dfc53a294cf6bf357104b5eeb610fbf479aaef78e398b774200fc",
                   "argument_config", "ca4m4.norvale.argument.actual-record"},
    FrozenResource{"argument-counterfactual.candidate.json", "argument-counterfactual.json",
                   "8fb8c1348b1f03afc45e8335e2f3ff84f055f875e60a247e907014ae871040f9",
                   "argument_config", "ca4m4.norvale.argument.adverse-record"},
    FrozenResource{"authority-set.candidate.json", "authority-set.json",
                   "2f046241ffb7b802b54bd98b5079256da708f4f25ce2bfd770c333c5e84d713d",
                   "authority_set", "ca4m4.norvale.authorities.case-specific"},
    FrozenResource{"bench-configuration.candidate.json", "bench-configuration.json",
                   "89f52481e722dac8dddc310010fe43308fe03ce72357b0d0d67ac14f6ba9853f",
                   "bench_configuration", "ca4m4.norvale.bench.three-judge"},
    FrozenResource{"case.candidate.json", "case.json",
                   "2f24762e0548d6e2c62544f6bf0918ba08c472d97677271a2251ee67e0c261e1", "case",
                   "ca4m4.case.norvale-injunction"},
    FrozenResource{"procedure-profile.candidate.json", "procedure-profile.json",
                   "4bb5fee6613a9a1e300d12d9a4d30248ee8478d1076d9f712e2f768bb852bdee",
                   "procedure_profile", "ca4m4.norvale.procedure.civil-appeal"},
    FrozenResource{"record.candidate.json", "record.json", record_sha256, "record",
                   "ca4m4.norvale.record"},
};

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] std::optional<QJsonObject> parseObject(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

[[nodiscard]] QSet<QString> strings(const QJsonArray& values) {
    QSet<QString> result;
    for (const auto& value : values) {
        result.insert(value.toString());
    }
    return result;
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, QByteArrayView value) {
    addUint64(hash, static_cast<std::uint64_t>(value.size()));
    hash.addData(value);
}

void addFrame(QCryptographicHash& hash, QStringView value) {
    const auto utf8 = value.toUtf8();
    addFrame(hash, QByteArrayView(utf8));
}

[[nodiscard]] QString
canonicalQuestionBankDigest(const QJsonObject& configuration,
                            const QHash<QString, QJsonObject>& authorities_by_id,
                            const QHash<QString, QJsonObject>& entries_by_id,
                            const QHash<QString, QJsonObject>& anchors_by_id) {
    const auto bank = configuration.value(QStringLiteral("grounded_question_bank")).toObject();
    std::vector<QJsonObject> bindings;
    for (const auto& value : bank.value(QStringLiteral("issue_topic_bindings")).toArray()) {
        bindings.push_back(value.toObject());
    }
    std::ranges::sort(bindings, [](const auto& left, const auto& right) {
        return left.value(QStringLiteral("issue_id")).toString() <
               right.value(QStringLiteral("issue_id")).toString();
    });
    std::vector<QJsonObject> questions;
    for (const auto& value : bank.value(QStringLiteral("questions")).toArray()) {
        questions.push_back(value.toObject());
    }
    std::ranges::sort(questions, [](const auto& left, const auto& right) {
        return left.value(QStringLiteral("question_id")).toString() <
               right.value(QStringLiteral("question_id")).toString();
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-grounded-question-bank-v1"));
    addFrame(hash, configuration.value(QStringLiteral("case_id")).toString());
    addFrame(hash, configuration.value(QStringLiteral("resource_id")).toString());
    addFrame(hash, bank.value(QStringLiteral("mode")).toString());
    addUint64(hash, bindings.size());
    for (const auto& binding : bindings) {
        addFrame(hash, binding.value(QStringLiteral("issue_id")).toString());
        std::vector<QString> topics;
        for (const auto& value : binding.value(QStringLiteral("topic_ids")).toArray()) {
            topics.push_back(value.toString());
        }
        std::ranges::sort(topics);
        addUint64(hash, topics.size());
        for (const auto& topic : topics) {
            addFrame(hash, QStringView(topic));
        }
    }
    addUint64(hash, questions.size());
    for (const auto& question : questions) {
        addFrame(hash, question.value(QStringLiteral("question_id")).toString());
        addFrame(hash, question.value(QStringLiteral("issue_id")).toString());
        addFrame(hash, question.value(QStringLiteral("topic_id")).toString());
        addFrame(hash, question.value(QStringLiteral("prompt")).toString());
        std::vector<QJsonObject> grounding;
        for (const auto& value : question.value(QStringLiteral("grounding")).toArray()) {
            grounding.push_back(value.toObject());
        }
        std::ranges::sort(grounding, [](const auto& left, const auto& right) {
            return left.value(QStringLiteral("grounding_id")).toString() <
                   right.value(QStringLiteral("grounding_id")).toString();
        });
        addUint64(hash, grounding.size());
        for (const auto& reference : grounding) {
            const auto kind = reference.value(QStringLiteral("kind")).toString();
            addFrame(hash, reference.value(QStringLiteral("grounding_id")).toString());
            addFrame(hash, kind);
            if (kind == QStringLiteral("authority")) {
                const auto authority = authorities_by_id.value(
                    reference.value(QStringLiteral("authority_id")).toString());
                for (const auto& field :
                     {QStringLiteral("authority_id"), QStringLiteral("citation"),
                      QStringLiteral("source_version"), QStringLiteral("proposition")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash, 1U);
                for (const auto& field :
                     {QStringLiteral("authority_type"), QStringLiteral("jurisdiction_id"),
                      QStringLiteral("issuing_body_id"), QStringLiteral("precedential_status")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash,
                          authority.value(QStringLiteral("official_source")).toBool() ? 1U : 0U);
                for (const auto& field : {QStringLiteral("checked_on"), QStringLiteral("locator"),
                                          QStringLiteral("source_url")}) {
                    addFrame(hash, authority.value(field).toString());
                }
            } else if (kind == QStringLiteral("brief_page")) {
                const auto entry_id = reference.value(QStringLiteral("entry_id")).toString();
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<std::uint64_t>(
                                    reference.value(QStringLiteral("page_number")).toInt()));
                addFrame(
                    hash,
                    entries_by_id.value(entry_id).value(QStringLiteral("asset_sha256")).toString());
            } else {
                const auto anchor_id = reference.value(QStringLiteral("anchor_id")).toString();
                const auto anchor = anchors_by_id.value(anchor_id);
                const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
                addFrame(hash, anchor_id);
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<std::uint64_t>(
                                    anchor.value(QStringLiteral("page_number")).toInt()));
                addFrame(
                    hash,
                    entries_by_id.value(entry_id).value(QStringLiteral("asset_sha256")).toString());
                const bool has_citation = anchor.contains(QStringLiteral("citation_label"));
                addUint64(hash, has_citation ? 1U : 0U);
                if (has_citation) {
                    addFrame(hash, anchor.value(QStringLiteral("citation_label")).toString());
                }
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] std::optional<QString> journalDigest(const QJsonArray& journal) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, static_cast<std::uint64_t>(journal.size()));
    for (const auto& value : journal) {
        const auto entry = value.toObject();
        const auto command_encoded =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(command_encoded);
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        if (entry.isEmpty() || command.isEmpty() || command.toBase64() != command_encoded) {
            return std::nullopt;
        }
        addFrame(hash, QByteArrayView(command));
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event : events) {
            const auto encoded = event.toString().toLatin1();
            const auto decoded = QByteArray::fromBase64(encoded);
            if (decoded.isEmpty() || decoded.toBase64() != encoded) {
                return std::nullopt;
            }
            addFrame(hash, QByteArrayView(decoded));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, QStringLiteral("ca4m4.case.norvale-injunction"));
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInt()));
    addUint64(hash, static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInt()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operation_ids.size()));
    for (const auto& value : operation_ids) {
        addFrame(hash, value.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const model::WorkflowCommandHeader&
commandHeader(const model::WorkflowCommand& command) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, command);
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, event);
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

[[nodiscard]] QString courtDate(const model::LegalDate& date) {
    return QStringLiteral("%1-%2-%3")
        .arg(static_cast<int>(date.value.year()), 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] bool validLegalTime(const QJsonObject& value) {
    const auto date =
        QDate::fromString(value.value(QStringLiteral("court_date")).toString(), Qt::ISODate);
    bool ok = false;
    const auto encoded =
        value.value(QStringLiteral("instant_unix_seconds")).toString().toLongLong(&ok);
    return date.isValid() && ok &&
           QDateTime(date, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch() == encoded;
}

[[nodiscard]] std::optional<QString> auditFrozenAuthoring(const QDir& root) {
    const auto plan_bytes =
        readAll(root.filePath(QStringLiteral("metadata/successor-document-plan.json")));
    const auto render_plan_bytes =
        readAll(root.filePath(QStringLiteral("metadata/render-plan-successor.json")));
    const auto inventory_bytes =
        readAll(root.filePath(QStringLiteral("metadata/render-inventory-successor.json")));
    const auto record_bytes =
        readAll(root.filePath(QStringLiteral("resources/record.candidate.json")));
    if (sha256(plan_bytes) != successor_plan_sha256 ||
        sha256(render_plan_bytes) != render_plan_sha256 ||
        sha256(inventory_bytes) != inventory_sha256 || sha256(record_bytes) != record_sha256) {
        return QStringLiteral("frozen plan, render, inventory, or record bytes drifted");
    }
    const auto plan_document = parseObject(plan_bytes);
    const auto inventory_document = parseObject(inventory_bytes);
    const auto record_document = parseObject(record_bytes);
    if (!plan_document || !inventory_document || !record_document) {
        return QStringLiteral("frozen plan, inventory, or record is not a JSON object");
    }
    const auto plan = *plan_document;
    const auto inventory = *inventory_document;
    const auto record = *record_document;
    const auto documents = plan.value(QStringLiteral("documents")).toArray();
    const auto inventory_entries = inventory.value(QStringLiteral("entries")).toArray();
    const auto record_entries = record.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    if (plan.value(QStringLiteral("schema_version")).toInt() != 1 ||
        plan.value(QStringLiteral("source_status")).toString() !=
            QStringLiteral("source_review_clear") ||
        plan.value(QStringLiteral("render_status")).toString() !=
            QStringLiteral("rendered_accepted") ||
        plan.value(QStringLiteral("capability_plan")).toArray().size() != 16 ||
        documents.size() != 73 || inventory_entries.size() != 73 || record_entries.size() != 73 ||
        anchors.size() != 383 ||
        inventory.value(QStringLiteral("plan_sha256")).toString() != render_plan_sha256) {
        return QStringLiteral("73-document/383-page accepted authoring envelope drifted");
    }

    QHash<QString, QJsonObject> plan_by_output;
    QHash<QString, QJsonObject> plan_by_code;
    QCryptographicHash source_closure(QCryptographicHash::Sha256);
    int planned_pages = 0;
    int lower_documents = 0;
    int lower_pages = 0;
    int actual_documents = 0;
    int actual_pages = 0;
    int branch_documents = 0;
    int branch_pages = 0;
    for (const auto& value : documents) {
        const auto document = value.toObject();
        const auto code = document.value(QStringLiteral("code")).toString();
        const auto source_path = document.value(QStringLiteral("source_path")).toString();
        const auto output_path = document.value(QStringLiteral("output_path")).toString();
        const auto source = readAll(root.filePath(source_path));
        const int page_count = document.value(QStringLiteral("page_count")).toInt();
        if (code.isEmpty() || source.isEmpty() || output_path.isEmpty() || page_count <= 0 ||
            plan_by_code.contains(code) || plan_by_output.contains(output_path) ||
            document.value(QStringLiteral("source_status")).toString() !=
                QStringLiteral("source_review_clear") ||
            source.count(QByteArrayLiteral("<!-- PAGE BREAK -->")) + 1 != page_count) {
            return QStringLiteral(
                       "source plan row is incomplete, duplicate, or page-mismatched: %1")
                .arg(code);
        }
        const auto path_utf8 = source_path.toUtf8();
        source_closure.addData(QByteArrayView(path_utf8));
        source_closure.addData(QByteArrayView("\0", 1));
        source_closure.addData(QByteArrayView(source));
        source_closure.addData(QByteArrayView("\0", 1));
        plan_by_code.insert(code, document);
        plan_by_output.insert(output_path, document);
        planned_pages += page_count;
        const auto classification = document.value(QStringLiteral("classification")).toString();
        if (classification == QStringLiteral("lower_record")) {
            ++lower_documents;
            lower_pages += page_count;
        } else if (classification == QStringLiteral("actual_appellate_docket")) {
            ++actual_documents;
            actual_pages += page_count;
        } else if (classification == QStringLiteral("counterfactual_appellate_branch")) {
            ++branch_documents;
            branch_pages += page_count;
        } else {
            return QStringLiteral("unknown record classification: %1").arg(classification);
        }
    }
    if (source_closure.result().toHex() != source_closure_sha256 || planned_pages != 383 ||
        lower_documents != 24 || lower_pages != 149 || actual_documents != 23 ||
        actual_pages != 135 || branch_documents != 26 || branch_pages != 99) {
        return QStringLiteral("source closure or 24/149 + 23/135 + 26/99 allocation drifted");
    }

    QHash<QString, QJsonObject> inventory_by_output;
    QSet<QString> pdf_hashes;
    QCryptographicHash raw_pdf_closure(QCryptographicHash::Sha256);
    QCryptographicHash framed_pdf_closure(QCryptographicHash::Sha256);
    QString previous_output;
    qint64 total_bytes = 0;
    int rendered_pages = 0;
    for (const auto& value : inventory_entries) {
        const auto entry = value.toObject();
        const auto output_path = entry.value(QStringLiteral("output_path")).toString();
        const auto digest = entry.value(QStringLiteral("pdf_sha256")).toString();
        const auto page_count = entry.value(QStringLiteral("page_count")).toInt();
        if (!plan_by_output.contains(output_path) || inventory_by_output.contains(output_path) ||
            pdf_hashes.contains(digest) ||
            (!previous_output.isEmpty() && output_path < previous_output)) {
            return QStringLiteral("render inventory order, path, or uniqueness drifted: %1")
                .arg(output_path);
        }
        const auto plan_row = plan_by_output.value(output_path);
        const auto source_path = plan_row.value(QStringLiteral("source_path")).toString();
        const auto source_bytes = readAll(root.filePath(source_path));
        const auto pdf_path = root.filePath(QStringLiteral("pack-candidate/%1").arg(output_path));
        const auto pdf_bytes = readAll(pdf_path);
        const auto labels = entry.value(QStringLiteral("page_labels")).toObject();
        if (pdf_bytes.isEmpty() || sha256(pdf_bytes) != digest.toLatin1() ||
            pdf_bytes.size() != entry.value(QStringLiteral("byte_size")).toInteger() ||
            page_count != plan_row.value(QStringLiteral("page_count")).toInt() ||
            entry.value(QStringLiteral("source_sha256")).toString() !=
                QString::fromLatin1(sha256(source_bytes)) ||
            labels.value(QStringLiteral("prefix")).toString() !=
                plan_row.value(QStringLiteral("page_label_prefix")).toString() ||
            labels.value(QStringLiteral("first_number")).toInt() !=
                plan_row.value(QStringLiteral("page_label_start")).toInt() ||
            labels.value(QStringLiteral("last_number")).toInt() !=
                plan_row.value(QStringLiteral("page_label_end")).toInt()) {
            return QStringLiteral("accepted render identity drifted: %1").arg(output_path);
        }
        QPdfDocument pdf;
        if (pdf.load(pdf_path) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready || pdf.pageCount() != page_count) {
            return QStringLiteral("accepted PDF cannot be loaded: %1").arg(output_path);
        }
        const auto prefix = labels.value(QStringLiteral("prefix")).toString();
        const int first_number = labels.value(QStringLiteral("first_number")).toInt();
        for (int page = 0; page < page_count; ++page) {
            const auto text = pdf.getAllText(page).text().simplified();
            const auto expected_label = QStringLiteral("%1%2").arg(prefix).arg(first_number + page);
            if (text.size() < 120 || !text.endsWith(expected_label)) {
                return QStringLiteral("PDF page is not searchable or has the wrong footer: %1/%2")
                    .arg(output_path)
                    .arg(page + 1);
            }
        }
        raw_pdf_closure.addData(QByteArrayView(pdf_bytes));
        const auto path_utf8 = output_path.toUtf8();
        framed_pdf_closure.addData(QByteArrayView(path_utf8));
        framed_pdf_closure.addData(QByteArrayView("\0", 1));
        framed_pdf_closure.addData(QByteArrayView(pdf_bytes));
        framed_pdf_closure.addData(QByteArrayView("\0", 1));
        inventory_by_output.insert(output_path, entry);
        pdf_hashes.insert(digest);
        previous_output = output_path;
        rendered_pages += page_count;
        total_bytes += pdf_bytes.size();
    }
    if (rendered_pages != 383 || total_bytes != 2'639'080 ||
        raw_pdf_closure.result().toHex() != raw_pdf_closure_sha256 ||
        framed_pdf_closure.result().toHex() != framed_pdf_closure_sha256) {
        return QStringLiteral("accepted PDF count, byte total, or aggregate closure drifted");
    }

    const auto case_bytes = readAll(root.filePath(QStringLiteral("resources/case.candidate.json")));
    const auto case_document = parseObject(case_bytes);
    if (!case_document) {
        return QStringLiteral("case candidate is not a JSON object");
    }
    QHash<QString, QString> actor_names;
    for (const auto& value : case_document->value(QStringLiteral("actors")).toArray()) {
        const auto actor = value.toObject();
        if (!actor.value(QStringLiteral("synthetic")).toBool()) {
            return QStringLiteral("Norvale actor is not marked synthetic");
        }
        actor_names.insert(actor.value(QStringLiteral("actor_id")).toString(),
                           actor.value(QStringLiteral("display_name")).toString());
    }
    if (actor_names.size() != 5 ||
        case_document->value(QStringLiteral("issues")).toArray().size() != 4 ||
        case_document->value(QStringLiteral("disposition_plans")).toArray().size() != 2 ||
        case_document->value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.norvale.disposition.authored-limited-remand") ||
        case_document->value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.norvale.operation.issue-actual-judgment")) {
        return QStringLiteral("Norvale five-actor/four-issue/two-disposition case shape drifted");
    }

    QHash<QString, QJsonObject> entries_by_id;
    QHash<QString, QJsonObject> entries_by_output;
    QHash<QString, int> docket_documents;
    QHash<QString, int> docket_pages;
    for (int index = 0; index < record_entries.size(); ++index) {
        const auto entry = record_entries.at(index).toObject();
        const auto plan_row = documents.at(index).toObject();
        const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
        const auto output_path = entry.value(QStringLiteral("asset_path")).toString();
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        QStringList expected_actor_names;
        for (const auto& actor_value : plan_row.value(QStringLiteral("actor_ids")).toArray()) {
            expected_actor_names.push_back(actor_names.value(actor_value.toString()));
        }
        const auto expected_actor = expected_actor_names.join(QStringLiteral(" and "));
        if (entries_by_id.contains(entry_id) || entries_by_output.contains(output_path) ||
            entry_id != QStringLiteral("ca4m4.norvale.record.entry.%1")
                            .arg(plan_row.value(QStringLiteral("code")).toString()) ||
            entry.value(QStringLiteral("entry_number")).toInt() != index + 1 ||
            entry.value(QStringLiteral("docket_id")).toString() !=
                plan_row.value(QStringLiteral("docket_id")).toString() ||
            entry.value(QStringLiteral("filed_on")).toString() !=
                plan_row.value(QStringLiteral("filed_on")).toString() ||
            entry.value(QStringLiteral("title")).toString() !=
                plan_row.value(QStringLiteral("title")).toString() ||
            entry.value(QStringLiteral("actor")).toString() != expected_actor ||
            output_path != plan_row.value(QStringLiteral("output_path")).toString() ||
            entry.value(QStringLiteral("page_count")).toInt() !=
                plan_row.value(QStringLiteral("page_count")).toInt() ||
            entry.value(QStringLiteral("asset_sha256")).toString() !=
                inventory_by_output.value(output_path)
                    .value(QStringLiteral("pdf_sha256"))
                    .toString() ||
            !tags.contains(plan_row.value(QStringLiteral("classification")).toString()) ||
            !tags.contains(plan_row.value(QStringLiteral("record_category")).toString()) ||
            entry.value(QStringLiteral("sealed")).toBool(true) ||
            entry.contains(QStringLiteral("parent_entry_id")) ||
            entry.contains(QStringLiteral("relationship"))) {
            return QStringLiteral("record/plan identity, actor, date, title, or tag drifted: %1")
                .arg(entry_id);
        }
        const bool branch = index >= 47;
        if (branch != tags.contains(QStringLiteral("never_filed")) ||
            branch != tags.contains(QStringLiteral("never_occurred_on_actual_docket"))) {
            return QStringLiteral("actual/counterfactual isolation tags drifted: %1").arg(entry_id);
        }
        entries_by_id.insert(entry_id, entry);
        entries_by_output.insert(output_path, entry);
        const auto docket_id = entry.value(QStringLiteral("docket_id")).toString();
        ++docket_documents[docket_id];
        docket_pages[docket_id] += entry.value(QStringLiteral("page_count")).toInt();
    }
    const auto district_id = QStringLiteral("ca4m4.norvale.docket.district");
    const auto actual_id = QStringLiteral("ca4m4.norvale.docket.appellate");
    const auto branch_id = QStringLiteral("ca4m4.norvale.docket.counterfactual-branches");
    if (record.value(QStringLiteral("dockets")).toArray().size() != 3 ||
        docket_documents.value(district_id) != 24 || docket_pages.value(district_id) != 149 ||
        docket_documents.value(actual_id) != 23 || docket_pages.value(actual_id) != 135 ||
        docket_documents.value(branch_id) != 26 || docket_pages.value(branch_id) != 99) {
        return QStringLiteral("three-docket 24/149 + 23/135 + 26/99 closure drifted");
    }

    QHash<QString, QJsonObject> anchors_by_id;
    QHash<QString, int> anchors_per_entry;
    for (int index = 0; index < anchors.size(); ++index) {
        const auto anchor = anchors.at(index).toObject();
        const bool joint_appendix = index < 149;
        const int ordinal = joint_appendix ? index + 1 : index - 148;
        const auto prefix = joint_appendix ? QStringLiteral("JA") : QStringLiteral("PA");
        const auto expected_label = QStringLiteral("%1%2").arg(prefix).arg(ordinal);
        const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
        const auto anchor_id = anchor.value(QStringLiteral("anchor_id")).toString();
        const auto entry = entries_by_id.value(entry_id);
        if (anchor_id != QStringLiteral("ca4m4.norvale.anchor.%1").arg(expected_label.toLower()) ||
            anchor.value(QStringLiteral("citation_label")).toString() != expected_label ||
            entry.isEmpty() || anchor.value(QStringLiteral("page_number")).toInt() <= 0 ||
            anchor.value(QStringLiteral("page_number")).toInt() >
                entry.value(QStringLiteral("page_count")).toInt() ||
            anchors_by_id.contains(anchor_id)) {
            return QStringLiteral("JA1-JA149 / PA1-PA234 anchor continuity drifted");
        }
        anchors_by_id.insert(anchor_id, anchor);
        ++anchors_per_entry[entry_id];
    }
    for (auto it = entries_by_id.cbegin(); it != entries_by_id.cend(); ++it) {
        if (anchors_per_entry.value(it.key()) !=
            it.value().value(QStringLiteral("page_count")).toInt()) {
            return QStringLiteral("record entry does not own exactly one anchor per page: %1")
                .arg(it.key());
        }
    }

    QHash<QString, QJsonObject> core_documents;
    for (const auto& expected : frozen_resources) {
        const auto candidate_path = root.filePath(
            QStringLiteral("resources/%1").arg(QString::fromLatin1(expected.candidate)));
        const auto promoted_path = root.filePath(QStringLiteral("pack-candidate/resources/%1")
                                                     .arg(QString::fromLatin1(expected.promoted)));
        const auto candidate = readAll(candidate_path);
        const auto promoted = readAll(promoted_path);
        const auto parsed = parseObject(candidate);
        if (candidate.isEmpty() || candidate != promoted || sha256(candidate) != expected.sha256 ||
            !parsed || parsed->value(QStringLiteral("schema_version")).toInt() != 2 ||
            parsed->value(QStringLiteral("resource_kind")).toString() != expected.kind ||
            parsed->value(QStringLiteral("resource_id")).toString() != expected.id) {
            return QStringLiteral("frozen candidate/promoted core resource drifted: %1")
                .arg(QString::fromLatin1(expected.candidate));
        }
        core_documents.insert(QString::fromLatin1(expected.id), *parsed);
    }
    const auto authority_set =
        core_documents.value(QStringLiteral("ca4m4.norvale.authorities.case-specific"));
    const auto bench = core_documents.value(QStringLiteral("ca4m4.norvale.bench.three-judge"));
    if (authority_set.value(QStringLiteral("authorities")).toArray().size() != 8 ||
        bench.value(QStringLiteral("seats")).toArray().size() != 3 ||
        bench.value(QStringLiteral("presiding_seat_id")).toString() !=
            QStringLiteral("ca4m4.norvale.seat.quill")) {
        return QStringLiteral("eight-authority/three-seat core resource shape drifted");
    }
    QHash<QString, QJsonObject> authorities_by_id;
    for (const auto& value : authority_set.value(QStringLiteral("authorities")).toArray()) {
        const auto authority = value.toObject();
        authorities_by_id.insert(authority.value(QStringLiteral("authority_id")).toString(),
                                 authority);
    }
    const std::array banks{
        std::pair{QStringLiteral("ca4m4.norvale.argument.actual-record"),
                  QString::fromLatin1(actual_bank_grounding_sha256)},
        std::pair{QStringLiteral("ca4m4.norvale.argument.adverse-record"),
                  QString::fromLatin1(counterfactual_bank_grounding_sha256)},
    };
    for (const auto& [id, expected_digest] : banks) {
        const auto configuration = core_documents.value(id);
        const auto bank = configuration.value(QStringLiteral("grounded_question_bank")).toObject();
        int authority_grounding = 0;
        int record_grounding = 0;
        QSet<QString> question_ids;
        for (const auto& value : bank.value(QStringLiteral("questions")).toArray()) {
            const auto question = value.toObject();
            question_ids.insert(question.value(QStringLiteral("question_id")).toString());
            for (const auto& grounding_value :
                 question.value(QStringLiteral("grounding")).toArray()) {
                const auto grounding = grounding_value.toObject();
                if (grounding.value(QStringLiteral("kind")).toString() ==
                    QStringLiteral("authority")) {
                    ++authority_grounding;
                } else if (grounding.value(QStringLiteral("kind")).toString() ==
                           QStringLiteral("record_page")) {
                    ++record_grounding;
                }
            }
        }
        if (configuration.value(QStringLiteral("permitted_issue_ids")).toArray().size() != 4 ||
            bank.value(QStringLiteral("issue_topic_bindings")).toArray().size() != 4 ||
            bank.value(QStringLiteral("questions")).toArray().size() != 16 ||
            question_ids.size() != 16 || authority_grounding != 16 || record_grounding != 34 ||
            bank.value(QStringLiteral("grounding_digest")).toString() != expected_digest ||
            canonicalQuestionBankDigest(configuration, authorities_by_id, entries_by_id,
                                        anchors_by_id) != expected_digest) {
            return QStringLiteral("grounded-question bank shape or canonical digest drifted: %1")
                .arg(id);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<QString> auditWorkflowCandidate(const QDir& root) {
    const auto bytes = readAll(root.filePath(QStringLiteral("resources/workflow.candidate.json")));
    const auto parsed = parseObject(bytes);
    if (sha256(bytes) != workflow_sha256 || !parsed) {
        return QStringLiteral("frozen workflow candidate is absent, drifted, or invalid JSON");
    }
    const auto workflow = *parsed;
    const auto stages = workflow.value(QStringLiteral("stages")).toArray();
    const auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (workflow.value(QStringLiteral("schema_version")).toInt() != 2 ||
        workflow.value(QStringLiteral("resource_kind")).toString() != QStringLiteral("workflow") ||
        workflow.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.norvale.workflow.civil-injunction") ||
        stages.size() != 16 || operations.size() != 90 || routes.size() != 13 ||
        workflow.value(QStringLiteral("initial_stage_id")).toString() !=
            QStringLiteral("ca4m4.norvale.stage.district-stay")) {
        return QStringLiteral("workflow 16-stage/90-operation/13-route envelope drifted");
    }
    QSet<QString> stage_ids;
    for (const auto& value : stages) {
        stage_ids.insert(value.toString());
    }
    if (stage_ids.size() != 16 ||
        !stage_ids.contains(QStringLiteral("ca4m4.norvale.stage.terminated"))) {
        return QStringLiteral("workflow stage identity is duplicate or incomplete");
    }
    QHash<QString, QJsonObject> operations_by_id;
    QSet<QString> deadline_ids;
    QSet<QString> deadline_concepts;
    int deadline_operations = 0;
    int document_bindings = 0;
    int disposition_bindings = 0;
    const QHash<QString, std::pair<QString, QString>> expected_deadlines{
        {QStringLiteral("ca4m4.norvale.operation.calculate-notice-of-appeal-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.notice-of-appeal"),
          QStringLiteral("notice-of-appeal")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-transcript-certificate-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.transcript-certificate"),
          QStringLiteral("transcript-certificate")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-docketing-statement-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.docketing-statement"),
          QStringLiteral("docketing-statement")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-appearance-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.appearance"), QStringLiteral("appearance")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-opening-brief-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.opening-brief"), QStringLiteral("opening-brief")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-response-brief-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.response-brief"),
          QStringLiteral("response-brief")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-reply-brief-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.reply-brief"), QStringLiteral("reply-brief")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-adverse-rehearing-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.rehearing-adverse"), QStringLiteral("rehearing")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-actual-rehearing-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.rehearing-actual"), QStringLiteral("rehearing")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-adverse-ordinary-mandate-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.mandate-ordinary-adverse"),
          QStringLiteral("ordinary-mandate")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-actual-ordinary-mandate-deadline"),
         {QStringLiteral("ca4m4.norvale.deadline.mandate-ordinary-actual"),
          QStringLiteral("ordinary-mandate")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-mandate-after-rehearing-denial"),
         {QStringLiteral("ca4m4.norvale.deadline.mandate-after-rehearing-denial"),
          QStringLiteral("mandate-after-rehearing-denial")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-mandate-after-stay-denial"),
         {QStringLiteral("ca4m4.norvale.deadline.mandate-after-stay-denial"),
          QStringLiteral("mandate-after-stay-denial")}},
        {QStringLiteral("ca4m4.norvale.operation.calculate-stay-expiration"),
         {QStringLiteral("ca4m4.norvale.deadline.stay-expiration"),
          QStringLiteral("stay-expiration")}},
    };
    QSet<QString> concrete_deadline_ids;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        const auto stage_id = operation.value(QStringLiteral("stage_id")).toString();
        const auto opcode = operation.value(QStringLiteral("opcode")).toString();
        if (id.isEmpty() || operations_by_id.contains(id) || !stage_ids.contains(stage_id) ||
            operation.value(QStringLiteral("authority"))
                .toObject()
                .value(QStringLiteral("primary_authority_id"))
                .toString()
                .isEmpty() ||
            operation.value(QStringLiteral("allowed_legal_times")).toArray().isEmpty()) {
            return QStringLiteral(
                       "workflow operation identity, stage, authority, or time drifted: %1")
                .arg(id);
        }
        for (const auto& time : operation.value(QStringLiteral("allowed_legal_times")).toArray()) {
            if (!validLegalTime(time.toObject())) {
                return QStringLiteral("workflow legal-time UTC encoding drifted: %1").arg(id);
            }
        }
        if (operation.contains(QStringLiteral("next_stage_id")) &&
            !stage_ids.contains(operation.value(QStringLiteral("next_stage_id")).toString())) {
            return QStringLiteral("workflow transition targets an unknown stage: %1").arg(id);
        }
        if (opcode == QStringLiteral("calculate_deadline")) {
            ++deadline_operations;
            const auto expected = expected_deadlines.constFind(id);
            if (expected == expected_deadlines.constEnd()) {
                return QStringLiteral("unexpected CalculateDeadline operation: %1").arg(id);
            }
            concrete_deadline_ids.insert(expected->first);
            deadline_concepts.insert(expected->second);
            const auto deadline_id =
                operation.value(QStringLiteral("produced_deadline_id")).toString();
            if (!deadline_id.isEmpty()) {
                if (deadline_ids.contains(deadline_id) || deadline_id != expected->first) {
                    return QStringLiteral("workflow produces a duplicate named deadline");
                }
                deadline_ids.insert(deadline_id);
            }
        }
        if (operation.contains(QStringLiteral("document_binding"))) {
            ++document_bindings;
        }
        if (operation.contains(QStringLiteral("disposition_plan_id"))) {
            ++disposition_bindings;
        }
        operations_by_id.insert(id, operation);
    }
    if (operations_by_id.size() != 90 || deadline_operations != 14 ||
        expected_deadlines.size() != 14 || concrete_deadline_ids.size() != 14 ||
        deadline_concepts.size() != 12 || deadline_ids.size() != 11 || document_bindings != 31 ||
        disposition_bindings != 2) {
        return QStringLiteral(
            "workflow 90-operation/14-deadline/12-concept/31-document/2-disposition counts "
            "drifted");
    }

    const auto operation_by_suffix = [&](QStringView suffix) {
        return operations_by_id.value(QStringLiteral("ca4m4.norvale.operation.%1").arg(suffix));
    };
    const auto has_record_precondition = [](const QJsonObject& candidate, QStringView entry_id) {
        return std::ranges::any_of(
            candidate.value(QStringLiteral("preconditions")).toArray(), [&](const auto& value) {
                return value.toObject().value(QStringLiteral("record_entry_id")).toString() ==
                       entry_id;
            });
    };
    const auto abstracted_counterfactual =
        operation_by_suffix(u"advance-counterfactual-rule8-scenario-to-appellate-stay");
    if (abstracted_counterfactual.value(QStringLiteral("stage_id")).toString() !=
            QStringLiteral("ca4m4.norvale.stage.district-stay") ||
        abstracted_counterfactual.value(QStringLiteral("next_stage_id")).toString() !=
            QStringLiteral("ca4m4.norvale.stage.appellate-stay") ||
        abstracted_counterfactual.contains(QStringLiteral("document_binding")) ||
        !has_record_precondition(abstracted_counterfactual, u"ca4m4.norvale.record.entry.l17") ||
        !has_record_precondition(abstracted_counterfactual, u"ca4m4.norvale.record.entry.l18")) {
        return QStringLiteral("abstracted counterfactual notice/docket transition drifted");
    }
    int appellate_stay_departures = 0;
    for (const auto& value : operations) {
        const auto candidate = value.toObject();
        if (candidate.value(QStringLiteral("stage_id")).toString() ==
                QStringLiteral("ca4m4.norvale.stage.appellate-stay") &&
            candidate.contains(QStringLiteral("next_stage_id"))) {
            ++appellate_stay_departures;
            if (candidate.value(QStringLiteral("operation_id")).toString() !=
                    QStringLiteral(
                        "ca4m4.norvale.operation.advance-actual-stay-denial-to-record") ||
                candidate.value(QStringLiteral("next_stage_id")).toString() !=
                    QStringLiteral("ca4m4.norvale.stage.record") ||
                !has_record_precondition(candidate, u"ca4m4.norvale.record.entry.a07")) {
                return QStringLiteral("a counterfactual stay outcome can enter the actual record");
            }
        }
    }
    if (appellate_stay_departures != 1) {
        return QStringLiteral("A07 is not the sole appellate-stay continuation lineage");
    }
    const std::array scoped_counterfactual_endpoints{
        std::pair{QStringLiteral("enter-rule8-threshold-denial"),
                  QStringLiteral("ca4m4.norvale.record.entry.b02")},
        std::pair{QStringLiteral("enter-impracticability-stay-denial"),
                  QStringLiteral("ca4m4.norvale.record.entry.b05")},
        std::pair{QStringLiteral("enter-appellate-stay-dissolution"),
                  QStringLiteral("ca4m4.norvale.record.entry.b07")},
    };
    for (const auto& [suffix, entry_id] : scoped_counterfactual_endpoints) {
        const auto endpoint = operation_by_suffix(suffix);
        if (endpoint.value(QStringLiteral("stage_id")).toString() !=
                QStringLiteral("ca4m4.norvale.stage.appellate-stay") ||
            endpoint.contains(QStringLiteral("next_stage_id")) ||
            endpoint.value(QStringLiteral("document_binding"))
                    .toObject()
                    .value(QStringLiteral("record_entry_id"))
                    .toString() != entry_id) {
            return QStringLiteral("counterfactual stay endpoint escaped its scoped stage: %1")
                .arg(suffix);
        }
    }
    if (operation_by_suffix(u"enter-record-complete-briefing-order")
                .value(QStringLiteral("document_binding"))
                .toObject()
                .value(QStringLiteral("record_entry_id"))
                .toString() != QStringLiteral("ca4m4.norvale.record.entry.a10") ||
        operation_by_suffix(u"enter-actual-opinion")
                .value(QStringLiteral("document_binding"))
                .toObject()
                .value(QStringLiteral("record_entry_id"))
                .toString() != QStringLiteral("ca4m4.norvale.record.entry.a20")) {
        return QStringLiteral("actual A08/A10/A20 continuation bindings drifted");
    }

    const auto record =
        parseObject(readAll(root.filePath(QStringLiteral("resources/record.candidate.json"))));
    const auto case_document =
        parseObject(readAll(root.filePath(QStringLiteral("resources/case.candidate.json"))));
    if (!record || !case_document) {
        return QStringLiteral("workflow cross-reference inputs cannot be parsed");
    }
    QHash<QString, QJsonObject> entries;
    for (const auto& value : record->value(QStringLiteral("docket_entries")).toArray()) {
        const auto entry = value.toObject();
        entries.insert(entry.value(QStringLiteral("entry_id")).toString(), entry);
    }
    QSet<QString> actors;
    for (const auto& value : case_document->value(QStringLiteral("actors")).toArray()) {
        actors.insert(value.toObject().value(QStringLiteral("actor_id")).toString());
    }
    const auto validate_binding = [&](const QJsonObject& binding) {
        const auto entry =
            entries.value(binding.value(QStringLiteral("record_entry_id")).toString());
        return !entry.isEmpty() &&
               entry.value(QStringLiteral("asset_sha256")).toString() ==
                   binding.value(QStringLiteral("document_sha256")).toString() &&
               (!binding.contains(QStringLiteral("expected_court_date")) ||
                entry.value(QStringLiteral("filed_on")).toString() ==
                    binding.value(QStringLiteral("expected_court_date")).toString());
    };
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        if (operation.contains(QStringLiteral("document_binding")) &&
            !validate_binding(operation.value(QStringLiteral("document_binding")).toObject())) {
            return QStringLiteral("operation document binding does not match frozen record: %1")
                .arg(operation.value(QStringLiteral("operation_id")).toString());
        }
    }

    QSet<QString> route_keys;
    QSet<QString> filing_ids;
    int filing_bindings = 0;
    for (const auto& value : routes) {
        const auto route = value.toObject();
        const auto stage_id = route.value(QStringLiteral("stage_id")).toString();
        const auto route_key =
            stage_id + QLatin1Char('|') + route.value(QStringLiteral("filing_type_id")).toString();
        const auto accept_id = route.value(QStringLiteral("accept_operation_id")).toString();
        const auto reject_id = route.value(QStringLiteral("reject_operation_id")).toString();
        const auto accept = operations_by_id.value(accept_id);
        const auto reject = operations_by_id.value(reject_id);
        if (!stage_ids.contains(stage_id) || route_keys.contains(route_key) || accept.isEmpty() ||
            reject.isEmpty() || accept.value(QStringLiteral("stage_id")).toString() != stage_id ||
            reject.value(QStringLiteral("stage_id")).toString() != stage_id ||
            accept.value(QStringLiteral("opcode")).toString() != QStringLiteral("accept_filing") ||
            reject.value(QStringLiteral("opcode")).toString() != QStringLiteral("reject_filing")) {
            return QStringLiteral("filing route topology drifted: %1").arg(route_key);
        }
        route_keys.insert(route_key);
        for (const auto& binding_value : route.value(QStringLiteral("filing_bindings")).toArray()) {
            const auto binding = binding_value.toObject();
            const auto filing_id = binding.value(QStringLiteral("filing_id")).toString();
            if (filing_id.isEmpty() || filing_ids.contains(filing_id) ||
                !actors.contains(binding.value(QStringLiteral("actor_id")).toString()) ||
                !validate_binding(binding) ||
                !validLegalTime(binding.value(QStringLiteral("expected_legal_time")).toObject())) {
                return QStringLiteral(
                           "route filing binding does not match actor, date, or record: %1")
                    .arg(filing_id);
            }
            filing_ids.insert(filing_id);
            ++filing_bindings;
        }
    }
    if (route_keys.size() != 13 || filing_bindings != 24 || filing_ids.size() != 24) {
        return QStringLiteral("workflow 13-route/24-filing-binding closure drifted");
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<QString> auditTraceEvidence(const QDir& root) {
    const QDir traces_root(root.filePath(QStringLiteral("traces")));
    QStringList expected_files{
        QStringLiteral("actual-through-mandate.json"),
        QStringLiteral("adverse-on-briefs-through-mandate.json"),
        QStringLiteral("dissolution-release-through-mandate.json"),
        QStringLiteral("impracticability-denial-scoped-endpoint.json"),
        QStringLiteral("mandate-stay-denial-through-mandate.json"),
        QStringLiteral("mandate-stay-grant-blocked.json"),
        QStringLiteral("rehearing-denial-through-mandate.json"),
        QStringLiteral("rule8-threshold-scoped-endpoint.json"),
        QStringLiteral("stay-grant-dissolution-scoped-endpoint.json"),
    };
    expected_files.sort();
    const auto plan_bytes =
        readAll(root.filePath(QStringLiteral("metadata/successor-appellate-plan.json")));
    const auto plan = parseObject(plan_bytes);
    if (sha256(plan_bytes) != trace_plan_sha256 || !plan ||
        plan->value(QStringLiteral("planned_traces")).toArray().size() != 9) {
        return QStringLiteral("frozen nine-trace plan is absent or drifted");
    }
    if (traces_root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name) !=
        expected_files) {
        return QStringLiteral("nine-file trace inventory is absent or drifted");
    }

    const auto workflow =
        parseObject(readAll(root.filePath(QStringLiteral("resources/workflow.candidate.json"))));
    const auto record =
        parseObject(readAll(root.filePath(QStringLiteral("resources/record.candidate.json"))));
    if (!workflow || !record) {
        return QStringLiteral("trace cross-reference inputs cannot be parsed");
    }
    QSet<QString> workflow_operation_ids;
    for (const auto& value : workflow->value(QStringLiteral("operations")).toArray()) {
        workflow_operation_ids.insert(
            value.toObject().value(QStringLiteral("operation_id")).toString());
    }
    QHash<QString, QJsonObject> record_entry_by_sha;
    for (const auto& value : record->value(QStringLiteral("docket_entries")).toArray()) {
        const auto entry = value.toObject();
        record_entry_by_sha.insert(entry.value(QStringLiteral("asset_sha256")).toString(), entry);
    }

    const QHash<QString, QString> scoped_endpoints{
        {QStringLiteral("ca4m4.norvale.trace.rule8-threshold"),
         QStringLiteral("ca4m4.norvale.operation.enter-rule8-threshold-denial")},
        {QStringLiteral("ca4m4.norvale.trace.impracticability-denial"),
         QStringLiteral("ca4m4.norvale.operation.enter-impracticability-stay-denial")},
        {QStringLiteral("ca4m4.norvale.trace.stay-grant-dissolution"),
         QStringLiteral("ca4m4.norvale.operation.enter-appellate-stay-dissolution")},
    };
    const auto actual_continuation =
        QStringLiteral("ca4m4.norvale.operation.advance-actual-stay-denial-to-record");
    QSet<QString> trace_ids;
    QSet<QString> evidence_ids;
    QSet<QString> executed_operations;
    QSet<QString> concrete_deadline_ids;
    QHash<QString, int> terminal_counts;
    int total_commands = 0;
    int total_events = 0;
    int rejection_count = 0;
    QCryptographicHash trace_closure(QCryptographicHash::Sha256);

    for (const auto& file_name : expected_files) {
        const auto trace_bytes = readAll(traces_root.filePath(file_name));
        const auto parsed = parseObject(trace_bytes);
        if (!parsed) {
            return QStringLiteral("trace is not a JSON object: %1").arg(file_name);
        }
        const auto trace = *parsed;
        const auto trace_id = trace.value(QStringLiteral("trace_id")).toString();
        const auto evidence_id = trace.value(QStringLiteral("evidence_id")).toString();
        const auto journal_json = trace.value(QStringLiteral("journal")).toArray();
        const auto declared_operations = trace.value(QStringLiteral("operation_ids")).toArray();
        const auto computed_journal = journalDigest(journal_json);
        const auto terminal_stage = trace.value(QStringLiteral("terminal_stage_id")).toString();
        if (trace_id.isEmpty() || evidence_id.isEmpty() || trace_ids.contains(trace_id) ||
            evidence_ids.contains(evidence_id) ||
            trace.value(QStringLiteral("workflow_id")).toString() !=
                QStringLiteral("ca4m4.norvale.workflow.civil-injunction") ||
            trace.value(QStringLiteral("engine_revision")).toString() !=
                QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1") ||
            trace.value(QStringLiteral("command_count")).toInt() != journal_json.size() ||
            trace.value(QStringLiteral("event_count")).toInt() != declared_operations.size() ||
            !computed_journal ||
            trace.value(QStringLiteral("journal_sha256")).toString() != *computed_journal ||
            trace.value(QStringLiteral("digest")).toString() != traceDigest(trace)) {
            return QStringLiteral("trace envelope or canonical digest drifted: %1").arg(file_name);
        }
        trace_ids.insert(trace_id);
        evidence_ids.insert(evidence_id);
        ++terminal_counts[terminal_stage];

        const auto relative_path = QStringLiteral("traces/%1").arg(file_name).toUtf8();
        trace_closure.addData(QByteArrayView(relative_path));
        trace_closure.addData(QByteArrayView("\0", 1));
        trace_closure.addData(QByteArrayView(trace_bytes));
        trace_closure.addData(QByteArrayView("\0", 1));

        std::vector<model::WorkflowEvent> flat_events;
        QJsonArray decoded_operation_ids;
        QString session_id;
        std::uint64_t expected_sequence = 1;
        for (const auto& journal_value : journal_json) {
            const auto journal_entry = journal_value.toObject();
            const auto command_bytes = QByteArray::fromBase64(
                journal_entry.value(QStringLiteral("command_base64")).toString().toLatin1());
            const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
            if (!command || storage::encodeWorkflowCommand(*command) != command_bytes) {
                return QStringLiteral("trace command codec is noncanonical: %1").arg(file_name);
            }
            const auto& command_header = commandHeader(*command);
            const auto command_session = QString::fromStdString(command_header.session_id);
            if (session_id.isEmpty()) {
                session_id = command_session;
            }
            if (command_session != session_id) {
                return QStringLiteral("trace session identity changes mid-journal: %1")
                    .arg(file_name);
            }
            if (const auto* deadline = std::get_if<model::CalculateWorkflowDeadline>(&*command)) {
                concrete_deadline_ids.insert(QString::fromStdString(deadline->deadline_id.value));
            }
            if (const auto digest = commandDocumentDigest(*command); digest.has_value()) {
                const auto entry = record_entry_by_sha.value(QString::fromStdString(*digest));
                if (entry.isEmpty() || entry.value(QStringLiteral("filed_on")).toString() !=
                                           courtDate(command_header.occurred_at.court_date)) {
                    return QStringLiteral("trace document/date does not resolve: %1")
                        .arg(file_name);
                }
            }

            const auto event_values =
                journal_entry.value(QStringLiteral("events_base64")).toArray();
            for (int event_index = 0; event_index < event_values.size(); ++event_index) {
                const auto event_bytes =
                    QByteArray::fromBase64(event_values.at(event_index).toString().toLatin1());
                const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
                if (!event || storage::encodeWorkflowEvent(*event) != event_bytes) {
                    return QStringLiteral("trace event codec is noncanonical: %1").arg(file_name);
                }
                const auto& header = eventHeader(*event);
                const auto operation_id = QString::fromStdString(header.operation_id.value);
                if (!workflow_operation_ids.contains(operation_id) ||
                    QString::fromStdString(header.session_id) != session_id ||
                    header.command_id != command_header.command_id ||
                    header.occurred_at != command_header.occurred_at ||
                    header.sequence != expected_sequence ||
                    header.command_event_index != static_cast<std::uint32_t>(event_index) ||
                    header.command_event_count != static_cast<std::uint32_t>(event_values.size())) {
                    return QStringLiteral("trace event header, sequence, or operation drifted: %1")
                        .arg(file_name);
                }
                ++expected_sequence;
                decoded_operation_ids.push_back(operation_id);
                executed_operations.insert(operation_id);
                if (const auto* deadline =
                        std::get_if<model::WorkflowDeadlineCalculated>(&*event)) {
                    concrete_deadline_ids.insert(
                        QString::fromStdString(deadline->deadline_id.value));
                }
                flat_events.push_back(*event);
            }
        }
        if (decoded_operation_ids != declared_operations ||
            static_cast<int>(flat_events.size()) !=
                trace.value(QStringLiteral("event_count")).toInt()) {
            return QStringLiteral("trace decoded operation order/count drifted: %1").arg(file_name);
        }

        for (std::size_t index = 0; index < flat_events.size(); ++index) {
            const auto* rejected =
                std::get_if<model::WorkflowFilingRejected>(&flat_events.at(index));
            if (rejected == nullptr) {
                continue;
            }
            ++rejection_count;
            if (rejected->reason != model::WorkflowFilingRejectionReason::NonconformingFiling) {
                return QStringLiteral("trace rejection is not NonconformingFiling: %1")
                    .arg(file_name);
            }
            const auto recovery = std::ranges::find_if(
                flat_events.begin() + static_cast<std::ptrdiff_t>(index + 1U), flat_events.end(),
                [&](const auto& event) {
                    const auto* accepted = std::get_if<model::WorkflowFilingAccepted>(&event);
                    return accepted != nullptr && accepted->filing_id == rejected->filing_id;
                });
            if (recovery == flat_events.end()) {
                return QStringLiteral("trace rejection lacks in-journal same-ID recovery: %1")
                    .arg(file_name);
            }
        }

        const auto trace_operations = strings(declared_operations);
        if (scoped_endpoints.contains(trace_id)) {
            if (terminal_stage != QStringLiteral("ca4m4.norvale.stage.appellate-stay") ||
                !trace_operations.contains(scoped_endpoints.value(trace_id)) ||
                trace_operations.contains(actual_continuation) ||
                trace_operations.contains(QStringLiteral(
                    "ca4m4.norvale.operation.enter-record-complete-briefing-order")) ||
                trace_operations.contains(
                    QStringLiteral("ca4m4.norvale.operation.enter-actual-opinion"))) {
                return QStringLiteral("scoped Rule 8 trace entered the actual lineage: %1")
                    .arg(file_name);
            }
        } else if (!trace_operations.contains(actual_continuation)) {
            return QStringLiteral("non-scoped trace did not use the A07 continuation: %1")
                .arg(file_name);
        }
        total_commands += trace.value(QStringLiteral("command_count")).toInt();
        total_events += trace.value(QStringLiteral("event_count")).toInt();
    }

    QSet<QString> deadline_concepts;
    for (const auto& concrete_id : concrete_deadline_ids) {
        auto logical_id = concrete_id;
        logical_id.remove(QStringLiteral("-actual"));
        logical_id.remove(QStringLiteral("-adverse"));
        deadline_concepts.insert(logical_id);
    }
    if (trace_ids.size() != 9 || evidence_ids.size() != 9 || total_commands != 316 ||
        total_events != 334 || rejection_count != 9 ||
        executed_operations != workflow_operation_ids || concrete_deadline_ids.size() != 14 ||
        deadline_concepts.size() != 12 ||
        terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.appellate-stay")) != 3 ||
        terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.mandate-stayed")) != 1 ||
        terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.terminated")) != 5) {
        return QStringLiteral("nine-trace closure drifted: traces=%1 evidence=%2 commands=%3 "
                              "events=%4 rejects=%5 operations=%6 deadlines=%7 concepts=%8 "
                              "endpoints=%9/%10/%11")
            .arg(trace_ids.size())
            .arg(evidence_ids.size())
            .arg(total_commands)
            .arg(total_events)
            .arg(rejection_count)
            .arg(executed_operations.size())
            .arg(concrete_deadline_ids.size())
            .arg(deadline_concepts.size())
            .arg(terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.appellate-stay")))
            .arg(terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.mandate-stayed")))
            .arg(terminal_counts.value(QStringLiteral("ca4m4.norvale.stage.terminated")));
    }
    if (trace_closure.result().toHex().toStdString() != ReleasePins::trace_closure) {
        return QStringLiteral("frozen path-framed trace closure drifted");
    }
    return std::nullopt;
}

[[nodiscard]] auto decodeJournal(const QJsonArray& journal_json)
    -> std::expected<std::vector<model::WorkflowJournalEntry>, QString> {
    std::vector<model::WorkflowJournalEntry> journal;
    journal.reserve(static_cast<std::size_t>(journal_json.size()));
    for (const auto& value : journal_json) {
        const auto entry = value.toObject();
        const auto command_bytes = QByteArray::fromBase64(
            entry.value(QStringLiteral("command_base64")).toString().toLatin1());
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (!command || storage::encodeWorkflowCommand(*command) != command_bytes) {
            return std::unexpected(QStringLiteral("command codec is noncanonical"));
        }
        std::vector<model::WorkflowEvent> events;
        for (const auto& event_value : entry.value(QStringLiteral("events_base64")).toArray()) {
            const auto event_bytes = QByteArray::fromBase64(event_value.toString().toLatin1());
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
            if (!event || storage::encodeWorkflowEvent(*event) != event_bytes) {
                return std::unexpected(QStringLiteral("event codec is noncanonical"));
            }
            events.push_back(*event);
        }
        journal.push_back(model::WorkflowJournalEntry{*command, std::move(events)});
    }
    return journal;
}

[[nodiscard]] model::WorkflowState initialState(const model::WorkflowDefinition& workflow,
                                                const model::WorkflowCommand& first_command) {
    model::WorkflowState state;
    state.session_id = commandHeader(first_command).session_id;
    state.workflow_id = workflow.id;
    state.current_stage_id = workflow.initial_stage_id;
    return state;
}

[[nodiscard]] std::optional<QString> auditFinalPackAndReplay(const QDir& root,
                                                             const QDir& foundations) {
    const QDir pack_root(root.filePath(QStringLiteral("pack-candidate")));
    const auto manifest_bytes = readAll(pack_root.filePath(QStringLiteral("manifest.json")));
    const auto review_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/realism-review.json")));
    const auto workflow_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/workflow.json")));
    const auto manifest = parseObject(manifest_bytes);
    const auto review = parseObject(review_bytes);
    if (sha256(manifest_bytes).toStdString() != ReleasePins::manifest_sha256 ||
        sha256(review_bytes).toStdString() != ReleasePins::realism_review_sha256 ||
        sha256(workflow_bytes) != workflow_sha256 || !manifest || !review ||
        review->value(QStringLiteral("evidence"))
                .toObject()
                .value(QStringLiteral("closure_digest"))
                .toString()
                .toStdString() != ReleasePins::evidence_closure ||
        review->value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending")) {
        return QStringLiteral("final manifest, review, workflow, or evidence closure drifted");
    }
    const auto evidence = review->value(QStringLiteral("evidence")).toObject();
    if (manifest->value(QStringLiteral("schema_version")).toInt() != 2 ||
        manifest->value(QStringLiteral("pack_id")).toString() !=
            QStringLiteral("us.ca4.m4.norvale-injunction") ||
        manifest->value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        manifest->value(QStringLiteral("contents")).toArray().size() != 9 ||
        manifest->value(QStringLiteral("blobs")).toArray().size() != 73 ||
        manifest->value(QStringLiteral("required_capabilities")).toArray().size() != 16 ||
        manifest->value(QStringLiteral("dependencies")).toArray().size() != 3 ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 35 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 73 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 9) {
        return QStringLiteral("final 9-resource/73-blob/16-capability evidence envelope drifted");
    }

    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.norvale-injunction"}, "1.2.0",
                                            std::string(ReleasePins::root_revision)};
    const std::array expected_dependencies{
        model::PackRevision{model::PackId{"foundation.us-federal"}, "2025.12.01",
                            "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9"},
        model::PackRevision{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                            "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262"},
        model::PackRevision{model::PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                            "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d"},
    };
    const auto loaded = packs::PackReader::readDirectory(
        pack_root.path(), packs::PackValidationScope::ResolvedClosure);
    if (!loaded || loaded->revision != expected_root ||
        loaded->graph_state != packs::PackGraphState::DeferredReferences ||
        loaded->dependencies.size() != std::size_t{3} ||
        loaded->required_capabilities.size() != std::size_t{16} ||
        loaded->resources.size() != std::size_t{9} || loaded->blobs.size() != std::size_t{73}) {
        return QStringLiteral("final deferred pack envelope or root revision drifted");
    }
    for (const auto& expected : expected_dependencies) {
        if (std::ranges::find(loaded->dependencies, expected, [](const auto& dependency) {
                return dependency.revision;
            }) == loaded->dependencies.end()) {
            return QStringLiteral("final pack is missing dependency %1")
                .arg(QString::fromStdString(expected.id.value));
        }
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return QStringLiteral("cannot create final-pack test directory");
    }
    const auto archive_a = QDir(temporary.path()).filePath(QStringLiteral("norvale-a.awpack"));
    const auto archive_b = QDir(temporary.path()).filePath(QStringLiteral("norvale-b.awpack"));
    const auto exported_a = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_a, {}, packs::PackValidationScope::ResolvedClosure);
    const auto exported_b = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_b, {}, packs::PackValidationScope::ResolvedClosure);
    const auto archive_bytes = readAll(archive_a);
    if (!exported_a || !exported_b || *exported_a != expected_root ||
        *exported_b != expected_root || archive_bytes != readAll(archive_b) ||
        static_cast<std::uint64_t>(archive_bytes.size()) != ReleasePins::archive_byte_size ||
        sha256(archive_bytes).toStdString() != ReleasePins::archive_sha256) {
        return QStringLiteral("final archive export identity or determinism drifted");
    }

    const auto catalog =
        packs::PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    if (!catalog) {
        return QStringLiteral("cannot open final-pack test catalog: %1")
            .arg(catalog.error().message);
    }
    const auto install = [&](const QString& path, QString time) {
        return (*catalog)->installArchive(path, std::move(time));
    };
    const auto federal = install(
        foundations.filePath(QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack")),
        QStringLiteral("2026-08-12T00:00:00Z"));
    const auto ca4 =
        install(foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
                QStringLiteral("2026-08-12T00:00:01Z"));
    const auto bench =
        install(foundations.filePath(QStringLiteral(
                    "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
                QStringLiteral("2026-08-12T00:00:02Z"));
    const auto installed_root = install(archive_a, QStringLiteral("2026-08-12T00:00:03Z"));
    if (!federal || !ca4 || !bench || !installed_root ||
        federal->revision != expected_dependencies.at(0) ||
        ca4->revision != expected_dependencies.at(1) ||
        bench->revision != expected_dependencies.at(2) ||
        installed_root->revision != expected_root ||
        installed_root->archive_sha256.toStdString() != ReleasePins::archive_sha256) {
        return QStringLiteral("exact dependency/root archive installation drifted");
    }
    const auto resolved = (*catalog)->loadResolved(expected_root);
    if (!resolved || resolved->revisionsByPackId().size() != std::size_t{4}) {
        return QStringLiteral("four-revision resolved closure cannot be loaded");
    }
    const auto runtime = packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != expected_root || runtime->cases.size() != std::size_t{1}) {
        return QStringLiteral("resolved Norvale runtime cannot be constructed");
    }
    const auto& runtime_case = runtime->cases.front();
    if (runtime_case.definition.id.value != "ca4m4.case.norvale-injunction" ||
        runtime_case.workflow.operations.size() != std::size_t{90} ||
        runtime_case.workflow.filing_routes.size() != std::size_t{13} ||
        runtime_case.record.docket_entries.size() != std::size_t{73} ||
        runtime_case.record.page_anchors.size() != std::size_t{383} ||
        runtime_case.argument_configurations.size() != std::size_t{2}) {
        return QStringLiteral("resolved runtime 90/13/73/383/two-bank shape drifted");
    }
    for (const auto& blob : loaded->blobs) {
        const auto materialized = (*catalog)->materializeBlob(*resolved, expected_root, blob.path);
        if (!materialized || materialized->descriptor != blob ||
            sha256(readAll(materialized->local_path)).toStdString() != blob.sha256) {
            return QStringLiteral("installed blob cannot be materialized exactly: %1")
                .arg(QString::fromStdString(blob.path));
        }
    }

    const QDir traces_root(root.filePath(QStringLiteral("traces")));
    const auto trace_files =
        traces_root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& file_name : trace_files) {
        const auto trace = parseObject(readAll(traces_root.filePath(file_name)));
        if (!trace) {
            return QStringLiteral("final replay trace cannot be parsed: %1").arg(file_name);
        }
        const auto decoded = decodeJournal(trace->value(QStringLiteral("journal")).toArray());
        if (!decoded || decoded->empty()) {
            return QStringLiteral("final replay trace cannot be decoded: %1").arg(file_name);
        }
        const auto initial = initialState(runtime_case.workflow, decoded->front().command);
        model::WorkflowState rolling = initial;
        for (std::size_t index = 0; index < decoded->size(); ++index) {
            if (index > 0) {
                const auto prefix =
                    std::span<const model::WorkflowJournalEntry>(decoded->data(), index);
                const auto prefix_state = engine::replayWorkflow(
                    runtime_case.workflow, runtime_case.definition, initial, prefix);
                if (!prefix_state) {
                    return QStringLiteral("production prefix replay failed: %1").arg(file_name);
                }
                rolling = *prefix_state;
            }
            const auto& entry = decoded->at(index);
            const auto decided = engine::decideWorkflow(
                runtime_case.workflow, runtime_case.definition, rolling, entry.command);
            if (!decided || *decided != entry.events) {
                return QStringLiteral("trace redecision differs from frozen events: %1")
                    .arg(file_name);
            }
        }
        const auto first = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                  initial, *decoded);
        const auto second = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                   initial, *decoded);
        if (!first || !second || *first != *second ||
            first->current_stage_id.value !=
                trace->value(QStringLiteral("terminal_stage_id")).toString().toStdString()) {
            return QStringLiteral("full production replay is nondeterministic: %1").arg(file_name);
        }

        auto event_tamper = *decoded;
        std::visit([](auto& event) { ++event.header.sequence; },
                   event_tamper.front().events.front());
        if (engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   event_tamper)) {
            return QStringLiteral("event-sequence tamper replayed: %1").arg(file_name);
        }
        auto legal_time_tamper = *decoded;
        std::visit([](auto& command) { command.header.occurred_at.instant += 1s; },
                   legal_time_tamper.front().command);
        if (engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   legal_time_tamper)) {
            return QStringLiteral("legal-time tamper replayed: %1").arg(file_name);
        }
        auto document_tamper = *decoded;
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
            return QStringLiteral("document-SHA tamper replayed: %1").arg(file_name);
        }
    }
    return std::nullopt;
}

int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QDir root(QStringLiteral(APPELLATE_M4_NORVALE_ROOT));
    if (const auto failure = auditFrozenAuthoring(root)) {
        return fail(*failure);
    }
    if (const auto failure = auditWorkflowCandidate(root)) {
        return fail(*failure);
    }
    if (const auto failure = auditTraceEvidence(root)) {
        return fail(*failure);
    }
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    if (const auto failure = auditFinalPackAndReplay(root, foundations)) {
        return fail(*failure);
    }
    std::cout << "Norvale injunction integration passed: 9 resources / 73 PDFs / 383 anchors; "
                 "16 stages / 90 operations / 13 routes; 9 traces / 316 commands / 334 events.\n";
    return 0;
}
