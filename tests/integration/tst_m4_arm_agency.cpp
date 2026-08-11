#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_ARM_ROOT
#error "APPELLATE_M4_ARM_ROOT must name content/m4/arm-agency"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

using appellate::model::PackId;
using appellate::model::PackRevision;
using appellate::model::ResourceKind;
using appellate::packs::PackArchive;
using appellate::packs::PackCatalog;
using appellate::packs::PackGraphState;
using appellate::packs::PackReader;
using appellate::packs::PackValidationScope;
using appellate::packs::ValidatedResource;

constexpr auto root_digest = "37788a776fc41ec5028ab28b703e647220da8360cf53ed7aabc41e351bbbf963";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto archive_digest = "43a70affac25abd23702c16908e876fe6de3eae987ac44f810db11d12fb68f85";

[[nodiscard]] QByteArray readAll(const QString& file_name) {
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
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

[[nodiscard]] appellate::model::LegalTime legalTime(int year, unsigned month, unsigned day) {
    const auto date = std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};
    return appellate::model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{date}},
                                       appellate::model::LegalDate{date}};
}

[[nodiscard]] appellate::model::WorkflowCommandHeader commandHeader(std::string command_id) {
    return appellate::model::WorkflowCommandHeader{
        "ca4m4.arm.session.negative-gates",
        appellate::model::WorkflowCommandId{std::move(command_id)},
        appellate::model::ActorId{"ca4m4.arm.actor.composite-panel"}, legalTime(2026, 8U, 11U)};
}

