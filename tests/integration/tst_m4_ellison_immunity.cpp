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

#ifndef APPELLATE_M4_ELLISON_ROOT
#error "APPELLATE_M4_ELLISON_ROOT must name content/m4/ellison-immunity"
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
    "4ed1cd955eb66ee7a4b1c656a821ef17727a92b2ce8fe8b53e89fe0f8e248dc2";
constexpr std::string_view manifest_sha256 =
    "8f0d614a73a4850a93170a7338229b64e2b1d042134e678785eaab481fd8ca42";
constexpr std::string_view realism_review_sha256 =
    "5545977962535b58f029de10959cf2f9e49348a12fb4f7bff17574aa688b8867";
constexpr std::string_view evidence_closure =
    "8032c5547dd522cad241b9c816bd611d198dbe7e007b0cd29781b8b471de41ac";
constexpr std::string_view root_revision =
    "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0";
constexpr std::string_view archive_sha256 =
    "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0";
constexpr std::uint64_t archive_byte_size = 4'230'462;
} // namespace ReleasePins

constexpr auto successor_plan_sha256 =
    "e7293dc1350ce66b63b7c70afc2ee088d55906b307cc85e90dcd663b5635d7a0";
constexpr auto source_closure_sha256 =
    "0a33de749c1c6118d87bf3c0f550afcbd82f26fe3e5a2b38d5b8d183ecada3c3";
constexpr auto render_plan_sha256 =
    "f68262e843d8af527a77e8ecf7d6b2e14cf83933e3e1808caa50bc82c0bd995d";
constexpr auto inventory_sha256 =
    "dffa33a8607a08c3ef3f8b5af5c15d505255d850a58fad7dc04bc7162aff3f14";
constexpr auto raw_pdf_closure_sha256 =
    "9e1ce3a92c61b1eb4240e97b49de83530c2e37dd38f2526ddd261d7edebfdc23";
constexpr auto framed_pdf_closure_sha256 =
    "277c650a0d1263acb972d1cb86c7be242f3a80d5c134c11eb1e85a61abe36afa";
constexpr auto record_sha256 = "3269cf7be84b0e1545f17d19876a64e86930d44259bb13d543d53ddb12bd199f";
constexpr auto workflow_sha256 = "cd69b276a63ae508ba0d98bbee15585847a405b5b55a48df69fffe45811ca23a";
constexpr auto trace_plan_sha256 =
    "82f1afa17e4d15a192cc6567ff3ffaa3415d2dd95f609aa58c760c026c78273d";
constexpr auto actual_bank_grounding_sha256 =
    "5b5d06119598daa1ff883de75642f3476aff4d5b47b56c5d562a2e3f2314152e";
constexpr auto counterfactual_bank_grounding_sha256 =
    "cf7097c563a40f40feac132943f3f231efa6c1fb1ade0e3b7e433e12f1a38849";

struct FrozenResource final {
    std::string_view candidate;
    std::string_view promoted;
    std::string_view sha256;
    std::string_view kind;
    std::string_view id;
};

