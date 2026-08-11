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

constexpr auto root_digest = "bd1bd37e1e99ecb8239fa41b040aa72a0a856dd012442bcc1061b7d137e6651d";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto archive_digest = "57f84192541e5a273ff4f69b27902a06b1fdea0a58acc7752b4c22d9c69f338e";

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

    const PackRevision expected_root{PackId{"us.ca4.m4.arm-agency"}, "1.1.0", root_digest};
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
        source->resources.size() != std::size_t{8} || source->blobs.size() != std::size_t{19}) {
        return fail(QStringLiteral("source pack revision/count contract mismatch"));
    }

    const auto readme =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral("README.md"))))
            .simplified();
    if (!readme.contains(QStringLiteral("incomplete, pre-release")) ||
        !readme.contains(QStringLiteral("not a releasable pack")) ||
        !readme.contains(QStringLiteral("eighteen AR-labeled agency PDFs")) ||
        !readme.contains(QStringLiteral("AR1–AR238")) ||
        !readme.contains(QStringLiteral("PA1–PA8")) ||
        !readme.contains(QStringLiteral("not counted toward the 18-PDF/238-page")) ||
        !readme.contains(QStringLiteral("Two minimal argument configurations")) ||
        !readme.contains(QStringLiteral("No appellate result or realism level")) ||
        !readme.contains(QStringLiteral("default no-rehearing-petition/no-stay branch"))) {
        return fail(QStringLiteral("README does not preserve the incomplete boundary"));
    }

    QStringList markdown_paths;
    const std::array source_directories{
        std::pair{QStringLiteral("documents/batch-1"), 7},
        std::pair{QStringLiteral("documents/batch-2"), 12},
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
    if (markdown_paths.size() != 19) {
        return fail(QStringLiteral("ARM must have exactly nineteen rendered sources"));
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
    for (const auto& markdown_path : markdown_paths) {
        const auto markdown_name = QFileInfo(markdown_path).fileName();
        const auto raw = QString::fromUtf8(readAll(markdown_path));
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
        const auto searchable_body = lower_body.simplified();
        if (lower_body.contains(QStringLiteral("placeholder"))) {
            return fail(QStringLiteral("placeholder token escaped into %1").arg(markdown_name));
        }
        const bool may_identify_post_order_material =
            markdown_name.startsWith(QStringLiteral("pa")) ||
            markdown_name.startsWith(QStringLiteral("18-"));
        if (!may_identify_post_order_material &&
            (lower_body.contains(QStringLiteral("cousin's")) ||
             lower_body.contains(QStringLiteral("cousin declaration")) ||
             lower_body.contains(QStringLiteral("appellate proffer")) ||
             lower_body.contains(QStringLiteral("post-order account")) ||
             lower_body.contains(QStringLiteral("later declaration")))) {
            return fail(
                QStringLiteral("future-record knowledge leaked into %1").arg(markdown_name));
        }
        for (const auto& phrase : forbidden_authoring_voice) {
            if (lower_body.contains(phrase)) {
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
    if (authored_page_count != 246 || distinct_markdown_pages.size() != 246 ||
        normalized_markdown_pages_by_asset.size() != 19) {
        return fail(QStringLiteral("normalized Markdown page closure mismatch"));
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
        return fail(QStringLiteral("ARM content lane must not contain a structured disposition"));
    }
    for (const auto& issue_value :
         case_resource->document.value(QStringLiteral("issues")).toArray()) {
        if (issue_value.toObject().contains(QStringLiteral("target_ids"))) {
            return fail(QStringLiteral("deferred disposition target leaked into ARM content"));
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

    const QHash<QString, QSet<QString>> actual_question_groundings{
        {QStringLiteral("ca4m4.arm.question.actual-record-composition"),
         {QStringLiteral("ca4m4.arm.grounding.actual-record-rule|authority|ca4m4.arm.authority."
                         "frap-16-record"),
          QStringLiteral("ca4m4.arm.grounding.actual-record-statute|authority|ca4m4.arm.authority."
                         "usc-1252-record-limit"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-index|record_page|ca4m4.arm.anchor.ar30"),
          QStringLiteral("ca4m4.arm.grounding.actual-record-p7|record_page|ca4m4.arm.anchor.ar33"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-admission|record_page|ca4m4.arm.anchor.ar117"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-initial-gap|record_page|ca4m4.arm.anchor.ar219"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-stipulation|record_page|ca4m4.arm.anchor.ar227"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-receipt|record_page|ca4m4.arm.anchor.ar229"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-cause|record_page|ca4m4.arm.anchor.ar232"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-identity|record_page|ca4m4.arm.anchor.ar233"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-exclusion|record_page|ca4m4.arm.anchor.ar235"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-record-correction|record_page|ca4m4.arm.anchor.ar237"),
          QStringLiteral("ca4m4.arm.grounding.actual-record-pa|record_page|ca4m4.arm.anchor.pa1")}},
        {QStringLiteral("ca4m4.arm.question.actual-aggregate-risk"),
         {QStringLiteral("ca4m4.arm.grounding.actual-aggregate-rule|authority|ca4m4.arm.authority."
                         "rodriguez-arias-aggregation"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-aggregate-page|record_page|ca4m4.arm.anchor.ar29"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-aggregate-report|record_page|ca4m4.arm.anchor.ar86"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-aggregate-testimony|record_page|ca4m4.arm.anchor.ar137"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-aggregate-ij|record_page|ca4m4.arm.anchor.ar171"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-aggregate-board|record_page|ca4m4.arm.anchor.ar212")}},
        {QStringLiteral("ca4m4.arm.question.actual-acquiescence"),
         {QStringLiteral("ca4m4.arm.grounding.actual-acquiescence-rule|authority|ca4m4.arm."
                         "authority.cfr-1208-torture-acquiescence"),
          QStringLiteral("ca4m4.arm.grounding.actual-acquiescence-declaration|record_page|ca4m4."
                         "arm.anchor.ar45"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-acquiescence-routing|record_page|ca4m4.arm.anchor.ar66"),
          QStringLiteral("ca4m4.arm.grounding.actual-acquiescence-testimony|record_page|ca4m4.arm."
                         "anchor.ar148"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-acquiescence-ij|record_page|ca4m4.arm.anchor.ar172"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-acquiescence-board|record_page|ca4m4.arm.anchor.ar215")}},
        {QStringLiteral("ca4m4.arm.question.actual-review-standard"),
         {QStringLiteral("ca4m4.arm.grounding.actual-review-rule|authority|ca4m4.arm.authority.usc-"
                         "1252-review-standard"),
          QStringLiteral("ca4m4.arm.grounding.actual-review-consideration|authority|ca4m4.arm."
                         "authority.rodriguez-arias-consideration"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-review-application|record_page|ca4m4.arm.anchor.ar31"),
          QStringLiteral("ca4m4.arm.grounding.actual-review-ij|record_page|ca4m4.arm.anchor.ar171"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-review-brief|record_page|ca4m4.arm.anchor.ar197"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-review-board|record_page|ca4m4.arm.anchor.ar216")}},
        {QStringLiteral("ca4m4.arm.question.actual-timeliness"),
         {QStringLiteral("ca4m4.arm.grounding.actual-timeliness-statute|authority|ca4m4.arm."
                         "authority.usc-1252-deadline"),
          QStringLiteral("ca4m4.arm.grounding.actual-timeliness-riley|authority|ca4m4.arm."
                         "authority.riley-claims-processing"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-timeliness-order|record_page|ca4m4.arm.anchor.ar211"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-timeliness-notice|record_page|ca4m4.arm.anchor.ar218"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-timeliness-index|record_page|ca4m4.arm.anchor.ar219"),
          QStringLiteral(
              "ca4m4.arm.grounding.actual-timeliness-page|record_page|ca4m4.arm.anchor.pa1")}},
    };
    const QHash<QString, QSet<QString>> counterfactual_question_groundings{
        {QStringLiteral("ca4m4.arm.question.counterfactual-record-composition"),
         {QStringLiteral("ca4m4.arm.grounding.counterfactual-record-rule|authority|ca4m4.arm."
                         "authority.frap-16-record"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-statute|authority|ca4m4.arm."
                         "authority.usc-1252-record-limit"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-record-index|record_page|ca4m4.arm.anchor.ar30"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-record-p7|record_page|ca4m4.arm.anchor.ar33"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-admission|record_page|ca4m4."
                         "arm.anchor.ar117"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-initial-gap|record_page|ca4m4."
                         "arm.anchor.ar219"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-stipulation|record_page|ca4m4."
                         "arm.anchor.ar227"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-receipt|record_page|ca4m4.arm."
                         "anchor.ar229"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-exclusion|record_page|ca4m4."
                         "arm.anchor.ar235"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-record-correction|record_page|ca4m4."
                         "arm.anchor.ar237"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-record-pa|record_page|ca4m4.arm.anchor.pa1")}},
        {QStringLiteral("ca4m4.arm.question.counterfactual-aggregate-risk"),
         {QStringLiteral("ca4m4.arm.grounding.counterfactual-aggregate-rule|authority|ca4m4.arm."
                         "authority.rodriguez-arias-aggregation"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-aggregate-page|record_page|ca4m4.arm."
                         "anchor.ar29"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-aggregate-report|record_page|ca4m4."
                         "arm.anchor.ar86"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-aggregate-ij|record_page|ca4m4.arm.anchor.ar171"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-aggregate-board|record_page|ca4m4.arm."
                         "anchor.ar212")}},
        {QStringLiteral("ca4m4.arm.question.counterfactual-acquiescence"),
         {QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-rule|authority|ca4m4.arm."
                         "authority.cfr-1208-torture-acquiescence"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-routing|record_page|"
                         "ca4m4.arm.anchor.ar66"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-authentication|record_"
                         "page|ca4m4.arm.anchor.ar71"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-testimony|record_page|"
                         "ca4m4.arm.anchor.ar148"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-ij|record_page|ca4m4.arm."
                         "anchor.ar172"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-acquiescence-board|record_page|ca4m4."
                         "arm.anchor.ar215")}},
        {QStringLiteral("ca4m4.arm.question.counterfactual-review-standard"),
         {QStringLiteral("ca4m4.arm.grounding.counterfactual-review-rule|authority|ca4m4.arm."
                         "authority.rodriguez-arias-consideration"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-review-standard|authority|ca4m4.arm."
                         "authority.usc-1252-review-standard"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-review-application|record_page|ca4m4."
                         "arm.anchor.ar31"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-review-ij|record_page|ca4m4.arm.anchor.ar171"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-review-board|record_page|ca4m4.arm."
                         "anchor.ar216")}},
        {QStringLiteral("ca4m4.arm.question.counterfactual-day-31"),
         {QStringLiteral("ca4m4.arm.grounding.counterfactual-day-31-statute|authority|ca4m4.arm."
                         "authority.usc-1252-deadline"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-day-31-riley|authority|ca4m4.arm."
                         "authority.riley-claims-processing"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-day-31-order|record_page|ca4m4.arm.anchor.ar211"),
          QStringLiteral("ca4m4.arm.grounding.counterfactual-day-31-notice|record_page|ca4m4.arm."
                         "anchor.ar218"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-day-31-index|record_page|ca4m4.arm.anchor.ar219"),
          QStringLiteral(
              "ca4m4.arm.grounding.counterfactual-day-31-page|record_page|ca4m4.arm.anchor.pa1")}},
    };

    const auto check_argument_bank =
        [&](const ValidatedResource& resource, const QString& expected_mode,
            const QString& expected_digest,
            const QHash<QString, QSet<QString>>& expected_question_groundings)
        -> std::optional<QString> {
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
        QSet<QString> question_ids;
        QSet<QString> grounding_ids;
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
            const auto question_id = question.value(QStringLiteral("question_id")).toString();
            if (!permitted.contains(issue) || question_ids.contains(question_id) ||
                !expected_question_groundings.contains(question_id) ||
                question.value(QStringLiteral("prompt")).toString().isEmpty()) {
                return QStringLiteral("argument-bank question coverage mismatch");
            }
            question_ids.insert(question_id);
            question_issues.insert(issue);
            QSet<QString> actual_groundings;
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
                    saw_pa = saw_pa || target.startsWith(QStringLiteral("ca4m4.arm.anchor.pa"));
                    saw_ar = saw_ar || target.startsWith(QStringLiteral("ca4m4.arm.anchor.ar"));
                } else if (kind == QStringLiteral("authority")) {
                    target = grounding.value(QStringLiteral("authority_id")).toString();
                } else {
                    return QStringLiteral("argument bank uses noncanonical grounding kind");
                }
                actual_groundings.insert(
                    QStringLiteral("%1|%2|%3").arg(grounding_id, kind, target));
            }
            if (actual_groundings != expected_question_groundings.value(question_id)) {
                return QStringLiteral("per-question grounding set mismatch: %1").arg(question_id);
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
            question_ids != QSet<QString>(expected_question_groundings.keyBegin(),
                                          expected_question_groundings.keyEnd()) ||
            topics != expected_topics || !saw_ar || !saw_pa) {
            return QStringLiteral("argument bank is not grounded across the five-issue matrix");
        }
        return std::nullopt;
    };
    if (const auto error = check_argument_bank(
            *actual_argument, QStringLiteral("actual_record"),
            QStringLiteral("9da889348681230a0f65b8cec31713970001b567597a8b0066baa21f66421b8c"),
            actual_question_groundings);
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = check_argument_bank(
            *counterfactual_argument, QStringLiteral("counterfactual_training"),
            QStringLiteral("a2eceeedf028ed50080e511f7a0cdbc81cc542c3a76a14c47ec77363da16d582"),
            counterfactual_question_groundings);
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
    if (dockets.size() != 2 || entries.size() != 19 || anchors.size() != 246) {
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
    int expected_ar = 1;
    int expected_pa = 1;
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
            !generated && tags.contains(QStringLiteral("certified_administrative_record"));
        if (administrative == generated || tags.contains(QStringLiteral("batch_1"))) {
            return fail(QStringLiteral("entry is ambiguously classified as AR/generated"));
        }
        const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
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
        record_label_starts.insert(relative_asset, administrative ? expected_ar : expected_pa);
        record_label_prefixes.insert(relative_asset,
                                     administrative ? QStringLiteral("AR") : QStringLiteral("PA"));
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
        if (relative_asset == QStringLiteral("assets/09-certified-translation-packet.pdf")) {
            const auto combined_pdf_bytes = readAll(QDir(pack_root).filePath(relative_asset));
            if (combined_pdf_bytes.contains("/Subtype /Image") ||
                combined_pdf_bytes.contains("/Subtype/Image")) {
                return fail(QStringLiteral("combined searchable-text packet contains raster image "
                                           "objects contrary to its certification"));
            }
        }
        int extracted_banner_occurrences = 0;

        for (int page_index = 0; page_index < pdf.pageCount(); ++page_index) {
            const auto expected_label = administrative ? QStringLiteral("AR%1").arg(expected_ar++)
                                                       : QStringLiteral("PA%1").arg(expected_pa++);
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
            extracted_banner_occurrences += static_cast<int>(page_text.count(record_banner));
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
            if (lower_page_text.contains(QStringLiteral("placeholder"))) {
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
                if (lower_page_text.contains(phrase)) {
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
            if (anchor.isEmpty() ||
                anchor.value(QStringLiteral("entry_id")).toString() !=
                    entry.value(QStringLiteral("entry_id")).toString() ||
                anchor.value(QStringLiteral("page_number")).toInt() != page_index + 1 ||
                anchor.value(QStringLiteral("anchor_id")).toString() !=
                    QStringLiteral("ca4m4.arm.anchor.%1").arg(expected_label.toLower())) {
                return fail(QStringLiteral("page-anchor mismatch at %1").arg(expected_label));
            }
        }
        if (administrative && entry_number >= 7 && entry_number <= 18 &&
            extracted_banner_occurrences != 1) {
            return fail(QStringLiteral("batch-2 PDF must contain exactly one safety banner: %1")
                            .arg(relative_asset));
        }
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
    if (administrative_documents != 18 || administrative_pages != 238 || expected_ar != 239 ||
        generated_documents != 1 || generated_pages != 8 || expected_pa != 9 ||
        distinct_page_bodies.size() != 246 || !saw_proven_p7 || !saw_new_proffer ||
        !saw_exact_combined_packet_entry || category_counts != expected_categories ||
        rendered_placeholder_occurrences != 0 || !saw_admission_page ||
        !saw_adjournment_admission_page || !saw_reconvening_admission_page ||
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
        const auto render_inventory =
            QJsonDocument::fromJson(readAll(authoring_root.filePath(specification.inventory_path)))
                .object();
        const auto render_entries = render_inventory.value(QStringLiteral("entries")).toArray();
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
    if (pinned_render_entries != 19 || superseded_historical_entries != 2 ||
        authored_source_paths.size() != 19 || planned_source_paths != authored_source_paths ||
        inventoried_source_paths != authored_source_paths ||
        planned_output_paths != record_asset_paths ||
        inventoried_output_paths != record_asset_paths ||
        manifest_blob_paths != record_asset_paths || source_hashes.size() != 19 ||
        assembly_hashes.size() != 19 || semantic_plan_hashes.size() != 19 ||
        semantic_render_hashes.size() != 19) {
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

    std::cout << "ARM batch-2 integration contract passed: 18 AR PDFs / 238 AR pages, "
                 "1 PA proffer / 8 PA pages, 246 unique searchable pages, two grounded banks, "
                 "runtime negative gates and D+52 positive mandate path, four exact revisions.\n";
    return 0;
}
