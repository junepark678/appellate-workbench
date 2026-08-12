#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

constexpr auto root_digest = "ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto manifest_digest = "4e39f7b614201623f33bc317810a6b5ae5d93fd54826dada3af7874276ab6d4b";
constexpr auto archive_digest = "a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2";
constexpr auto successor_inventory_digest =
    "b29e419b9b92dc60c2b014381d9172bd753931825a48266368e0ed2472b7669c";
constexpr auto legacy_blob_identity_digest =
    "ba25b4baa63c51fc95906516244c3abf0143900d19aa40fced1eae40226fd03b";
constexpr auto realism_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";

[[nodiscard]] QByteArray readAll(const QString& file_name) {
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void addDigestFrame(QCryptographicHash& hash, QByteArrayView name, QByteArrayView value) {
    hash.addData(name);
    hash.addData(QByteArrayView("\0", 1));
    const auto size = QByteArray::number(value.size());
    hash.addData(size);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(value);
    hash.addData(QByteArrayView("\0", 1));
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes.at(static_cast<std::size_t>(index)) = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addEvidenceFrame(QCryptographicHash& hash, QByteArrayView bytes) {
    addUint64(hash, static_cast<std::uint64_t>(bytes.size()));
    hash.addData(bytes);
}

void addEvidenceFrame(QCryptographicHash& hash, QStringView value) {
    const auto bytes = value.toUtf8();
    addEvidenceFrame(hash, QByteArrayView(bytes));
}

[[nodiscard]] std::optional<QString> realismJournalDigest(const QJsonArray& journal) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addEvidenceFrame(hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, static_cast<std::uint64_t>(journal.size()));
    for (const auto& entry_value : journal) {
        const auto entry = entry_value.toObject();
        const auto command_encoded =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(command_encoded);
        if (command.isEmpty() || command.toBase64() != command_encoded) {
            return std::nullopt;
        }
        addEvidenceFrame(hash, QByteArrayView(command));
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event_value : events) {
            const auto event_encoded = event_value.toString().toLatin1();
            const auto event = QByteArray::fromBase64(event_encoded);
            if (event.isEmpty() || event.toBase64() != event_encoded) {
                return std::nullopt;
            }
            addEvidenceFrame(hash, QByteArrayView(event));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString realismTraceDigest(const QString& case_id, const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addEvidenceFrame(hash, QStringLiteral("appellate-workbench-executed-trace-evidence-v1"));
    addEvidenceFrame(hash, case_id);
    addEvidenceFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addEvidenceFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addEvidenceFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addEvidenceFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInt()));
    addUint64(hash, static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInt()));
    addEvidenceFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operations = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operations.size()));
    for (const auto& operation : operations) {
        addEvidenceFrame(hash, operation.toString());
    }
    addEvidenceFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString semanticRenderDigest(QByteArrayView source, QStringView title,
                                           QStringView provenance) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto title_bytes = title.toUtf8();
    const auto provenance_bytes = provenance.toUtf8();
    addDigestFrame(hash, QByteArrayView("canonical-markdown-utf8"), source);
    addDigestFrame(hash, QByteArrayView("pdf-title-utf8"), QByteArrayView(title_bytes));
    addDigestFrame(hash, QByteArrayView("renderer-provenance-utf8"),
                   QByteArrayView(provenance_bytes));
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString semanticPlanDigest(QStringView assembly_digest,
                                         QStringView semantic_render_digest) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto assembly_bytes = assembly_digest.toLatin1();
    const auto semantic_render_bytes = semantic_render_digest.toLatin1();
    addDigestFrame(hash, QByteArrayView("assembly-plan-sha256"), QByteArrayView(assembly_bytes));
    addDigestFrame(hash, QByteArrayView("semantic-render-sha256"),
                   QByteArrayView(semantic_render_bytes));
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

[[nodiscard]] QString normalizedSemanticText(QString text) {
    QStringList tokens;
    const QRegularExpression token(QStringLiteral("[\\p{L}\\p{N}]+"));
    auto matches = token.globalMatch(text.normalized(QString::NormalizationForm_C));
    while (matches.hasNext()) {
        tokens.push_back(matches.next().captured().toLower());
    }
    return tokens.join(QLatin1Char(' '));
}

[[nodiscard]] QStringList normalizedMarkdownPages(const QString& markdown) {
    QStringList pages;
    const auto raw_pages = markdown.split(QStringLiteral("<!-- PAGE BREAK -->"));
    pages.reserve(raw_pages.size());
    for (const auto& raw_page : raw_pages) {
        pages.push_back(normalizedSemanticText(raw_page));
    }
    return pages;
}

[[nodiscard]] QString normalizedBiaCopyPage(QString page) {
    page.remove(QRegularExpression(
        QStringLiteral("^\\s*SYNTHETIC (?:TRAINING RECORD|TRAINING APPELLATE DOCKET|"
                       "COUNTERFACTUAL TRAINING BRANCH)[^\\n]*\\n+"),
        QRegularExpression::CaseInsensitiveOption));
    page.remove(QRegularExpression(QStringLiteral("^\\s*CERTIFIED EXERCISE COPY[^\\n]*\\n+"),
                                   QRegularExpression::CaseInsensitiveOption));
    return normalizedSemanticText(page);
}