constexpr std::array frozen_resources{
    FrozenResource{"argument-actual.candidate.json", "argument-actual.json",
                   "2abba8688906f2d6c248028ba56b49a83e88514757e5470502d10f5e485203d7",
                   "argument_config", "ca4m4.ellison.argument.actual-record"},
    FrozenResource{"argument-counterfactual.candidate.json", "argument-counterfactual.json",
                   "5baff068b6b38dd1dace80f47e518aa3cda5f64ae534ca838afd4fffa9fdb550",
                   "argument_config", "ca4m4.ellison.argument.adverse-record"},
    FrozenResource{"authority-set.candidate.json", "authority-set.json",
                   "9acda100f645562809f050bc338dbba04d81d8bfaa88da6820d3ef4a5d818591",
                   "authority_set", "ca4m4.ellison.authorities.case-specific"},
    FrozenResource{"bench-configuration.candidate.json", "bench-configuration.json",
                   "829c7e0ae4721bbd246dbb12987f014ddbc836b7be0e32a825d204a396b155a6",
                   "bench_configuration", "ca4m4.ellison.bench.three-judge"},
    FrozenResource{"case.candidate.json", "case.json",
                   "e29d11e570d49b058a6dc57c020e433dc4210638fbd3434d3aacb40467f91822", "case",
                   "ca4m4.case.ellison-immunity"},
    FrozenResource{"procedure-profile.candidate.json", "procedure-profile.json",
                   "050d5fe439316267c79eeecff9528ede7f6abf7272c8004e51682baa608190ba",
                   "procedure_profile", "ca4m4.ellison.procedure.civil-collateral-order-appeal"},
    FrozenResource{"record.candidate.json", "record.json", record_sha256, "record",
                   "ca4m4.ellison.record"},
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
    addFrame(hash, QStringLiteral("ca4m4.case.ellison-immunity"));
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
            QStringLiteral("source_authoring_in_progress") ||
        plan.value(QStringLiteral("render_status")).toString() !=
            QStringLiteral("pending_render") ||
        plan.value(QStringLiteral("capability_plan")).toArray().size() != 16 ||
        documents.size() != 72 || inventory_entries.size() != 72 || record_entries.size() != 72 ||
        anchors.size() != 449 ||
        inventory.value(QStringLiteral("plan_sha256")).toString() != render_plan_sha256) {
        return QStringLiteral("72-document/449-page accepted authoring envelope drifted");
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
        const auto classification = document.value(QStringLiteral("classification")).toString();
        if (code.isEmpty() || source.isEmpty() || output_path.isEmpty() || page_count <= 0 ||
            plan_by_code.contains(code) || plan_by_output.contains(output_path) ||
            document.value(QStringLiteral("source_status")).toString() !=
                (classification == QStringLiteral("lower_record")
                     ? QStringLiteral("source_authoring_in_progress")
                     : QStringLiteral("planned")) ||
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
        if (classification == QStringLiteral("lower_record")) {
            ++lower_documents;
            lower_pages += page_count;
        } else if (classification == QStringLiteral("actual_appellate")) {
            ++actual_documents;
            actual_pages += page_count;
        } else if (classification == QStringLiteral("counterfactual_appellate")) {
            ++branch_documents;
            branch_pages += page_count;
        } else {
            return QStringLiteral("unknown record classification: %1").arg(classification);
        }
    }
    if (source_closure.result().toHex() != source_closure_sha256 || planned_pages != 449 ||
        lower_documents != 37 || lower_pages != 275 || actual_documents != 15 ||
        actual_pages != 91 || branch_documents != 20 || branch_pages != 83) {
        return QStringLiteral("source closure or 37/275 + 15/91 + 20/83 allocation drifted");
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
    if (rendered_pages != 449 || total_bytes != 2'763'463 ||
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
            return QStringLiteral("Ellison actor is not marked synthetic");
        }
        actor_names.insert(actor.value(QStringLiteral("actor_id")).toString(),
                           actor.value(QStringLiteral("display_name")).toString());
    }
    if (actor_names.size() != 6 ||
        case_document->value(QStringLiteral("issues")).toArray().size() != 3 ||
        case_document->value(QStringLiteral("disposition_plans")).toArray().size() != 2 ||
        case_document->value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.ellison.disposition.authored-mixed") ||
        case_document->value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.ellison.operation.issue-actual-judgment")) {
        return QStringLiteral("Ellison six-actor/three-issue/two-disposition case shape drifted");
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
            entry_id != QStringLiteral("ca4m4.ellison.record.entry.%1")
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
            !tags.contains(plan_row.value(QStringLiteral("classification")).toString() ==
                                   QStringLiteral("actual_appellate")
                               ? QStringLiteral("actual_appellate_docket")
                           : plan_row.value(QStringLiteral("classification")).toString() ==
                                   QStringLiteral("counterfactual_appellate")
                               ? QStringLiteral("counterfactual_appellate_branch")
                               : QStringLiteral("lower_record")) ||
            (plan_row.contains(QStringLiteral("record_category")) &&
             !tags.contains(plan_row.value(QStringLiteral("record_category")).toString())) ||
            entry.value(QStringLiteral("sealed")).toBool(true) ||
            entry.contains(QStringLiteral("parent_entry_id")) ||
            entry.contains(QStringLiteral("relationship"))) {
            return QStringLiteral("record/plan identity, actor, date, title, or tag drifted: %1")
                .arg(entry_id);
        }
        const bool branch = index >= 52;
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
    const auto district_id = QStringLiteral("ca4m4.ellison.docket.district");
    const auto actual_id = QStringLiteral("ca4m4.ellison.docket.appellate");
    const auto branch_id = QStringLiteral("ca4m4.ellison.docket.counterfactual-branches");
    if (record.value(QStringLiteral("dockets")).toArray().size() != 3 ||
        docket_documents.value(district_id) != 37 || docket_pages.value(district_id) != 275 ||
        docket_documents.value(actual_id) != 15 || docket_pages.value(actual_id) != 91 ||
        docket_documents.value(branch_id) != 20 || docket_pages.value(branch_id) != 83) {
        return QStringLiteral("three-docket 37/275 + 15/91 + 20/83 closure drifted");
    }

    QHash<QString, QJsonObject> anchors_by_id;
    QHash<QString, int> anchors_per_entry;
    for (int index = 0; index < anchors.size(); ++index) {
        const auto anchor = anchors.at(index).toObject();
        const bool joint_appendix = index < 275;
        const int ordinal = joint_appendix ? index + 1 : index - 274;
        const auto prefix = joint_appendix ? QStringLiteral("JA") : QStringLiteral("PA");
        const auto expected_label = QStringLiteral("%1%2").arg(prefix).arg(ordinal);
        const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
        const auto anchor_id = anchor.value(QStringLiteral("anchor_id")).toString();
        const auto entry = entries_by_id.value(entry_id);
        if (anchor_id != QStringLiteral("ca4m4.ellison.anchor.%1").arg(expected_label.toLower()) ||
            anchor.value(QStringLiteral("citation_label")).toString() != expected_label ||
            entry.isEmpty() || anchor.value(QStringLiteral("page_number")).toInt() <= 0 ||
            anchor.value(QStringLiteral("page_number")).toInt() >
                entry.value(QStringLiteral("page_count")).toInt() ||
            anchors_by_id.contains(anchor_id)) {
            return QStringLiteral("JA1-JA275 / PA1-PA174 anchor continuity drifted");
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
        core_documents.value(QStringLiteral("ca4m4.ellison.authorities.case-specific"));
    const auto bench = core_documents.value(QStringLiteral("ca4m4.ellison.bench.three-judge"));
    if (authority_set.value(QStringLiteral("authorities")).toArray().size() != 11 ||
        bench.value(QStringLiteral("seats")).toArray().size() != 3 ||
        bench.value(QStringLiteral("presiding_seat_id")).toString() !=
            QStringLiteral("ca4m4.ellison.seat.alder")) {
        return QStringLiteral("eleven-authority/three-seat core resource shape drifted");
    }
    QHash<QString, QJsonObject> authorities_by_id;
    for (const auto& value : authority_set.value(QStringLiteral("authorities")).toArray()) {
        const auto authority = value.toObject();
        authorities_by_id.insert(authority.value(QStringLiteral("authority_id")).toString(),
                                 authority);
    }
    const std::array banks{
        std::pair{QStringLiteral("ca4m4.ellison.argument.actual-record"),
                  QString::fromLatin1(actual_bank_grounding_sha256)},
        std::pair{QStringLiteral("ca4m4.ellison.argument.adverse-record"),
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
        if (configuration.value(QStringLiteral("permitted_issue_ids")).toArray().size() != 3 ||
            bank.value(QStringLiteral("issue_topic_bindings")).toArray().size() != 3 ||
            bank.value(QStringLiteral("questions")).toArray().size() != 12 ||
            question_ids.size() != 12 || authority_grounding != 17 || record_grounding != 29 ||
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
            QStringLiteral("ca4m4.ellison.workflow.qualified-immunity-appeal") ||
        stages.size() != 20 || operations.size() != 77 || routes.size() != 11 ||
        workflow.value(QStringLiteral("initial_stage_id")).toString() !=
            QStringLiteral("ca4m4.ellison.stage.notice-of-appeal")) {
        return QStringLiteral("workflow 20-stage/77-operation/11-route envelope drifted");
    }
    QSet<QString> stage_ids;
    for (const auto& value : stages) {
        stage_ids.insert(value.toString());
    }
    if (stage_ids.size() != 20 ||
        !stage_ids.contains(QStringLiteral("ca4m4.ellison.stage.terminated"))) {
        return QStringLiteral("workflow stage identity is duplicate or incomplete");
    }
    QHash<QString, QJsonObject> operations_by_id;
    QSet<QString> deadline_ids;
    QSet<QString> deadline_concepts;
    int deadline_operations = 0;
    int document_bindings = 0;
    int disposition_bindings = 0;
    const QHash<QString, QString> expected_deadlines{
        {QStringLiteral("ca4m4.ellison.operation.calculate-opening-brief-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.opening-brief")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-response-brief-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.response-brief")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-reply-brief-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.reply-brief")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-adverse-rehearing-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.adverse-rehearing-petition")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-actual-rehearing-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.actual-rehearing-petition")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-adverse-ordinary-mandate-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.adverse-ordinary-mandate")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-actual-ordinary-mandate-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.actual-ordinary-mandate")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-shortened-post-denial-mandate-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.actual-post-stay-denial-mandate")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-mandate-stay-through-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.actual-stay-through")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-post-dissolution-mandate-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.actual-post-dissolution-mandate")},
        {QStringLiteral("ca4m4.ellison.operation.calculate-post-rehearing-mandate-deadline"),
         QStringLiteral("ca4m4.ellison.deadline.adverse-post-rehearing-mandate")},
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
            concrete_deadline_ids.insert(*expected);
            deadline_concepts.insert(*expected);
            const auto deadline_id =
                operation.value(QStringLiteral("produced_deadline_id")).toString();
            if (!deadline_id.isEmpty()) {
                if (deadline_ids.contains(deadline_id) || deadline_id != *expected) {
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
    if (operations_by_id.size() != 77 || deadline_operations != 11 ||
        expected_deadlines.size() != 11 || concrete_deadline_ids.size() != 11 ||
        deadline_concepts.size() != 11 || deadline_ids.size() != 11 || document_bindings != 24 ||
        disposition_bindings != 2) {
        return QStringLiteral("workflow 77-operation/11-deadline/24-document/2-disposition counts "
                              "drifted");
    }

    const auto operation_by_suffix = [&](QStringView suffix) {
        return operations_by_id.value(QStringLiteral("ca4m4.ellison.operation.%1").arg(suffix));
    };
    const auto actual_submission = operation_by_suffix(u"enter-oral-argument-calendar-notice");
    const auto adverse_submission = operation_by_suffix(u"enter-submission-on-briefs-order");
    const auto actual_submission_binding =
        actual_submission.value(QStringLiteral("document_binding")).toObject();
    const auto adverse_submission_binding =
        adverse_submission.value(QStringLiteral("document_binding")).toObject();
    if (actual_submission_binding.value(QStringLiteral("record_entry_id")).toString() !=
            QStringLiteral("ca4m4.ellison.record.entry.a10") ||
        adverse_submission_binding.value(QStringLiteral("record_entry_id")).toString() !=
            QStringLiteral("ca4m4.ellison.record.entry.b01") ||
        actual_submission_binding.value(QStringLiteral("order_id")).toString() !=
            QStringLiteral("ca4m4.ellison.order.submission-mode") ||
        adverse_submission_binding.value(QStringLiteral("order_id")).toString() !=
            actual_submission_binding.value(QStringLiteral("order_id")).toString() ||
        actual_submission_binding.value(QStringLiteral("disposition")).toString() !=
            QStringLiteral("granted") ||
        adverse_submission_binding.value(QStringLiteral("disposition")).toString() !=
            QStringLiteral("denied") ||
        actual_submission.value(QStringLiteral("preconditions")).toArray() !=
            adverse_submission.value(QStringLiteral("preconditions")).toArray()) {
        return QStringLiteral("A10/B01 shared-order submission exclusivity drifted");
    }

    const auto b18 = operation_by_suffix(u"enter-certiorari-abandonment-stay-dissolution");
    const auto b18_binding = b18.value(QStringLiteral("document_binding")).toObject();
    if (b18.value(QStringLiteral("opcode")).toString() != QStringLiteral("enter_order") ||
        b18.value(QStringLiteral("stage_id")).toString() !=
            QStringLiteral("ca4m4.ellison.stage.actual-mandate-stayed") ||
        b18.contains(QStringLiteral("next_stage_id")) ||
        b18_binding.value(QStringLiteral("record_entry_id")).toString() !=
            QStringLiteral("ca4m4.ellison.record.entry.b18") ||
        b18_binding.value(QStringLiteral("order_id")).toString() !=
            QStringLiteral("ca4m4.ellison.order.mandate-stay-dissolution") ||
        b18_binding.value(QStringLiteral("disposition")).toString() != QStringLiteral("other")) {
        return QStringLiteral("B18 is not the exact EnterOrder dissolution document");
    }

    const QSet<QString> expected_court_entries{
        QStringLiteral("ca4m4.ellison.record.entry.l37"),
        QStringLiteral("ca4m4.ellison.record.entry.a01"),
        QStringLiteral("ca4m4.ellison.record.entry.a05"),
        QStringLiteral("ca4m4.ellison.record.entry.a10"),
        QStringLiteral("ca4m4.ellison.record.entry.a11"),
        QStringLiteral("ca4m4.ellison.record.entry.a12"),
        QStringLiteral("ca4m4.ellison.record.entry.a13"),
        QStringLiteral("ca4m4.ellison.record.entry.a14"),
        QStringLiteral("ca4m4.ellison.record.entry.a15"),
        QStringLiteral("ca4m4.ellison.record.entry.b01"),
        QStringLiteral("ca4m4.ellison.record.entry.b02"),
        QStringLiteral("ca4m4.ellison.record.entry.b03"),
        QStringLiteral("ca4m4.ellison.record.entry.b05"),
        QStringLiteral("ca4m4.ellison.record.entry.b06"),
        QStringLiteral("ca4m4.ellison.record.entry.b07"),
        QStringLiteral("ca4m4.ellison.record.entry.b08"),
        QStringLiteral("ca4m4.ellison.record.entry.b09"),
        QStringLiteral("ca4m4.ellison.record.entry.b12"),
        QStringLiteral("ca4m4.ellison.record.entry.b13"),
        QStringLiteral("ca4m4.ellison.record.entry.b14"),
        QStringLiteral("ca4m4.ellison.record.entry.b17"),
        QStringLiteral("ca4m4.ellison.record.entry.b18"),
        QStringLiteral("ca4m4.ellison.record.entry.b19"),
        QStringLiteral("ca4m4.ellison.record.entry.b20"),
    };
    QSet<QString> court_entries;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
        if (preconditions.isEmpty()) {
            return QStringLiteral("workflow operation is unguarded: %1")
                .arg(operation.value(QStringLiteral("operation_id")).toString());
        }
        if (!operation.contains(QStringLiteral("document_binding"))) {
            continue;
        }
        const auto entry_id = operation.value(QStringLiteral("document_binding"))
                                  .toObject()
                                  .value(QStringLiteral("record_entry_id"))
                                  .toString();
        court_entries.insert(entry_id);
        const auto stage_id = operation.value(QStringLiteral("stage_id")).toString();
        if ((entry_id.contains(QStringLiteral(".entry.b0")) &&
             entry_id.right(3) >= QStringLiteral("b04") &&
             entry_id.right(3) <= QStringLiteral("b09") &&
             !stage_id.contains(QStringLiteral("adverse"))) ||
            (entry_id.contains(QStringLiteral(".entry.b")) &&
             entry_id.right(3) >= QStringLiteral("b10") &&
             entry_id.right(3) <= QStringLiteral("b20") &&
             !stage_id.contains(QStringLiteral("actual")))) {
            return QStringLiteral("actual/adverse document lineage crossed: %1").arg(entry_id);
        }
    }
    if (court_entries != expected_court_entries) {
        return QStringLiteral("exact 24-court-document operation union drifted");
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
    QHash<QString, QString> actor_roles;
    for (const auto& value : case_document->value(QStringLiteral("actors")).toArray()) {
        const auto actor = value.toObject();
        const auto actor_id = actor.value(QStringLiteral("actor_id")).toString();
        actors.insert(actor_id);
        actor_roles.insert(actor_id, actor.value(QStringLiteral("role_id")).toString());
    }
    if (actor_roles.value(QStringLiteral("ca4m4.ellison.actor.ellison")) !=
            QStringLiteral("us.ca4.role.responding-party") ||
        actor_roles.value(QStringLiteral("ca4m4.ellison.actor.alder-creek")) !=
            QStringLiteral("ca4m4.ellison.role.former-district-defendant") ||
        std::ranges::count(actor_roles, QStringLiteral("us.ca4.role.responding-party")) != 1) {
        return QStringLiteral("active service party/former district defendant roles drifted");
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
    QSet<QString> filing_entries;
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
        const auto service_roles =
            strings(route.value(QStringLiteral("required_service_role_ids")).toArray());
        if (!stage_ids.contains(stage_id) || route_keys.contains(route_key) || accept.isEmpty() ||
            reject.isEmpty() || accept.value(QStringLiteral("stage_id")).toString() != stage_id ||
            reject.value(QStringLiteral("stage_id")).toString() != stage_id ||
            accept.value(QStringLiteral("opcode")).toString() != QStringLiteral("accept_filing") ||
            reject.value(QStringLiteral("opcode")).toString() != QStringLiteral("reject_filing") ||
            service_roles.contains(
                QStringLiteral("ca4m4.ellison.role.former-district-defendant"))) {
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
            filing_entries.insert(binding.value(QStringLiteral("record_entry_id")).toString());
            ++filing_bindings;
        }
    }
    const QSet<QString> expected_filing_entries{
        QStringLiteral("ca4m4.ellison.record.entry.l35"),
        QStringLiteral("ca4m4.ellison.record.entry.l36"),
        QStringLiteral("ca4m4.ellison.record.entry.a02"),
        QStringLiteral("ca4m4.ellison.record.entry.a03"),
        QStringLiteral("ca4m4.ellison.record.entry.a04"),
        QStringLiteral("ca4m4.ellison.record.entry.a06"),
        QStringLiteral("ca4m4.ellison.record.entry.a07"),
        QStringLiteral("ca4m4.ellison.record.entry.a08"),
        QStringLiteral("ca4m4.ellison.record.entry.a09"),
        QStringLiteral("ca4m4.ellison.record.entry.b04"),
        QStringLiteral("ca4m4.ellison.record.entry.b10"),
        QStringLiteral("ca4m4.ellison.record.entry.b11"),
        QStringLiteral("ca4m4.ellison.record.entry.b15"),
        QStringLiteral("ca4m4.ellison.record.entry.b16"),
    };
    const auto has_filing_fence = [&](QStringView filing_id, QStringView opposite_id,
                                      bool expected_present) {
        for (const auto& route_value : routes) {
            for (const auto& binding_value :
                 route_value.toObject().value(QStringLiteral("filing_bindings")).toArray()) {
                const auto binding = binding_value.toObject();
                if (binding.value(QStringLiteral("filing_id")).toString() != filing_id) {
                    continue;
                }
                return std::ranges::any_of(
                    binding.value(QStringLiteral("preconditions")).toArray(),
                    [&](const auto& value) {
                        const auto guard = value.toObject();
                        return guard.value(QStringLiteral("kind")).toString() ==
                                   QStringLiteral("filing_instance") &&
                               guard.value(QStringLiteral("filing_id")).toString() == opposite_id &&
                               guard.value(QStringLiteral("present")).toBool() == expected_present;
                    });
            }
        }
        return false;
    };
    if (route_keys.size() != 11 || filing_bindings != 14 || filing_ids.size() != 14 ||
        filing_entries != expected_filing_entries ||
        filing_entries.contains(QStringLiteral("ca4m4.ellison.record.entry.b18")) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b10-stay-motion-denial",
                          u"ca4m4.ellison.filing.b15-stay-motion-grant", false) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b15-stay-motion-grant",
                          u"ca4m4.ellison.filing.b10-stay-motion-denial", false) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b11-stay-response-denial",
                          u"ca4m4.ellison.filing.b10-stay-motion-denial", true) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b11-stay-response-denial",
                          u"ca4m4.ellison.filing.b16-stay-response-grant", false) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b16-stay-response-grant",
                          u"ca4m4.ellison.filing.b15-stay-motion-grant", true) ||
        !has_filing_fence(u"ca4m4.ellison.filing.b16-stay-response-grant",
                          u"ca4m4.ellison.filing.b11-stay-response-denial", false)) {
        return QStringLiteral("workflow 11-route/14-binding service and branch fences drifted");
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<QString> auditTraceEvidence(const QDir& root) {
    const QDir traces_root(root.filePath(QStringLiteral("traces")));
    QStringList expected_files{
        QStringLiteral("actual-ordinary-through-mandate.json"),
        QStringLiteral("adverse-ordinary-through-mandate.json"),
        QStringLiteral("adverse-rehearing-denial-through-mandate.json"),
        QStringLiteral("mandate-stay-denial-through-mandate.json"),
        QStringLiteral("mandate-stay-grant-blocked.json"),
        QStringLiteral("mandate-stay-grant-dissolution-through-mandate.json"),
    };
    expected_files.sort();
    const QHash<QString, int> expected_command_counts{
        {QStringLiteral("actual-ordinary-through-mandate.json"), 41},
        {QStringLiteral("adverse-ordinary-through-mandate.json"), 30},
        {QStringLiteral("adverse-rehearing-denial-through-mandate.json"), 36},
        {QStringLiteral("mandate-stay-denial-through-mandate.json"), 41},
        {QStringLiteral("mandate-stay-grant-blocked.json"), 39},
        {QStringLiteral("mandate-stay-grant-dissolution-through-mandate.json"), 42},
    };
    const auto plan_bytes =
        readAll(root.filePath(QStringLiteral("metadata/successor-appellate-plan.json")));
    const auto plan = parseObject(plan_bytes);
    if (sha256(plan_bytes) != trace_plan_sha256 || !plan ||
        plan->value(QStringLiteral("planned_traces")).toArray().size() != 6) {
        return QStringLiteral("frozen six-trace plan is absent or drifted");
    }
    if (traces_root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name) !=
        expected_files) {
        return QStringLiteral("six-file trace inventory is absent or drifted");
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

    QSet<QString> trace_ids;
    QSet<QString> evidence_ids;
    QSet<QString> executed_operations;
    QSet<QString> concrete_deadline_ids;
    QHash<QString, QString> deadline_dates;
    QSet<QString> accepted_filing_ids;
    QSet<QString> rejected_filing_ids;
    QSet<QString> court_document_entries;
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
                QStringLiteral("ca4m4.ellison.workflow.qualified-immunity-appeal") ||
            trace.value(QStringLiteral("engine_revision")).toString() !=
                QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1") ||
            trace.value(QStringLiteral("command_count")).toInt() !=
                expected_command_counts.value(file_name) ||
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
        QSet<QString> trace_document_entries;
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
                const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
                trace_document_entries.insert(entry_id);
                if (!std::holds_alternative<model::SubmitWorkflowFiling>(*command)) {
                    court_document_entries.insert(entry_id);
                }
                if (entry_id == QStringLiteral("ca4m4.ellison.record.entry.b18")) {
                    const auto* order = std::get_if<model::EnterWorkflowOrder>(&*command);
                    if (trace_id != QStringLiteral("ca4m4.ellison.trace.stay-grant-dissolution") ||
                        order == nullptr ||
                        order->operation_id.value != "ca4m4.ellison.operation.enter-certiorari-"
                                                     "abandonment-stay-dissolution" ||
                        order->order_id.value != "ca4m4.ellison.order.mandate-stay-dissolution" ||
                        order->disposition != model::WorkflowOrderDisposition::Other) {
                        return QStringLiteral(
                            "B18 trace command is not the exact EnterOrder event");
                    }
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
                    const auto deadline_id = QString::fromStdString(deadline->deadline_id.value);
                    const auto due_date = courtDate(deadline->due_date);
                    if (deadline_dates.contains(deadline_id) &&
                        deadline_dates.value(deadline_id) != due_date) {
                        return QStringLiteral("deadline date changes across traces: %1")
                            .arg(deadline_id);
                    }
                    concrete_deadline_ids.insert(deadline_id);
                    deadline_dates.insert(deadline_id, due_date);
                }
                if (const auto* accepted = std::get_if<model::WorkflowFilingAccepted>(&*event)) {
                    accepted_filing_ids.insert(QString::fromStdString(accepted->filing_id.value));
                    const auto former = model::ActorId{"ca4m4.ellison.actor.alder-creek"};
                    const auto active_opponent =
                        accepted->actor_id.value == "ca4m4.ellison.actor.rusk"
                            ? model::ActorId{"ca4m4.ellison.actor.ellison"}
                            : model::ActorId{"ca4m4.ellison.actor.rusk"};
                    if (std::ranges::find(accepted->served_actors, former) !=
                            accepted->served_actors.end() ||
                        std::ranges::find(accepted->served_actors, active_opponent) ==
                            accepted->served_actors.end()) {
                        return QStringLiteral(
                                   "accepted filing service includes the former party or "
                                   "omits the active opponent: %1")
                            .arg(file_name);
                    }
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
            rejected_filing_ids.insert(QString::fromStdString(rejected->filing_id.value));
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

        for (const auto& entry_id : trace_document_entries) {
            if (!entry_id.contains(QStringLiteral(".entry.b"))) {
                continue;
            }
            const auto code = entry_id.right(3);
            const bool adverse_trace =
                trace_id == QStringLiteral("ca4m4.ellison.trace.adverse-ordinary") ||
                trace_id == QStringLiteral("ca4m4.ellison.trace.adverse-rehearing");
            const bool stay_trace =
                trace_id.startsWith(QStringLiteral("ca4m4.ellison.trace.stay-"));
            if (trace_id == QStringLiteral("ca4m4.ellison.trace.actual-ordinary") ||
                (adverse_trace && (code < QStringLiteral("b01") || code > QStringLiteral("b09"))) ||
                (stay_trace && (code < QStringLiteral("b10") || code > QStringLiteral("b20")))) {
                return QStringLiteral("actual/counterfactual trace document isolation drifted: %1")
                    .arg(file_name);
            }
        }
        total_commands += trace.value(QStringLiteral("command_count")).toInt();
        total_events += trace.value(QStringLiteral("event_count")).toInt();
    }

    const QHash<QString, QString> expected_deadline_dates{
        {QStringLiteral("ca4m4.ellison.deadline.opening-brief"), QStringLiteral("2026-03-16")},
        {QStringLiteral("ca4m4.ellison.deadline.response-brief"), QStringLiteral("2026-04-15")},
        {QStringLiteral("ca4m4.ellison.deadline.reply-brief"), QStringLiteral("2026-05-06")},
        {QStringLiteral("ca4m4.ellison.deadline.actual-rehearing-petition"),
         QStringLiteral("2026-07-14")},
        {QStringLiteral("ca4m4.ellison.deadline.actual-ordinary-mandate"),
         QStringLiteral("2026-07-21")},
        {QStringLiteral("ca4m4.ellison.deadline.adverse-rehearing-petition"),
         QStringLiteral("2026-07-14")},
        {QStringLiteral("ca4m4.ellison.deadline.adverse-ordinary-mandate"),
         QStringLiteral("2026-07-21")},
        {QStringLiteral("ca4m4.ellison.deadline.actual-post-stay-denial-mandate"),
         QStringLiteral("2026-07-21")},
        {QStringLiteral("ca4m4.ellison.deadline.actual-stay-through"),
         QStringLiteral("2026-08-10")},
        {QStringLiteral("ca4m4.ellison.deadline.actual-post-dissolution-mandate"),
         QStringLiteral("2026-08-11")},
        {QStringLiteral("ca4m4.ellison.deadline.adverse-post-rehearing-mandate"),
         QStringLiteral("2026-08-11")},
    };
    if (trace_ids.size() != 6 || evidence_ids.size() != 6 || total_commands != 229 ||
        total_events != 229 || rejection_count != 14 || accepted_filing_ids.size() != 14 ||
        rejected_filing_ids != accepted_filing_ids ||
        executed_operations != workflow_operation_ids ||
        concrete_deadline_ids !=
            QSet<QString>(expected_deadline_dates.keyBegin(), expected_deadline_dates.keyEnd()) ||
        deadline_dates != expected_deadline_dates || court_document_entries.size() != 24 ||
        terminal_counts.value(QStringLiteral("ca4m4.ellison.stage.actual-mandate-stayed")) != 1 ||
        terminal_counts.value(QStringLiteral("ca4m4.ellison.stage.terminated")) != 5) {
        return QStringLiteral("six-trace closure drifted: traces=%1 evidence=%2 commands=%3 "
                              "events=%4 rejects=%5 operations=%6 deadlines=%7 docs=%8 "
                              "endpoints=%9/%10")
            .arg(trace_ids.size())
            .arg(evidence_ids.size())
            .arg(total_commands)
            .arg(total_events)
            .arg(rejection_count)
            .arg(executed_operations.size())
            .arg(concrete_deadline_ids.size())
            .arg(court_document_entries.size())
            .arg(terminal_counts.value(QStringLiteral("ca4m4.ellison.stage.actual-mandate-stayed")))
            .arg(terminal_counts.value(QStringLiteral("ca4m4.ellison.stage.terminated")));
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
            QStringLiteral("us.ca4.m4.ellison-immunity") ||
        manifest->value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        manifest->value(QStringLiteral("contents")).toArray().size() != 9 ||
        manifest->value(QStringLiteral("blobs")).toArray().size() != 72 ||
        manifest->value(QStringLiteral("required_capabilities")).toArray().size() != 16 ||
        manifest->value(QStringLiteral("dependencies")).toArray().size() != 3 ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 35 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 72 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 6) {
        return QStringLiteral("final 9-resource/72-blob/16-capability evidence envelope drifted");
    }

    const auto dimensions = review->value(QStringLiteral("dimensions")).toObject();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const QSet<QString> expected_dimensions{
        QStringLiteral("procedural_law"),     QStringLiteral("deadlines_authority"),
        QStringLiteral("record_consistency"), QStringLiteral("consequences"),
        QStringLiteral("oral_argument"),      QStringLiteral("bench_differentiation"),
        QStringLiteral("provenance"),
    };
    QSet<QString> dimension_keys;
    QSet<QString> dimension_evidence_keys;
    bool dimensions_are_two = true;
    for (auto iterator = dimensions.constBegin(); iterator != dimensions.constEnd(); ++iterator) {
        dimension_keys.insert(iterator.key());
        dimensions_are_two = dimensions_are_two && iterator.value().toInt() == 2;
    }
    for (auto iterator = dimension_evidence.constBegin(); iterator != dimension_evidence.constEnd();
         ++iterator) {
        dimension_evidence_keys.insert(iterator.key());
    }
    QSet<QString> evidence_ids;
    const std::array evidence_groups{
        evidence.value(QStringLiteral("resources")).toArray(),
        evidence.value(QStringLiteral("blobs")).toArray(),
        evidence.value(QStringLiteral("traces")).toArray(),
        evidence.value(QStringLiteral("record_checks")).toArray(),
        evidence.value(QStringLiteral("authorities")).toArray(),
    };
    for (const auto& group : evidence_groups) {
        for (const auto& value : group) {
            const auto id = value.toObject().value(QStringLiteral("evidence_id")).toString();
            if (id.isEmpty() || evidence_ids.contains(id)) {
                return QStringLiteral("realism evidence IDs are empty or duplicated");
            }
            evidence_ids.insert(id);
        }
    }
    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("procedural_law"), 49},     {QStringLiteral("deadlines_authority"), 19},
        {QStringLiteral("record_consistency"), 75}, {QStringLiteral("consequences"), 33},
        {QStringLiteral("oral_argument"), 17},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 114},
    };
    if (dimension_keys != expected_dimensions || dimension_evidence_keys != expected_dimensions ||
        !dimensions_are_two || evidence_ids.size() != 159) {
        return QStringLiteral("realism dimensions or 159-evidence-ID closure drifted");
    }
    for (const auto& dimension : expected_dimensions) {
        const auto references = dimension_evidence.value(dimension).toArray();
        const auto unique = strings(references);
        if (references.size() != expected_dimension_counts.value(dimension) ||
            unique.size() != references.size() || std::ranges::any_of(unique, [&](const auto& id) {
                return !evidence_ids.contains(id);
            })) {
            return QStringLiteral("realism dimension evidence drifted: %1").arg(dimension);
        }
    }

    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.ellison-immunity"}, "1.2.0",
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
        loaded->resources.size() != std::size_t{9} || loaded->blobs.size() != std::size_t{72}) {
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
    const auto archive_a = QDir(temporary.path()).filePath(QStringLiteral("ellison-a.awpack"));
    const auto archive_b = QDir(temporary.path()).filePath(QStringLiteral("ellison-b.awpack"));
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
        return QStringLiteral("resolved Ellison runtime cannot be constructed");
    }
    const auto& runtime_case = runtime->cases.front();
    if (runtime_case.definition.id.value != "ca4m4.case.ellison-immunity" ||
        runtime_case.workflow.stages.size() != std::size_t{20} ||
        runtime_case.workflow.operations.size() != std::size_t{77} ||
        runtime_case.workflow.filing_routes.size() != std::size_t{11} ||
        runtime_case.record.docket_entries.size() != std::size_t{72} ||
        runtime_case.record.page_anchors.size() != std::size_t{449} ||
        runtime_case.argument_configurations.size() != std::size_t{2}) {
        return QStringLiteral("resolved runtime 20/77/11/72/449/two-bank shape drifted");
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

        auto service_tamper = *decoded;
        bool changed_service = false;
        for (auto& entry : service_tamper) {
            auto* filing = std::get_if<model::SubmitWorkflowFiling>(&entry.command);
            if (filing != nullptr && !filing->served_actors.empty() && !entry.events.empty() &&
                eventHeader(entry.events.front())
                    .operation_id.value.starts_with("ca4m4.ellison.operation.accept-")) {
                filing->served_actors.clear();
                changed_service = true;
                break;
            }
        }
        if (!changed_service ||
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   service_tamper)) {
            return QStringLiteral("required-service tamper replayed: %1").arg(file_name);
        }

        auto order_id_tamper = *decoded;
        bool changed_order_id = false;
        for (auto& entry : order_id_tamper) {
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->order_id = model::WorkflowOrderId{"ca4m4.ellison.order.tampered"};
                changed_order_id = true;
                break;
            }
        }
        if (!changed_order_id ||
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   order_id_tamper)) {
            return QStringLiteral("order-ID tamper replayed: %1").arg(file_name);
        }

        auto disposition_tamper = *decoded;
        bool changed_disposition = false;
        for (auto& entry : disposition_tamper) {
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->disposition = order->disposition == model::WorkflowOrderDisposition::Granted
                                         ? model::WorkflowOrderDisposition::Denied
                                         : model::WorkflowOrderDisposition::Granted;
                changed_disposition = true;
                break;
            }
        }
        if (!changed_disposition ||
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial,
                                   disposition_tamper)) {
            return QStringLiteral("order-disposition tamper replayed: %1").arg(file_name);
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
    const QDir root(QStringLiteral(APPELLATE_M4_ELLISON_ROOT));
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
    std::cout << "Ellison immunity integration passed: 9 resources / 72 PDFs / 449 anchors; "
                 "20 stages / 77 operations / 11 routes; 6 traces / 229 commands / 229 events.\n";
    return 0;
}