[[nodiscard]] appellate::model::WorkflowCommandHeader
positiveCommandHeader(std::string command_id, std::string actor_id, int year, unsigned month,
                      unsigned day) {
    return appellate::model::WorkflowCommandHeader{
        "ca4m4.arm.session.positive-path",
        appellate::model::WorkflowCommandId{std::move(command_id)},
        appellate::model::ActorId{std::move(actor_id)}, legalTime(year, month, day)};
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto authoring_root = QDir(QStringLiteral(APPELLATE_M4_ARM_ROOT));
    const auto pack_root = authoring_root.filePath(QStringLiteral("pack"));
    const auto foundations_root = QDir(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    const PackRevision expected_root{PackId{"us.ca4.m4.arm-agency"}, "1.0.0", root_digest};
    const PackRevision expected_federal{PackId{"foundation.us-federal"}, "2025.12.01",
                                        federal_digest};
    const PackRevision expected_ca4{PackId{"foundation.us-ca4"}, "2026.03.23", ca4_digest};
    const PackRevision expected_bench{PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                                      bench_digest};

    const auto source = PackReader::readDirectory(pack_root, PackValidationScope::ResolvedClosure);
    if (!source) {
        return fail(QStringLiteral("source pack: %1").arg(source.error().message));
    }
    if (source->revision != expected_root ||
        source->graph_state != PackGraphState::DeferredReferences ||
        source->dependencies.size() != std::size_t{3} ||
        source->required_capabilities.size() != std::size_t{8} ||
        source->resources.size() != std::size_t{8} || source->blobs.size() != std::size_t{7}) {
        return fail(QStringLiteral("source pack revision/count contract mismatch"));
    }

    const auto readme =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral("README.md"))))
            .simplified();
    if (!readme.contains(QStringLiteral("incomplete, pre-release")) ||
        !readme.contains(QStringLiteral("not a releasable pack")) ||
        !readme.contains(QStringLiteral("six AR-labeled agency PDFs")) ||
        !readme.contains(QStringLiteral("AR1–AR72")) ||
        !readme.contains(QStringLiteral("PA1–PA8")) ||
        !readme.contains(QStringLiteral("not counted toward the 18-PDF/238-page")) ||
        !readme.contains(QStringLiteral("Two minimal argument configurations")) ||
        !readme.contains(QStringLiteral("No appellate result or realism level")) ||
        !readme.contains(QStringLiteral("default no-rehearing-petition/no-stay branch"))) {
        return fail(QStringLiteral("README does not preserve the incomplete boundary"));
    }

    const auto documents = QDir(authoring_root.filePath(QStringLiteral("documents/batch-1")));
    const auto markdown_names =
        documents.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    if (markdown_names.size() != 7) {
        return fail(QStringLiteral("batch 1 must have exactly seven rendered sources"));
    }
    const QString record_banner = QStringLiteral(
        "SYNTHETIC TRAINING RECORD — NOT FILED — ALL FACTS AND IDENTIFIERS ARE FICTIONAL");
    const QString proffer_banner = QStringLiteral(
        "SYNTHETIC TRAINING APPELLATE PROFFER — NOT ADMINISTRATIVE RECORD — ALL FACTS ARE "
        "FICTIONAL");
    const QStringList forbidden_authoring_voice{
        QStringLiteral("initial appellate certification"),
        QStringLiteral("corrected administrative record"),
        QStringLiteral("later correction restores"),
        QStringLiteral("current workflow operation"),
        QStringLiteral("eventual authored disposition"),
        QStringLiteral("proof root"),
        QStringLiteral("renderer"),
        QStringLiteral("workbench"),
        QStringLiteral("authoring note"),
        QStringLiteral("what materials were transmitted to the court of appeals"),
        QStringLiteral("later master-calendar order"),
        QStringLiteral("appellate filing clocks"),
        QStringLiteral("record on petition for review"),
        QStringLiteral("later admitted"),
    };
    const QRegularExpression compiled_ar_label(QStringLiteral("\\bAR\\d+\\b"));
    for (const auto& markdown_name : markdown_names) {
        const auto raw = QString::fromUtf8(readAll(documents.filePath(markdown_name)));
        const auto expected_banner =
            markdown_name.startsWith(QStringLiteral("pa")) ? proffer_banner : record_banner;
        const auto newline = raw.indexOf(QLatin1Char('\n'));
        const auto body = raw.mid(newline + 1);
        if (!raw.startsWith(expected_banner + QLatin1Char('\n')) ||
            raw.count(expected_banner) != 1 || newline < 0 ||
            compiled_ar_label.match(body).hasMatch()) {
            return fail(
                QStringLiteral("source safety/temporal boundary mismatch: %1").arg(markdown_name));
        }
        const auto lower_body = body.toLower();
        for (const auto& phrase : forbidden_authoring_voice) {
            if (lower_body.contains(phrase)) {
                return fail(QStringLiteral("inline authoring voice leaked into %1: %2")
                                .arg(markdown_name, phrase));
            }
        }
    }

    const std::array expected_dependencies{expected_federal, expected_ca4, expected_bench};
    for (const auto& expected : expected_dependencies) {
        if (std::ranges::find(source->dependencies, expected, [](const auto& dependency) {
                return dependency.revision;
            }) == source->dependencies.end()) {
            return fail(QStringLiteral("missing exact dependency %1")
                            .arg(QString::fromStdString(expected.id.value)));
        }
    }

    const auto grounded_capability =
        std::ranges::find_if(source->required_capabilities, [](const auto& capability) {
            return capability.id == "workbench.pack.grounded-questions" && capability.version == 1U;
        });
    const auto dependent_deadline_capability =
        std::ranges::find_if(source->required_capabilities, [](const auto& capability) {
            return capability.id == "workbench.pack.dependent-deadlines" &&
                   capability.version == 1U;
        });
    const auto named_deadline_capability =
        std::ranges::find_if(source->required_capabilities, [](const auto& capability) {
            return capability.id == "workbench.pack.named-deadlines" && capability.version == 1U;
        });
    const auto event_date_deadline_capability =
        std::ranges::find_if(source->required_capabilities, [](const auto& capability) {
            return capability.id == "workbench.pack.event-date-deadlines" &&
                   capability.version == 1U;
        });
    const auto argument_date_capability =
        std::ranges::find_if(source->required_capabilities, [](const auto& capability) {
            return capability.id == "workbench.pack.argument-date-guards" &&
                   capability.version == 1U;
        });
    if (grounded_capability == source->required_capabilities.end() ||
        dependent_deadline_capability == source->required_capabilities.end() ||
        named_deadline_capability == source->required_capabilities.end() ||
        event_date_deadline_capability == source->required_capabilities.end() ||
        argument_date_capability == source->required_capabilities.end() ||
        std::ranges::any_of(source->required_capabilities,
                            [](const auto& capability) {
                                return capability.id == "workbench.pack.structured-disposition" ||
                                       capability.id == "workbench.pack.realism-evidence";
                            }) ||
        std::ranges::any_of(source->resources, [](const auto& resource) {
            return resource.descriptor.kind == ResourceKind::RealismReview;
        })) {
        return fail(QStringLiteral("grounded/deferred capability boundary mismatch"));
    }

    for (const auto& resource : source->resources) {
        const auto bytes =
            readAll(QDir(pack_root).filePath(QString::fromStdString(resource.descriptor.path)));
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        if (bytes.isEmpty() || digest != QByteArray::fromStdString(resource.descriptor.sha256)) {
            return fail(QStringLiteral("resource descriptor mismatch: %1")
                            .arg(QString::fromStdString(resource.descriptor.path)));
        }
    }
    for (const auto& blob : source->blobs) {
        const auto bytes = readAll(QDir(pack_root).filePath(QString::fromStdString(blob.path)));
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        if (bytes.size() != static_cast<qsizetype>(blob.byte_size) ||
            digest != QByteArray::fromStdString(blob.sha256)) {
            return fail(QStringLiteral("blob bytes/digest mismatch: %1")
                            .arg(QString::fromStdString(blob.path)));
        }
    }

    const auto* case_resource = findResource(source->resources, "ca4m4.case.arm-agency");
    const auto* record_resource = findResource(source->resources, "ca4m4.arm.record");
    const auto* authority_resource =
        findResource(source->resources, "ca4m4.arm.authorities.case-specific");
    const auto* workflow_resource =
        findResource(source->resources, "ca4m4.arm.workflow.agency-review");
    const auto* bench_resource = findResource(source->resources, "ca4m4.arm.bench.three-judge");
    const auto* actual_argument =
        findResource(source->resources, "ca4m4.arm.argument.actual-record");
    const auto* counterfactual_argument =
        findResource(source->resources, "ca4m4.arm.argument.counterfactual");
    if (case_resource == nullptr || record_resource == nullptr || authority_resource == nullptr ||
        workflow_resource == nullptr || bench_resource == nullptr || actual_argument == nullptr ||
        counterfactual_argument == nullptr ||
        case_resource->descriptor.kind != ResourceKind::Case ||
        record_resource->descriptor.kind != ResourceKind::Record ||
        actual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        counterfactual_argument->descriptor.kind != ResourceKind::ArgumentConfig) {
        return fail(QStringLiteral("required ARM resources are absent"));
    }

    if (case_resource->document.contains(QStringLiteral("disposition_plans")) ||
        case_resource->document.contains(QStringLiteral("authored_disposition_plan_id"))) {
        return fail(QStringLiteral("batch 1 must not contain a structured disposition"));
    }
    for (const auto& issue_value :
         case_resource->document.value(QStringLiteral("issues")).toArray()) {
        if (issue_value.toObject().contains(QStringLiteral("target_ids"))) {
            return fail(QStringLiteral("deferred disposition target leaked into batch 1"));
        }
    }
    for (const auto& actor_value :
         case_resource->document.value(QStringLiteral("actors")).toArray()) {
        if (!actor_value.toObject().value(QStringLiteral("synthetic")).toBool()) {
            return fail(QStringLiteral("case actor is not explicitly synthetic"));
        }
    }
    const auto case_issues = case_resource->document.value(QStringLiteral("issues")).toArray();
    if (case_issues.size() != 5) {
        return fail(QStringLiteral("ARM issue matrix must contain five current issues"));
    }

    const auto check_argument_bank = [&](const ValidatedResource& resource,
                                         const QString& expected_mode,
                                         const QString& expected_digest) -> std::optional<QString> {
        const auto document = resource.document;
        const auto permitted =
            strings(document.value(QStringLiteral("permitted_issue_ids")).toArray());
        const auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
        const auto bindings = bank.value(QStringLiteral("issue_topic_bindings")).toArray();
        const auto questions = bank.value(QStringLiteral("questions")).toArray();
        if (document.value(QStringLiteral("case_id")).toString() !=
                QStringLiteral("ca4m4.case.arm-agency") ||
            document.value(QStringLiteral("bench_configuration_id")).toString() !=
                QStringLiteral("ca4m4.arm.bench.three-judge") ||
            permitted.size() != 5 ||
            bank.value(QStringLiteral("mode")).toString() != expected_mode ||
            bank.value(QStringLiteral("grounding_digest")).toString() != expected_digest ||
            bindings.size() != 5 || questions.size() != 5) {
            return QStringLiteral("argument-bank envelope mismatch");
        }
        QSet<QString> bound_issues;
        QSet<QString> question_issues;
        QSet<QString> topics;
        bool saw_pa = false;
        bool saw_ar = false;
        for (const auto& binding_value : bindings) {
            const auto binding = binding_value.toObject();
            const auto issue = binding.value(QStringLiteral("issue_id")).toString();
            const auto bound_topics = binding.value(QStringLiteral("topic_ids")).toArray();
            if (!permitted.contains(issue) || bound_topics.size() != 1) {
                return QStringLiteral("argument-bank issue binding mismatch");
            }
            bound_issues.insert(issue);
            topics.insert(bound_topics.at(0).toString());
        }
        for (const auto& question_value : questions) {
            const auto question = question_value.toObject();
            const auto issue = question.value(QStringLiteral("issue_id")).toString();
            if (!permitted.contains(issue) ||
                question.value(QStringLiteral("prompt")).toString().isEmpty()) {
                return QStringLiteral("argument-bank question coverage mismatch");
            }
            question_issues.insert(issue);
            for (const auto& grounding_value :
                 question.value(QStringLiteral("grounding")).toArray()) {
                const auto grounding = grounding_value.toObject();
                const auto kind = grounding.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("record_page")) {
                    const auto anchor = grounding.value(QStringLiteral("anchor_id")).toString();
                    saw_pa = saw_pa || anchor.startsWith(QStringLiteral("ca4m4.arm.anchor.pa"));
                    saw_ar = saw_ar || anchor.startsWith(QStringLiteral("ca4m4.arm.anchor.ar"));
                } else if (kind != QStringLiteral("authority")) {
                    return QStringLiteral("argument bank uses noncanonical grounding kind");
                }
            }
        }
        const QSet<QString> expected_topics{
            QStringLiteral("workbench.topic.record-support"),
            QStringLiteral("workbench.topic.governing-authority"),
            QStringLiteral("workbench.topic.merits"),
            QStringLiteral("workbench.topic.standard-of-review"),
            QStringLiteral("workbench.topic.jurisdiction"),
        };
        if (bound_issues != permitted || question_issues != permitted ||
            topics != expected_topics || !saw_ar || !saw_pa) {
            return QStringLiteral("argument bank is not grounded across the five-issue matrix");
        }
        return std::nullopt;
    };
    if (const auto error = check_argument_bank(
            *actual_argument, QStringLiteral("actual_record"),
            QStringLiteral("42723995341868f238d07f4b3ca69c7a562bcc679f2ba6f5aa2bb317f550088f"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = check_argument_bank(
            *counterfactual_argument, QStringLiteral("counterfactual_training"),
            QStringLiteral("35c097a6c824451c1315b994a39cc3a957e5c4c0f73a0c0a2b8927dea784e721"));
        error.has_value()) {
        return fail(*error);
    }
    const auto counterfactual_text = QString::fromUtf8(
        QJsonDocument(counterfactual_argument->document).toJson(QJsonDocument::Compact));
    if (!counterfactual_text.contains(QStringLiteral("day 31")) ||
        !counterfactual_text.contains(QStringLiteral("timely invoked"))) {
        return fail(QStringLiteral("day-31 invocation remains ungrounded or implicit"));
    }

    const auto authorities =
        authority_resource->document.value(QStringLiteral("authorities")).toArray();
    if (authorities.size() != 11) {
        return fail(QStringLiteral("case-specific authority count is not eleven"));
    }
    bool saw_rodriguez_part_four = false;
    for (const auto& authority_value : authorities) {
        const auto authority = authority_value.toObject();
        if (!authority.value(QStringLiteral("official_source")).toBool() ||
            authority.value(QStringLiteral("checked_on")).toString() !=
                QStringLiteral("2026-08-11") ||
            authority.value(QStringLiteral("source_version")).toString().isEmpty() ||
            authority.value(QStringLiteral("locator")).toString().isEmpty() ||
            authority.value(QStringLiteral("source_url")).toString().isEmpty() ||
            authority.value(QStringLiteral("proposition")).toString().isEmpty()) {
            return fail(QStringLiteral("canonical authority provenance contract mismatch"));
        }
        if (authority.value(QStringLiteral("authority_id")).toString() ==
            QStringLiteral("ca4m4.arm.authority.rodriguez-arias-consideration")) {
            const auto locator = authority.value(QStringLiteral("locator")).toString();
            saw_rodriguez_part_four = locator.contains(QStringLiteral("Part IV")) &&
                                      locator.contains(QStringLiteral("977-980")) &&
                                      locator.contains(QStringLiteral("pages 10-13"));
        }
    }
    if (!saw_rodriguez_part_four) {
        return fail(QStringLiteral("Rodriguez-Arias Part IV locator drifted"));
    }

    const auto dockets = record_resource->document.value(QStringLiteral("dockets")).toArray();
    const auto entries =
        record_resource->document.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record_resource->document.value(QStringLiteral("page_anchors")).toArray();
    if (dockets.size() != 2 || entries.size() != 7 || anchors.size() != 80) {
        return fail(QStringLiteral("record count contract mismatch"));
    }

    QHash<QString, QJsonObject> anchor_by_label;
    for (const auto& anchor_value : anchors) {
        const auto anchor = anchor_value.toObject();
        const auto label = anchor.value(QStringLiteral("citation_label")).toString();
        if (label.isEmpty() || anchor_by_label.contains(label)) {
            return fail(QStringLiteral("duplicate or empty page-anchor label"));
        }
        anchor_by_label.insert(label, anchor);
    }

    const QRegularExpression any_page_label(QStringLiteral("\\b(?:AR|PA)\\d+\\b"));
    QSet<QString> distinct_page_bodies;
    int administrative_documents = 0;
    int administrative_pages = 0;
    int generated_documents = 0;
    int generated_pages = 0;
    int expected_ar = 1;
    int expected_pa = 1;
    bool saw_disputed_p7 = false;
    bool saw_new_proffer = false;

    for (const auto& entry_value : entries) {
        const auto entry = entry_value.toObject();
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        const bool generated = tags.contains(QStringLiteral("generated_appellate_filing"));
        const bool administrative =
            !generated &&
            (tags.contains(QStringLiteral("certified_administrative_record")) ||
             tags.contains(QStringLiteral("planned_certified_administrative_record")));
        if (administrative == generated || tags.contains(QStringLiteral("batch_1"))) {
            return fail(QStringLiteral("entry is ambiguously classified as AR/generated"));
        }

        if (administrative) {
            ++administrative_documents;
            administrative_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (entry.value(QStringLiteral("docket_id")).toString() !=
                QStringLiteral("ca4m4.arm.docket.agency")) {
                return fail(QStringLiteral("administrative record entry is on wrong docket"));
            }
        } else {
            ++generated_documents;
            generated_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (!tags.contains(QStringLiteral("extra_record_proffer")) ||
                !tags.contains(QStringLiteral("not_administrative_record")) ||
                entry.value(QStringLiteral("docket_id")).toString() !=
                    QStringLiteral("ca4m4.arm.docket.ca4")) {
                return fail(QStringLiteral("generated PA proffer classification mismatch"));
            }
            saw_new_proffer = true;
        }

        if (entry.value(QStringLiteral("entry_id")).toString() ==
            QStringLiteral("ca4m4.arm.record.ar04")) {
            saw_disputed_p7 =
                tags.contains(QStringLiteral("claimed_admission")) &&
                tags.contains(QStringLiteral("claimed_initial_omission")) &&
                tags.contains(QStringLiteral("planned_corrected_record")) &&
                tags.contains(QStringLiteral("planned_certified_administrative_record")) &&
                !tags.contains(QStringLiteral("admitted")) &&
                !tags.contains(QStringLiteral("initially_omitted")) &&
                !tags.contains(QStringLiteral("corrected_record")) &&
                entry.value(QStringLiteral("description"))
                    .toString()
                    .contains(QStringLiteral("remain disputed"));
        }

        const auto relative_asset = entry.value(QStringLiteral("asset_path")).toString();
        QPdfDocument pdf;
        if (pdf.load(QDir(pack_root).filePath(relative_asset)) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready ||
            pdf.pageCount() != entry.value(QStringLiteral("page_count")).toInt()) {
            return fail(QStringLiteral("PDF load/page-count failure: %1").arg(relative_asset));
        }

        for (int page_index = 0; page_index < pdf.pageCount(); ++page_index) {
            const auto expected_label = administrative ? QStringLiteral("AR%1").arg(expected_ar++)
                                                       : QStringLiteral("PA%1").arg(expected_pa++);
            auto page_text = pdf.getAllText(page_index).text().simplified();
            if (page_text.size() < 500 || !page_text.contains(expected_label)) {
                return fail(QStringLiteral("thin or unlabeled searchable page %1 in %2")
                                .arg(expected_label, relative_asset));
            }
            const auto lower_page_text = page_text.toLower();
            for (const auto& phrase : forbidden_authoring_voice) {
                if (lower_page_text.contains(phrase)) {
                    return fail(QStringLiteral("rendered authoring voice leaked at %1: %2")
                                    .arg(expected_label, phrase));
                }
            }
            page_text.remove(any_page_label);
            page_text = page_text.simplified();
            if (distinct_page_bodies.contains(page_text)) {
                return fail(
                    QStringLiteral("duplicate substantive page body at %1").arg(expected_label));
            }
            distinct_page_bodies.insert(page_text);

            const auto anchor = anchor_by_label.value(expected_label);
            if (anchor.isEmpty() ||
                anchor.value(QStringLiteral("entry_id")).toString() !=
                    entry.value(QStringLiteral("entry_id")).toString() ||
                anchor.value(QStringLiteral("page_number")).toInt() != page_index + 1 ||
                anchor.value(QStringLiteral("anchor_id")).toString() !=
                    QStringLiteral("ca4m4.arm.anchor.%1").arg(expected_label.toLower())) {
                return fail(QStringLiteral("page-anchor mismatch at %1").arg(expected_label));
            }
        }
    }

    if (administrative_documents != 6 || administrative_pages != 72 || expected_ar != 73 ||
        generated_documents != 1 || generated_pages != 8 || expected_pa != 9 ||
        distinct_page_bodies.size() != 80 || !saw_disputed_p7 || !saw_new_proffer) {
        return fail(QStringLiteral("AR/PA count, continuity, or semantic distinction mismatch"));
    }

    const auto render_inventory =
        QJsonDocument::fromJson(readAll(authoring_root.filePath(
                                    QStringLiteral("metadata/render-inventory-batch-1.json"))))
            .object();
    const auto render_entries = render_inventory.value(QStringLiteral("entries")).toArray();
    if (render_inventory.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
        render_inventory.value(QStringLiteral("renderer_contract")).toString() !=
            QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
        render_entries.size() != 7) {
        return fail(QStringLiteral("single-render inventory contract mismatch"));
    }
    for (const auto& rendered_value : render_entries) {
        const auto rendered = rendered_value.toObject();
        const auto output_path = rendered.value(QStringLiteral("output_path")).toString();
        const auto assembly = rendered.value(QStringLiteral("assembly_provenance")).toObject();
        const auto source_path = assembly.value(QStringLiteral("source_path")).toString();
        const auto source_bytes = readAll(authoring_root.filePath(source_path));
        const auto source_digest =
            QCryptographicHash::hash(source_bytes, QCryptographicHash::Sha256).toHex();
        const auto pdf_bytes = readAll(QDir(pack_root).filePath(output_path));
        const auto pdf_digest =
            QCryptographicHash::hash(pdf_bytes, QCryptographicHash::Sha256).toHex();
        const auto blob = std::ranges::find(source->blobs, output_path.toStdString(),
                                            &appellate::model::BlobDescriptor::path);
        const auto record_entry = std::ranges::find_if(entries, [&](const auto& value) {
            return value.toObject().value(QStringLiteral("asset_path")).toString() == output_path;
        });
        if (source_bytes.isEmpty() || pdf_bytes.isEmpty() || blob == source->blobs.end() ||
            record_entry == entries.end() ||
            rendered.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
            rendered.value(QStringLiteral("source_sha256")).toString().toLatin1() !=
                source_digest ||
            assembly.value(QStringLiteral("source_path")).toString() != source_path ||
            assembly.value(QStringLiteral("source_sha256")).toString().toLatin1() !=
                source_digest ||
            rendered.value(QStringLiteral("pdf_sha256")).toString().toLatin1() != pdf_digest ||
            rendered.value(QStringLiteral("byte_size")).toInteger() != pdf_bytes.size() ||
            rendered.value(QStringLiteral("page_count")).toInt() !=
                record_entry->toObject().value(QStringLiteral("page_count")).toInt() ||
            blob->sha256 != pdf_digest.toStdString() ||
            blob->byte_size != static_cast<std::uint64_t>(pdf_bytes.size())) {
            return fail(QStringLiteral("render inventory does not pin current source/PDF bytes: %1")
                            .arg(output_path));
        }
    }

    const QJsonArray expected_seats{
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.arm.seat.rowan")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.rowan")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.arm.seat.reed")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.reed")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.arm.seat.quill")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.quill")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
    };
    if (bench_resource->document.value(QStringLiteral("presiding_seat_id")).toString() !=
            QStringLiteral("ca4m4.arm.seat.rowan") ||
        bench_resource->document.value(QStringLiteral("seats")).toArray() != expected_seats) {
        return fail(QStringLiteral("Rowan/Reed/Quill bench contract mismatch"));
    }

    bool saw_correction_order = false;
    bool saw_supplement_order = false;
    bool saw_docketing_clock = false;
    bool saw_briefing_complete_gate = false;
    bool saw_argument_schedule_gate = false;
    bool saw_argument_held_gate = false;
    bool saw_submitted_gate = false;
    bool saw_judgment_gate = false;
    bool saw_rehearing_clock = false;
    bool saw_mandate_wait_gate = false;
    bool saw_mandate_delay = false;
    bool saw_mandate_gate = false;
    int calculated_deadlines = 0;
    for (const auto& operation_value :
         workflow_resource->document.value(QStringLiteral("operations")).toArray()) {
        const auto operation = operation_value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        const auto authority = operation.value(QStringLiteral("authority")).toObject();
        if (operation.value(QStringLiteral("opcode")).toString() ==
            QStringLiteral("calculate_deadline")) {
            ++calculated_deadlines;
        }
        if (id == QStringLiteral("ca4m4.arm.operation.order-correct-record")) {
            saw_correction_order =
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("ca4m4.arm.authority.frap-16-record") &&
                operation.value(QStringLiteral("authorized_role_ids")).toArray() ==
                    QJsonArray{QStringLiteral("us.ca4.role.court")};
        }
        if (id == QStringLiteral("ca4m4.arm.operation.order-deny-supplement")) {
            saw_supplement_order =
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("ca4m4.arm.authority.usc-1252-record-limit") &&
                operation.value(QStringLiteral("authorized_role_ids")).toArray() ==
                    QJsonArray{QStringLiteral("us.ca4.role.court")};
        }
        const auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
        const auto precondition_text =
            QString::fromUtf8(QJsonDocument(preconditions).toJson(QJsonDocument::Compact));
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-docketing")) {
            saw_docketing_clock =
                operation.value(QStringLiteral("deadline_days")).toInt() == 14 &&
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("us.ca4.authority.local-rule-3b-docketing") &&
                precondition_text.contains(
                    QStringLiteral("us.ca4.filing.agency-petition-for-review"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.order-briefing-complete")) {
            saw_briefing_complete_gate =
                precondition_text.contains(QStringLiteral("us.ca4.filing.principal-brief"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.schedule-argument")) {
            saw_argument_schedule_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.briefing-complete"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.order-argument-held")) {
            saw_argument_held_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.briefing-complete")) &&
                precondition_text.contains(QStringLiteral("argument_scheduled")) &&
                precondition_text.contains(QStringLiteral("argument_date_status")) &&
                precondition_text.contains(QStringLiteral("reached"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.advance-submitted")) {
            saw_submitted_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.briefing-complete")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.argument-held")) &&
                !precondition_text.contains(QStringLiteral("filing_presence")) &&
                !precondition_text.contains(QStringLiteral("argument_scheduled"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.issue-judgment")) {
            saw_judgment_gate =
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.rehearing") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.briefing-complete")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.argument-held"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-rehearing-deadline")) {
            saw_rehearing_clock =
                operation.value(QStringLiteral("stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.rehearing") &&
                operation.value(QStringLiteral("deadline_days")).toInt() == 45 &&
                operation.value(QStringLiteral("produced_deadline_id")).toString() ==
                    QStringLiteral("ca4m4.arm.deadline.rehearing") &&
                operation.value(QStringLiteral("deadline_event_base"))
                        .toObject()
                        .value(QStringLiteral("kind"))
                        .toString() == QStringLiteral("judgment_occurred") &&
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("us.federal.authority.frap-40-rehearing") &&
                precondition_text.contains(QStringLiteral("judgment_issued"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.advance-mandate-wait")) {
            saw_mandate_wait_gate =
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-wait") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.rehearing")) &&
                precondition_text.contains(QStringLiteral("elapsed"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-mandate-no-petition")) {
            saw_mandate_delay =
                operation.value(QStringLiteral("stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-wait") &&
                operation.value(QStringLiteral("deadline_days")).toInt() == 7 &&
                operation.value(QStringLiteral("deadline_base_id")).toString() ==
                    QStringLiteral("ca4m4.arm.deadline.rehearing") &&
                operation.value(QStringLiteral("produced_deadline_id")).toString() ==
                    QStringLiteral("ca4m4.arm.deadline.mandate-no-petition") &&
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("us.federal.authority.frap-41-mandate") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.rehearing")) &&
                precondition_text.contains(QStringLiteral("elapsed"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.issue-mandate-no-petition")) {
            saw_mandate_gate =
                operation.value(QStringLiteral("stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-wait") &&
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-issued") &&
                precondition_text.contains(QStringLiteral("judgment_issued")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.rehearing")) &&
                precondition_text.contains(
                    QStringLiteral("ca4m4.arm.deadline.mandate-no-petition")) &&
                precondition_text.count(QStringLiteral("elapsed")) == 1 &&
                precondition_text.contains(QStringLiteral("reached"));
        }
    }
    if (calculated_deadlines != 3 || !saw_docketing_clock || !saw_correction_order ||
        !saw_supplement_order || !saw_briefing_complete_gate || !saw_argument_schedule_gate ||
        !saw_argument_held_gate || !saw_submitted_gate || !saw_judgment_gate ||
        !saw_rehearing_clock || !saw_mandate_wait_gate || !saw_mandate_delay || !saw_mandate_gate) {
        return fail(QStringLiteral("record/order/timing workflow contract mismatch"));
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return fail(QStringLiteral("cannot create temporary validation directory"));
    }
    const auto archive_a = QDir(temporary.path()).filePath(QStringLiteral("arm-a.awpack"));
    const auto archive_b = QDir(temporary.path()).filePath(QStringLiteral("arm-b.awpack"));
    const auto exported_a = PackArchive::exportDirectory(pack_root, archive_a, {},
                                                         PackValidationScope::ResolvedClosure);
    const auto exported_b = PackArchive::exportDirectory(pack_root, archive_b, {},
                                                         PackValidationScope::ResolvedClosure);
    if (!exported_a || !exported_b || *exported_a != expected_root ||
        *exported_b != expected_root || readAll(archive_a).isEmpty() ||
        readAll(archive_a) != readAll(archive_b) ||
        QCryptographicHash::hash(readAll(archive_a), QCryptographicHash::Sha256).toHex() !=
            QByteArray(archive_digest)) {
        return fail(QStringLiteral("deferred archive export is not stable"));
    }
    const auto imported =
        PackArchive::importArchive(archive_a, {}, PackValidationScope::ResolvedClosure);
    if (!imported || imported->revision != source->revision ||
        imported->resources.size() != source->resources.size() ||
        imported->blobs != source->blobs) {
        return fail(QStringLiteral("directory/archive descriptor equality mismatch"));
    }

    const auto catalog_result =
        PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    if (!catalog_result) {
        return fail(QStringLiteral("catalog open: %1").arg(catalog_result.error().message));
    }
    auto& catalog = *catalog_result;
    const auto federal_archive = foundations_root.filePath(
        QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack"));
    const auto ca4_archive =
        foundations_root.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack"));
    const auto bench_archive = foundations_root.filePath(
        QStringLiteral("us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack"));
    const auto installed_federal =
        catalog->installArchive(federal_archive, QStringLiteral("2026-08-11T00:00:00Z"));
    const auto installed_ca4 =
        catalog->installArchive(ca4_archive, QStringLiteral("2026-08-11T00:00:01Z"));
    const auto installed_bench =
        catalog->installArchive(bench_archive, QStringLiteral("2026-08-11T00:00:02Z"));
    const auto installed_root =
        catalog->installArchive(archive_a, QStringLiteral("2026-08-11T00:00:03Z"));
    if (!installed_federal || !installed_ca4 || !installed_bench || !installed_root ||
        installed_federal->revision != expected_federal ||
        installed_ca4->revision != expected_ca4 || installed_bench->revision != expected_bench ||
        installed_root->revision != expected_root) {
        return fail(QStringLiteral("exact catalog installation failed"));
    }

    const auto resolved = catalog->loadResolved(expected_root);
    if (!resolved || resolved->root().revision != expected_root ||
        resolved->revisionsByPackId().size() != std::size_t{4} ||
        resolved->resourceOwner("us.ca4.court.appeals") !=
            std::optional<PackRevision>{expected_ca4} ||
        resolved->resourceOwner("us.ca4.bench-profile.rowan") !=
            std::optional<PackRevision>{expected_bench} ||
        resolved->resourceOwner("us.federal.authorities.appellate-rules") !=
            std::optional<PackRevision>{expected_federal} ||
        resolved->resourceOwner("ca4m4.arm.record") != std::optional<PackRevision>{expected_root}) {
        return fail(QStringLiteral("resolved graph does not match exact pins"));
    }

    const auto runtime = appellate::packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != expected_root || runtime->cases.size() != std::size_t{1} ||
        runtime->cases.front().argument_configurations.size() != std::size_t{2} ||
        std::ranges::any_of(
            runtime->cases.front().argument_configurations, [](const auto& configuration) {
                return !configuration.grounded_question_bank.has_value() ||
                       configuration.permitted_issue_ids.size() != std::size_t{5} ||
                       configuration.grounded_question_bank->questions.size() != std::size_t{5};
            })) {
        return fail(QStringLiteral("catalog-valid ARM closure is not runtime-loadable"));
    }

    const auto& runtime_case = runtime->cases.front();
    appellate::model::WorkflowState briefing_state;
    briefing_state.session_id = "ca4m4.arm.session.negative-gates";
    briefing_state.workflow_id = runtime_case.workflow.id;
    briefing_state.current_stage_id = appellate::model::WorkflowStageId{"ca4m4.arm.stage.briefing"};
    briefing_state.next_event_sequence = 2;
    briefing_state.decided_commands.push_back(
        appellate::model::WorkflowCommandId{"ca4m4.arm.command.snapshot-briefing"});
    briefing_state.legal_time_cursor = legalTime(2026, 8U, 11U);
    briefing_state.accepted_filings.push_back(appellate::model::WorkflowFilingRecord{
        appellate::model::WorkflowFilingId{"ca4m4.arm.filing.one-principal-brief"},
        appellate::model::FilingTypeId{"us.ca4.filing.principal-brief"},
        appellate::model::ActorId{"ca4m4.arm.actor.petitioner"},
        std::string(64, 'a'),
        legalTime(2026, 8U, 10U),
        {appellate::model::ActorId{"ca4m4.arm.actor.respondent"}},
    });
    const auto early_schedule = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, briefing_state,
        appellate::model::WorkflowCommand{appellate::model::ScheduleWorkflowArgument{
            commandHeader("ca4m4.arm.command.premature-schedule"),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.schedule-argument"},
            legalTime(2026, 8U, 12U).court_date}});
    if (early_schedule) {
        return fail(QStringLiteral("argument scheduling bypasses briefing-complete order"));
    }
    if (early_schedule.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("argument schedule negative gate returned: %1")
                        .arg(QString::fromStdString(early_schedule.error().message)));
    }

    auto scheduled_briefing_state = briefing_state;
    scheduled_briefing_state.argument_date = legalTime(2026, 8U, 11U).court_date;
    const auto early_submit = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, scheduled_briefing_state,
        appellate::model::WorkflowCommand{appellate::model::AdvanceWorkflowStage{
            commandHeader("ca4m4.arm.command.premature-submit"),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.advance-submitted"}}});
    if (early_submit ||
        early_submit.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("one principal brief/scheduled argument bypasses completion"));
    }

    auto submitted_state = scheduled_briefing_state;
    submitted_state.current_stage_id =
        appellate::model::WorkflowStageId{"ca4m4.arm.stage.submitted"};
    const auto early_judgment = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, submitted_state,
        appellate::model::WorkflowCommand{appellate::model::IssueWorkflowJudgment{
            commandHeader("ca4m4.arm.command.premature-judgment"),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.issue-judgment"},
            std::string(64, 'b'), std::string("premature judgment")}});
    if (early_judgment ||
        early_judgment.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("scheduled argument bypasses argument-held order"));
    }

    appellate::model::WorkflowState mandate_state;
    mandate_state.session_id = "ca4m4.arm.session.negative-gates";
    mandate_state.workflow_id = runtime_case.workflow.id;
    mandate_state.current_stage_id =
        appellate::model::WorkflowStageId{"ca4m4.arm.stage.mandate-wait"};
    mandate_state.next_event_sequence = 2;
    mandate_state.decided_commands.push_back(
        appellate::model::WorkflowCommandId{"ca4m4.arm.command.snapshot-judgment"});
    mandate_state.legal_time_cursor = legalTime(2026, 8U, 11U);
    mandate_state.judgment_sha256 = std::string(64, 'c');
    mandate_state.judgment_disposition = std::string("judgment entered");
    mandate_state.judgment_issued_at = legalTime(2026, 8U, 10U);
    const auto early_mandate = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, mandate_state,
        appellate::model::WorkflowCommand{appellate::model::IssueWorkflowMandate{
            commandHeader("ca4m4.arm.command.premature-mandate"),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.issue-mandate-no-petition"},
            std::string(64, 'd')}});
    if (early_mandate ||
        early_mandate.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("judgment alone bypasses rehearing/mandate delay guards"));
    }

    appellate::model::WorkflowState holiday_roll_state;
    holiday_roll_state.session_id = "ca4m4.arm.session.holiday-roll";
    holiday_roll_state.workflow_id = runtime_case.workflow.id;
    holiday_roll_state.current_stage_id =
        appellate::model::WorkflowStageId{"ca4m4.arm.stage.mandate-wait"};
    holiday_roll_state.next_event_sequence = 2;
    holiday_roll_state.decided_commands.push_back(
        appellate::model::WorkflowCommandId{"ca4m4.arm.command.holiday-snapshot"});
    holiday_roll_state.legal_time_cursor = legalTime(2026, 6U, 26U);
    holiday_roll_state.judgment_sha256 = std::string(64, 'e');
    holiday_roll_state.judgment_disposition = std::string("judgment entered");
    holiday_roll_state.judgment_issued_at = legalTime(2026, 5U, 12U);
    holiday_roll_state.deadlines.push_back(appellate::model::WorkflowDeadlineRecord{
        appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.rehearing"},
        appellate::model::WorkflowDeadlinePurpose::Filing,
        legalTime(2026, 6U, 26U).court_date,
        appellate::model::WorkflowDeadlineStatus::Open,
    });
    const auto holiday_roll = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, holiday_roll_state,
        appellate::model::WorkflowCommand{appellate::model::CalculateWorkflowDeadline{
            appellate::model::WorkflowCommandHeader{
                holiday_roll_state.session_id,
                appellate::model::WorkflowCommandId{"ca4m4.arm.command.holiday-roll"},
                appellate::model::ActorId{"ca4m4.arm.actor.ca4-clerk"}, legalTime(2026, 6U, 27U)},
            appellate::model::WorkflowOperationId{
                "ca4m4.arm.operation.calculate-mandate-no-petition"},
            appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.mandate-no-petition"}}});
    if (!holiday_roll || holiday_roll->size() != std::size_t{1}) {
        return fail(
            holiday_roll
                ? QStringLiteral("holiday-roll mandate calculation emitted the wrong event count")
                : QStringLiteral("holiday-roll mandate calculation was rejected: %1")
                      .arg(QString::fromStdString(holiday_roll.error().message)));
    }
    const auto* holiday_deadline =
        std::get_if<appellate::model::WorkflowDeadlineCalculated>(&holiday_roll->front());
    if (holiday_deadline == nullptr ||
        holiday_deadline->base_date != legalTime(2026, 6U, 26U).court_date ||
        holiday_deadline->due_date != legalTime(2026, 7U, 6U).court_date) {
        return fail(QStringLiteral("July 3 holiday/weekend roll did not land on July 6"));
    }

    const auto fields = [](std::initializer_list<std::string_view> ids) {
        std::vector<appellate::model::WorkflowFieldValue> result;
        result.reserve(ids.size());
        for (const auto id : ids) {
            result.push_back(appellate::model::WorkflowFieldValue{
                appellate::model::FilingFieldId{std::string(id)}, "present"});
        }
        return result;
    };
    appellate::model::WorkflowState positive_initial;
    positive_initial.session_id = "ca4m4.arm.session.positive-path";
    positive_initial.workflow_id = runtime_case.workflow.id;
    positive_initial.current_stage_id = runtime_case.workflow.initial_stage_id;
    auto positive_state = positive_initial;
    std::vector<appellate::model::WorkflowJournalEntry> positive_journal;
    const auto execute = [&](appellate::model::WorkflowCommand command,
                             std::vector<appellate::model::WorkflowEvent>* emitted =
                                 nullptr) -> std::optional<QString> {
        auto decision = appellate::engine::decideWorkflow(
            runtime_case.workflow, runtime_case.definition, positive_state, command);
        if (!decision) {
            return QStringLiteral("positive command rejected: %1")
                .arg(QString::fromStdString(decision.error().message));
        }
        if (emitted != nullptr) {
            *emitted = *decision;
        }
        positive_journal.push_back(
            appellate::model::WorkflowJournalEntry{std::move(command), std::move(*decision)});
        auto replayed = appellate::engine::replayWorkflow(
            runtime_case.workflow, runtime_case.definition, positive_initial, positive_journal);
        if (!replayed) {
            return QStringLiteral("positive journal replay failed: %1")
                .arg(QString::fromStdString(replayed.error().message));
        }
        positive_state = std::move(*replayed);
        return std::nullopt;
    };
    const auto require_execute = [&](appellate::model::WorkflowCommand command,
                                     std::vector<appellate::model::WorkflowEvent>* emitted =
                                         nullptr) -> std::optional<QString> {
        return execute(std::move(command), emitted);
    };

    if (const auto error = require_execute(appellate::model::SubmitWorkflowFiling{
            positiveCommandHeader("ca4m4.arm.command.file-petition", "ca4m4.arm.actor.petitioner",
                                  2025, 2U, 11U),
            appellate::model::WorkflowFilingId{"ca4m4.arm.filing.petition"},
            appellate::model::FilingTypeId{"us.ca4.filing.agency-petition-for-review"},
            std::string(64, '1'),
            fields({"us.ca4.field.agency-petition.caption",
                    "us.ca4.field.agency-petition.parties-seeking-review",
                    "us.ca4.field.agency-petition.agency",
                    "us.ca4.field.agency-petition.order-reference",
                    "us.ca4.field.agency-petition.order-copy-attached",
                    "us.ca4.field.agency-petition.respondent-names-addresses"}),
            {appellate::model::ActorId{"ca4m4.arm.actor.respondent"}},
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (positive_state.current_stage_id.value != "ca4m4.arm.stage.record") {
        return fail(QStringLiteral("petition route did not enter the record stage"));
    }
    if (const auto error = require_execute(appellate::model::CalculateWorkflowDeadline{
            positiveCommandHeader("ca4m4.arm.command.calculate-docketing",
                                  "ca4m4.arm.actor.ca4-clerk", 2025, 2U, 12U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.calculate-docketing"},
            appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.docketing"}});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::SubmitWorkflowFiling{
            positiveCommandHeader("ca4m4.arm.command.file-record", "ca4m4.arm.actor.respondent",
                                  2025, 3U, 3U),
            appellate::model::WorkflowFilingId{"ca4m4.arm.filing.agency-record"},
            appellate::model::FilingTypeId{"us.ca4.filing.agency-record"},
            std::string(64, '2'),
            fields(
                {"us.ca4.field.agency-record.index", "us.ca4.field.agency-record.certification"}),
            {appellate::model::ActorId{"ca4m4.arm.actor.petitioner"}},
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::AdvanceWorkflowStage{
            positiveCommandHeader("ca4m4.arm.command.advance-briefing", "ca4m4.arm.actor.ca4-clerk",
                                  2025, 3U, 3U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.advance-briefing"}});
        error.has_value()) {
        return fail(*error);
    }
    const auto brief_fields =
        fields({"us.ca4.field.brief.issues", "us.ca4.field.brief.argument",
                "us.ca4.field.brief.record-citations", "us.ca4.field.brief.authority-citations"});
    if (const auto error = require_execute(appellate::model::SubmitWorkflowFiling{
            positiveCommandHeader("ca4m4.arm.command.file-petitioner-brief",
                                  "ca4m4.arm.actor.petitioner", 2025, 3U, 10U),
            appellate::model::WorkflowFilingId{"ca4m4.arm.filing.petitioner-brief"},
            appellate::model::FilingTypeId{"us.ca4.filing.principal-brief"},
            std::string(64, '3'),
            brief_fields,
            {appellate::model::ActorId{"ca4m4.arm.actor.respondent"}},
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::SubmitWorkflowFiling{
            positiveCommandHeader("ca4m4.arm.command.file-respondent-brief",
                                  "ca4m4.arm.actor.respondent", 2025, 3U, 20U),
            appellate::model::WorkflowFilingId{"ca4m4.arm.filing.respondent-brief"},
            appellate::model::FilingTypeId{"us.ca4.filing.principal-brief"},
            std::string(64, '4'),
            brief_fields,
            {appellate::model::ActorId{"ca4m4.arm.actor.petitioner"}},
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::EnterWorkflowOrder{
            positiveCommandHeader("ca4m4.arm.command.briefing-complete",
                                  "ca4m4.arm.actor.composite-panel", 2025, 3U, 21U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.order-briefing-complete"},
            appellate::model::WorkflowOrderId{"ca4m4.arm.order.briefing-complete"},
            appellate::model::WorkflowOrderDisposition::Granted, std::string(64, '5'),
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::ScheduleWorkflowArgument{
            positiveCommandHeader("ca4m4.arm.command.schedule-argument",
                                  "ca4m4.arm.actor.composite-panel", 2025, 3U, 22U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.schedule-argument"},
            legalTime(2025, 5U, 1U).court_date});
        error.has_value()) {
        return fail(*error);
    }
    const auto argument_day_minus_one = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, positive_state,
        appellate::model::WorkflowCommand{appellate::model::EnterWorkflowOrder{
            positiveCommandHeader("ca4m4.arm.command.argument-held-early",
                                  "ca4m4.arm.actor.composite-panel", 2025, 4U, 30U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.order-argument-held"},
            appellate::model::WorkflowOrderId{"ca4m4.arm.order.argument-held"},
            appellate::model::WorkflowOrderDisposition::Granted, std::string(64, '6'),
            std::nullopt}});
    if (argument_day_minus_one || argument_day_minus_one.error().code !=
                                      appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("argument-held order did not reject on scheduled date D-1"));
    }
    if (const auto error = require_execute(appellate::model::EnterWorkflowOrder{
            positiveCommandHeader("ca4m4.arm.command.argument-held",
                                  "ca4m4.arm.actor.composite-panel", 2025, 5U, 1U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.order-argument-held"},
            appellate::model::WorkflowOrderId{"ca4m4.arm.order.argument-held"},
            appellate::model::WorkflowOrderDisposition::Granted, std::string(64, '6'),
            std::nullopt});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::AdvanceWorkflowStage{
            positiveCommandHeader("ca4m4.arm.command.advance-submitted",
                                  "ca4m4.arm.actor.composite-panel", 2025, 5U, 1U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.advance-submitted"}});
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = require_execute(appellate::model::IssueWorkflowJudgment{
            positiveCommandHeader("ca4m4.arm.command.issue-judgment",
                                  "ca4m4.arm.actor.composite-panel", 2026, 3U, 2U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.issue-judgment"},
            std::string(64, '7'), std::string("petition resolved on the authored record")});
        error.has_value()) {
        return fail(*error);
    }
    std::vector<appellate::model::WorkflowEvent> rehearing_events;
    if (const auto error = require_execute(
            appellate::model::CalculateWorkflowDeadline{
                positiveCommandHeader("ca4m4.arm.command.calculate-rehearing",
                                      "ca4m4.arm.actor.ca4-clerk", 2026, 3U, 5U),
                appellate::model::WorkflowOperationId{
                    "ca4m4.arm.operation.calculate-rehearing-deadline"},
                appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.rehearing"}},
            &rehearing_events);
        error.has_value()) {
        return fail(*error);
    }
    const auto* rehearing_event =
        std::get_if<appellate::model::WorkflowDeadlineCalculated>(&rehearing_events.front());
    if (rehearing_event == nullptr ||
        rehearing_event->base_date != legalTime(2026, 3U, 2U).court_date ||
        rehearing_event->due_date != legalTime(2026, 4U, 16U).court_date ||
        rehearing_event->produced_deadline_id !=
            std::optional{appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.rehearing"}} ||
        !rehearing_event->deadline_event_base.has_value() ||
        !std::holds_alternative<appellate::model::WorkflowJudgmentOccurredDeadlineBase>(
            *rehearing_event->deadline_event_base)) {
        return fail(QStringLiteral("delayed rehearing calculation did not bind judgment D+45"));
    }

    const auto at_d45 = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, positive_state,
        appellate::model::WorkflowCommand{appellate::model::AdvanceWorkflowStage{
            positiveCommandHeader("ca4m4.arm.command.advance-at-d45", "ca4m4.arm.actor.ca4-clerk",
                                  2026, 4U, 16U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.advance-mandate-wait"}}});
    if (at_d45 || at_d45.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("elapsed rehearing guard did not reject at D+45"));
    }
    if (const auto error = require_execute(appellate::model::AdvanceWorkflowStage{
            positiveCommandHeader("ca4m4.arm.command.advance-at-d46", "ca4m4.arm.actor.ca4-clerk",
                                  2026, 4U, 17U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.advance-mandate-wait"}});
        error.has_value()) {
        return fail(*error);
    }
    std::vector<appellate::model::WorkflowEvent> mandate_delay_events;
    if (const auto error = require_execute(
            appellate::model::CalculateWorkflowDeadline{
                positiveCommandHeader("ca4m4.arm.command.calculate-mandate-delay",
                                      "ca4m4.arm.actor.ca4-clerk", 2026, 4U, 17U),
                appellate::model::WorkflowOperationId{
                    "ca4m4.arm.operation.calculate-mandate-no-petition"},
                appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.mandate-no-petition"}},
            &mandate_delay_events);
        error.has_value()) {
        return fail(*error);
    }
    const auto* mandate_delay_event =
        std::get_if<appellate::model::WorkflowDeadlineCalculated>(&mandate_delay_events.front());
    if (mandate_delay_event == nullptr ||
        mandate_delay_event->base_date != legalTime(2026, 4U, 16U).court_date ||
        mandate_delay_event->due_date != legalTime(2026, 4U, 23U).court_date ||
        mandate_delay_event->deadline_base_id !=
            std::optional{appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.rehearing"}} ||
        mandate_delay_event->produced_deadline_id !=
            std::optional{
                appellate::model::WorkflowDeadlineId{"ca4m4.arm.deadline.mandate-no-petition"}}) {
        return fail(QStringLiteral("mandate delay did not use the exact rehearing due date"));
    }
    const auto at_d51 = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, positive_state,
        appellate::model::WorkflowCommand{appellate::model::IssueWorkflowMandate{
            positiveCommandHeader("ca4m4.arm.command.mandate-at-d51", "ca4m4.arm.actor.ca4-clerk",
                                  2026, 4U, 22U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.issue-mandate-no-petition"},
            std::string(64, '8')}});
    if (at_d51 || at_d51.error().code != appellate::engine::WorkflowErrorCode::UnmetPrecondition) {
        return fail(QStringLiteral("reached mandate guard did not reject through D+51"));
    }
    if (const auto error = require_execute(appellate::model::IssueWorkflowMandate{
            positiveCommandHeader("ca4m4.arm.command.mandate-at-d52", "ca4m4.arm.actor.ca4-clerk",
                                  2026, 4U, 23U),
            appellate::model::WorkflowOperationId{"ca4m4.arm.operation.issue-mandate-no-petition"},
            std::string(64, '9')});
        error.has_value()) {
        return fail(*error);
    }
    if (positive_state.current_stage_id.value != "ca4m4.arm.stage.mandate-issued" ||
        !positive_state.mandate_sha256.has_value() || positive_journal.size() != std::size_t{15}) {
        return fail(QStringLiteral("positive ARM workflow did not terminate at mandate"));
    }

    std::cout << "ARM batch-1 integration contract passed: 6 AR PDFs / 72 AR pages, "
                 "1 PA proffer / 8 PA pages, 80 unique searchable pages, two grounded banks, "
                 "runtime negative gates and D+52 positive mandate path, four exact revisions.\n";
    return 0;
}