[[nodiscard]] int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto authoring_root = QDir(QStringLiteral(APPELLATE_M4_ARM_ROOT));
    const auto pack_root = authoring_root.filePath(QStringLiteral("pack"));
    const auto foundations_root = QDir(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    const PackRevision expected_root{PackId{"us.ca4.m4.arm-agency"}, "1.2.0", root_digest};
    const PackRevision expected_federal{PackId{"foundation.us-federal"}, "2025.12.01",
                                        federal_digest};
    const PackRevision expected_ca4{PackId{"foundation.us-ca4"}, "2026.03.23", ca4_digest};
    const PackRevision expected_bench{PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                                      bench_digest};

    const auto source = PackReader::readDirectory(pack_root, PackValidationScope::ResolvedClosure);
    if (!source) {
        return fail(QStringLiteral("source pack: %1").arg(source.error().message));
    }
    const auto manifest_bytes = readAll(QDir(pack_root).filePath(QStringLiteral("manifest.json")));
    if (QCryptographicHash::hash(manifest_bytes, QCryptographicHash::Sha256).toHex() !=
        QByteArray(manifest_digest)) {
        return fail(QStringLiteral("frozen manifest digest mismatch"));
    }
    if (source->revision != expected_root ||
        source->graph_state != PackGraphState::DeferredReferences ||
        source->dependencies.size() != std::size_t{3} ||
        source->required_capabilities.size() != std::size_t{14} ||
        source->resources.size() != std::size_t{9} || source->blobs.size() != std::size_t{54}) {
        return fail(QStringLiteral("source pack revision/count contract mismatch"));
    }

    const auto readme =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral("README.md"))))
            .simplified();
    if (!readme.contains(QStringLiteral("us.ca4.m4.arm-agency@1.2.0")) ||
        !readme.contains(QStringLiteral("installable schema-v2 root")) ||
        !readme.contains(QStringLiteral("18 PDFs and 238")) ||
        !readme.contains(QStringLiteral("AR1–AR238")) ||
        !readme.contains(QStringLiteral("PA1–PA8")) ||
        !readme.contains(QStringLiteral("PA9–PA127")) ||
        !readme.contains(QStringLiteral("PA128–PA177")) ||
        !readme.contains(QStringLiteral("54 PDFs and 415")) ||
        !readme.contains(QStringLiteral("Seven canonical journals")) ||
        !readme.contains(QStringLiteral("15 and 10 questions")) ||
        !readme.contains(QStringLiteral("independent_review_pending"))) {
        return fail(QStringLiteral("README does not describe the finalized 1.2 boundary"));
    }

    QStringList markdown_paths;
    const std::array source_directories{
        std::pair{QStringLiteral("documents/batch-1"), 7},
        std::pair{QStringLiteral("documents/batch-2"), 12},
        std::pair{QStringLiteral("documents/appellate-actual"), 22},
        std::pair{QStringLiteral("documents/appellate-branches"), 13},
    };
    for (const auto& [relative_path, expected_count] : source_directories) {
        const auto documents = QDir(authoring_root.filePath(relative_path));
        const auto names = documents.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
        if (names.size() != expected_count) {
            return fail(QStringLiteral("source count mismatch for %1").arg(relative_path));
        }
        for (const auto& name : names) {
            markdown_paths.push_back(documents.filePath(name));
        }
    }
    if (markdown_paths.size() != 54) {
        return fail(QStringLiteral("ARM must have exactly fifty-four rendered sources"));
    }
    const QString record_banner = QStringLiteral(
        "SYNTHETIC TRAINING RECORD — NOT FILED — ALL FACTS AND IDENTIFIERS ARE FICTIONAL");
    const QString proffer_banner = QStringLiteral(
        "SYNTHETIC TRAINING APPELLATE PROFFER — NOT ADMINISTRATIVE RECORD — ALL FACTS ARE "
        "FICTIONAL");
    const QString appellate_banner = QStringLiteral(
        "SYNTHETIC TRAINING APPELLATE DOCKET — NOT FILED — ALL FACTS AND IDENTIFIERS ARE "
        "FICTIONAL");
    const QString branch_banner = QStringLiteral(
        "SYNTHETIC COUNTERFACTUAL TRAINING BRANCH — NEVER FILED — ALL FACTS AND IDENTIFIERS ARE "
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
        QStringLiteral("todo"),
        QStringLiteral("tbd"),
        QStringLiteral("lorem ipsum"),
        QStringLiteral("dummy text"),
        QStringLiteral("sample text"),
    };
    const QStringList forbidden_batch_two_record_voice{
        QStringLiteral("fictional"),       QStringLiteral("synthetic"),
        QStringLiteral("exercise"),        QStringLiteral("invented"),
        QStringLiteral("does not exist"),  QStringLiteral("doesn't exist"),
        QStringLiteral("no real address"),
    };
    const QRegularExpression compiled_ar_label(QStringLiteral("\\bAR\\d+\\b"));
    const QHash<QString, QStringList> required_source_canon{
        {QStringLiteral("03-cat-application.md"),
         {QStringLiteral("may 21 clinic visit at 14:20"),
          QStringLiteral("four selected kalyrian-language message exports with authenticated "
                         "searchable transcriptions"),
          QStringLiteral("physically combined with p-4 and p-5")}},
        {QStringLiteral("05-sibling-declaration.md"),
         {QStringLiteral("between august 9 and september 2, four messages"),
          QStringLiteral("june 30 its status remained")}},
        {QStringLiteral("06-family-corroboration.md"),
         {QStringLiteral("six harbor league invoices"), QStringLiteral("total 137,400 units"),
          QStringLiteral("first register version groups"),
          QStringLiteral("corrected version removes"), QStringLiteral("14:20 on may 21"),
          QStringLiteral("limited left-shoulder elevation"),
          QStringLiteral("right-ankle swelling")}},
        {QStringLiteral("07-country-conditions-report.md"),
         {QStringLiteral("through june 7, 2024"), QStringLiteral("six harbor league invoices"),
          QStringLiteral("totaling 137,400 units"),
          QStringLiteral("corrected version removes that grouping")}},
        {QStringLiteral("08-trauma-medical-evaluation.md"),
         {QStringLiteral("may 21, 2023 clinic notation"), QStringLiteral("registration at 14:20"),
          QStringLiteral("right-ankle swelling"), QStringLiteral("limited left-shoulder"),
          QStringLiteral("source cutoff is may 29, 2024")}},
        {QStringLiteral("09-certified-translation-packet.md"),
         {QStringLiteral("single ten-page physical file"),
          QStringLiteral("april filing receipt lists p-4, p-5, and p-6 separately as lodged"),
          QStringLiteral("p-4 maps to agency pages 106–107"),
          QStringLiteral("p-5 maps to agency pages 108–109"),
          QStringLiteral("p-6 maps to shared control page 105 and agency pages 110–114"),
          QStringLiteral("contains no raster image object")}},
        {QStringLiteral("10-merits-hearing-transcript-vol-1.md"),
         {QStringLiteral("six harbor league invoices"), QStringLiteral("totaled 137,400 units"),
          QStringLiteral("clinic registered a.r.m. that afternoon at 14:20"),
          QStringLiteral("june 30 with “regional response pending”")}},
        {QStringLiteral("11-merits-hearing-transcript-vol-2.md"),
         {QStringLiteral("swollen right ankle"), QStringLiteral("may 21 at 14:20"),
          QStringLiteral("totaling 137,400 units"),
          QStringLiteral("corrected version removes that grouping"),
          QStringLiteral("remain in evidence under the prior exhibit receipt")}},
        {QStringLiteral("12-ij-decision.md"),
         {QStringLiteral("may 21 at 14:20"), QStringLiteral("right-ankle swelling"),
          QStringLiteral("six invoices totaling 137,400 units"),
          QStringLiteral("first grouped and later corrected to remove grouping")}},
        {QStringLiteral("17-initial-certified-index.md"),
         {QStringLiteral("label_discontinuity_18"), QStringLiteral("cause `unresolved`"),
          QStringLiteral("no deletion event, replacement event, or digest failure")}},
        {QStringLiteral("18-exhibit-receipt-audit-corrected-certification.md"),
         {QStringLiteral("b26f4d1618332a6e006839a09c4ab77319bde4f58c6d3e219da4fbcfbe1ce855"),
          QStringLiteral("september hearing receipt — complete table"),
          QStringLiteral("admitted september 17; testimony received september 18"),
          QStringLiteral("admitted september 17; weight addressed september 18"),
          QStringLiteral("08e8294532c23fe9feb5962ca5b7780ae958178e6c8e2b4840d1ee28f3c5d212"),
          QStringLiteral("itemized corrected index — documents 10 through 18")}},
    };
    const QHash<QString, QStringList> forbidden_future_knowledge_by_source{
        {QStringLiteral("03-cat-application.md"),
         {QStringLiteral("admitted september 17"), QStringLiteral("september hearing receipt"),
          QStringLiteral("october 22 decision"), QStringLiteral("january 13 board"),
          QStringLiteral("february 18, 2025"), QStringLiteral("march 3, 2025")}},
        {QStringLiteral("05-sibling-declaration.md"),
         {QStringLiteral("admitted september 17"), QStringLiteral("september hearing receipt"),
          QStringLiteral("october 22 decision"), QStringLiteral("january 13 board"),
          QStringLiteral("february 18, 2025"), QStringLiteral("march 3, 2025")}},
        {QStringLiteral("07-country-conditions-report.md"),
         {QStringLiteral("admitted september 17"), QStringLiteral("september hearing receipt"),
          QStringLiteral("october 22 decision"), QStringLiteral("january 13 board"),
          QStringLiteral("february 18, 2025"), QStringLiteral("march 3, 2025")}},
        {QStringLiteral("08-trauma-medical-evaluation.md"),
         {QStringLiteral("admitted september 17"), QStringLiteral("september hearing receipt"),
          QStringLiteral("october 22 decision"), QStringLiteral("january 13 board"),
          QStringLiteral("february 18, 2025"), QStringLiteral("march 3, 2025")}},
        {QStringLiteral("09-certified-translation-packet.md"),
         {QStringLiteral("september hearing"), QStringLiteral("september receipt"),
          QStringLiteral("admitted september"), QStringLiteral("october 22"),
          QStringLiteral("january 13"), QStringLiteral("february 18"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)")}},
        {QStringLiteral("10-merits-hearing-transcript-vol-1.md"),
         {QStringLiteral("september 18, 2024"), QStringLiteral("october 22"),
          QStringLiteral("january 13"), QStringLiteral("february 18"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer")}},
        {QStringLiteral("11-merits-hearing-transcript-vol-2.md"),
         {QStringLiteral("october 22"), QStringLiteral("january 13"), QStringLiteral("february 18"),
          QStringLiteral("march 3"), QStringLiteral("rule 16(b)"),
          QStringLiteral("appellate proffer")}},
        {QStringLiteral("12-ij-decision.md"),
         {QStringLiteral("november 18"), QStringLiteral("january 13"),
          QStringLiteral("february 18"), QStringLiteral("march 3"), QStringLiteral("rule 16(b)"),
          QStringLiteral("appellate proffer")}},
        {QStringLiteral("13-bia-notice-appeal.md"),
         {QStringLiteral("january 13"), QStringLiteral("february 18"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer")}},
        {QStringLiteral("14-bia-opening-brief.md"),
         {QStringLiteral("january 13"), QStringLiteral("february 18"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer")}},
        {QStringLiteral("15-dhs-bia-response.md"),
         {QStringLiteral("january 13"), QStringLiteral("february 18"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer")}},
        {QStringLiteral("16-bia-final-order.md"),
         {QStringLiteral("february 11"), QStringLiteral("february 18"),
          QStringLiteral("february 20"), QStringLiteral("february 24"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer")}},
        {QStringLiteral("17-initial-certified-index.md"),
         {QStringLiteral("february 20"), QStringLiteral("february 24"), QStringLiteral("march 3"),
          QStringLiteral("rule 16(b)"), QStringLiteral("appellate proffer"),
          QStringLiteral("stale status"), QStringLiteral("without joining the hearing receipt")}},
    };
    QHash<QString, QStringList> normalized_markdown_pages_by_asset;
    QSet<QString> distinct_markdown_pages;
    QString batch_two_corpus;
    int authored_page_count = 0;
    int legacy_authored_page_count = 0;
    int successor_authored_page_count = 0;
    for (const auto& markdown_path : markdown_paths) {
        const auto markdown_name = QFileInfo(markdown_path).fileName();
        const auto raw = QString::fromUtf8(readAll(markdown_path));
        const bool actual_source = markdown_path.contains(QStringLiteral("/appellate-actual/"));
        const bool branch_source = markdown_path.contains(QStringLiteral("/appellate-branches/"));
        const bool proffer_source = markdown_name.startsWith(QStringLiteral("pa"));
        const bool legacy_source = !actual_source && !branch_source;
        const auto expected_banner = branch_source    ? branch_banner
                                     : actual_source  ? appellate_banner
                                     : proffer_source ? proffer_banner
                                                      : record_banner;
        const auto newline = raw.indexOf(QLatin1Char('\n'));
        const auto body = raw.mid(newline + 1);
        if (!raw.startsWith(expected_banner + QLatin1Char('\n')) ||
            raw.count(expected_banner) != 1 || newline < 0 ||
            (legacy_source && compiled_ar_label.match(body).hasMatch())) {
            return fail(
                QStringLiteral("source safety/temporal boundary mismatch: %1").arg(markdown_name));
        }
        const auto lower_body = body.toLower();
        const auto searchable_body = lower_body.simplified();
        if (legacy_source && lower_body.contains(QStringLiteral("placeholder"))) {
            return fail(QStringLiteral("placeholder token escaped into %1").arg(markdown_name));
        }
        const bool may_identify_post_order_material =
            proffer_source || markdown_name.startsWith(QStringLiteral("18-"));
        if (legacy_source && !may_identify_post_order_material &&
            (lower_body.contains(QStringLiteral("cousin's")) ||
             lower_body.contains(QStringLiteral("cousin declaration")) ||
             lower_body.contains(QStringLiteral("appellate proffer")) ||
             lower_body.contains(QStringLiteral("post-order account")) ||
             lower_body.contains(QStringLiteral("later declaration")))) {
            return fail(
                QStringLiteral("future-record knowledge leaked into %1").arg(markdown_name));
        }
        for (const auto& phrase : forbidden_authoring_voice) {
            if (legacy_source && lower_body.contains(phrase)) {
                return fail(QStringLiteral("inline authoring voice leaked into %1: %2")
                                .arg(markdown_name, phrase));
            }
        }
        if (const auto required = required_source_canon.constFind(markdown_name);
            required != required_source_canon.cend()) {
            for (const auto& phrase : *required) {
                if (!searchable_body.contains(phrase)) {
                    return fail(QStringLiteral("source canon missing in %1: %2")
                                    .arg(markdown_name, phrase));
                }
            }
        }
        if (const auto forbidden = forbidden_future_knowledge_by_source.constFind(markdown_name);
            forbidden != forbidden_future_knowledge_by_source.cend()) {
            for (const auto& phrase : *forbidden) {
                if (searchable_body.contains(phrase)) {
                    return fail(QStringLiteral("date-bounded future knowledge leaked into %1: %2")
                                    .arg(markdown_name, phrase));
                }
            }
        }
        const auto normalized_pages = normalizedMarkdownPages(raw);
        const auto asset_path =
            QStringLiteral("assets/%1.pdf").arg(QFileInfo(markdown_path).completeBaseName());
        if (normalized_markdown_pages_by_asset.contains(asset_path)) {
            return fail(QStringLiteral("duplicate source-to-asset mapping: %1").arg(asset_path));
        }
        for (const auto& normalized_page : normalized_pages) {
            if (normalized_page.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() < 76 ||
                distinct_markdown_pages.contains(normalized_page)) {
                return fail(QStringLiteral("thin or duplicate normalized Markdown page: %1")
                                .arg(markdown_name));
            }
            distinct_markdown_pages.insert(normalized_page);
            ++authored_page_count;
            if (legacy_source) {
                ++legacy_authored_page_count;
            } else {
                ++successor_authored_page_count;
            }
        }
        normalized_markdown_pages_by_asset.insert(asset_path, normalized_pages);
        if (markdown_path.contains(QStringLiteral("/batch-2/"))) {
            batch_two_corpus += searchable_body;
            for (const auto& phrase : forbidden_batch_two_record_voice) {
                if (lower_body.contains(phrase)) {
                    return fail(QStringLiteral("batch-2 record voice leaked into %1: %2")
                                    .arg(markdown_name, phrase));
                }
            }
        }
    }
    const QStringList forbidden_canon_drift{
        QStringLiteral("left ankle"),
        QStringLiteral("400,000"),
        QStringLiteral("oci-mr-0617"),
        QStringLiteral("ov-2"),
        QStringLiteral("mr-s4"),
        QStringLiteral("three messages"),
        QStringLiteral("later version groups"),
        QStringLiteral("second adds a consolidated"),
        QStringLiteral("second version consolidated"),
    };
    if (QRegularExpression(QStringLiteral("\\bjune\\s*(?:2|3)(?!\\d)"))
            .match(batch_two_corpus)
            .hasMatch()) {
        return fail(QStringLiteral("cross-batch canon drift remains: June 2/3"));
    }
    for (const auto& phrase : forbidden_canon_drift) {
        if (batch_two_corpus.contains(phrase)) {
            return fail(QStringLiteral("cross-batch canon drift remains: %1").arg(phrase));
        }
    }
    const auto initial_index =
        QString::fromUtf8(readAll(authoring_root.filePath(
                              QStringLiteral("documents/batch-2/17-initial-certified-index.md"))))
            .toLower();
    for (const auto& phrase :
         {QStringLiteral("p-7"), QStringLiteral("sworn declaration"), QStringLiteral("omission"),
          QStringLiteral("missing"), QStringLiteral("stale"), QStringLiteral("metadata"),
          QStringLiteral("cousin"), QStringLiteral("proffer")}) {
        if (initial_index.contains(phrase)) {
            return fail(
                QStringLiteral("initial index contains retrospective diagnosis: %1").arg(phrase));
        }
    }
    if (authored_page_count != 415 || legacy_authored_page_count != 246 ||
        successor_authored_page_count != 169 || distinct_markdown_pages.size() != 415 ||
        normalized_markdown_pages_by_asset.size() != 54) {
        return fail(QStringLiteral("normalized Markdown page closure mismatch"));
    }

    const auto bia_pages = QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral(
                                                 "documents/batch-2/16-bia-final-order.md"))))
                               .split(QStringLiteral("<!-- PAGE BREAK -->"));
    const auto actual_copy_pages =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral(
                              "documents/appellate-actual/a03-cured-petition-order-copy.md"))))
            .split(QStringLiteral("<!-- PAGE BREAK -->"));
    const auto branch_copy_pages =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral(
                              "documents/appellate-branches/b01-day31-petition-order-copy.md"))))
            .split(QStringLiteral("<!-- PAGE BREAK -->"));
    if (bia_pages.size() != 8 || actual_copy_pages.size() != 12 || branch_copy_pages.size() != 12) {
        return fail(QStringLiteral("BIA copy source page contract mismatch"));
    }
    for (qsizetype index = 0; index < bia_pages.size(); ++index) {
        if (normalizedBiaCopyPage(bia_pages.at(index)) !=
                normalizedBiaCopyPage(actual_copy_pages.at(index + 4)) ||
            normalizedBiaCopyPage(bia_pages.at(index)) !=
                normalizedBiaCopyPage(branch_copy_pages.at(index + 4))) {
            return fail(QStringLiteral("BIA source-copy equivalence drifted at copy page %1")
                            .arg(index + 1));
        }
    }

    const std::array controlled_objects{
        std::pair{
            QByteArrayLiteral("KAL-MSG-1|2023-07-13T08:22:00+09:00|sender="
                              "harbor_lantern_01|recipient=family_account|source_text=Nor "
                              "kai luma doset eva.|attachment=none"),
            QByteArrayLiteral("c634626d66ffe2705fb504903cb6a90a50ceb455d11215071ae419c013bdefc3")},
        std::pair{
            QByteArrayLiteral("KAL-MSG-2|2023-08-09T19:04:00+09:00|sender="
                              "harbor_lantern_02|recipient=family_account|source_text=Kada "
                              "anex dal reva?|attachment=none"),
            QByteArrayLiteral("13d243c3dd7d367a2a091e1bfaa596883bb4625ba264d36214a36c26951b6475")},
        std::pair{
            QByteArrayLiteral("KAL-MSG-3|2023-08-17T06:41:00+09:00|sender="
                              "harbor_lantern_03|recipient=family_account|source_text=Korik "
                              "16:42 nosh.|attachment=none"),
            QByteArrayLiteral("29cc5d4f974ca33b81214343a2af227ac5fda767858816c9a1d80f4ddb04eb63")},
        std::pair{
            QByteArrayLiteral("KAL-MSG-4|2023-08-28T22:10:00+09:00|sender="
                              "harbor_lantern_04|recipient=family_account|source_text="
                              "Regional vek ARM-RX-14.|attachment=none"),
            QByteArrayLiteral("48572608f2d08537c42df701f8f01ed7315190e13e004d51cc1c0be73bf76b31")},
        std::pair{
            QByteArrayLiteral("KAL-OCI-1|accepted=2023-06-07T11:32:00+09:00|complaint="
                              "OCI-23-441|subject=detention after procurement disclosure|"
                              "witness=SM|initial_route=IC-2|requested_action=independent "
                              "conduct review"),
            QByteArrayLiteral("2e9b8904310bf6c0060c36d4c945092bcdee24eea9b00a04b0b68a947f12d051")},
        std::pair{
            QByteArrayLiteral("KAL-ROUTE-1|2023-06-09:link=CIV-23-184 via ARM-RX-14|"
                              "2023-06-10:destination=RC-4 Unit Four regional command reason="
                              "operational-security nexus|2023-06-30:status=regional response "
                              "pending|interview=none"),
            QByteArrayLiteral("c54179ed8d35de13bb4767c3efc8ddbdd4b2f30d7305e23498b6c0c6b03ea679")},
        std::pair{
            QByteArrayLiteral("KAL-CLINIC-1|registered=2023-05-21T14:20:00+09:00|history="
                              "reported assault during two-day absence|observed=limited left-"
                              "shoulder elevation;right-ankle swelling;rib tenderness;hoarse "
                              "voice|care=shoulder sling;anti-inflammatory medication;"
                              "hydration instruction;return precautions"),
            QByteArrayLiteral("ce82b649862ff37d47ccb97820c887b7aa1793b020b94116c2fa540e3749143d")},
        std::pair{
            QByteArrayLiteral("KAL-LEDGER-1|version=first|threshold=25000|cycle=NQ-APR-A|"
                              "NQL-3101=22900|NQL-3102=22700|NQL-3103=23400|NQL-3104=22600|"
                              "NQL-3105=22800|NQL-3106=23000|total=137400|grouping=all six "
                              "grouped"),
            QByteArrayLiteral("de363cf933ea6218b608fce156b0a79df3e7340f362be5e8eca81874045f3642")},
        std::pair{
            QByteArrayLiteral("KAL-LEDGER-2|version=corrected|threshold=25000|cycle=none|"
                              "NQL-3101=22900|NQL-3102=22700|NQL-3103=23400|NQL-3104=22600|"
                              "NQL-3105=22800|NQL-3106=23000|total=137400|grouping=removed;"
                              "each urgent standalone repair"),
            QByteArrayLiteral("72e118dec40c82bc0a8b2c6c444bbd5bdf6f5058079bed43976e208808f3e50a")},
        std::pair{
            QByteArrayLiteral("KAL-TRANS-1|MSG1=The North Quay Lamps file is still open.|MSG2="
                              "When will the annex clerk return?|MSG3=The correction-log time "
                              "is 16:42.|MSG4=The regional inquiry follows employment code "
                              "ARM-RX-14.|OCI=detention after procurement disclosure;"
                              "independent conduct review|ROUTE=linked June 9;destination "
                              "changed June 10;regional response pending June 30|CLINIC=May "
                              "21 at 14:20;limited left shoulder;right ankle swelling;rib "
                              "tenderness;hoarse voice|LEDGER=six invoices below 25,000;total "
                              "137,400;first grouped;corrected grouping removed"),
            QByteArrayLiteral("b8928fc635fc914b9abc49c20ed5a307067c5692a7eb9109158c81723eb1ee71")},
    };
    const auto combined_source = readAll(authoring_root.filePath(
        QStringLiteral("documents/batch-2/09-certified-translation-packet.md")));
    for (const auto& [object_bytes, expected_hash] : controlled_objects) {
        const auto actual_hash =
            QCryptographicHash::hash(object_bytes, QCryptographicHash::Sha256).toHex();
        if (actual_hash != expected_hash || !combined_source.contains(object_bytes) ||
            !combined_source.contains(expected_hash)) {
            return fail(QStringLiteral("controlled P-4/P-5/P-6 object bytes or hash drifted"));
        }
    }
    const QByteArray stipulation_bytes = QByteArrayLiteral(
        "STIPULATION-16B|date=2025-03-03|docket=SYN-CA4-25-AG-4301|parties=ARM,DHS|agree="
        "correct omission by restoring unchanged Agency Exhibit P-7 at agency record pages 33-50;"
        "file itemized corrected index;preserve initial transmission;exclude appellate proffer "
        "pages 1-8 from administrative record|authority=Fed. R. App. P. 16(b)|signatures=ARM-C-9,"
        "DHS-APP-4");
    const QByteArray stipulation_hash =
        QByteArrayLiteral("b26f4d1618332a6e006839a09c4ab77319bde4f58c6d3e219da4fbcfbe1ce855");
    const auto correction_source = readAll(authoring_root.filePath(
        QStringLiteral("documents/batch-2/18-exhibit-receipt-audit-corrected-certification.md")));
    if (QCryptographicHash::hash(stipulation_bytes, QCryptographicHash::Sha256).toHex() !=
            stipulation_hash ||
        !correction_source.contains(stipulation_bytes) ||
        !correction_source.contains(stipulation_hash)) {
        return fail(QStringLiteral("Rule 16(b) stipulation bytes or hash drifted"));
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

    QSet<QString> actual_capabilities;
    for (const auto& capability : source->required_capabilities) {
        actual_capabilities.insert(QStringLiteral("%1@%2")
                                       .arg(QString::fromStdString(capability.id))
                                       .arg(capability.version));
    }
    const QSet<QString> expected_capabilities{
        QStringLiteral("workbench.pack.declarative-resources@2"),
        QStringLiteral("workbench.pack.canonical-authority@1"),
        QStringLiteral("workbench.pack.workflow-preconditions@1"),
        QStringLiteral("workbench.pack.dependent-deadlines@1"),
        QStringLiteral("workbench.pack.named-deadlines@1"),
        QStringLiteral("workbench.pack.event-date-deadlines@1"),
        QStringLiteral("workbench.pack.argument-date-guards@1"),
        QStringLiteral("workbench.pack.grounded-questions@1"),
        QStringLiteral("workbench.pack.route-role-subsets@1"),
        QStringLiteral("workbench.pack.workflow-instance-preconditions@1"),
        QStringLiteral("workbench.pack.static-deficiency-deadlines@1"),
        QStringLiteral("workbench.pack.operation-document-bindings@1"),
        QStringLiteral("workbench.pack.structured-disposition@1"),
        QStringLiteral("workbench.pack.realism-evidence@1"),
    };
    if (actual_capabilities != expected_capabilities) {
        return fail(QStringLiteral("exact 1.2 capability contract mismatch"));
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
    const auto* procedure_resource =
        findResource(source->resources, "ca4m4.arm.procedure.agency-review");
    const auto* realism_resource =
        findResource(source->resources, "ca4m4.arm.review.authoring-2026-08-12");
    if (case_resource == nullptr || record_resource == nullptr || authority_resource == nullptr ||
        workflow_resource == nullptr || bench_resource == nullptr || actual_argument == nullptr ||
        counterfactual_argument == nullptr || procedure_resource == nullptr ||
        realism_resource == nullptr || case_resource->descriptor.kind != ResourceKind::Case ||
        record_resource->descriptor.kind != ResourceKind::Record ||
        actual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        counterfactual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        procedure_resource->descriptor.kind != ResourceKind::ProcedureProfile ||
        realism_resource->descriptor.kind != ResourceKind::RealismReview) {
        return fail(QStringLiteral("required ARM resources are absent"));
    }

    for (const auto& actor_value :
         case_resource->document.value(QStringLiteral("actors")).toArray()) {
        if (!actor_value.toObject().value(QStringLiteral("synthetic")).toBool()) {
            return fail(QStringLiteral("case actor is not explicitly synthetic"));
        }
    }
    const auto case_issues = case_resource->document.value(QStringLiteral("issues")).toArray();
    const auto disposition_plans =
        case_resource->document.value(QStringLiteral("disposition_plans")).toArray();
    if (case_issues.size() != 6 || disposition_plans.size() != 1 ||
        case_resource->document.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.arm.disposition.authored-vacate-remand") ||
        case_resource->document.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.arm.operation.issue-judgment")) {
        return fail(QStringLiteral("ARM issue/disposition envelope mismatch"));
    }
    QHash<QString, QSet<QString>> issue_authorities;
    QHash<QString, QSet<QString>> issue_anchors;
    QHash<QString, QSet<QString>> issue_targets;
    QSet<QString> all_targets;
    for (const auto& issue_value : case_issues) {
        const auto issue = issue_value.toObject();
        const auto issue_id = issue.value(QStringLiteral("issue_id")).toString();
        if (issue_id.isEmpty() || issue_authorities.contains(issue_id)) {
            return fail(QStringLiteral("case issue IDs are not exact and unique"));
        }
        issue_authorities.insert(issue_id,
                                 strings(issue.value(QStringLiteral("authority_ids")).toArray()));
        issue_anchors.insert(issue_id,
                             strings(issue.value(QStringLiteral("record_anchor_ids")).toArray()));
        issue_targets.insert(issue_id,
                             strings(issue.value(QStringLiteral("target_ids")).toArray()));
        all_targets.unite(issue_targets.value(issue_id));
    }
    const auto disposition = disposition_plans.first().toObject();
    const auto components = disposition.value(QStringLiteral("components")).toArray();
    const QSet<QString> expected_components{
        QStringLiteral("ca4m4.arm.issue.record-composition|ca4m4.arm.target.corrected-certified-"
                       "record|whole|affirm|0"),
        QStringLiteral("ca4m4.arm.issue.record-composition|ca4m4.arm.target.extra-record-proffer|"
                       "whole|deny|0"),
        QStringLiteral("ca4m4.arm.issue.aggregate-cat-risk|ca4m4.arm.target.aggregate-analysis|"
                       "whole|vacate|1"),
        QStringLiteral(
            "ca4m4.arm.issue.official-acquiescence|ca4m4.arm.target.acquiescence-analysis|"
            "whole|vacate|1"),
        QStringLiteral("ca4m4.arm.issue.review-standard|ca4m4.arm.target.meaningful-consideration|"
                       "whole|vacate|1"),
        QStringLiteral(
            "ca4m4.arm.issue.petition-timeliness|ca4m4.arm.target.actual-timing-challenge|"
            "whole|deny|0"),
        QStringLiteral("ca4m4.arm.issue.disposition-remedy|ca4m4.arm.target.petition-for-review|"
                       "part|grant|1"),
    };
    QSet<QString> actual_components;
    QSet<QString> component_targets;
    for (const auto& component_value : components) {
        const auto component = component_value.toObject();
        const auto issue_id = component.value(QStringLiteral("issue_id")).toString();
        const auto target_id = component.value(QStringLiteral("target_id")).toString();
        const auto authorities =
            strings(component.value(QStringLiteral("authority_ids")).toArray());
        const auto anchors =
            strings(component.value(QStringLiteral("record_anchor_ids")).toArray());
        if (!issue_targets.value(issue_id).contains(target_id) || authorities.isEmpty() ||
            anchors.isEmpty() || !(authorities - issue_authorities.value(issue_id)).isEmpty() ||
            !(anchors - issue_anchors.value(issue_id)).isEmpty() ||
            component_targets.contains(target_id)) {
            return fail(QStringLiteral("disposition component is ungrounded or duplicated: %1")
                            .arg(target_id));
        }
        component_targets.insert(target_id);
        actual_components.insert(
            QStringLiteral("%1|%2|%3|%4|%5")
                .arg(issue_id, target_id, component.value(QStringLiteral("scope")).toString(),
                     component.value(QStringLiteral("action")).toString())
                .arg(component.value(QStringLiteral("remand")).toBool() ? 1 : 0));
    }
    if (disposition.value(QStringLiteral("plan_id")).toString() !=
            QStringLiteral("ca4m4.arm.disposition.authored-vacate-remand") ||
        disposition.value(QStringLiteral("finality")).toString() != QStringLiteral("final") ||
        disposition.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("cd104002da124bd64ba967deedde73be0d70b307e9ebcd760350b5b5c6eb95f2") ||
        components.size() != 7 || all_targets.size() != 7 || component_targets != all_targets ||
        actual_components != expected_components) {
        return fail(QStringLiteral("seven-component disposition contract mismatch"));
    }

    const auto check_argument_bank =
        [&](const ValidatedResource& resource, const QString& expected_mode,
            const QString& expected_digest, int expected_question_count) -> std::optional<QString> {
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
            bindings.size() != 5 || questions.size() != expected_question_count) {
            return QStringLiteral("argument-bank envelope mismatch");
        }
        QSet<QString> bound_issues;
        QSet<QString> question_issues;
        QSet<QString> question_ids;
        QSet<QString> grounding_ids;
        QSet<QString> topics;
        QHash<QString, QSet<QString>> binding_topics;
        QHash<QString, int> questions_per_issue;
        bool saw_branch_pa = false;
        const bool actual_mode = expected_mode == QStringLiteral("actual_record");
        for (const auto& binding_value : bindings) {
            const auto binding = binding_value.toObject();
            const auto issue = binding.value(QStringLiteral("issue_id")).toString();
            const auto bound_topics = strings(binding.value(QStringLiteral("topic_ids")).toArray());
            if (!permitted.contains(issue) || bound_topics.size() != (actual_mode ? 3 : 2) ||
                !bound_topics.contains(QStringLiteral("workbench.topic.remedy")) ||
                (bound_topics.contains(QStringLiteral("workbench.topic.practical-consequences")) !=
                 actual_mode)) {
                return QStringLiteral("argument-bank issue binding mismatch");
            }
            bound_issues.insert(issue);
            topics.unite(bound_topics);
            binding_topics.insert(issue, bound_topics);
        }
        for (const auto& question_value : questions) {
            const auto question = question_value.toObject();
            const auto issue = question.value(QStringLiteral("issue_id")).toString();
            const auto question_id = question.value(QStringLiteral("question_id")).toString();
            const auto topic_id = question.value(QStringLiteral("topic_id")).toString();
            if (!permitted.contains(issue) || question_ids.contains(question_id) ||
                !binding_topics.value(issue).contains(topic_id) ||
                !question_id.startsWith(
                    expected_mode == QStringLiteral("actual_record")
                        ? QStringLiteral("ca4m4.arm.question.actual-")
                        : QStringLiteral("ca4m4.arm.question.counterfactual-")) ||
                question.value(QStringLiteral("prompt")).toString().isEmpty()) {
                return QStringLiteral("argument-bank question coverage mismatch");
            }
            question_ids.insert(question_id);
            question_issues.insert(issue);
            ++questions_per_issue[issue];
            bool question_has_authority = false;
            bool question_has_ar = false;
            bool question_has_pa = false;
            for (const auto& grounding_value :
                 question.value(QStringLiteral("grounding")).toArray()) {
                const auto grounding = grounding_value.toObject();
                const auto grounding_id =
                    grounding.value(QStringLiteral("grounding_id")).toString();
                const auto kind = grounding.value(QStringLiteral("kind")).toString();
                if (grounding_id.isEmpty() || grounding_ids.contains(grounding_id)) {
                    return QStringLiteral("argument bank grounding IDs are not exact and unique");
                }
                grounding_ids.insert(grounding_id);
                QString target;
                if (kind == QStringLiteral("record_page")) {
                    target = grounding.value(QStringLiteral("anchor_id")).toString();
                    if (!issue_anchors.value(issue).contains(target)) {
                        return QStringLiteral("question uses anchor outside its issue: %1")
                            .arg(question_id);
                    }
                    question_has_pa =
                        question_has_pa || target.startsWith(QStringLiteral("ca4m4.arm.anchor.pa"));
                    question_has_ar =
                        question_has_ar || target.startsWith(QStringLiteral("ca4m4.arm.anchor.ar"));
                    if (target.startsWith(QStringLiteral("ca4m4.arm.anchor.pa"))) {
                        bool parsed = false;
                        const auto page =
                            target.mid(QStringLiteral("ca4m4.arm.anchor.pa").size()).toInt(&parsed);
                        if (!parsed ||
                            (page <= 8 &&
                             issue != QStringLiteral("ca4m4.arm.issue.record-composition")) ||
                            (expected_mode == QStringLiteral("actual_record") && page >= 128)) {
                            return QStringLiteral(
                                "argument bank crosses the PA classification boundary");
                        }
                        saw_branch_pa = saw_branch_pa || page >= 128;
                    }
                } else if (kind == QStringLiteral("authority")) {
                    target = grounding.value(QStringLiteral("authority_id")).toString();
                    if (!issue_authorities.value(issue).contains(target)) {
                        return QStringLiteral("question uses authority outside its issue: %1")
                            .arg(question_id);
                    }
                    question_has_authority = true;
                } else {
                    return QStringLiteral("argument bank uses noncanonical grounding kind");
                }
            }
            if (!question_has_authority || !question_has_ar || !question_has_pa) {
                return QStringLiteral("question lacks authority/AR/PA grounding: %1")
                    .arg(question_id);
            }
        }
        QSet<QString> expected_topics{
            QStringLiteral("workbench.topic.record-support"),
            QStringLiteral("workbench.topic.governing-authority"),
            QStringLiteral("workbench.topic.merits"),
            QStringLiteral("workbench.topic.standard-of-review"),
            QStringLiteral("workbench.topic.jurisdiction"),
            QStringLiteral("workbench.topic.remedy"),
        };
        if (actual_mode) {
            expected_topics.insert(QStringLiteral("workbench.topic.practical-consequences"));
        }
        const auto expected_per_issue = expected_question_count / 5;
        if (bound_issues != permitted || question_issues != permitted ||
            topics != expected_topics ||
            std::ranges::any_of(permitted,
                                [&](const auto& issue) {
                                    return questions_per_issue.value(issue) != expected_per_issue;
                                }) ||
            (actual_mode ? saw_branch_pa : !saw_branch_pa)) {
            return QStringLiteral("argument bank is not grounded across the five-issue matrix");
        }
        return std::nullopt;
    };
    if (const auto error = check_argument_bank(
            *actual_argument, QStringLiteral("actual_record"),
            QStringLiteral("0bf9b67b1ad8bf28c5c061deda496a1047d731ce66fdec9a864c112c637cd2b5"), 15);
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = check_argument_bank(
            *counterfactual_argument, QStringLiteral("counterfactual_training"),
            QStringLiteral("27f6387c45efb7ac62c798164dbd1a78cfc699bc8cc53a7c36b75e8a220e7124"), 10);
        error.has_value()) {
        return fail(*error);
    }
    const auto counterfactual_text = QString::fromUtf8(
        QJsonDocument(counterfactual_argument->document).toJson(QJsonDocument::Compact));
    if (!counterfactual_text.contains(QStringLiteral(
            "ca4m4.arm.question.counterfactual-petition-timeliness-premise-reversal")) ||
        !counterfactual_text.contains(QStringLiteral(
            "ca4m4.arm.question.counterfactual-petition-timeliness-adverse-remedy")) ||
        !counterfactual_text.contains(QStringLiteral("day-31 petition")) ||
        !counterfactual_text.contains(QStringLiteral("timely Government invocation"))) {
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
    if (dockets.size() != 3 || entries.size() != 54 || anchors.size() != 415) {
        return fail(QStringLiteral("record count contract mismatch"));
    }

    QHash<QString, QJsonObject> anchor_by_label;
    QHash<QString, QJsonObject> anchor_by_entry_page;
    for (const auto& anchor_value : anchors) {
        const auto anchor = anchor_value.toObject();
        const auto label = anchor.value(QStringLiteral("citation_label")).toString();
        const auto entry_page = QStringLiteral("%1|%2")
                                    .arg(anchor.value(QStringLiteral("entry_id")).toString())
                                    .arg(anchor.value(QStringLiteral("page_number")).toInt());
        if (label.isEmpty() || anchor_by_label.contains(label) ||
            anchor_by_entry_page.contains(entry_page)) {
            return fail(QStringLiteral("duplicate or empty page-anchor label"));
        }
        anchor_by_label.insert(label, anchor);
        anchor_by_entry_page.insert(entry_page, anchor);
    }
    for (int page = 1; page <= 238; ++page) {
        if (!anchor_by_label.contains(QStringLiteral("AR%1").arg(page))) {
            return fail(QStringLiteral("AR continuity mismatch at %1").arg(page));
        }
    }
    for (int page = 1; page <= 177; ++page) {
        if (!anchor_by_label.contains(QStringLiteral("PA%1").arg(page))) {
            return fail(QStringLiteral("PA continuity mismatch at %1").arg(page));
        }
    }

    const QRegularExpression any_page_label(QStringLiteral("\\b(?:AR|PA)\\d+\\b"));
    const QRegularExpression footer_page_label(QStringLiteral("\\b(?:AR|PA)\\d+\\s*$"));
    const QRegularExpression pa_page_label(QStringLiteral("\\bPA\\d+\\b"));
    const QHash<QString, QStringList> required_page_propositions{
        {QStringLiteral("AR105"),
         {QStringLiteral("single ten-page physical file"),
          QStringLiteral("controlled text exports rather than raster reproductions"),
          QStringLiteral("p-4, p-5, and p-6 separately as lodged with a merits ruling required")}},
        {QStringLiteral("AR106"),
         {QStringLiteral("kal-msg-1"),
          QStringLiteral("c634626d66ffe2705fb504903cb6a90a50ceb455d11215071ae419c013bdefc3"),
          QStringLiteral("kal-msg-2"),
          QStringLiteral("13d243c3dd7d367a2a091e1bfaa596883bb4625ba264d36214a36c26951b6475")}},
        {QStringLiteral("AR107"),
         {QStringLiteral("kal-msg-3"), QStringLiteral("kal-msg-4"),
          QStringLiteral("four selected p-4 messages"),
          QStringLiteral("four post-departure messages")}},
        {QStringLiteral("AR108"),
         {QStringLiteral("kal-oci-1"), QStringLiteral("oci-23-441"),
          QStringLiteral("initial route ic-2"),
          QStringLiteral("2e9b8904310bf6c0060c36d4c945092bcdee24eea9b00a04b0b68a947f12d051")}},
        {QStringLiteral("AR109"),
         {QStringLiteral("kal-route-1"), QStringLiteral("2023-06-09:link=civ-23-184"),
          QStringLiteral("2023-06-10:destination=rc-4"),
          QStringLiteral("2023-06-30:status=regional response pending")}},
        {QStringLiteral("AR110"),
         {QStringLiteral("kal-clinic-1"), QStringLiteral("2023-05-21t14:20:00+09:00"),
          QStringLiteral("limited left-shoulder elevation"),
          QStringLiteral("right-ankle swelling")}},
        {QStringLiteral("AR111"),
         {QStringLiteral("kal-ledger-1"), QStringLiteral("threshold=25000"),
          QStringLiteral("total=137400"), QStringLiteral("grouping=all six grouped")}},
        {QStringLiteral("AR112"),
         {QStringLiteral("kal-ledger-2"), QStringLiteral("grouping=removed"),
          QStringLiteral("first grouped, then corrected to remove grouping")}},
        {QStringLiteral("AR113"),
         {QStringLiteral("kal-trans-1"),
          QStringLiteral("b8928fc635fc914b9abc49c20ed5a307067c5692a7eb9109158c81723eb1ee71"),
          QStringLiteral("ten listed controls")}},
        {QStringLiteral("AR114"),
         {QStringLiteral("p-4 maps to agency pages 106–107"),
          QStringLiteral("p-5 maps to agency pages 108–109"),
          QStringLiteral("p-6 maps to shared control page 105 and agency pages 110–114"),
          QStringLiteral("contains no raster image object")}},
        {QStringLiteral("AR117"),
         {QStringLiteral("no authenticity objection"), QStringLiteral("judge admits p-7")}},
        {QStringLiteral("AR138"),
         {QStringLiteral("admission of p-1 through p-9"),
          QStringLiteral("no objection remains to p-7")}},
        {QStringLiteral("AR139"),
         {QStringLiteral("records p-7 as admitted"), QStringLiteral("same parties")}},
        {QStringLiteral("AR161"),
         {QStringLiteral("confirms admission of p-1 through p-9"),
          QStringLiteral("excludes no part of p-7")}},
        {QStringLiteral("AR164"),
         {QStringLiteral("admitted exhibits are p-1 through p-9"),
          QStringLiteral("no material outside")}},
        {QStringLiteral("AR219"),
         {QStringLiteral("sequence-control order"),
          QStringLiteral("sixteen pdf files and 208 physical pages"),
          QStringLiteral("certifies this production")}},
        {QStringLiteral("AR220"),
         {QStringLiteral("03-cat-application.pdf"), QStringLiteral("05-sibling-declaration.pdf"),
          QStringLiteral("moves from page 32 to page 51"),
          QStringLiteral("makes no inference from the numerical interval")}},
        {QStringLiteral("AR221"),
         {QStringLiteral("combined physical p-4/p-5/p-6 packet"),
          QStringLiteral("internal p-4, p-5, and p-6 logical mapping unchanged")}},
        {QStringLiteral("AR222"),
         {QStringLiteral("16-bia-final-order.pdf"),
          QStringLiteral("closes the selected production at page 226")}},
        {QStringLiteral("AR223"),
         {QStringLiteral("opened each of the sixteen listed pdfs"),
          QStringLiteral("counted 208 pages"),
          QStringLiteral("limited to the selection returned by the export query")}},
        {QStringLiteral("AR224"),
         {QStringLiteral("label_discontinuity_18"), QStringLiteral("cause unresolved"),
          QStringLiteral("no deletion event"),
          QStringLiteral("does not attribute the discontinuity to a cause")}},
        {QStringLiteral("AR225"),
         {QStringLiteral("pages 1 through 32 and 51 through 226"),
          QStringLiteral("complete agency materials for this transmission"),
          QStringLiteral("does not claim a field-by-field comparison")}},
        {QStringLiteral("AR226"),
         {QStringLiteral("discrepancy notice must identify"),
          QStringLiteral("preserve the original selection"),
          QStringLiteral("initial transmittal available as a distinct historical filing")}},
        {QStringLiteral("AR227"),
         {QStringLiteral("stipulation-16b"),
          QStringLiteral("b26f4d1618332a6e006839a09c4ab77319bde4f58c6d3e219da4fbcfbe1ce855"),
          QStringLiteral("federal rule of appellate procedure 16(b)"),
          QStringLiteral("not evidence presented to the immigration judge on the merits")}},
        {QStringLiteral("AR228"),
         {QStringLiteral("april 12 lodging receipt"), QStringLiteral("kal-msg-1 through kal-msg-4"),
          QStringLiteral("kal-oci-1 and kal-route-1"),
          QStringLiteral("04-arm-sworn-declaration.pdf")}},
        {QStringLiteral("AR229"),
         {QStringLiteral("september hearing receipt"),
          QStringLiteral("every exhibit p-1 through p-9 admitted"),
          QStringLiteral("no authenticity or timeliness objection"),
          QStringLiteral("agency page 105 and pages 110–114")}},
        {QStringLiteral("AR230"),
         {QStringLiteral("agency page 117"), QStringLiteral("agency page 138"),
          QStringLiteral("agency page 139"), QStringLiteral("agency page 161"),
          QStringLiteral("verify the historic rulings")}},
        {QStringLiteral("AR231"),
         {QStringLiteral("agency page 164"), QStringLiteral("label_discontinuity_18"),
          QStringLiteral("do not yet assign a technical cause")}},
        {QStringLiteral("AR232"),
         {QStringLiteral("lodged_merits_ruling_required"), QStringLiteral("admitted_2024-09-17"),
          QStringLiteral("without joining the hearing receipt"),
          QStringLiteral("appears first in this march audit")}},
        {QStringLiteral("AR233"),
         {QStringLiteral("08e8294532c23fe9feb5962ca5b7780ae958178e6c8e2b4840d1ee28f3c5d212"),
          QStringLiteral("no annotation"), QStringLiteral("unchanged admitted object")}},
        {QStringLiteral("AR234"),
         {QStringLiteral("agency pages 33–50"), QStringLiteral("every agency number 1 through 238"),
          QStringLiteral("p-4 is pages 106–107"), QStringLiteral("p-5 is pages 108–109"),
          QStringLiteral("p-6 is page 105 and pages 110–114")}},
        {QStringLiteral("AR235"),
         {QStringLiteral("t.r. declaration signed february 20, 2025"),
          QStringLiteral("appellate proffer pages 1–8"),
          QStringLiteral("receive no agency-record label"),
          QStringLiteral("contains no page, text object, or digest from that proffer")}},
        {QStringLiteral("AR236"),
         {QStringLiteral("documents 1 through 9"),
          QStringLiteral("rows 1 through 9 total 114 pages"),
          QStringLiteral("combined p-4/p-5/p-6"), QStringLiteral("04-arm-sworn-declaration.pdf")}},
        {QStringLiteral("AR237"),
         {QStringLiteral("documents 10 through 18"),
          QStringLiteral("rows 10 through 18 total 124 pages"),
          QStringLiteral("exactly eighteen unique pdfs and 238 pages"),
          QStringLiteral("appellate proffer pages 1–8 are excluded")}},
        {QStringLiteral("AR238"),
         {QStringLiteral("rule 16(b) stipulation"), QStringLiteral("restored agency pages 33–50"),
          QStringLiteral("contains no bytes from appellate proffer pages 1–8"),
          QStringLiteral("initial february selection")}},
    };
    QSet<QString> verified_page_propositions;
    QSet<QString> distinct_page_bodies;
    int administrative_documents = 0;
    int administrative_pages = 0;
    int generated_documents = 0;
    int generated_pages = 0;
    int actual_documents = 0;
    int actual_pages = 0;
    int branch_documents = 0;
    int branch_pages = 0;
    int rendered_placeholder_occurrences = 0;
    bool saw_proven_p7 = false;
    bool saw_new_proffer = false;
    bool saw_admission_page = false;
    bool saw_adjournment_admission_page = false;
    bool saw_reconvening_admission_page = false;
    bool saw_closed_record_page = false;
    bool saw_ij_admission_page = false;
    bool saw_initial_gap_page = false;
    bool saw_initial_certification_page = false;
    bool saw_audit_receipt_page = false;
    bool saw_transcript_crosscheck_page = false;
    bool saw_decision_crosscheck_page = false;
    bool saw_exact_identity_page = false;
    bool saw_correction_page = false;
    bool saw_pa_exclusion_page = false;
    bool saw_correction_certification_exclusion_page = false;
    bool saw_final_transmission_exclusion_page = false;
    QSet<int> entry_numbers;
    QSet<QString> record_asset_paths;
    QHash<QString, int> record_page_counts;
    QHash<QString, int> record_label_starts;
    QHash<QString, QString> record_label_prefixes;
    int expected_entry_number = 1;
    QHash<QString, int> category_counts;
    const QSet<QString> administrative_categories{
        QStringLiteral("nta_pleading"),
        QStringLiteral("application_declaration_family"),
        QStringLiteral("country_medical_translation"),
        QStringLiteral("ij_transcript"),
        QStringLiteral("ij_decision"),
        QStringLiteral("bia_notice_brief_response"),
        QStringLiteral("bia_final_order"),
        QStringLiteral("certified_index_omission"),
    };
    const QSet<QString> admitted_exhibit_entries{
        QStringLiteral("ca4m4.arm.record.ar03"), QStringLiteral("ca4m4.arm.record.ar04"),
        QStringLiteral("ca4m4.arm.record.ar05"), QStringLiteral("ca4m4.arm.record.ar06"),
        QStringLiteral("ca4m4.arm.record.ar07"), QStringLiteral("ca4m4.arm.record.ar08"),
        QStringLiteral("ca4m4.arm.record.ar09"),
    };
    bool saw_exact_combined_packet_entry = false;
    QHash<QString, QJsonObject> record_entry_by_id;
    QHash<QString, QString> record_entry_id_by_sha;
    QHash<QString, QString> last_filed_on_by_docket;
    QStringList legacy_blob_identity_lines;

    for (const auto& entry_value : entries) {
        const auto entry = entry_value.toObject();
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        const auto entry_number = entry.value(QStringLiteral("entry_number")).toInt();
        if (entry_number != expected_entry_number++ || entry_numbers.contains(entry_number)) {
            return fail(QStringLiteral("record entry numbers are not exact, ordered, and unique"));
        }
        entry_numbers.insert(entry_number);
        const bool generated = tags.contains(QStringLiteral("generated_appellate_filing"));
        const bool administrative =
            tags.contains(QStringLiteral("certified_administrative_record"));
        const bool actual = tags.contains(QStringLiteral("actual_appellate_docket"));
        const bool branch = tags.contains(QStringLiteral("counterfactual_appellate_branch"));
        const auto classification_count = static_cast<int>(administrative) +
                                          static_cast<int>(generated) + static_cast<int>(actual) +
                                          static_cast<int>(branch);
        if (classification_count != 1 || tags.contains(QStringLiteral("batch_1")) ||
            (branch != tags.contains(QStringLiteral("never_filed"))) ||
            (administrative && tags.contains(QStringLiteral("not_administrative_record"))) ||
            (!administrative && !tags.contains(QStringLiteral("not_administrative_record")))) {
            return fail(QStringLiteral("entry classification is ambiguous or crosses dockets"));
        }
        const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
        const auto docket_id = entry.value(QStringLiteral("docket_id")).toString();
        const auto filed_on = entry.value(QStringLiteral("filed_on")).toString();
        const auto asset_sha = entry.value(QStringLiteral("asset_sha256")).toString();
        if (entry_id.isEmpty() || record_entry_by_id.contains(entry_id) || asset_sha.isEmpty() ||
            record_entry_id_by_sha.contains(asset_sha) ||
            (!administrative && !last_filed_on_by_docket.value(docket_id).isEmpty() &&
             last_filed_on_by_docket.value(docket_id) > filed_on)) {
            return fail(QStringLiteral("record identity, digest, or chronology mismatch: %1")
                            .arg(entry_id));
        }
        record_entry_by_id.insert(entry_id, entry);
        record_entry_id_by_sha.insert(asset_sha, entry_id);
        if (!administrative) {
            last_filed_on_by_docket.insert(docket_id, filed_on);
        }
        if (admitted_exhibit_entries.contains(entry_id) !=
            tags.contains(QStringLiteral("admitted"))) {
            return fail(
                QStringLiteral("P-1-through-P-9 admitted tag contract drifted: %1").arg(entry_id));
        }
        if (entry_id == QStringLiteral("ca4m4.arm.record.ar09")) {
            saw_exact_combined_packet_entry =
                entry.value(QStringLiteral("entry_label")).toString() ==
                    QStringLiteral("Combined Agency Exhibits P-4/P-5/P-6") &&
                tags.contains(QStringLiteral("combined_physical_packet")) &&
                tags.contains(QStringLiteral("logical_exhibit_p4")) &&
                tags.contains(QStringLiteral("logical_exhibit_p5")) &&
                tags.contains(QStringLiteral("logical_exhibit_p6")) &&
                entry.value(QStringLiteral("description"))
                    .toString()
                    .contains(QStringLiteral("exact object hashes"));
        }

        if (administrative) {
            ++administrative_documents;
            administrative_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (entry.value(QStringLiteral("docket_id")).toString() !=
                QStringLiteral("ca4m4.arm.docket.agency")) {
                return fail(QStringLiteral("administrative record entry is on wrong docket"));
            }
            QString category;
            for (const auto& candidate : administrative_categories) {
                if (tags.contains(candidate)) {
                    if (!category.isEmpty()) {
                        return fail(QStringLiteral("administrative entry has multiple categories"));
                    }
                    category = candidate;
                }
            }
            if (category.isEmpty()) {
                return fail(QStringLiteral("administrative entry has no frozen category"));
            }
            ++category_counts[category];
        } else if (generated) {
            ++generated_documents;
            generated_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (!tags.contains(QStringLiteral("extra_record_proffer")) ||
                !tags.contains(QStringLiteral("not_administrative_record")) ||
                docket_id != QStringLiteral("ca4m4.arm.docket.ca4") ||
                entry_id != QStringLiteral("ca4m4.arm.record.pa01") ||
                entry.value(QStringLiteral("parent_entry_id")).toString() !=
                    QStringLiteral("ca4m4.arm.record.a06") ||
                entry.value(QStringLiteral("relationship")).toString() !=
                    QStringLiteral("attachment")) {
                return fail(QStringLiteral("generated PA proffer classification mismatch"));
            }
            saw_new_proffer = true;
        } else if (actual) {
            ++actual_documents;
            actual_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (docket_id != QStringLiteral("ca4m4.arm.docket.ca4")) {
                return fail(QStringLiteral("actual appellate entry is on wrong docket"));
            }
        } else {
            ++branch_documents;
            branch_pages += entry.value(QStringLiteral("page_count")).toInt();
            if (docket_id != QStringLiteral("ca4m4.arm.docket.counterfactual-branches")) {
                return fail(QStringLiteral("counterfactual entry is on wrong docket"));
            }
        }

        if (entry_id == QStringLiteral("ca4m4.arm.record.ar04")) {
            saw_proven_p7 = tags.contains(QStringLiteral("admitted")) &&
                            tags.contains(QStringLiteral("initially_omitted")) &&
                            tags.contains(QStringLiteral("corrected_record")) &&
                            tags.contains(QStringLiteral("certified_administrative_record")) &&
                            !tags.contains(QStringLiteral("claimed_admission")) &&
                            !tags.contains(QStringLiteral("claimed_initial_omission")) &&
                            !tags.contains(QStringLiteral("planned_corrected_record")) &&
                            entry.value(QStringLiteral("description"))
                                .toString()
                                .contains(QStringLiteral("restored unchanged"));
        }

        const auto relative_asset = entry.value(QStringLiteral("asset_path")).toString();
        if (relative_asset.isEmpty() || record_asset_paths.contains(relative_asset)) {
            return fail(QStringLiteral("record asset paths are not exact and unique"));
        }
        record_asset_paths.insert(relative_asset);
        record_page_counts.insert(relative_asset,
                                  entry.value(QStringLiteral("page_count")).toInt());
        const auto first_anchor = anchor_by_entry_page.value(QStringLiteral("%1|1").arg(entry_id));
        const auto first_label = first_anchor.value(QStringLiteral("citation_label")).toString();
        bool label_start_parsed = false;
        const auto label_start = first_label.mid(2).toInt(&label_start_parsed);
        if (first_anchor.isEmpty() || !label_start_parsed || label_start < 1 ||
            first_label.startsWith(administrative ? QStringLiteral("AR") : QStringLiteral("PA")) ==
                false) {
            return fail(QStringLiteral("entry has no exact first page anchor: %1").arg(entry_id));
        }
        record_label_starts.insert(relative_asset, label_start);
        record_label_prefixes.insert(relative_asset,
                                     administrative ? QStringLiteral("AR") : QStringLiteral("PA"));
        const auto pdf_bytes = readAll(QDir(pack_root).filePath(relative_asset));
        if (administrative || generated) {
            legacy_blob_identity_lines.push_back(
                QStringLiteral("%1|%2|%3\n").arg(relative_asset, asset_sha).arg(pdf_bytes.size()));
        }
        QPdfDocument pdf;
        if (pdf.load(QDir(pack_root).filePath(relative_asset)) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready ||
            pdf.pageCount() != entry.value(QStringLiteral("page_count")).toInt()) {
            return fail(QStringLiteral("PDF load/page-count failure: %1").arg(relative_asset));
        }
        const auto normalized_markdown_pages =
            normalized_markdown_pages_by_asset.value(relative_asset);
        if (normalized_markdown_pages.size() != pdf.pageCount()) {
            return fail(QStringLiteral("Markdown/PDF page-count mismatch: %1").arg(relative_asset));
        }
        if (pdf_bytes.contains("/Subtype /Image") || pdf_bytes.contains("/Subtype/Image")) {
            return fail(QStringLiteral("searchable-text PDF contains raster image objects: %1")
                            .arg(relative_asset));
        }
        int extracted_banner_occurrences = 0;
        const auto entry_banner = branch      ? branch_banner
                                  : actual    ? appellate_banner
                                  : generated ? proffer_banner
                                              : record_banner;

        for (int page_index = 0; page_index < pdf.pageCount(); ++page_index) {
            const auto page_anchor = anchor_by_entry_page.value(
                QStringLiteral("%1|%2").arg(entry_id).arg(page_index + 1));
            const auto expected_label =
                page_anchor.value(QStringLiteral("citation_label")).toString();
            auto page_text = pdf.getAllText(page_index).text().simplified();
            QStringList extracted_labels;
            auto label_matches = any_page_label.globalMatch(page_text);
            while (label_matches.hasNext()) {
                extracted_labels.push_back(label_matches.next().captured());
            }
            const auto footer_match = footer_page_label.match(page_text);
            const bool batch_two_administrative =
                administrative && entry_number >= 7 && entry_number <= 18;
            if (page_text.size() < 500 || !footer_match.hasMatch() ||
                footer_match.captured() != expected_label ||
                (administrative && extracted_labels != QStringList{expected_label})) {
                return fail(QStringLiteral("thin or unlabeled searchable page %1 in %2")
                                .arg(expected_label, relative_asset));
            }
            auto extracted_body = page_text;
            extracted_body.remove(QRegularExpression(
                QStringLiteral("\\s*%1\\s*$").arg(QRegularExpression::escape(expected_label))));
            if (normalizedSemanticText(extracted_body) !=
                normalized_markdown_pages.at(page_index)) {
                return fail(QStringLiteral("full normalized Markdown/PDF text mismatch at %1 in %2")
                                .arg(expected_label, relative_asset));
            }
            const auto lower_page_text = page_text.toLower();
            extracted_banner_occurrences += static_cast<int>(page_text.count(entry_banner));
            if (batch_two_administrative) {
                auto lower_record_body = lower_page_text;
                lower_record_body.remove(record_banner.toLower());
                for (const auto& phrase : forbidden_batch_two_record_voice) {
                    if (lower_record_body.contains(phrase)) {
                        return fail(QStringLiteral("rendered batch-2 record voice leaked at %1: %2")
                                        .arg(expected_label, phrase));
                    }
                }
            }
            if ((administrative && pa_page_label.match(page_text).hasMatch()) ||
                (generated && compiled_ar_label.match(page_text).hasMatch())) {
                return fail(
                    QStringLiteral("AR/PA label universe contaminated at %1").arg(expected_label));
            }
            if ((administrative || generated) &&
                lower_page_text.contains(QStringLiteral("placeholder"))) {
                ++rendered_placeholder_occurrences;
                return fail(
                    QStringLiteral("rendered placeholder escaped into %1").arg(expected_label));
            }
            saw_admission_page =
                saw_admission_page || (expected_label == QStringLiteral("AR117") &&
                                       lower_page_text.contains(QStringLiteral("admits p-7")) &&
                                       lower_page_text.contains(QStringLiteral("no authenticity")));
            saw_adjournment_admission_page =
                saw_adjournment_admission_page ||
                (expected_label == QStringLiteral("AR138") &&
                 lower_page_text.contains(QStringLiteral("admission of p-1 through p-9")) &&
                 lower_page_text.contains(QStringLiteral("no objection remains to p-7")));
            saw_reconvening_admission_page =
                saw_reconvening_admission_page ||
                (expected_label == QStringLiteral("AR139") &&
                 lower_page_text.contains(QStringLiteral("records p-7 as admitted")));
            saw_closed_record_page =
                saw_closed_record_page ||
                (expected_label == QStringLiteral("AR161") &&
                 lower_page_text.contains(
                     QStringLiteral("confirms admission of p-1 through p-9")) &&
                 lower_page_text.contains(QStringLiteral("excludes no part of p-7")));
            saw_ij_admission_page =
                saw_ij_admission_page ||
                (expected_label == QStringLiteral("AR164") &&
                 lower_page_text.contains(
                     QStringLiteral("admitted exhibits are p-1 through p-9")) &&
                 lower_page_text.contains(QStringLiteral("no material outside")));
            saw_initial_gap_page =
                saw_initial_gap_page ||
                (expected_label == QStringLiteral("AR224") &&
                 lower_page_text.contains(QStringLiteral("label_discontinuity_18")) &&
                 lower_page_text.contains(QStringLiteral("cause unresolved")));
            saw_initial_certification_page =
                saw_initial_certification_page ||
                (expected_label == QStringLiteral("AR225") &&
                 lower_page_text.contains(QStringLiteral("complete agency materials")) &&
                 lower_page_text.contains(QStringLiteral("field-by-field comparison")));
            saw_audit_receipt_page =
                saw_audit_receipt_page ||
                (expected_label == QStringLiteral("AR229") &&
                 lower_page_text.contains(
                     QStringLiteral("every exhibit p-1 through p-9 admitted")) &&
                 lower_page_text.contains(
                     QStringLiteral("no authenticity or timeliness objection")));
            saw_transcript_crosscheck_page =
                saw_transcript_crosscheck_page ||
                (expected_label == QStringLiteral("AR230") &&
                 lower_page_text.contains(QStringLiteral("agency page 117")) &&
                 lower_page_text.contains(QStringLiteral("agency page 161")));
            saw_decision_crosscheck_page =
                saw_decision_crosscheck_page ||
                (expected_label == QStringLiteral("AR231") &&
                 lower_page_text.contains(QStringLiteral("agency page 164")) &&
                 lower_page_text.contains(QStringLiteral("do not yet assign a technical cause")));
            saw_exact_identity_page =
                saw_exact_identity_page ||
                (expected_label == QStringLiteral("AR233") &&
                 lower_page_text.contains(QStringLiteral(
                     "08e8294532c23fe9feb5962ca5b7780ae958178e6c8e2b4840d1ee28f3c5d212")) &&
                 lower_page_text.contains(QStringLiteral("no annotation")));
            saw_correction_page =
                saw_correction_page ||
                (expected_label == QStringLiteral("AR237") &&
                 lower_page_text.contains(QStringLiteral("exactly eighteen unique pdfs")) &&
                 lower_page_text.contains(QStringLiteral("238 pages")));
            saw_pa_exclusion_page =
                saw_pa_exclusion_page ||
                (expected_label == QStringLiteral("AR235") &&
                 lower_page_text.contains(QStringLiteral("appellate proffer pages 1–8")) &&
                 lower_page_text.contains(QStringLiteral("receive no agency-record label")));
            saw_correction_certification_exclusion_page =
                saw_correction_certification_exclusion_page ||
                (expected_label == QStringLiteral("AR237") &&
                 lower_page_text.contains(
                     QStringLiteral("appellate proffer pages 1–8 are excluded")));
            saw_final_transmission_exclusion_page =
                saw_final_transmission_exclusion_page ||
                (expected_label == QStringLiteral("AR238") &&
                 lower_page_text.contains(
                     QStringLiteral("contains no bytes from appellate proffer pages 1–8")));
            for (const auto& phrase : forbidden_authoring_voice) {
                if ((administrative || generated) && lower_page_text.contains(phrase)) {
                    return fail(QStringLiteral("rendered authoring voice leaked at %1: %2")
                                    .arg(expected_label, phrase));
                }
            }
            if (const auto required = required_page_propositions.constFind(expected_label);
                required != required_page_propositions.cend()) {
                for (const auto& phrase : *required) {
                    if (!lower_page_text.contains(phrase)) {
                        return fail(QStringLiteral("record proposition missing at %1: %2")
                                        .arg(expected_label, phrase));
                    }
                }
                if (expected_label == QStringLiteral("AR224") &&
                    (lower_page_text.contains(QStringLiteral("metadata")) ||
                     lower_page_text.contains(QStringLiteral("status field")) ||
                     lower_page_text.contains(QStringLiteral("queried")))) {
                    return fail(QStringLiteral("AR224 prematurely explains the later-found cause"));
                }
                verified_page_propositions.insert(expected_label);
            }
            page_text.remove(any_page_label);
            page_text = page_text.simplified();
            if (distinct_page_bodies.contains(page_text)) {
                return fail(
                    QStringLiteral("duplicate substantive page body at %1").arg(expected_label));
            }
            distinct_page_bodies.insert(page_text);

            const auto anchor = anchor_by_label.value(expected_label);
            if (anchor.isEmpty() || page_anchor != anchor ||
                anchor.value(QStringLiteral("entry_id")).toString() !=
                    entry.value(QStringLiteral("entry_id")).toString() ||
                anchor.value(QStringLiteral("page_number")).toInt() != page_index + 1 ||
                anchor.value(QStringLiteral("anchor_id")).toString() !=
                    QStringLiteral("ca4m4.arm.anchor.%1").arg(expected_label.toLower())) {
                return fail(QStringLiteral("page-anchor mismatch at %1").arg(expected_label));
            }
        }
        if (extracted_banner_occurrences != 1) {
            return fail(QStringLiteral("PDF must contain exactly one classification banner: %1")
                            .arg(relative_asset));
        }
    }

    std::ranges::sort(legacy_blob_identity_lines);
    QCryptographicHash legacy_identity_hash(QCryptographicHash::Sha256);
    for (const auto& line : legacy_blob_identity_lines) {
        const auto bytes = line.toUtf8();
        legacy_identity_hash.addData(QByteArrayView(bytes));
    }
    if (legacy_blob_identity_lines.size() != 19 ||
        legacy_identity_hash.result().toHex() != QByteArray(legacy_blob_identity_digest)) {
        return fail(QStringLiteral("original 19-PDF identity set drifted from ARM 1.1"));
    }

    const QHash<QString, int> expected_categories{
        {QStringLiteral("nta_pleading"), 2},
        {QStringLiteral("application_declaration_family"), 4},
        {QStringLiteral("country_medical_translation"), 3},
        {QStringLiteral("ij_transcript"), 2},
        {QStringLiteral("ij_decision"), 1},
        {QStringLiteral("bia_notice_brief_response"), 3},
        {QStringLiteral("bia_final_order"), 1},
        {QStringLiteral("certified_index_omission"), 2},
    };
    if (administrative_documents != 18 || administrative_pages != 238 || actual_documents != 22 ||
        actual_pages != 119 || generated_documents != 1 || generated_pages != 8 ||
        branch_documents != 13 || branch_pages != 50 || distinct_page_bodies.size() != 415 ||
        !saw_proven_p7 || !saw_new_proffer || !saw_exact_combined_packet_entry ||
        category_counts != expected_categories || rendered_placeholder_occurrences != 0 ||
        !saw_admission_page || !saw_adjournment_admission_page || !saw_reconvening_admission_page ||
        !saw_closed_record_page || !saw_ij_admission_page || !saw_initial_gap_page ||
        !saw_initial_certification_page || !saw_audit_receipt_page ||
        !saw_transcript_crosscheck_page || !saw_decision_crosscheck_page ||
        !saw_exact_identity_page || !saw_correction_page || !saw_pa_exclusion_page ||
        !saw_correction_certification_exclusion_page || !saw_final_transmission_exclusion_page) {
        return fail(QStringLiteral("AR/PA count, continuity, or semantic distinction mismatch"));
    }
    if (verified_page_propositions.size() != required_page_propositions.size()) {
        return fail(QStringLiteral("P-7 admission/omission/correction chain is incomplete"));
    }
    const auto p7_bytes =
        readAll(QDir(pack_root).filePath(QStringLiteral("assets/04-arm-sworn-declaration.pdf")));
    const auto appellate_stipulation_source = readAll(authoring_root.filePath(
        QStringLiteral("documents/appellate-actual/a11-rule16b-stipulation.md")));
    if (QCryptographicHash::hash(p7_bytes, QCryptographicHash::Sha256).toHex() !=
            QByteArrayLiteral("08e8294532c23fe9feb5962ca5b7780ae958178e6c8e2b4840d1ee28f3c5d212") ||
        !appellate_stipulation_source.contains(stipulation_bytes) ||
        !appellate_stipulation_source.contains(stipulation_hash)) {
        return fail(QStringLiteral("P-7 or Rule 16(b) successor identity drifted"));
    }

    struct RenderInventorySpec final {
        QString plan_path;
        QString inventory_path;
        int entry_count{};
        bool historical_base{};
    };
    const std::array render_inventory_specs{
        RenderInventorySpec{QStringLiteral("render-plan-batch-1.json"),
                            QStringLiteral("metadata/render-inventory-batch-1.json"), 7, true},
        RenderInventorySpec{QStringLiteral("render-plan-canonical-repair.json"),
                            QStringLiteral("metadata/render-inventory-canonical-repair.json"), 14,
                            false},
        RenderInventorySpec{QStringLiteral("render-plan-successor.json"),
                            QStringLiteral("metadata/render-inventory-successor.json"), 35, false},
    };
    if (QFileInfo::exists(authoring_root.filePath(QStringLiteral("render-plan-batch-2.json"))) ||
        QFileInfo::exists(
            authoring_root.filePath(QStringLiteral("metadata/render-inventory-batch-2.json")))) {
        return fail(QStringLiteral("rejected batch-2 plan or inventory remains canonical-looking"));
    }
    QSet<QString> authored_source_paths;
    for (const auto& markdown_path : markdown_paths) {
        authored_source_paths.insert(authoring_root.relativeFilePath(markdown_path));
    }
    QSet<QString> manifest_blob_paths;
    for (const auto& blob : source->blobs) {
        const auto path = QString::fromStdString(blob.path);
        if (blob.media_type != "application/pdf" || manifest_blob_paths.contains(path)) {
            return fail(QStringLiteral("manifest blob paths are not exact, unique PDFs"));
        }
        manifest_blob_paths.insert(path);
    }
    QSet<QString> planned_source_paths;
    QSet<QString> planned_output_paths;
    QSet<QString> inventoried_source_paths;
    QSet<QString> inventoried_output_paths;
    QSet<QString> source_hashes;
    QSet<QString> assembly_hashes;
    QSet<QString> semantic_plan_hashes;
    QSet<QString> semantic_render_hashes;
    const QRegularExpression exact_sha256(QStringLiteral("^[0-9a-f]{64}$"));
    qsizetype pinned_render_entries = 0;
    qsizetype superseded_historical_entries = 0;
    for (const auto& specification : render_inventory_specs) {
        const auto plan_bytes = readAll(authoring_root.filePath(specification.plan_path));
        const auto plan = QJsonDocument::fromJson(plan_bytes).object();
        const auto plan_entries = plan.value(QStringLiteral("entries")).toArray();
        const auto plan_digest = QString::fromLatin1(
            QCryptographicHash::hash(plan_bytes, QCryptographicHash::Sha256).toHex());
        const auto inventory_bytes = readAll(authoring_root.filePath(specification.inventory_path));
        const auto render_inventory = QJsonDocument::fromJson(inventory_bytes).object();
        const auto render_entries = render_inventory.value(QStringLiteral("entries")).toArray();
        if (specification.inventory_path ==
                QStringLiteral("metadata/render-inventory-successor.json") &&
            QCryptographicHash::hash(inventory_bytes, QCryptographicHash::Sha256).toHex() !=
                QByteArray(successor_inventory_digest)) {
            return fail(QStringLiteral("successor render inventory digest mismatch"));
        }
        if (plan.value(QStringLiteral("schema_version")).toInt() != 1 || plan_bytes.isEmpty() ||
            plan_entries.size() != specification.entry_count ||
            render_inventory.value(QStringLiteral("schema_version")).toInt() != 1 ||
            render_inventory.value(QStringLiteral("plan_sha256")).toString() != plan_digest ||
            render_inventory.value(QStringLiteral("ordering")).toString() !=
                QStringLiteral("output_path_casefolded_then_codepoint") ||
            render_inventory.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
            render_inventory.value(QStringLiteral("renderer_contract")).toString() !=
                QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
            render_entries.size() != specification.entry_count) {
            return fail(QStringLiteral("canonical render inventory contract mismatch: %1")
                            .arg(specification.inventory_path));
        }
        QString previous_output_path;
        for (qsizetype index = 0; index < render_entries.size(); ++index) {
            const auto planned = plan_entries.at(index).toObject();
            const auto rendered_value = render_entries.at(index);
            const auto rendered = rendered_value.toObject();
            const auto output_path = rendered.value(QStringLiteral("output_path")).toString();
            const auto source_path = planned.value(QStringLiteral("source_path")).toString();
            const auto title = planned.value(QStringLiteral("title")).toString();
            const bool superseded_historical_entry =
                specification.historical_base &&
                (output_path == QStringLiteral("assets/03-cat-application.pdf") ||
                 output_path == QStringLiteral("assets/05-sibling-declaration.pdf"));
            if (output_path.isEmpty() || source_path.isEmpty() || title.isEmpty() ||
                (!previous_output_path.isEmpty() &&
                 previous_output_path.toCaseFolded() >= output_path.toCaseFolded()) ||
                planned.value(QStringLiteral("output_path")).toString() != output_path) {
                return fail(
                    QStringLiteral("render plan/inventory ordering drifted: %1").arg(output_path));
            }
            previous_output_path = output_path;
            if (superseded_historical_entry) {
                ++superseded_historical_entries;
                continue;
            }
            ++pinned_render_entries;
            const auto assembly = rendered.value(QStringLiteral("assembly_provenance")).toObject();
            const auto source_bytes = readAll(authoring_root.filePath(source_path));
            const auto source_digest =
                QCryptographicHash::hash(source_bytes, QCryptographicHash::Sha256).toHex();
            const auto pdf_bytes = readAll(QDir(pack_root).filePath(output_path));
            const auto pdf_digest =
                QCryptographicHash::hash(pdf_bytes, QCryptographicHash::Sha256).toHex();
            const auto blob = std::ranges::find(source->blobs, output_path.toStdString(),
                                                &appellate::model::BlobDescriptor::path);
            const auto record_entry = std::ranges::find_if(entries, [&](const auto& value) {
                return value.toObject().value(QStringLiteral("asset_path")).toString() ==
                       output_path;
            });
            const auto page_labels = rendered.value(QStringLiteral("page_labels")).toObject();
            const auto assembly_digest = QString::fromLatin1(
                QCryptographicHash::hash(QJsonDocument(assembly).toJson(QJsonDocument::Compact),
                                         QCryptographicHash::Sha256)
                    .toHex());
            const auto renderer_provenance =
                rendered.value(QStringLiteral("renderer_provenance")).toString();
            const auto semantic_render_digest =
                semanticRenderDigest(QByteArrayView(source_bytes), title, renderer_provenance);
            const auto semantic_plan_digest =
                semanticPlanDigest(assembly_digest, semantic_render_digest);
            const auto recorded_source_hash =
                rendered.value(QStringLiteral("source_sha256")).toString();
            const auto recorded_assembly_hash =
                rendered.value(QStringLiteral("assembly_plan_sha256")).toString();
            const auto recorded_semantic_plan_hash =
                rendered.value(QStringLiteral("semantic_plan_sha256")).toString();
            const auto recorded_semantic_render_hash =
                rendered.value(QStringLiteral("semantic_render_sha256")).toString();
            if (source_bytes.isEmpty() || pdf_bytes.isEmpty() || blob == source->blobs.end() ||
                record_entry == entries.end() || output_path.isEmpty() || source_path.isEmpty() ||
                planned_source_paths.contains(source_path) ||
                planned_output_paths.contains(output_path) ||
                inventoried_source_paths.contains(
                    assembly.value(QStringLiteral("source_path")).toString()) ||
                inventoried_output_paths.contains(output_path) ||
                planned.value(QStringLiteral("output_path")).toString() != output_path ||
                rendered.value(QStringLiteral("title")).toString() != title ||
                planned.value(QStringLiteral("page_label_prefix")).toString() !=
                    record_label_prefixes.value(output_path) ||
                planned.value(QStringLiteral("page_label_start")).toInt() !=
                    record_label_starts.value(output_path) ||
                rendered.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
                rendered.value(QStringLiteral("renderer_contract")).toString() !=
                    QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
                recorded_source_hash.toLatin1() != source_digest ||
                assembly.value(QStringLiteral("assembly_contract")).toString() !=
                    QStringLiteral("appellate.markdown-assembly.v1") ||
                assembly.value(QStringLiteral("kind")).toString() !=
                    QStringLiteral("single_source") ||
                assembly.value(QStringLiteral("source_path")).toString() != source_path ||
                assembly.value(QStringLiteral("source_sha256")).toString().toLatin1() !=
                    source_digest ||
                assembly.value(QStringLiteral("logical_page_count")).toInt() !=
                    record_page_counts.value(output_path) ||
                recorded_assembly_hash != assembly_digest ||
                recorded_semantic_render_hash != semantic_render_digest ||
                recorded_semantic_plan_hash != semantic_plan_digest ||
                !exact_sha256.match(recorded_source_hash).hasMatch() ||
                !exact_sha256.match(recorded_assembly_hash).hasMatch() ||
                !exact_sha256.match(recorded_semantic_render_hash).hasMatch() ||
                !exact_sha256.match(recorded_semantic_plan_hash).hasMatch() ||
                rendered.value(QStringLiteral("pdf_sha256")).toString().toLatin1() != pdf_digest ||
                rendered.value(QStringLiteral("byte_size")).toInteger() != pdf_bytes.size() ||
                rendered.value(QStringLiteral("page_count")).toInt() !=
                    record_page_counts.value(output_path) ||
                page_labels.value(QStringLiteral("prefix")).toString() !=
                    record_label_prefixes.value(output_path) ||
                page_labels.value(QStringLiteral("first_number")).toInt() !=
                    record_label_starts.value(output_path) ||
                page_labels.value(QStringLiteral("last_number")).toInt() !=
                    record_label_starts.value(output_path) + record_page_counts.value(output_path) -
                        1 ||
                record_entry->toObject().value(QStringLiteral("page_count")).toInt() !=
                    record_page_counts.value(output_path) ||
                record_entry->toObject()
                        .value(QStringLiteral("asset_sha256"))
                        .toString()
                        .toLatin1() != pdf_digest ||
                blob->sha256 != pdf_digest.toStdString() ||
                blob->byte_size != static_cast<std::uint64_t>(pdf_bytes.size())) {
                return fail(QStringLiteral("source/plan/inventory/record/blob closure mismatch: %1")
                                .arg(output_path));
            }
            if (source_hashes.contains(recorded_source_hash) ||
                assembly_hashes.contains(recorded_assembly_hash) ||
                semantic_plan_hashes.contains(recorded_semantic_plan_hash) ||
                semantic_render_hashes.contains(recorded_semantic_render_hash)) {
                return fail(QStringLiteral("render semantic or assembly identity is not unique: %1")
                                .arg(output_path));
            }
            planned_source_paths.insert(source_path);
            planned_output_paths.insert(output_path);
            inventoried_source_paths.insert(
                assembly.value(QStringLiteral("source_path")).toString());
            inventoried_output_paths.insert(output_path);
            source_hashes.insert(recorded_source_hash);
            assembly_hashes.insert(recorded_assembly_hash);
            semantic_plan_hashes.insert(recorded_semantic_plan_hash);
            semantic_render_hashes.insert(recorded_semantic_render_hash);
        }
    }
    if (pinned_render_entries != 54 || superseded_historical_entries != 2 ||
        authored_source_paths.size() != 54 || planned_source_paths != authored_source_paths ||
        inventoried_source_paths != authored_source_paths ||
        planned_output_paths != record_asset_paths ||
        inventoried_output_paths != record_asset_paths ||
        manifest_blob_paths != record_asset_paths || source_hashes.size() != 54 ||
        assembly_hashes.size() != 54 || semantic_plan_hashes.size() != 54 ||
        semantic_render_hashes.size() != 54) {
        return fail(QStringLiteral(
            "render plans and inventories do not close exactly over sources, record, and blobs"));
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
    int document_bindings = 0;
    QSet<QString> workflow_operation_ids;
    const auto workflow_stages =
        workflow_resource->document.value(QStringLiteral("stages")).toArray();
    const auto workflow_operations =
        workflow_resource->document.value(QStringLiteral("operations")).toArray();
    const auto workflow_routes =
        workflow_resource->document.value(QStringLiteral("filing_routes")).toArray();
    if (workflow_stages.size() != 13 || workflow_operations.size() != 67 ||
        workflow_routes.size() != 11 ||
        workflow_resource->document.value(QStringLiteral("initial_stage_id")).toString() !=
            QStringLiteral("ca4m4.arm.stage.opened")) {
        return fail(QStringLiteral("workflow 13/67/11 envelope mismatch"));
    }
    for (const auto& operation_value : workflow_operations) {
        const auto operation = operation_value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        const auto authority = operation.value(QStringLiteral("authority")).toObject();
        if (id.isEmpty() || workflow_operation_ids.contains(id)) {
            return fail(QStringLiteral("workflow operation IDs are empty or duplicated"));
        }
        workflow_operation_ids.insert(id);
        if (operation.value(QStringLiteral("opcode")).toString() ==
            QStringLiteral("calculate_deadline")) {
            ++calculated_deadlines;
        }
        const auto document_binding =
            operation.value(QStringLiteral("document_binding")).toObject();
        if (!document_binding.isEmpty()) {
            ++document_bindings;
            const auto bound_entry_id =
                document_binding.value(QStringLiteral("record_entry_id")).toString();
            const auto bound_entry = record_entry_by_id.value(bound_entry_id);
            if (bound_entry.isEmpty() ||
                document_binding.value(QStringLiteral("document_sha256")).toString() !=
                    bound_entry.value(QStringLiteral("asset_sha256")).toString() ||
                document_binding.value(QStringLiteral("expected_court_date")).toString() !=
                    bound_entry.value(QStringLiteral("filed_on")).toString()) {
                return fail(QStringLiteral("workflow document binding mismatch: %1").arg(id));
            }
        }
        if (id == QStringLiteral("ca4m4.arm.operation.enter-record-correction-order")) {
            saw_correction_order =
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("ca4m4.arm.authority.frap-16-record") &&
                operation.value(QStringLiteral("authorized_role_ids")).toArray() ==
                    QJsonArray{QStringLiteral("us.ca4.role.court")};
        }
        if (id == QStringLiteral("ca4m4.arm.operation.enter-supplement-denial-order")) {
            saw_supplement_order =
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("ca4m4.arm.authority.usc-1252-record-limit") &&
                operation.value(QStringLiteral("authorized_role_ids")).toArray() ==
                    QJsonArray{QStringLiteral("us.ca4.role.court")};
        }
        const auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
        const auto precondition_text =
            QString::fromUtf8(QJsonDocument(preconditions).toJson(QJsonDocument::Compact));
        for (const auto& precondition_value : preconditions) {
            const auto precondition = precondition_value.toObject();
            const auto entry_id = precondition.value(QStringLiteral("record_entry_id")).toString();
            if (!entry_id.isEmpty() &&
                precondition.value(QStringLiteral("document_sha256")).toString() !=
                    record_entry_by_id.value(entry_id)
                        .value(QStringLiteral("asset_sha256"))
                        .toString()) {
                return fail(QStringLiteral("workflow precondition binding mismatch: %1").arg(id));
            }
        }
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-docketing-statement")) {
            saw_docketing_clock =
                operation.value(QStringLiteral("deadline_days")).toInt() == 14 &&
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("us.ca4.authority.local-rule-3b-docketing") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.docketing-notice"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.advance-reply-to-argument")) {
            saw_briefing_complete_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.record.a17")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.reply-brief"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.schedule-argument")) {
            saw_argument_schedule_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.record.a17")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.record.a18"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.enter-argument-held-order")) {
            saw_argument_held_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.record.a18")) &&
                precondition_text.contains(QStringLiteral("argument_scheduled")) &&
                precondition_text.contains(QStringLiteral("argument_date_status")) &&
                precondition_text.contains(QStringLiteral("reached"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.advance-argument-to-submitted")) {
            saw_submitted_gate =
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.argument-held")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.record.a19"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.issue-judgment")) {
            saw_judgment_gate =
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.rehearing") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.order.argument-held")) &&
                document_binding.value(QStringLiteral("record_entry_id")).toString() ==
                    QStringLiteral("ca4m4.arm.record.a21");
        }
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-rehearing")) {
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
        if (id == QStringLiteral("ca4m4.arm.operation.advance-ordinary-mandate-wait")) {
            saw_mandate_wait_gate =
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-wait") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.rehearing")) &&
                precondition_text.contains(QStringLiteral("elapsed"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.calculate-ordinary-mandate")) {
            saw_mandate_delay =
                operation.value(QStringLiteral("stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.rehearing") &&
                operation.value(QStringLiteral("deadline_days")).toInt() == 7 &&
                operation.value(QStringLiteral("deadline_base_id")).toString() ==
                    QStringLiteral("ca4m4.arm.deadline.rehearing") &&
                operation.value(QStringLiteral("produced_deadline_id")).toString() ==
                    QStringLiteral("ca4m4.arm.deadline.mandate-ordinary") &&
                authority.value(QStringLiteral("primary_authority_id")).toString() ==
                    QStringLiteral("us.federal.authority.frap-41-mandate") &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.rehearing")) &&
                precondition_text.contains(QStringLiteral("open"));
        }
        if (id == QStringLiteral("ca4m4.arm.operation.issue-ordinary-mandate")) {
            saw_mandate_gate =
                operation.value(QStringLiteral("stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-wait") &&
                operation.value(QStringLiteral("next_stage_id")).toString() ==
                    QStringLiteral("ca4m4.arm.stage.mandate-issued") &&
                precondition_text.contains(QStringLiteral("judgment_issued")) &&
                precondition_text.contains(QStringLiteral("ca4m4.arm.deadline.mandate-ordinary")) &&
                precondition_text.contains(QStringLiteral("reached")) &&
                document_binding.value(QStringLiteral("record_entry_id")).toString() ==
                    QStringLiteral("ca4m4.arm.record.a22");
        }
    }
    if (calculated_deadlines != 14 || document_bindings != 19 || !saw_docketing_clock ||
        !saw_correction_order || !saw_supplement_order || !saw_briefing_complete_gate ||
        !saw_argument_schedule_gate || !saw_argument_held_gate || !saw_submitted_gate ||
        !saw_judgment_gate || !saw_rehearing_clock || !saw_mandate_wait_gate ||
        !saw_mandate_delay || !saw_mandate_gate) {
        return fail(QStringLiteral("record/order/timing workflow contract mismatch"));
    }

    const auto review = realism_resource->document;
    const auto dimensions = review.value(QStringLiteral("dimensions")).toObject();
    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    const auto evidence_packs = evidence.value(QStringLiteral("packs")).toArray();
    const auto evidence_resources = evidence.value(QStringLiteral("resources")).toArray();
    const auto evidence_blobs = evidence.value(QStringLiteral("blobs")).toArray();
    const auto evidence_traces = evidence.value(QStringLiteral("traces")).toArray();
    const auto evidence_record_checks = evidence.value(QStringLiteral("record_checks")).toArray();
    const auto evidence_authorities = evidence.value(QStringLiteral("authorities")).toArray();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const QSet<QString> expected_dimensions{
        QStringLiteral("procedural_law"),     QStringLiteral("deadlines_authority"),
        QStringLiteral("record_consistency"), QStringLiteral("bench_differentiation"),
        QStringLiteral("oral_argument"),      QStringLiteral("consequences"),
        QStringLiteral("provenance"),
    };
    QSet<QString> dimension_keys;
    bool all_dimensions_two = true;
    for (auto iterator = dimensions.constBegin(); iterator != dimensions.constEnd(); ++iterator) {
        dimension_keys.insert(iterator.key());
        all_dimensions_two = all_dimensions_two && iterator.value().toInt() == 2;
    }
    QSet<QString> dimension_evidence_keys;
    for (auto iterator = dimension_evidence.constBegin(); iterator != dimension_evidence.constEnd();
         ++iterator) {
        dimension_evidence_keys.insert(iterator.key());
    }
    if (review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        review.value(QStringLiteral("reviewed_on")).toString() != QStringLiteral("2026-08-12") ||
        dimension_keys != expected_dimensions || dimension_evidence_keys != expected_dimensions ||
        !all_dimensions_two || evidence_packs.size() != 4 || evidence_resources.size() != 44 ||
        evidence_blobs.size() != 54 || evidence_traces.size() != 7 ||
        evidence_record_checks.size() != 2 || evidence_authorities.size() != 32 ||
        evidence.value(QStringLiteral("closure_digest")).toString() !=
            QStringLiteral("5718e65aff5986c9640dc75ca99995482bf0065b1191a5154c4d934de785aa2c")) {
        return fail(QStringLiteral("realism evidence 4/44/54/7/2/32 envelope mismatch"));
    }
    const QSet<QString> expected_uncertainty_ids{
        QStringLiteral("ca4m4.arm.uncertainty.qualified-review-pending"),
        QStringLiteral("ca4m4.arm.uncertainty.automated-legal-realism-limit"),
        QStringLiteral("ca4m4.arm.uncertainty.exact-document-classification-scope"),
        QStringLiteral("ca4m4.arm.uncertainty.authored-final-order-deadline-base"),
        QStringLiteral("ca4m4.arm.uncertainty.counterfactual-never-filed-isolation"),
        QStringLiteral("ca4m4.arm.uncertainty.synthetic-bench-oral-limit"),
        QStringLiteral("ca4m4.arm.uncertainty.generated-pdf-provenance-limit"),
    };
    QSet<QString> uncertainty_ids;
    for (const auto& uncertainty_value :
         review.value(QStringLiteral("known_uncertainty")).toArray()) {
        const auto uncertainty = uncertainty_value.toObject();
        const auto id = uncertainty.value(QStringLiteral("uncertainty_id")).toString();
        if (id.isEmpty() || uncertainty_ids.contains(id) ||
            uncertainty.value(QStringLiteral("blocking")).toBool()) {
            return fail(QStringLiteral("pending realism uncertainty contract mismatch"));
        }
        uncertainty_ids.insert(id);
    }
    if (uncertainty_ids != expected_uncertainty_ids) {
        return fail(QStringLiteral("pending realism uncertainty IDs drifted"));
    }
    QSet<QString> evidence_ids;
    const std::array evidence_groups{evidence_resources, evidence_blobs, evidence_traces,
                                     evidence_record_checks, evidence_authorities};
    for (const auto& group : evidence_groups) {
        for (const auto& value : group) {
            const auto id = value.toObject().value(QStringLiteral("evidence_id")).toString();
            if (id.isEmpty() || evidence_ids.contains(id)) {
                return fail(QStringLiteral("realism evidence IDs are empty or duplicated"));
            }
            evidence_ids.insert(id);
        }
    }
    if (evidence_ids.size() != 139 ||
        std::ranges::any_of(evidence_resources, [](const auto& value) {
            return value.toObject().value(QStringLiteral("resource_kind")).toString() ==
                   QStringLiteral("realism_review");
        })) {
        return fail(QStringLiteral("realism review exclusion or 139-ID closure mismatch"));
    }
    const QHash<QString, int> expected_dimension_evidence_counts{
        {QStringLiteral("bench_differentiation"), 4}, {QStringLiteral("consequences"), 32},
        {QStringLiteral("deadlines_authority"), 26},  {QStringLiteral("oral_argument"), 18},
        {QStringLiteral("procedural_law"), 46},       {QStringLiteral("provenance"), 92},
        {QStringLiteral("record_consistency"), 57},
    };
    for (const auto& dimension : expected_dimensions) {
        const auto references = dimension_evidence.value(dimension).toArray();
        const auto unique_references = strings(references);
        if (references.size() != expected_dimension_evidence_counts.value(dimension) ||
            unique_references.size() != references.size()) {
            return fail(QStringLiteral("realism dimension evidence cardinality drifted: %1")
                            .arg(dimension));
        }
        for (const auto& reference : unique_references) {
            if (!evidence_ids.contains(reference)) {
                return fail(QStringLiteral("realism dimension has an unresolved evidence ID"));
            }
        }
    }

    struct TraceSpec final {
        QString file_name;
        QString sha256;
        QString trace_id;
        QString evidence_id;
        QString terminal_stage_id;
        int command_count{};
        int event_count{};
        QSet<QString> branch_entries;
    };
    const std::array trace_specs{
        TraceSpec{
            QStringLiteral("actual-through-mandate.json"),
            QStringLiteral("4d27adefa6f47a17da749d7bd2071367ae96498599495f3dedc8e9daa9d27283"),
            QStringLiteral("ca4m4.arm.trace.actual-through-mandate"),
            QStringLiteral("ca4m4.arm.evidence.trace.actual-through-mandate"),
            QStringLiteral("ca4m4.arm.stage.mandate-issued"),
            39,
            42,
            {}},
        TraceSpec{
            QStringLiteral("day31-government-forfeiture.json"),
            QStringLiteral("9928e6d50b06b3d69e6c7d181491c2335bc7e9aeb78d0dafb9e1def637e26aae"),
            QStringLiteral("ca4m4.arm.trace.day31-government-forfeiture"),
            QStringLiteral("ca4m4.arm.evidence.trace.day31-government-forfeiture"),
            QStringLiteral("ca4m4.arm.stage.record"),
            8,
            8,
            {QStringLiteral("ca4m4.arm.record.b01"), QStringLiteral("ca4m4.arm.record.b02"),
             QStringLiteral("ca4m4.arm.record.b06")}},
        TraceSpec{
            QStringLiteral("day31-invoked-dismissal.json"),
            QStringLiteral("c7e19c9f432e99667e4687f1a2186a3ae63f6becb26d671d8338e24e8c6d41e8"),
            QStringLiteral("ca4m4.arm.trace.day31-invoked-dismissal"),
            QStringLiteral("ca4m4.arm.evidence.trace.day31-invoked-dismissal"),
            QStringLiteral("ca4m4.arm.stage.terminated"),
            9,
            9,
            {QStringLiteral("ca4m4.arm.record.b01"), QStringLiteral("ca4m4.arm.record.b02"),
             QStringLiteral("ca4m4.arm.record.b03"), QStringLiteral("ca4m4.arm.record.b04")}},
        TraceSpec{
            QStringLiteral("deficient-petition-uncured.json"),
            QStringLiteral("e951c729f3ab8257a3d5fe1ddb47bf551eae699d79a210ed9130a33c5a03af11"),
            QStringLiteral("ca4m4.arm.trace.deficient-petition-uncured"),
            QStringLiteral("ca4m4.arm.evidence.trace.deficient-petition-uncured"),
            QStringLiteral("ca4m4.arm.stage.terminated"),
            6,
            7,
            {QStringLiteral("ca4m4.arm.record.b05")}},
        TraceSpec{
            QStringLiteral("stay-denied-later-of-mandate.json"),
            QStringLiteral("a1314702f842ec5ed654592000bbf095e5475a60cd0082a138926a70260c07c3"),
            QStringLiteral("ca4m4.arm.trace.stay-denied-later-of-mandate"),
            QStringLiteral("ca4m4.arm.evidence.trace.stay-denied-later-of-mandate"),
            QStringLiteral("ca4m4.arm.stage.mandate-issued"),
            42,
            45,
            {QStringLiteral("ca4m4.arm.record.b09"), QStringLiteral("ca4m4.arm.record.b10"),
             QStringLiteral("ca4m4.arm.record.b13")}},
        TraceSpec{
            QStringLiteral("stay-granted-blocks-mandate.json"),
            QStringLiteral("06f196cf5d15ee499fce930cd1553b0e5b4f84bbc9c5bfc2e6f528fc32276edd"),
            QStringLiteral("ca4m4.arm.trace.stay-granted-blocks-mandate"),
            QStringLiteral("ca4m4.arm.evidence.trace.stay-granted-blocks-mandate"),
            QStringLiteral("ca4m4.arm.stage.mandate-stayed"),
            41,
            44,
            {QStringLiteral("ca4m4.arm.record.b09"), QStringLiteral("ca4m4.arm.record.b11")}},
        TraceSpec{
            QStringLiteral("timely-rehearing-denied-mandate.json"),
            QStringLiteral("0ff21c0f47cb92fb3cdcf4eaa7d6f38bcba0e33746673787a7b64760606653e7"),
            QStringLiteral("ca4m4.arm.trace.timely-rehearing-denied-mandate"),
            QStringLiteral("ca4m4.arm.evidence.trace.timely-rehearing-denied-mandate"),
            QStringLiteral("ca4m4.arm.stage.mandate-issued"),
            42,
            45,
            {QStringLiteral("ca4m4.arm.record.b07"), QStringLiteral("ca4m4.arm.record.b08"),
             QStringLiteral("ca4m4.arm.record.b12")}},
    };
    QHash<QString, QJsonObject> embedded_trace_by_id;
    for (const auto& trace_value : evidence_traces) {
        const auto trace = trace_value.toObject();
        embedded_trace_by_id.insert(trace.value(QStringLiteral("trace_id")).toString(), trace);
    }
    const auto traces_root = QDir(authoring_root.filePath(QStringLiteral("traces")));
    if (traces_root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name).size() != 7) {
        return fail(QStringLiteral("canonical trace source count mismatch"));
    }
    QSet<QString> seen_trace_ids;
    QSet<QString> seen_trace_evidence_ids;
    QSet<QString> executed_operation_ids;
    for (const auto& specification : trace_specs) {
        const auto trace_bytes = readAll(traces_root.filePath(specification.file_name));
        const auto trace = QJsonDocument::fromJson(trace_bytes).object();
        const auto trace_id = trace.value(QStringLiteral("trace_id")).toString();
        const auto trace_evidence_id = trace.value(QStringLiteral("evidence_id")).toString();
        const auto journal = trace.value(QStringLiteral("journal")).toArray();
        const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
        if (QCryptographicHash::hash(trace_bytes, QCryptographicHash::Sha256).toHex() !=
                specification.sha256.toLatin1() ||
            embedded_trace_by_id.value(trace_id) != trace || seen_trace_ids.contains(trace_id) ||
            seen_trace_evidence_ids.contains(trace_evidence_id) ||
            trace_id != specification.trace_id || trace_evidence_id != specification.evidence_id ||
            trace.value(QStringLiteral("workflow_id")).toString() !=
                QStringLiteral("ca4m4.arm.workflow.agency-review") ||
            trace.value(QStringLiteral("engine_revision")).toString() !=
                QString::fromLatin1(realism_engine_revision) ||
            trace.value(QStringLiteral("terminal_stage_id")).toString() !=
                specification.terminal_stage_id ||
            trace.value(QStringLiteral("command_count")).toInt() != specification.command_count ||
            journal.size() != specification.command_count ||
            trace.value(QStringLiteral("event_count")).toInt() != specification.event_count ||
            operation_ids.size() != specification.event_count ||
            realismJournalDigest(journal) !=
                std::optional{trace.value(QStringLiteral("journal_sha256")).toString()} ||
            realismTraceDigest(QStringLiteral("ca4m4.case.arm-agency"), trace) !=
                trace.value(QStringLiteral("digest")).toString()) {
            return fail(QStringLiteral("canonical trace envelope/digest mismatch: %1")
                            .arg(specification.file_name));
        }
        seen_trace_ids.insert(trace_id);
        seen_trace_evidence_ids.insert(trace_evidence_id);
        executed_operation_ids.unite(strings(operation_ids));
        QSet<QString> used_branch_entries;
        int event_count = 0;
        QJsonArray decoded_operation_ids;
        for (const auto& journal_entry_value : journal) {
            const auto journal_entry = journal_entry_value.toObject();
            const auto command_encoded =
                journal_entry.value(QStringLiteral("command_base64")).toString().toLatin1();
            const auto command_bytes = QByteArray::fromBase64(command_encoded);
            const auto command_document = QJsonDocument::fromJson(command_bytes);
            if (command_bytes.isEmpty() || command_bytes.toBase64() != command_encoded ||
                !command_document.isObject() ||
                command_document.toJson(QJsonDocument::Compact) != command_bytes) {
                return fail(QStringLiteral("trace command is not canonical base64 JSON"));
            }
            const auto document_sha = command_document.object()
                                          .value(QStringLiteral("payload"))
                                          .toObject()
                                          .value(QStringLiteral("document_sha256"))
                                          .toString();
            if (!document_sha.isEmpty()) {
                const auto record_entry_id = record_entry_id_by_sha.value(document_sha);
                if (record_entry_id.isEmpty()) {
                    return fail(QStringLiteral("trace command document SHA does not resolve"));
                }
                const auto tags = strings(record_entry_by_id.value(record_entry_id)
                                              .value(QStringLiteral("tags"))
                                              .toArray());
                if (tags.contains(QStringLiteral("never_filed"))) {
                    used_branch_entries.insert(record_entry_id);
                }
            }
            for (const auto& event_value :
                 journal_entry.value(QStringLiteral("events_base64")).toArray()) {
                ++event_count;
                const auto event_encoded = event_value.toString().toLatin1();
                const auto event_bytes = QByteArray::fromBase64(event_encoded);
                const auto event_document = QJsonDocument::fromJson(event_bytes);
                if (event_bytes.isEmpty() || event_bytes.toBase64() != event_encoded ||
                    !event_document.isObject() ||
                    event_document.toJson(QJsonDocument::Compact) != event_bytes) {
                    return fail(QStringLiteral("trace event is not canonical base64 JSON"));
                }
                decoded_operation_ids.push_back(event_document.object()
                                                    .value(QStringLiteral("payload"))
                                                    .toObject()
                                                    .value(QStringLiteral("operation_id"))
                                                    .toString());
            }
        }
        if (event_count != trace.value(QStringLiteral("event_count")).toInt() ||
            decoded_operation_ids != operation_ids ||
            used_branch_entries != specification.branch_entries ||
            (specification.file_name == QStringLiteral("actual-through-mandate.json") &&
             !used_branch_entries.isEmpty())) {
            return fail(QStringLiteral("trace replay operation or branch-document mismatch: %1")
                            .arg(specification.file_name));
        }
    }
    const QSet<QString> intentionally_unexecuted_reject_operations{
        QStringLiteral("ca4m4.arm.operation.reject-opened"),
        QStringLiteral("ca4m4.arm.operation.reject-timeliness"),
        QStringLiteral("ca4m4.arm.operation.reject-record"),
        QStringLiteral("ca4m4.arm.operation.reject-opening-brief"),
        QStringLiteral("ca4m4.arm.operation.reject-response-brief"),
        QStringLiteral("ca4m4.arm.operation.reject-reply-brief"),
        QStringLiteral("ca4m4.arm.operation.reject-rehearing"),
    };
    if (executed_operation_ids.size() != 60 ||
        !(executed_operation_ids - workflow_operation_ids).isEmpty() ||
        workflow_operation_ids - executed_operation_ids !=
            intentionally_unexecuted_reject_operations) {
        return fail(QStringLiteral("seven-trace workflow operation coverage drifted"));
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

    std::vector<const appellate::packs::LoadedPack*> resolved_dependencies;
    resolved_dependencies.reserve(resolved->dependenciesDependencyFirst().size());
    for (const auto& dependency : resolved->dependenciesDependencyFirst()) {
        resolved_dependencies.push_back(&dependency);
    }

    const auto expect_realism_mutation_rejected =
        [&](appellate::packs::LoadedPack candidate, const auto& mutate) -> std::optional<QString> {
        const auto candidate_review = std::ranges::find(
            candidate.resources, std::string_view("ca4m4.arm.review.authoring-2026-08-12"),
            [](const auto& resource) { return std::string_view(resource.descriptor.id); });
        if (candidate_review == candidate.resources.end()) {
            return QStringLiteral("resolved root lost its realism review");
        }
        mutate(candidate_review->document);
        const auto validation = PackReader::validateResolvedGraph(candidate, resolved_dependencies);
        if (validation ||
            validation.error().code != appellate::packs::ErrorCode::CrossReferenceFailure) {
            return QStringLiteral("mutated realism evidence did not fail closed");
        }
        return std::nullopt;
    };
    if (const auto error = expect_realism_mutation_rejected(
            resolved->root(),
            [](QJsonObject& document) {
                auto mutated_dimensions = document.value(QStringLiteral("dimensions")).toObject();
                mutated_dimensions.insert(QStringLiteral("procedural_law"), 3);
                document.insert(QStringLiteral("dimensions"), mutated_dimensions);
            });
        error.has_value()) {
        return fail(*error + QStringLiteral(": score-3 pending-review mutation"));
    }
    if (const auto error = expect_realism_mutation_rejected(
            resolved->root(),
            [](QJsonObject& document) {
                auto mutated_evidence = document.value(QStringLiteral("evidence")).toObject();
                auto traces = mutated_evidence.value(QStringLiteral("traces")).toArray();
                auto trace = traces.at(1).toObject();
                auto journal = trace.value(QStringLiteral("journal")).toArray();
                auto entry = journal.at(0).toObject();
                auto events = entry.value(QStringLiteral("events_base64")).toArray();
                auto bytes = QByteArray::fromBase64(events.at(0).toString().toLatin1());
                bytes[0] = bytes.at(0) == '{' ? '[' : '{';
                events.replace(0, QString::fromLatin1(bytes.toBase64()));
                entry.insert(QStringLiteral("events_base64"), events);
                journal.replace(0, entry);
                trace.insert(QStringLiteral("journal"), journal);
                traces.replace(1, trace);
                mutated_evidence.insert(QStringLiteral("traces"), traces);
                document.insert(QStringLiteral("evidence"), mutated_evidence);
            });
        error.has_value()) {
        return fail(*error + QStringLiteral(": second-trace event mutation"));
    }

    const auto runtime = appellate::packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != expected_root || runtime->cases.size() != std::size_t{1} ||
        runtime->cases.front().argument_configurations.size() != std::size_t{2}) {
        return fail(QStringLiteral("catalog-valid ARM closure is not runtime-loadable"));
    }

    QHash<QString, int> runtime_question_counts;
    for (const auto& configuration : runtime->cases.front().argument_configurations) {
        if (!configuration.grounded_question_bank.has_value() ||
            configuration.permitted_issue_ids.size() != std::size_t{5}) {
            return fail(QStringLiteral("runtime argument bank lost grounded five-issue coverage"));
        }
        runtime_question_counts.insert(
            QString::fromStdString(configuration.id.value),
            static_cast<int>(configuration.grounded_question_bank->questions.size()));
    }
    const QHash<QString, int> expected_runtime_question_counts{
        {QStringLiteral("ca4m4.arm.argument.actual-record"), 15},
        {QStringLiteral("ca4m4.arm.argument.counterfactual"), 10},
    };
    if (runtime_question_counts != expected_runtime_question_counts) {
        return fail(QStringLiteral("runtime argument bank 15/10 split drifted"));
    }

    std::cout << "ARM 1.2 integration contract passed: 54 PDFs / 415 pages (AR1-238 and "
                 "PA1-177), 15/10 grounded banks, 13-stage/67-operation workflow, seven "
                 "replayed traces, deterministic archive, and four exact revisions.\n";
    return 0;
}
