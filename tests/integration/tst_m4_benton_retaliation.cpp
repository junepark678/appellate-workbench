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
#include <expected>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_BENTON_ROOT
#error "APPELLATE_M4_BENTON_ROOT must name content/m4/benton-retaliation"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

namespace engine = appellate::engine;
namespace model = appellate::model;
namespace packs = appellate::packs;

using model::PackId;
using model::PackRevision;
using model::ResourceKind;
using packs::PackArchive;
using packs::PackCatalog;
using packs::PackGraphState;
using packs::PackReader;
using packs::PackValidationScope;
using packs::ValidatedResource;

constexpr auto root_digest = "eaf5f52940d968f33a3b3501e20414081f7f3573d90ba1abb7c3b2f33636ad4e";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto archive_digest = "867b45b117b51f08419d1ee2993dd5cd3af27b94367124a9c3ba531d9fb27bda";
constexpr auto actual_bank_digest =
    "161431c279887ac0914029a8912515fa271a9c3a6d1957ab507f3b6facbf6ff6";
constexpr auto counterfactual_bank_digest =
    "ab366be43b263bff2f3951b6c793cbe10543358779820bda83e855cbf2765758";

constexpr auto retaliation_issue = "ca4m4.benton.issue.retaliation-summary-judgment";
constexpr auto exclusion_issue = "ca4m4.benton.issue.late-comparator-declaration-exclusion";
constexpr auto appellant_actor = "ca4m4.benton.actor.leora-benton";
constexpr auto appellee_actor = "ca4m4.benton.actor.blue-cedar";
constexpr auto clerk_actor = "ca4m4.benton.actor.ca4-clerk";
constexpr auto panel_actor = "ca4m4.benton.actor.composite-panel";

struct RenderExpected final {
    std::string_view source_path;
    std::string_view output_path;
    std::string_view title;
    std::uint32_t first_page{};
    std::uint32_t page_count{};
    std::uint64_t byte_size{};
    std::string_view source_sha256;
    std::string_view pdf_sha256;
};

constexpr std::array expected_render{
    RenderExpected{"documents/batch-1/01-complaint.md", "objects/01-complaint.pdf",
                   "Synthetic Complaint and Jury Demand", 1, 11, 44357,
                   "b84ff95ba2e1a28c7caf927560b9ac57324cbc864d4e85d9b5a5b071f42d9e38",
                   "b3cffb233c293c107f33107f49fbf41db8e544aee4b146caab01b9f0a6802242"},
    RenderExpected{"documents/batch-1/02-answer.md", "objects/02-answer.pdf",
                   "Synthetic Answer and Affirmative Defenses", 12, 7, 33118,
                   "887341de8661f091a2ea2bde3cbf207753b82c7dad27fb1b99a46f331d4559db",
                   "485edb096f79451809f23672f4d7d819cd2e4c41a04d397819929074b73f6698"},
    RenderExpected{"documents/batch-1/03-rule-26-report.md", "objects/03-rule-26-report.pdf",
                   "Synthetic Joint Rule 26 Report", 19, 5, 27911,
                   "0afa75e5779c7d339e83d604a12293a6ce27fcf0dbd5b7045790854bca2dadcf",
                   "37b127979c200b9e15c3fc487835bbb8ede8382897f9c75a0dca38926a0e4fdf"},
    RenderExpected{"documents/batch-1/04-benton-disclosures.md",
                   "objects/04-benton-disclosures.pdf", "Synthetic Benton Initial Disclosures", 24,
                   6, 29625, "0540d88f267dc9a8cd6f3cab726ef22c0ef490f200cc1df9f837c0dd521b30ec",
                   "14a85104c5b8b8219957b64fa4e2abccb9d975e6701571db979ef3fe9bb6d714"},
    RenderExpected{"documents/batch-1/05-blue-cedar-disclosures.md",
                   "objects/05-blue-cedar-disclosures.pdf",
                   "Synthetic Blue Cedar Initial Disclosures", 30, 5, 27484,
                   "0303ea7f78e146892e9b1ed7650254c77f1d4d231bc5bd15c484062617560a05",
                   "4bcf56de9cc0a92ab0a8ad42bd7778fe024e75f7f0d2d1e920b8549f451be774"},
    RenderExpected{"documents/batch-1/06-eeoc-charge.md", "objects/06-eeoc-charge.pdf",
                   "Synthetic EEOC Charge and Amendment", 35, 5, 28950,
                   "00677274359a67ee51bacf3d551196821041398b830f256204b7c153ce8126a2",
                   "edb382280d303a9b78a530aaba330ecc821a3428f231481cd0d209bd0b41c941"},
    RenderExpected{"documents/batch-1/07-eeoc-position.md", "objects/07-eeoc-position.pdf",
                   "Synthetic Employer EEOC Position Statement", 40, 6, 30883,
                   "039caf3c8c3cf7574251814da3bf32a936f26079a2d2ad490559a18a76ffee43",
                   "6d9d2e6044550596964183b71b120570f1b3fb8ad0683864be60e4c9bb7ef523"},
    RenderExpected{"documents/batch-1/08-eeoc-notice-right-to-sue.md",
                   "objects/08-eeoc-notice-right-to-sue.pdf",
                   "Synthetic EEOC Notice and Right-to-Sue File", 46, 3, 23901,
                   "a1e54e316c20f2fff78d604f4b60bc53c897eaed296b04b1aa14eba343cc3639",
                   "fc55134bed254a1dd96624f49363ad377ef63600ab5716a53a4321e9b2716a3b"},
    RenderExpected{"documents/batch-1/09-policies-rif-protocol.md",
                   "objects/09-policies-rif-protocol.pdf",
                   "Synthetic Anti-Retaliation Policy and RIF Protocol", 49, 9, 36859,
                   "1e823ae1c18fb9534c87caf558f747a1197ff64fd203ceab635bdc79212fec13",
                   "515b6b22f0d443c04cd35abcf863d06add6234fd62117564e08606b8d8388e0b"},
    RenderExpected{"documents/batch-1/10-opposition-emails.md", "objects/10-opposition-emails.pdf",
                   "Synthetic Protected-Opposition Email Chain", 58, 5, 28050,
                   "d0c2fc65ee4c2923f07ef60a58a8765d56ceb79a0f101b247f37a88009381ae8",
                   "6e02930185e3d70ae596844581f2efbd21ab7933953f7233509f312452b0abc1"},
    RenderExpected{"documents/batch-1/11-eeoc-acknowledgments.md",
                   "objects/11-eeoc-acknowledgments.pdf",
                   "Synthetic EEOC Notice and Decisionmaker Acknowledgments", 63, 3, 23426,
                   "1bf078d0228673d50940a85b3980adc41562b7742ac59814e1dba54a339c3db3",
                   "7cdb4376eb92dc9ff1fdbc700e5c71a4f03c8c309aed8b1927901bc417966928"},
    RenderExpected{"documents/batch-1/12-rif-workbook.md", "objects/12-rif-workbook.pdf",
                   "Synthetic RIF Planning Workbook and Change Log", 66, 8, 36468,
                   "10b141cd01d9562271605800c5ad6b57143a2315b185c3ebd7bfae3e6556eaf5",
                   "0e36eb310f80e1fbd33b0525a6176e3e09280fc05da20c9608b54141df4de6c9"},
    RenderExpected{"documents/batch-1/13-comparator-scorecards.md",
                   "objects/13-comparator-scorecards.pdf", "Synthetic Comparator Scorecards", 74, 9,
                   35939, "a8001c0b5990225a1a31c9227b725251bdb7f1a5765cc17ad2d46183c44dc7e6",
                   "917227fb1151ba0ca13c7d60def844c907a1939498732f1892c3a93cf36e2fee"},
    RenderExpected{"documents/batch-1/14-calibration-minutes.md",
                   "objects/14-calibration-minutes.pdf",
                   "Synthetic RIF Calibration Minutes and Version Log", 83, 5, 28045,
                   "6eb23252fcf099af6d61b7c72528eac09d56b14d118657e2134b0ff2bb20c0d7",
                   "0cedbe9753d5e619fa8d1cec03a544677f0a3d215c45fb16429e93525e2a829b"},
    RenderExpected{"documents/batch-1/15-explanation-communications.md",
                   "objects/15-explanation-communications.pdf",
                   "Synthetic Termination-Explanation Communications", 88, 6, 29964,
                   "275f4035b93f1c0f10c5600bc3692b174624567d7dcc8885339fda9ddd61dbe8",
                   "2bb66986ed0b3fa29cdedba2edc218144d3a06da991f7323899f6d7e2aebbd33"},
    RenderExpected{"documents/batch-1/16-performance-metrics.md",
                   "objects/16-performance-metrics.pdf",
                   "Synthetic Benton Performance Reviews and Audit Metrics", 94, 7, 31880,
                   "324b685ec02a9c289c2adeb8fba5ef81c0e2ca0e24e8afb9e1e5703a407c80d8",
                   "fc538397e57768915638c4924b55b3d18873775fa663598b7d6cca8891cff4bc"},
    RenderExpected{"documents/batch-1/17-headcount-ledger.md", "objects/17-headcount-ledger.pdf",
                   "Synthetic Organization Chart and Headcount Ledger", 101, 5, 34240,
                   "8e7aab5d92f265cc67f194889678da7eacf6635e23a44a1f10bc898bc8068d43",
                   "90e043d7ccc3bdf13f9156912d01bec8f80740d9c338ae82d2bd2238f6d7fb05"},
    RenderExpected{"documents/batch-1/18-benton-deposition.md", "objects/18-benton-deposition.pdf",
                   "Synthetic Deposition of Leora Benton", 106, 10, 38765,
                   "25f628b4846c1b1699ad2eb9d0f74d39dccfb890f38c8383945532d7461d738f",
                   "261607e0b7cef29ac12c77620a4bd3b58c1826b20af27e20b433c2e8ecc77a47"},
    RenderExpected{"documents/batch-1/19-pike-deposition.md", "objects/19-pike-deposition.pdf",
                   "Synthetic Deposition of Draven Pike", 116, 10, 39023,
                   "b4b7be0ac4d60e237f15f962eb44ce78c79146bffa87af6b24b93bfc9601ede6",
                   "26c56da8a9098722e1932f816604f690330d48d9ae1936fac75af5025d6ce4df"},
    RenderExpected{"documents/batch-2/20-solis-deposition.md", "objects/20-solis-deposition.pdf",
                   "Synthetic Deposition of Mira Solis", 126, 10, 44500,
                   "519c7b564dd2d6e044e3065b98430442fdc09782e558da023957899141f50d33",
                   "5d12e3166d1f71ed770cbf3b55f165383dabfa2ab0424f0627131f7ff073c3fa"},
    RenderExpected{"documents/batch-2/21-wynn-deposition.md", "objects/21-wynn-deposition.pdf",
                   "Synthetic Deposition of Tessa Wynn", 136, 10, 42826,
                   "7bea95bff80ddc650d9627b5df654e678030a021d3070bc259e1b6b5a56b3dd1",
                   "085b7707ace592d949fa2f978151a1283eb644b829820e944f7709a724b7dce2"},
    RenderExpected{"documents/batch-2/22-ibarra-deposition.md", "objects/22-ibarra-deposition.pdf",
                   "Synthetic Deposition of Omar Ibarra", 146, 10, 42660,
                   "8ded036f0fb1121878153ad8123ad615542963b834843598ba8ad1f945f035fb",
                   "d0dc18408639a7322902ee19abefc666d19912284de50284f46f91543695bba1"},
    RenderExpected{"documents/batch-2/23-voss-deposition.md", "objects/23-voss-deposition.pdf",
                   "Synthetic Deposition of Naomi Voss", 156, 10, 43445,
                   "a5af68d7dcc5ff20f392de74c87b30cff2e01dba0710333c0e0ddd071dba6a98",
                   "feb915d99c9a0d4261d76f34f3defeef8a331f701f4bc243f4aabcd3c735d489"},
    RenderExpected{"documents/batch-2/24-blue-cedar-30b6-deposition.md",
                   "objects/24-blue-cedar-30b6-deposition.pdf",
                   "Synthetic Rule 30(b)(6) Deposition of Blue Cedar", 166, 10, 44824,
                   "d5ea69da87817c15e858b594a1becc667cb5782809f40b0be62cf1847359e180",
                   "ca2157694d54aeca2d2501712a90dd740c32cee4fd6ac2398ef6d6962707fb57"},
    RenderExpected{"documents/batch-2/25-blue-cedar-summary-judgment.md",
                   "objects/25-blue-cedar-summary-judgment.pdf",
                   "Synthetic Blue Cedar Summary-Judgment Memorandum", 176, 10, 47728,
                   "c0caaca4052565aedb712e0538e1582737c80af4d578924ad052a3aca24ebe02",
                   "059fadabb8a7a3be2a76a4fd96bb589fd972fc3389baa68a7ac8c2039a6a59ee"},
    RenderExpected{"documents/batch-2/26-benton-summary-judgment-opposition.md",
                   "objects/26-benton-summary-judgment-opposition.pdf",
                   "Synthetic Benton Summary-Judgment Opposition", 186, 10, 48182,
                   "b68b66e8cd2ea29a3100d57c7cc0d9a15b6f360e66e0947e25fea726e692e7ad",
                   "707f8be1e84180e606a6e25b786c069b4b37097b1b0a71ade11b86cb9ced4e7e"},
    RenderExpected{"documents/batch-2/27-blue-cedar-summary-judgment-reply.md",
                   "objects/27-blue-cedar-summary-judgment-reply.pdf",
                   "Synthetic Blue Cedar Summary-Judgment Reply", 196, 6, 36135,
                   "3d40e9c5788e03eca2e0dd983119db1a9cc12c5020cbc9ba87598db8301450ec",
                   "f2185ee3ba44aafefcce5407386ae4c2b7e0e39500ac713831df4d25122e0c9b"},
    RenderExpected{"documents/batch-2/28-benton-disputed-facts.md",
                   "objects/28-benton-disputed-facts.pdf",
                   "Synthetic Benton Statement of Disputed Facts", 202, 8, 41699,
                   "fbad4c022cc4bdb7bfe000d195c24d557e2b1e56c6b3678155f819f50ce18f5e",
                   "164060451f39e010b66b2e2dd658ffc2db2150cd08a319125365a70b6a65391e"},
    RenderExpected{"documents/batch-2/29-blue-cedar-undisputed-facts.md",
                   "objects/29-blue-cedar-undisputed-facts.pdf",
                   "Synthetic Blue Cedar Statement of Undisputed Facts", 210, 7, 38450,
                   "7c4c15f276d42d1a7c0c74c4ec67e24ede5dc801abbb52f2b7e8214855b720aa",
                   "7e4f4ac7d06f48f50ac64809f263e7a51e9afc32be727d9896bfca392ce28db2"},
    RenderExpected{"documents/batch-2/30-joint-causation-chart.md",
                   "objects/30-joint-causation-chart.pdf",
                   "Synthetic Joint Causation and Record-Citation Chart", 217, 6, 41336,
                   "871109b57e81906cac229abc442d0e629ed89921abc9d5d38848ff08363a135e",
                   "224f1bafa8a7f6681169b5d25310acf2fecbcd8962a9ed32006ba1dd85f5089f"},
    RenderExpected{"documents/batch-2/31-motion-to-exclude-wynn-declaration.md",
                   "objects/31-motion-to-exclude-wynn-declaration.pdf",
                   "Synthetic Motion to Exclude Late Wynn Declaration", 223, 4, 34090,
                   "c8442a3bde5087246967251582045483b2dbccd6bb11a684bf000fcbd5f8345c",
                   "76f33baa79c46d0623708251a247ac808cf6cbe34aecd6470e48ac8391104e19"},
    RenderExpected{"documents/batch-2/32-opposition-wynn-declaration-proffer.md",
                   "objects/32-opposition-wynn-declaration-proffer.pdf",
                   "Synthetic Opposition to Exclusion and Timing Proffer", 227, 5, 31411,
                   "8806568534b3d4f72f9d72cd8bea56631a043031c1b192b5a5d8ab81fafb612c",
                   "9ae1b0df5a8f838d9978fa619f4af9157ebc4f015098e5e50fbd3dd130eaf81e"},
    RenderExpected{"documents/batch-2/33-order-excluding-wynn-declaration.md",
                   "objects/33-order-excluding-wynn-declaration.pdf",
                   "Synthetic Order Excluding Late Wynn Declaration", 232, 4, 33165,
                   "aa5d902b8a8384f1f20743c91be0b6d55392477780aae4cae42f88456f4fd435",
                   "c78e9a751044faf8dacbf677e0a5410949745f632e0a3c4b42decf65407e929d"},
    RenderExpected{"documents/batch-2/34-summary-judgment-opinion.md",
                   "objects/34-summary-judgment-opinion.pdf",
                   "Synthetic Summary-Judgment Memorandum Opinion", 236, 12, 56721,
                   "8010e746f906f8d33400e41ae2314db7fc1daef09e34dc8da7f015eeafacfe0c",
                   "7839f0b00618f1f6c2ad26317ee8f1bd0886da8e5d5651624cc1e085f74da35d"},
    RenderExpected{"documents/batch-2/35-final-judgment.md", "objects/35-final-judgment.pdf",
                   "Synthetic Final Judgment", 248, 2, 21540,
                   "63db2aa8a87e332bc0cbabda1bf9100eea433d71d4944a5e34a46025526ae84a",
                   "8477dc5f05ab8be5c13f8b3dd0c55392ccee7a53a29ea974b58732b42534d171"},
    RenderExpected{"documents/batch-2/36-notice-of-appeal.md", "objects/36-notice-of-appeal.pdf",
                   "Synthetic Notice of Appeal", 250, 3, 24387,
                   "0be8aade5803015370e92fa417b9e4e930c603128f71e522ae03b95763d79575",
                   "3e75e87fd04fd52e35865085dbd2f13c48c0f92e92354c34eb8ba30c09432012"},
    RenderExpected{"documents/batch-2/37-certified-docket-record-certificate.md",
                   "objects/37-certified-docket-record-certificate.pdf",
                   "Synthetic Certified Docket and Record-Complete Certificate", 253, 10, 50778,
                   "c389e5c28c00f040c2f8639778ca87749bdfdc02cccddaf4b1e907f256be8c84",
                   "9b2ffb64c9501150c71f2bf5ef5fef5ca303ddc271339c23bcc43559ea612e78"},
};

constexpr std::array<std::string_view, 65> retaliation_anchors{
    "ca4m4.benton.anchor.ja3",   "ca4m4.benton.anchor.ja4",   "ca4m4.benton.anchor.ja5",
    "ca4m4.benton.anchor.ja6",   "ca4m4.benton.anchor.ja7",   "ca4m4.benton.anchor.ja42",
    "ca4m4.benton.anchor.ja43",  "ca4m4.benton.anchor.ja44",  "ca4m4.benton.anchor.ja47",
    "ca4m4.benton.anchor.ja58",  "ca4m4.benton.anchor.ja64",  "ca4m4.benton.anchor.ja65",
    "ca4m4.benton.anchor.ja68",  "ca4m4.benton.anchor.ja70",  "ca4m4.benton.anchor.ja71",
    "ca4m4.benton.anchor.ja72",  "ca4m4.benton.anchor.ja82",  "ca4m4.benton.anchor.ja88",
    "ca4m4.benton.anchor.ja89",  "ca4m4.benton.anchor.ja91",  "ca4m4.benton.anchor.ja93",
    "ca4m4.benton.anchor.ja96",  "ca4m4.benton.anchor.ja98",  "ca4m4.benton.anchor.ja103",
    "ca4m4.benton.anchor.ja110", "ca4m4.benton.anchor.ja111", "ca4m4.benton.anchor.ja112",
    "ca4m4.benton.anchor.ja113", "ca4m4.benton.anchor.ja118", "ca4m4.benton.anchor.ja120",
    "ca4m4.benton.anchor.ja121", "ca4m4.benton.anchor.ja122", "ca4m4.benton.anchor.ja124",
    "ca4m4.benton.anchor.ja125", "ca4m4.benton.anchor.ja126", "ca4m4.benton.anchor.ja132",
    "ca4m4.benton.anchor.ja136", "ca4m4.benton.anchor.ja140", "ca4m4.benton.anchor.ja146",
    "ca4m4.benton.anchor.ja156", "ca4m4.benton.anchor.ja166", "ca4m4.benton.anchor.ja188",
    "ca4m4.benton.anchor.ja189", "ca4m4.benton.anchor.ja190", "ca4m4.benton.anchor.ja191",
    "ca4m4.benton.anchor.ja192", "ca4m4.benton.anchor.ja193", "ca4m4.benton.anchor.ja217",
    "ca4m4.benton.anchor.ja218", "ca4m4.benton.anchor.ja219", "ca4m4.benton.anchor.ja220",
    "ca4m4.benton.anchor.ja221", "ca4m4.benton.anchor.ja222", "ca4m4.benton.anchor.ja236",
    "ca4m4.benton.anchor.ja237", "ca4m4.benton.anchor.ja238", "ca4m4.benton.anchor.ja239",
    "ca4m4.benton.anchor.ja240", "ca4m4.benton.anchor.ja241", "ca4m4.benton.anchor.ja242",
    "ca4m4.benton.anchor.ja243", "ca4m4.benton.anchor.ja244", "ca4m4.benton.anchor.ja245",
    "ca4m4.benton.anchor.ja246", "ca4m4.benton.anchor.ja247",
};

constexpr std::array<std::string_view, 29> exclusion_anchors{
    "ca4m4.benton.anchor.ja21",  "ca4m4.benton.anchor.ja23",  "ca4m4.benton.anchor.ja24",
    "ca4m4.benton.anchor.ja28",  "ca4m4.benton.anchor.ja29",  "ca4m4.benton.anchor.ja30",
    "ca4m4.benton.anchor.ja34",  "ca4m4.benton.anchor.ja114", "ca4m4.benton.anchor.ja121",
    "ca4m4.benton.anchor.ja136", "ca4m4.benton.anchor.ja140", "ca4m4.benton.anchor.ja145",
    "ca4m4.benton.anchor.ja194", "ca4m4.benton.anchor.ja195", "ca4m4.benton.anchor.ja222",
    "ca4m4.benton.anchor.ja223", "ca4m4.benton.anchor.ja224", "ca4m4.benton.anchor.ja225",
    "ca4m4.benton.anchor.ja226", "ca4m4.benton.anchor.ja227", "ca4m4.benton.anchor.ja228",
    "ca4m4.benton.anchor.ja229", "ca4m4.benton.anchor.ja230", "ca4m4.benton.anchor.ja231",
    "ca4m4.benton.anchor.ja232", "ca4m4.benton.anchor.ja233", "ca4m4.benton.anchor.ja234",
    "ca4m4.benton.anchor.ja235", "ca4m4.benton.anchor.ja237",
};

[[nodiscard]] QByteArray readAll(const QString& file_name) {
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
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

template <std::size_t Size>
[[nodiscard]] QJsonArray jsonArray(const std::array<std::string_view, Size>& values) {
    QJsonArray result;
    for (const auto value : values) {
        result.push_back(QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    }
    return result;
}

template <std::size_t Size>
[[nodiscard]] QSet<QString> stringSet(const std::array<std::string_view, Size>& values) {
    return strings(jsonArray(values));
}

[[nodiscard]] QJsonObject objectById(const QJsonArray& values, const QString& key,
                                     const QString& id) {
    const auto found = std::ranges::find_if(
        values, [&](const auto& value) { return value.toObject().value(key).toString() == id; });
    return found == values.end() ? QJsonObject{} : found->toObject();
}

[[nodiscard]] int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(model::LegalDate court_date) {
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}},
                            court_date};
}

[[nodiscard]] model::WorkflowCommandHeader header(std::string session_id, std::string command_id,
                                                  std::string actor_id,
                                                  model::LegalDate occurred_on) {
    return model::WorkflowCommandHeader{std::move(session_id),
                                        model::WorkflowCommandId{std::move(command_id)},
                                        model::ActorId{std::move(actor_id)}, at(occurred_on)};
}

[[nodiscard]] model::WorkflowFieldValue field(std::string id, std::string value) {
    return model::WorkflowFieldValue{model::FilingFieldId{std::move(id)}, std::move(value)};
}

[[nodiscard]] model::WorkflowState initialState(const packs::RuntimeCase& runtime_case,
                                                std::string session_id) {
    model::WorkflowState state;
    state.session_id = std::move(session_id);
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = runtime_case.workflow.initial_stage_id;
    return state;
}

struct Run final {
    model::WorkflowState initial_state;
    model::WorkflowState state;
    std::vector<model::WorkflowJournalEntry> journal;
    std::vector<model::WorkflowEvent> trace;
};

[[nodiscard]] Run emptyRun(const packs::RuntimeCase& runtime_case, std::string session_id) {
    auto initial = initialState(runtime_case, std::move(session_id));
    return Run{initial, initial, {}, {}};
}

[[nodiscard]] auto execute(const packs::RuntimeCase& runtime_case, Run& run,
                           model::WorkflowCommand command) -> std::expected<void, std::string> {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, run.state, command);
    if (!decision) {
        return std::unexpected(decision.error().message);
    }
    auto candidate_journal = run.journal;
    candidate_journal.push_back(model::WorkflowJournalEntry{std::move(command), *decision});
    const auto replayed = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 run.initial_state, candidate_journal);
    if (!replayed) {
        return std::unexpected(replayed.error().message);
    }
    run.trace.insert(run.trace.end(), decision->begin(), decision->end());
    run.journal = std::move(candidate_journal);
    run.state = *replayed;
    return {};
}

[[nodiscard]] bool isUnmet(const packs::RuntimeCase& runtime_case, const Run& run,
                           const model::WorkflowCommand& command) {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, run.state, command);
    return !decision && decision.error().code == engine::WorkflowErrorCode::UnmetPrecondition;
}

[[nodiscard]] bool isRejectedWith(const packs::RuntimeCase& runtime_case, const Run& run,
                                  const model::WorkflowCommand& command,
                                  engine::WorkflowErrorCode expected_code) {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, run.state, command);
    return !decision && decision.error().code == expected_code;
}

[[nodiscard]] model::SubmitWorkflowFiling notice(std::string session_id, std::string command_id,
                                                 std::string filing_id,
                                                 model::LegalDate occurred_on) {
    return model::SubmitWorkflowFiling{
        header(std::move(session_id), std::move(command_id), appellant_actor, occurred_on),
        model::WorkflowFilingId{std::move(filing_id)},
        model::FilingTypeId{"us.ca4.filing.civil-notice-of-appeal"},
        std::string(64, 'a'),
        {field("us.ca4.field.civil-notice.caption", "Leora Benton v. Blue Cedar Compliance"),
         field("us.ca4.field.civil-notice.appealing-parties", "Leora Benton"),
         field("us.ca4.field.civil-notice.originating-docket", "SYN-EDVA-25-CV-0412"),
         field("us.ca4.field.civil-notice.judgment-or-order",
               "Final summary-judgment order and judgment"),
         field("us.ca4.field.civil-notice.order-date", "2025-12-19"),
         field("us.ca4.field.civil-notice.destination-court",
               "United States Court of Appeals for the Fourth Circuit")},
        {model::ActorId{appellee_actor}},
        std::nullopt};
}

[[nodiscard]] model::SubmitWorkflowFiling
principalBrief(std::string session_id, std::string command_id, std::string filing_id,
               std::string actor_id, std::string served_actor_id, model::LegalDate occurred_on) {
    return model::SubmitWorkflowFiling{
        header(std::move(session_id), std::move(command_id), std::move(actor_id), occurred_on),
        model::WorkflowFilingId{std::move(filing_id)},
        model::FilingTypeId{"us.ca4.filing.principal-brief"},
        std::string(64, 'b'),
        {field("us.ca4.field.brief.issues", "Two preserved issues"),
         field("us.ca4.field.brief.argument", "Record-grounded merits argument"),
         field("us.ca4.field.brief.record-citations", "JA3-JA262 curated citations"),
         field("us.ca4.field.brief.authority-citations", "Title VII, Foster, Rules 26, 37, 56")},
        {model::ActorId{std::move(served_actor_id)}},
        std::nullopt};
}

[[nodiscard]] model::SubmitWorkflowFiling rehearingPetition(std::string session_id,
                                                            std::string command_id,
                                                            std::string filing_id,
                                                            model::LegalDate occurred_on) {
    return model::SubmitWorkflowFiling{
        header(std::move(session_id), std::move(command_id), appellant_actor, occurred_on),
        model::WorkflowFilingId{std::move(filing_id)},
        model::FilingTypeId{"us.ca4.filing.rehearing-petition"},
        std::string(64, 'a'),
        {field("us.ca4.field.rehearing.kind", "panel rehearing"),
         field("us.ca4.field.rehearing.purpose-statement", "Material point asserted"),
         field("us.ca4.field.rehearing.grounds", "FRAP 40 grounds")},
        {model::ActorId{appellee_actor}},
        std::nullopt};
}

[[nodiscard]] model::EnterWorkflowOrder order(std::string session_id, std::string command_id,
                                              std::string actor_id, std::string operation_id,
                                              std::string order_id,
                                              model::WorkflowOrderDisposition disposition,
                                              model::LegalDate occurred_on, char digest_char) {
    return model::EnterWorkflowOrder{
        header(std::move(session_id), std::move(command_id), std::move(actor_id), occurred_on),
        model::WorkflowOperationId{std::move(operation_id)},
        model::WorkflowOrderId{std::move(order_id)},
        disposition,
        std::string(64, digest_char),
        std::nullopt};
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto authoring_root = QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT));
    const auto pack_root = authoring_root.filePath(QStringLiteral("pack"));
    const auto foundations_root = QDir(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    const PackRevision expected_root{PackId{"us.ca4.m4.benton-retaliation"}, "1.1.0", root_digest};
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
        source->resources.size() != std::size_t{8} || source->blobs.size() != std::size_t{37}) {
        return fail(QStringLiteral("source pack revision/count contract mismatch"));
    }

    const auto readme =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral("README.md"))))
            .simplified();
    if (!readme.contains(QStringLiteral("record-complete successor")) ||
        !readme.contains(QStringLiteral("record-complete authoring checkpoint")) ||
        !readme.contains(
            QStringLiteral("37 unique substantive searchable PDFs totaling 262 pages")) ||
        !readme.contains(QStringLiteral("19 PDFs and JA1–JA125")) ||
        !readme.contains(QStringLiteral("18 PDFs and JA126–JA262")) ||
        !readme.contains(QStringLiteral("filing presence does not prove that both an")) ||
        !readme.contains(QStringLiteral("opening and response brief exist")) ||
        !readme.contains(
            QStringLiteral("Structured disposition, executed appellate workflow traces")) ||
        !readme.contains(QStringLiteral("actual, narrow exclusion of the late Wynn declaration")) ||
        !readme.contains(QStringLiteral("expressly does not rely on a sham-affidavit rule")) ||
        !readme.contains(QStringLiteral("court records `ca4m4.benton.deadline.rehearing`"))) {
        return fail(QStringLiteral("README does not preserve the 1.1 record-complete contract"));
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
    const auto event_deadline_capability =
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
        event_deadline_capability == source->required_capabilities.end() ||
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
        if (bytes.isEmpty() ||
            sha256(bytes) != QByteArray::fromStdString(resource.descriptor.sha256)) {
            return fail(QStringLiteral("resource descriptor digest mismatch: %1")
                            .arg(QString::fromStdString(resource.descriptor.path)));
        }
    }
    for (const auto& blob : source->blobs) {
        const auto bytes = readAll(QDir(pack_root).filePath(QString::fromStdString(blob.path)));
        if (bytes.size() != static_cast<qsizetype>(blob.byte_size) ||
            sha256(bytes) != QByteArray::fromStdString(blob.sha256)) {
            return fail(QStringLiteral("blob bytes/digest mismatch: %1")
                            .arg(QString::fromStdString(blob.path)));
        }
    }

    const auto* case_resource = findResource(source->resources, "ca4m4.case.benton-retaliation");
    const auto* record_resource = findResource(source->resources, "ca4m4.benton.record");
    const auto* authority_resource =
        findResource(source->resources, "ca4m4.benton.authorities.case-specific");
    const auto* workflow_resource =
        findResource(source->resources, "ca4m4.benton.workflow.civil-appeal");
    const auto* bench_resource = findResource(source->resources, "ca4m4.benton.bench.three-judge");
    const auto* actual_argument =
        findResource(source->resources, "ca4m4.benton.argument.actual-record");
    const auto* counterfactual_argument =
        findResource(source->resources, "ca4m4.benton.argument.no-knowledge-counterfactual");
    if (case_resource == nullptr || record_resource == nullptr || authority_resource == nullptr ||
        workflow_resource == nullptr || bench_resource == nullptr || actual_argument == nullptr ||
        counterfactual_argument == nullptr ||
        case_resource->descriptor.kind != ResourceKind::Case ||
        record_resource->descriptor.kind != ResourceKind::Record ||
        actual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        counterfactual_argument->descriptor.kind != ResourceKind::ArgumentConfig) {
        return fail(QStringLiteral("required Benton resources are absent"));
    }

    if (case_resource->document.contains(QStringLiteral("disposition_plans")) ||
        case_resource->document.contains(QStringLiteral("authored_disposition_plan_id")) ||
        case_resource->document.contains(QStringLiteral("authored_disposition_operation_id"))) {
        return fail(QStringLiteral("Benton 1.1 must not contain a structured disposition"));
    }
    for (const auto& issue_value :
         case_resource->document.value(QStringLiteral("issues")).toArray()) {
        if (issue_value.toObject().contains(QStringLiteral("target_ids"))) {
            return fail(QStringLiteral("future disposition target leaked into Benton 1.1"));
        }
    }
    for (const auto& actor_value :
         case_resource->document.value(QStringLiteral("actors")).toArray()) {
        if (!actor_value.toObject().value(QStringLiteral("synthetic")).toBool()) {
            return fail(QStringLiteral("case actor is not explicitly synthetic"));
        }
    }

    const auto case_issues = case_resource->document.value(QStringLiteral("issues")).toArray();
    if (case_issues.size() != 2) {
        return fail(QStringLiteral("Benton must expose exactly two current issues"));
    }
    const auto retaliation =
        objectById(case_issues, QStringLiteral("issue_id"), QString::fromLatin1(retaliation_issue));
    const auto exclusion =
        objectById(case_issues, QStringLiteral("issue_id"), QString::fromLatin1(exclusion_issue));
    const QJsonArray retaliation_authorities{
        QStringLiteral("ca4m4.benton.authority.title-vii-retaliation"),
        QStringLiteral("ca4m4.benton.authority.foster-framework"),
        QStringLiteral("ca4m4.benton.authority.foster-pretext"),
        QStringLiteral("ca4m4.benton.authority.frcp-56-summary-judgment"),
    };
    const QJsonArray exclusion_authorities{
        QStringLiteral("ca4m4.benton.authority.frcp-26e-supplementation"),
        QStringLiteral("ca4m4.benton.authority.frcp-37c1-nondisclosure"),
        QStringLiteral("ca4m4.benton.authority.benjamin-disclosure-sanction"),
    };
    if (retaliation.isEmpty() || exclusion.isEmpty() ||
        !exclusion.value(QStringLiteral("title"))
             .toString()
             .contains(QStringLiteral("narrowly excluding the late Wynn declaration")) ||
        retaliation.value(QStringLiteral("record_anchor_ids")).toArray() !=
            jsonArray(retaliation_anchors) ||
        exclusion.value(QStringLiteral("record_anchor_ids")).toArray() !=
            jsonArray(exclusion_anchors) ||
        retaliation.value(QStringLiteral("authority_ids")).toArray() != retaliation_authorities ||
        exclusion.value(QStringLiteral("authority_ids")).toArray() != exclusion_authorities) {
        return fail(QStringLiteral("curated issue anchor/authority contract mismatch"));
    }

    const QHash<QString, QSet<QString>> issue_anchors{
        {QString::fromLatin1(retaliation_issue), stringSet(retaliation_anchors)},
        {QString::fromLatin1(exclusion_issue), stringSet(exclusion_anchors)},
    };
    const QHash<QString, QSet<QString>> issue_authorities{
        {QString::fromLatin1(retaliation_issue), strings(retaliation_authorities)},
        {QString::fromLatin1(exclusion_issue), strings(exclusion_authorities)},
    };
    const QSet<QString> expected_issue_ids{QString::fromLatin1(retaliation_issue),
                                           QString::fromLatin1(exclusion_issue)};

    const auto check_argument_bank = [&](const ValidatedResource& resource,
                                         const QString& expected_id, const QString& expected_mode,
                                         const QString& expected_digest, int expected_total_seconds,
                                         int expected_rebuttal_seconds) -> std::optional<QString> {
        const auto document = resource.document;
        const auto permitted =
            strings(document.value(QStringLiteral("permitted_issue_ids")).toArray());
        const auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
        const auto bindings = bank.value(QStringLiteral("issue_topic_bindings")).toArray();
        const auto questions = bank.value(QStringLiteral("questions")).toArray();
        if (document.value(QStringLiteral("resource_id")).toString() != expected_id ||
            document.value(QStringLiteral("case_id")).toString() !=
                QStringLiteral("ca4m4.case.benton-retaliation") ||
            document.value(QStringLiteral("bench_configuration_id")).toString() !=
                QStringLiteral("ca4m4.benton.bench.three-judge") ||
            document.value(QStringLiteral("total_seconds")).toInt() != expected_total_seconds ||
            document.value(QStringLiteral("rebuttal_seconds")).toInt() !=
                expected_rebuttal_seconds ||
            permitted != expected_issue_ids ||
            bank.value(QStringLiteral("mode")).toString() != expected_mode ||
            bank.value(QStringLiteral("grounding_digest")).toString() != expected_digest ||
            bindings.size() != 2 || questions.size() != 12) {
            return QStringLiteral("argument-bank envelope mismatch: %1").arg(expected_id);
        }

        QSet<QString> binding_pairs;
        QSet<QString> binding_topics;
        for (const auto& binding_value : bindings) {
            const auto binding = binding_value.toObject();
            const auto issue_id = binding.value(QStringLiteral("issue_id")).toString();
            const auto topics = binding.value(QStringLiteral("topic_ids")).toArray();
            if (!permitted.contains(issue_id) || topics.size() != 6) {
                return QStringLiteral("argument-bank issue binding mismatch: %1").arg(expected_id);
            }
            for (const auto& topic_value : topics) {
                const auto topic = topic_value.toString();
                const auto pair = issue_id + QLatin1Char('|') + topic;
                if (binding_pairs.contains(pair)) {
                    return QStringLiteral("duplicate issue/topic binding: %1").arg(expected_id);
                }
                binding_pairs.insert(pair);
                binding_topics.insert(topic);
            }
        }

        QSet<QString> question_pairs;
        QHash<QString, QSet<QString>> used_anchors;
        QHash<QString, QSet<QString>> used_authorities;
        for (const auto& question_value : questions) {
            const auto question = question_value.toObject();
            const auto issue_id = question.value(QStringLiteral("issue_id")).toString();
            const auto topic_id = question.value(QStringLiteral("topic_id")).toString();
            const auto pair = issue_id + QLatin1Char('|') + topic_id;
            const auto grounding = question.value(QStringLiteral("grounding")).toArray();
            if (!permitted.contains(issue_id) ||
                question.value(QStringLiteral("question_id")).toString().isEmpty() ||
                question.value(QStringLiteral("prompt")).toString().isEmpty() ||
                grounding.isEmpty() || !binding_pairs.contains(pair) ||
                question_pairs.contains(pair)) {
                return QStringLiteral("argument-bank question coverage mismatch: %1")
                    .arg(expected_id);
            }
            if (issue_id == QString::fromLatin1(exclusion_issue) &&
                question.value(QStringLiteral("prompt"))
                    .toString()
                    .contains(QStringLiteral("hypothet"), Qt::CaseInsensitive)) {
                return QStringLiteral("exclusion question retained obsolete hypothetical: %1")
                    .arg(expected_id);
            }
            question_pairs.insert(pair);
            for (const auto& grounding_value : grounding) {
                const auto item = grounding_value.toObject();
                const auto kind = item.value(QStringLiteral("kind")).toString();
                if (item.value(QStringLiteral("grounding_id")).toString().isEmpty()) {
                    return QStringLiteral("empty grounding identity: %1").arg(expected_id);
                }
                if (kind == QStringLiteral("record_page")) {
                    const auto anchor = item.value(QStringLiteral("anchor_id")).toString();
                    if (!issue_anchors.value(issue_id).contains(anchor)) {
                        return QStringLiteral("cross-issue record grounding: %1").arg(expected_id);
                    }
                    used_anchors[issue_id].insert(anchor);
                } else if (kind == QStringLiteral("authority")) {
                    const auto authority = item.value(QStringLiteral("authority_id")).toString();
                    if (!issue_authorities.value(issue_id).contains(authority)) {
                        return QStringLiteral("cross-issue authority grounding: %1")
                            .arg(expected_id);
                    }
                    used_authorities[issue_id].insert(authority);
                } else {
                    return QStringLiteral("noncanonical grounding kind: %1").arg(expected_id);
                }
            }
        }
        if (question_pairs != binding_pairs ||
            used_anchors.value(QString::fromLatin1(retaliation_issue)).isEmpty() ||
            used_anchors.value(QString::fromLatin1(exclusion_issue)).isEmpty() ||
            !used_anchors.value(QString::fromLatin1(exclusion_issue))
                 .contains(QStringLiteral("ca4m4.benton.anchor.ja194")) ||
            !used_anchors.value(QString::fromLatin1(exclusion_issue))
                 .contains(QStringLiteral("ca4m4.benton.anchor.ja195")) ||
            !used_anchors.value(QString::fromLatin1(exclusion_issue))
                 .contains(QStringLiteral("ca4m4.benton.anchor.ja232")) ||
            !used_anchors.value(QString::fromLatin1(exclusion_issue))
                 .contains(QStringLiteral("ca4m4.benton.anchor.ja235")) ||
            used_authorities.value(QString::fromLatin1(retaliation_issue)) !=
                issue_authorities.value(QString::fromLatin1(retaliation_issue)) ||
            used_authorities.value(QString::fromLatin1(exclusion_issue)) !=
                issue_authorities.value(QString::fromLatin1(exclusion_issue))) {
            return QStringLiteral("argument bank grounding/authority coverage mismatch: %1")
                .arg(expected_id);
        }

        const QSet<QString> rowan_topics{
            QStringLiteral("workbench.topic.standard-of-review"),
            QStringLiteral("workbench.topic.record-support"),
            QStringLiteral("workbench.topic.governing-authority"),
        };
        const QSet<QString> alder_topics{
            QStringLiteral("workbench.topic.jurisdiction"),
            QStringLiteral("workbench.topic.preservation"),
            QStringLiteral("workbench.topic.remedy"),
        };
        const QSet<QString> fen_topics{
            QStringLiteral("workbench.topic.merits"),
            QStringLiteral("workbench.topic.practical-consequences"),
            QStringLiteral("workbench.topic.governing-authority"),
        };
        if ((binding_topics & rowan_topics).isEmpty() ||
            (binding_topics & alder_topics).isEmpty() || (binding_topics & fen_topics).isEmpty()) {
            return QStringLiteral("bank misses Rowan/Alder/Fen focus intersection: %1")
                .arg(expected_id);
        }
        return std::nullopt;
    };

    if (const auto error = check_argument_bank(
            *actual_argument, QStringLiteral("ca4m4.benton.argument.actual-record"),
            QStringLiteral("actual_record"), QString::fromLatin1(actual_bank_digest), 1200, 120);
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            check_argument_bank(*counterfactual_argument,
                                QStringLiteral("ca4m4.benton.argument.no-knowledge-counterfactual"),
                                QStringLiteral("counterfactual_training"),
                                QString::fromLatin1(counterfactual_bank_digest), 900, 90);
        error.has_value()) {
        return fail(*error);
    }
    const auto counterfactual_text = QString::fromUtf8(
        QJsonDocument(counterfactual_argument->document).toJson(QJsonDocument::Compact));
    if (!counterfactual_text.contains(QStringLiteral("no pretermination knowledge"),
                                      Qt::CaseInsensitive) ||
        counterfactual_text.contains(QStringLiteral("engine answer"), Qt::CaseInsensitive)) {
        return fail(QStringLiteral("counterfactual premise is missing or predicts an answer"));
    }

    struct AuthorityExpected final {
        std::string_view id;
        std::string_view version;
        std::string_view locator;
        std::string_view proposition;
        std::string_view url;
    };
    constexpr std::array expected_authorities{
        AuthorityExpected{
            "ca4m4.benton.authority.title-vii-retaliation", "2026-08-11", "Section 2000e-3(a)",
            "Title VII prohibits employer discrimination because an individual opposed an "
            "unlawful Title VII practice or participated in a Title VII investigation, "
            "proceeding, or hearing.",
            "https://uscode.house.gov/"
            "view.xhtml?req=%28title%3A42+section%3A2000e-3+edition%3Aprelim%29"},
        AuthorityExpected{
            "ca4m4.benton.authority.foster-framework", "2015-05-21", "Part III.B, PDF pages 17-20",
            "Nassar does not alter the prima-facie causation prong or displace the McDonnell "
            "Douglas framework for a Title VII retaliation claim proceeding through pretext.",
            "https://www.ca4.uscourts.gov/opinions/published/141073.p.pdf"},
        AuthorityExpected{
            "ca4m4.benton.authority.foster-pretext", "2015-05-21", "Parts III.B-C, PDF pages 18-23",
            "At pretext a retaliation plaintiff must show retaliation was the real reason and "
            "therefore a but-for cause; timing and evidence making stated reasons questionable "
            "can create a jury issue.",
            "https://www.ca4.uscourts.gov/opinions/published/141073.p.pdf"},
        AuthorityExpected{
            "ca4m4.benton.authority.frcp-56-summary-judgment", "2025-12-01", "Rule 56(a) and (c)",
            "Summary judgment requires no genuine dispute of material fact and entitlement to "
            "judgment as a matter of law on record materials the rule permits the parties to "
            "cite.",
            "https://www.uscourts.gov/sites/default/files/document/"
            "federal-rules-of-civil-procedure.pdf"},
        AuthorityExpected{
            "ca4m4.benton.authority.frcp-26e-supplementation", "2025-12-01",
            "Rule 26(a)(1)(A)(i) and (e)(1)",
            "Initial disclosures must identify each person likely to have discoverable "
            "information and the subjects of that information; a party must timely supplement "
            "or correct a materially incomplete or incorrect disclosure or response when the "
            "additional information has not otherwise been made known during discovery or in "
            "writing.",
            "https://www.uscourts.gov/sites/default/files/document/"
            "federal-rules-of-civil-procedure.pdf"},
        AuthorityExpected{
            "ca4m4.benton.authority.frcp-37c1-nondisclosure", "2025-12-01", "Rule 37(c)(1)",
            "Absent substantial justification or harmlessness, a party that fails to disclose "
            "or supplement as required may not use the information or witness on a motion, at a "
            "hearing, or at trial, in addition to other listed sanctions.",
            "https://www.uscourts.gov/sites/default/files/document/"
            "federal-rules-of-civil-procedure.pdf"},
        AuthorityExpected{
            "ca4m4.benton.authority.benjamin-disclosure-sanction", "2021-01-19",
            "No. 19-2041, official opinion lines 197-200 and 259-300",
            "Disclosure-violation and Rule 37(c)(1) exclusion rulings are reviewed for abuse of "
            "discretion. Surprise, ability to cure, disruption, importance, and explanation "
            "guide substantial justification and harmlessness; the first four principally "
            "address harmlessness, the fifth justification, and the nondisclosing party bears "
            "the burden.",
            "https://www.ca4.uscourts.gov/opinions/192041.P.pdf"},
    };
    const auto authorities =
        authority_resource->document.value(QStringLiteral("authorities")).toArray();
    if (authorities.size() != static_cast<qsizetype>(expected_authorities.size())) {
        return fail(QStringLiteral("case-specific authority count is not seven"));
    }
    for (const auto& expected : expected_authorities) {
        const auto authority =
            objectById(authorities, QStringLiteral("authority_id"), QString::fromUtf8(expected.id));
        if (authority.isEmpty() || !authority.value(QStringLiteral("official_source")).toBool() ||
            authority.value(QStringLiteral("source_version")).toString() !=
                QString::fromUtf8(expected.version) ||
            authority.value(QStringLiteral("checked_on")).toString() !=
                (QString::fromUtf8(expected.id).contains(QStringLiteral("frcp-26e")) ||
                         QString::fromUtf8(expected.id).contains(QStringLiteral("benjamin"))
                     ? QStringLiteral("2026-08-12")
                     : QStringLiteral("2026-08-11")) ||
            authority.value(QStringLiteral("locator")).toString() !=
                QString::fromUtf8(expected.locator) ||
            authority.value(QStringLiteral("proposition")).toString() !=
                QString::fromUtf8(expected.proposition) ||
            authority.value(QStringLiteral("source_url")).toString() !=
                QString::fromUtf8(expected.url)) {
            return fail(QStringLiteral("canonical authority provenance mismatch: %1")
                            .arg(QString::fromUtf8(expected.id)));
        }
    }

    const auto source_ledger =
        QString::fromUtf8(
            readAll(authoring_root.filePath(QStringLiteral("sources/SOURCE_LEDGER.md"))))
            .simplified();
    const auto fact_canon = QString::fromUtf8(
        readAll(authoring_root.filePath(QStringLiteral("sources/FACT_CANON.md"))));
    if (!source_ledger.contains(QStringLiteral("dynamic official compilation")) ||
        !source_ledger.contains(QStringLiteral("checked snapshot date, 2026-08-11")) ||
        !source_ledger.contains(QStringLiteral("Fictional-name collision review")) ||
        !source_ledger.contains(QStringLiteral("no match for the full fictional caption")) ||
        !source_ledger.contains(QStringLiteral("not represented as globally unique")) ||
        !source_ledger.contains(QStringLiteral("benjamin-disclosure-sanction")) ||
        !source_ledger.contains(QStringLiteral("The late Wynn declaration remains")) ||
        !fact_canon.contains(QStringLiteral("Counsel knows the new subject on October 15")) ||
        !fact_canon.contains(QStringLiteral("excludes only the new subject")) ||
        !fact_canon.contains(QStringLiteral("No appellate disposition is authored")) ||
        !fact_canon.contains(QStringLiteral("does not invoke the sham-affidavit rule")) ||
        !fact_canon.contains(QStringLiteral("23 = 21 + 2")) ||
        !fact_canon.contains(QStringLiteral("16 = 21 - 5")) ||
        !fact_canon.contains(QStringLiteral("16 = 23 - 5 - 2"))) {
        return fail(
            QStringLiteral("source/canon provenance or deferred-outcome boundary mismatch"));
    }

    QString record_source_semantics;
    for (const auto& expected : expected_render) {
        const auto source_path = QString::fromUtf8(
            expected.source_path.data(), static_cast<qsizetype>(expected.source_path.size()));
        record_source_semantics +=
            QString::fromUtf8(readAll(authoring_root.filePath(source_path))) + QLatin1Char('\n');
    }
    const auto current_semantics =
        QString::fromUtf8(QJsonDocument(case_resource->document).toJson(QJsonDocument::Compact)) +
        QString::fromUtf8(QJsonDocument(actual_argument->document).toJson(QJsonDocument::Compact)) +
        QString::fromUtf8(
            QJsonDocument(counterfactual_argument->document).toJson(QJsonDocument::Compact)) +
        fact_canon + source_ledger + record_source_semantics;
    const QStringList premature_appellate_claims{
        QStringLiteral("instruction-to-conceal"),
        QStringLiteral("instruction to conceal"),
        QStringLiteral("instructed Wynn"),
        QStringLiteral("directed concealment"),
        QStringLiteral("concealment account"),
        QStringLiteral("affirm exclusion"),
        QStringLiteral("vacate summary judgment"),
        QStringLiteral("reversed and remanded"),
        QStringLiteral("2025-12-12"),
        QStringLiteral("planned disposition target"),
    };
    for (const auto& claim : premature_appellate_claims) {
        if (current_semantics.contains(claim, Qt::CaseInsensitive)) {
            return fail(QStringLiteral("future appellate fact/outcome leaked: %1").arg(claim));
        }
    }
    for (const auto& required : {
             QStringLiteral("late Wynn declaration"),
             QStringLiteral("October 15"),
             QStringLiteral("October 17"),
             QStringLiteral("November 4"),
             QStringLiteral("Rules 26 and 37"),
             QStringLiteral("sham-affidavit"),
         }) {
        if (!current_semantics.contains(required, Qt::CaseInsensitive)) {
            return fail(QStringLiteral("actual exclusion record missing: %1").arg(required));
        }
    }

    const auto record_entries =
        record_resource->document.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record_resource->document.value(QStringLiteral("page_anchors")).toArray();
    if (record_resource->document.value(QStringLiteral("dockets")).toArray().size() != 2 ||
        record_entries.size() != 37 || anchors.size() != 262) {
        return fail(QStringLiteral("record count contract mismatch"));
    }
    const std::array composite_dates{
        std::pair{QStringLiteral("ca4m4.benton.record.entry.policies-rif-protocol"),
                  QStringLiteral("2025-02-14")},
        std::pair{QStringLiteral("ca4m4.benton.record.entry.opposition-emails"),
                  QStringLiteral("2025-01-31")},
        std::pair{QStringLiteral("ca4m4.benton.record.entry.eeoc-acknowledgments"),
                  QStringLiteral("2025-02-12")},
        std::pair{QStringLiteral("ca4m4.benton.record.entry.eeoc-notice-right-to-sue"),
                  QStringLiteral("2025-05-23")},
        std::pair{QStringLiteral("ca4m4.benton.record.entry.comparator-scorecards"),
                  QStringLiteral("2025-03-21")},
        std::pair{QStringLiteral("ca4m4.benton.record.entry.performance-metrics"),
                  QStringLiteral("2025-03-25")},
    };
    for (const auto& [entry_id, expected_date] : composite_dates) {
        const auto entry = objectById(record_entries, QStringLiteral("entry_id"), entry_id);
        if (entry.value(QStringLiteral("filed_on")).toString() != expected_date ||
            !entry.value(QStringLiteral("description"))
                 .toString()
                 .contains(QStringLiteral("through"), Qt::CaseInsensitive)) {
            return fail(
                QStringLiteral("composite production-date contract mismatch: %1").arg(entry_id));
        }
    }

    const auto scorecard_text = QString::fromUtf8(readAll(
        authoring_root.filePath(QStringLiteral("documents/batch-1/13-comparator-scorecards.md"))));
    const QStringList post_march_scorecard_claims{
        QStringLiteral("after termination"), QStringLiteral("after the RIF"),
        QStringLiteral("after selection"),   QStringLiteral("post-selection"),
        QStringLiteral("leadership later"),  QStringLiteral("factfinder"),
    };
    if (!scorecard_text.contains(QStringLiteral("March 20 B-7")) ||
        !scorecard_text.contains(QStringLiteral("March 21"))) {
        return fail(QStringLiteral("scorecard does not identify its pre-approval chronology"));
    }
    for (const auto& claim : post_march_scorecard_claims) {
        if (scorecard_text.contains(claim, Qt::CaseInsensitive)) {
            return fail(QStringLiteral("scorecard contains knowledge after its March 21 date: %1")
                            .arg(claim));
        }
    }

    const auto charge_text = QString::fromUtf8(
        readAll(authoring_root.filePath(QStringLiteral("documents/batch-1/06-eeoc-charge.md"))));
    const auto acknowledgment_text = QString::fromUtf8(readAll(
        authoring_root.filePath(QStringLiteral("documents/batch-1/11-eeoc-acknowledgments.md"))));
    const auto workbook_text = QString::fromUtf8(
        readAll(authoring_root.filePath(QStringLiteral("documents/batch-1/12-rif-workbook.md"))));
    if (!charge_text.contains(QStringLiteral("had not been produced to her")) ||
        charge_text.contains(QStringLiteral("imported score of 84"), Qt::CaseInsensitive) ||
        charge_text.contains(QStringLiteral("exceptions favoring"), Qt::CaseInsensitive) ||
        charge_text.contains(QStringLiteral("Pike opening"), Qt::CaseInsensitive) ||
        !acknowledgment_text.contains(QStringLiteral("no page-level reading history")) ||
        acknowledgment_text.contains(QStringLiteral("parties dispute the extent"),
                                     Qt::CaseInsensitive) ||
        acknowledgment_text.contains(QStringLiteral("offered to show"), Qt::CaseInsensitive) ||
        acknowledgment_text.contains(QStringLiteral("admission of retaliatory purpose"),
                                     Qt::CaseInsensitive) ||
        workbook_text.contains(QStringLiteral("parties dispute whether"), Qt::CaseInsensitive)) {
        return fail(
            QStringLiteral("charge/acknowledgment/workbook temporal-editorial audit failed"));
    }

    const auto headcount_text = QString::fromUtf8(readAll(
        authoring_root.filePath(QStringLiteral("documents/batch-1/17-headcount-ledger.md"))));
    const auto rif_text = workbook_text + headcount_text +
                          QString::fromUtf8(readAll(authoring_root.filePath(
                              QStringLiteral("documents/batch-1/19-pike-deposition.md"))));
    const QStringList rif_contract{
        QStringLiteral("$148,000"),        QStringLiteral("$94,600"),
        QStringLiteral("$88,800"),         QStringLiteral("$82,400"),
        QStringLiteral("$76,200"),         QStringLiteral("$66,400"),
        QStringLiteral("$62,000"),         QStringLiteral("$618,400"),
        QStringLiteral("$542,200"),        QStringLiteral("$69,800"),
        QStringLiteral("$470,400"),        QStringLiteral("five filled"),
        QStringLiteral("two vacant"),      QStringLiteral("seven-position"),
        QStringLiteral("$609,400"),        QStringLiteral("$602,400"),
        QStringLiteral("$598,400"),        QStringLiteral("$14,600"),
        QStringLiteral("archive-hosting"),
    };
    for (const auto& token : rif_contract) {
        if (!rif_text.contains(token, Qt::CaseInsensitive)) {
            return fail(QStringLiteral("final RIF arithmetic is missing: %1").arg(token));
        }
    }

    const auto baseline_match =
        QRegularExpression(
            QStringLiteral("contained\\s+(\\d+) authorized positions:\\s+(\\d+) filled and "
                           "(\\d+) vacant"),
            QRegularExpression::CaseInsensitiveOption)
            .match(headcount_text.simplified());
    const auto final_match =
        QRegularExpression(
            QStringLiteral("terminated\\s+(\\d+) occupants, leaving\\s+(\\d+) filled "
                           "positions, and eliminated both vacancies, leaving\\s+(\\d+) "
                           "vacant positions"),
            QRegularExpression::CaseInsensitiveOption)
            .match(headcount_text.simplified());
    const auto workbook_match =
        QRegularExpression(
            QStringLiteral("included\\s+(\\d+) authorized operations and support positions: "
                           "(\\d+) were filled and\\s+(\\d+) were vacant"),
            QRegularExpression::CaseInsensitiveOption)
            .match(workbook_text.simplified());
    if (!baseline_match.hasMatch() || !final_match.hasMatch() || !workbook_match.hasMatch()) {
        return fail(QStringLiteral("RIF headcount arithmetic fields are not machine-auditable"));
    }
    const int total_positions = baseline_match.captured(1).toInt();
    const int filled_positions = baseline_match.captured(2).toInt();
    const int vacant_positions = baseline_match.captured(3).toInt();
    const int terminations = final_match.captured(1).toInt();
    const int remaining_filled = final_match.captured(2).toInt();
    const int remaining_vacant = final_match.captured(3).toInt();
    if (total_positions != filled_positions + vacant_positions ||
        remaining_filled != filled_positions - terminations ||
        remaining_filled != total_positions - terminations - vacant_positions ||
        remaining_vacant != 0 || workbook_match.captured(1).toInt() != total_positions ||
        workbook_match.captured(2).toInt() != filled_positions ||
        workbook_match.captured(3).toInt() != vacant_positions || total_positions != 23 ||
        filled_positions != 21 || vacant_positions != 2 || terminations != 5 ||
        remaining_filled != 16) {
        return fail(QStringLiteral("23-position/21-filled/5-termination invariant failed"));
    }

    const auto render_plan_batch_1 =
        QJsonDocument::fromJson(
            readAll(authoring_root.filePath(QStringLiteral("render-plan-batch-1.json"))))
            .object();
    const auto render_plan_batch_2 =
        QJsonDocument::fromJson(
            readAll(authoring_root.filePath(QStringLiteral("render-plan-batch-2.json"))))
            .object();
    const auto plan_entries_batch_1 =
        render_plan_batch_1.value(QStringLiteral("entries")).toArray();
    const auto plan_entries_batch_2 =
        render_plan_batch_2.value(QStringLiteral("entries")).toArray();
    const auto render_inventory_batch_1 =
        QJsonDocument::fromJson(readAll(authoring_root.filePath(
                                    QStringLiteral("metadata/render-inventory-batch-1.json"))))
            .object();
    const auto render_inventory_batch_2 =
        QJsonDocument::fromJson(readAll(authoring_root.filePath(
                                    QStringLiteral("metadata/render-inventory-batch-2.json"))))
            .object();
    const auto rendered_entries_batch_1 =
        render_inventory_batch_1.value(QStringLiteral("entries")).toArray();
    const auto rendered_entries_batch_2 =
        render_inventory_batch_2.value(QStringLiteral("entries")).toArray();
    if (sha256(readAll(authoring_root.filePath(QStringLiteral("render-plan-batch-1.json")))) !=
            QByteArrayLiteral("ccb5802d6e6938daa6f2ae9a7349b0d0dd271741ccd06b9a4ddbcce889c488a0") ||
        sha256(readAll(authoring_root.filePath(QStringLiteral("render-plan-batch-2.json")))) !=
            QByteArrayLiteral("fc959667fe8f9229d3c1d8e06398822f2e21a87518339dc540fe4519352d3a81") ||
        sha256(readAll(
            authoring_root.filePath(QStringLiteral("metadata/render-inventory-batch-1.json")))) !=
            QByteArrayLiteral("990b875bb2be78e6860753a93a25aff67d67137b4323cea89070b9a2288c5964") ||
        sha256(readAll(
            authoring_root.filePath(QStringLiteral("metadata/render-inventory-batch-2.json")))) !=
            QByteArrayLiteral("2396666a07d95f5e4a2470cf2c769d1595774bb81c93980b5c8c06899f38939b") ||
        render_plan_batch_1.value(QStringLiteral("schema_version")).toInt() != 1 ||
        render_plan_batch_2.value(QStringLiteral("schema_version")).toInt() != 1 ||
        plan_entries_batch_1.size() != 19 || plan_entries_batch_2.size() != 18 ||
        rendered_entries_batch_1.size() != 19 || rendered_entries_batch_2.size() != 18 ||
        render_inventory_batch_1.value(QStringLiteral("plan_sha256")).toString() !=
            QStringLiteral("ccb5802d6e6938daa6f2ae9a7349b0d0dd271741ccd06b9a4ddbcce889c488a0") ||
        render_inventory_batch_2.value(QStringLiteral("plan_sha256")).toString() !=
            QStringLiteral("fc959667fe8f9229d3c1d8e06398822f2e21a87518339dc540fe4519352d3a81") ||
        render_inventory_batch_1.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
        render_inventory_batch_2.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
        render_inventory_batch_1.value(QStringLiteral("renderer_contract")).toString() !=
            QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
        render_inventory_batch_2.value(QStringLiteral("renderer_contract")).toString() !=
            QStringLiteral("appellate.markdown-pdf.semantic-layout.v2")) {
        return fail(QStringLiteral("two-render plan/inventory envelope mismatch"));
    }
    auto plan_entries = plan_entries_batch_1;
    for (const auto& entry : plan_entries_batch_2) {
        plan_entries.push_back(entry);
    }
    auto rendered_entries = rendered_entries_batch_1;
    for (const auto& entry : rendered_entries_batch_2) {
        rendered_entries.push_back(entry);
    }
    if (plan_entries.size() != static_cast<qsizetype>(expected_render.size()) ||
        rendered_entries.size() != static_cast<qsizetype>(expected_render.size())) {
        return fail(QStringLiteral("combined render closure mismatch"));
    }

    const QString banner =
        QStringLiteral("> **FICTIONAL TRAINING ARTIFACT — NOT FILED — NOT LEGAL ADVICE**");
    const QStringList forbidden_source_phrases{
        QStringLiteral("batch 2"),           QStringLiteral("future declaration"),
        QStringLiteral("later declaration"), QStringLiteral("authored disposition"),
        QStringLiteral("planned outcome"),   QStringLiteral("record anchor"),
        QStringLiteral("workbench"),         QStringLiteral("renderer"),
        QStringLiteral("placeholder"),       QStringLiteral("padding"),
        QStringLiteral("2025-12-12"),
    };
    const QStringList forbidden_batch_1_phrases{
        QStringLiteral("late Wynn declaration"),
        QStringLiteral("late declaration"),
        QStringLiteral("2025-12-19"),
        QStringLiteral("2026-01-16"),
    };
    const QRegularExpression any_page_label(QStringLiteral("\\bJA\\d+\\b"));
    QSet<QString> distinct_source_pages;
    QSet<QString> distinct_pdf_pages;
    int expected_ja = 1;
    for (std::size_t index = 0; index < expected_render.size(); ++index) {
        const auto& expected = expected_render.at(index);
        const auto plan = plan_entries.at(static_cast<qsizetype>(index)).toObject();
        const auto rendered = rendered_entries.at(static_cast<qsizetype>(index)).toObject();
        const auto record_entry = record_entries.at(static_cast<qsizetype>(index)).toObject();
        const auto source_path = QString::fromUtf8(
            expected.source_path.data(), static_cast<qsizetype>(expected.source_path.size()));
        const auto output_path = QString::fromUtf8(
            expected.output_path.data(), static_cast<qsizetype>(expected.output_path.size()));
        const auto title =
            QString::fromUtf8(expected.title.data(), static_cast<qsizetype>(expected.title.size()));
        const auto source_bytes = readAll(authoring_root.filePath(source_path));
        const auto pdf_bytes = readAll(QDir(pack_root).filePath(output_path));
        const auto assembly = rendered.value(QStringLiteral("assembly_provenance")).toObject();
        const auto page_labels = rendered.value(QStringLiteral("page_labels")).toObject();
        const auto blob =
            std::ranges::find(source->blobs, expected.output_path, &model::BlobDescriptor::path);

        if (plan.value(QStringLiteral("source_path")).toString() != source_path ||
            plan.value(QStringLiteral("output_path")).toString() != output_path ||
            plan.value(QStringLiteral("title")).toString() != title ||
            plan.value(QStringLiteral("page_label_prefix")).toString() != QStringLiteral("JA") ||
            plan.value(QStringLiteral("page_label_start")).toInt() !=
                static_cast<int>(expected.first_page) ||
            rendered.value(QStringLiteral("output_path")).toString() != output_path ||
            rendered.value(QStringLiteral("title")).toString() != title ||
            rendered.value(QStringLiteral("page_count")).toInt() !=
                static_cast<int>(expected.page_count) ||
            rendered.value(QStringLiteral("byte_size")).toInteger() !=
                static_cast<qint64>(expected.byte_size) ||
            rendered.value(QStringLiteral("source_sha256")).toString() !=
                QString::fromLatin1(expected.source_sha256) ||
            rendered.value(QStringLiteral("pdf_sha256")).toString() !=
                QString::fromLatin1(expected.pdf_sha256) ||
            rendered.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
            rendered.value(QStringLiteral("renderer_contract")).toString() !=
                QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
            assembly.value(QStringLiteral("assembly_contract")).toString() !=
                QStringLiteral("appellate.markdown-assembly.v1") ||
            assembly.value(QStringLiteral("kind")).toString() != QStringLiteral("single_source") ||
            assembly.value(QStringLiteral("source_path")).toString() != source_path ||
            assembly.value(QStringLiteral("source_sha256")).toString() !=
                QString::fromLatin1(expected.source_sha256) ||
            assembly.value(QStringLiteral("logical_page_count")).toInt() !=
                static_cast<int>(expected.page_count) ||
            page_labels.value(QStringLiteral("prefix")).toString() != QStringLiteral("JA") ||
            page_labels.value(QStringLiteral("first_number")).toInt() !=
                static_cast<int>(expected.first_page) ||
            page_labels.value(QStringLiteral("last_number")).toInt() !=
                static_cast<int>(expected.first_page + expected.page_count - 1) ||
            sha256(source_bytes) !=
                QByteArray::fromStdString(std::string(expected.source_sha256)) ||
            sha256(pdf_bytes) != QByteArray::fromStdString(std::string(expected.pdf_sha256)) ||
            pdf_bytes.size() != static_cast<qsizetype>(expected.byte_size) ||
            blob == source->blobs.end() || blob->sha256 != expected.pdf_sha256 ||
            blob->byte_size != expected.byte_size ||
            record_entry.value(QStringLiteral("entry_number")).toInt() !=
                static_cast<int>(index + 1) ||
            record_entry.value(QStringLiteral("asset_path")).toString() != output_path ||
            record_entry.value(QStringLiteral("asset_sha256")).toString() !=
                QString::fromLatin1(expected.pdf_sha256) ||
            record_entry.value(QStringLiteral("page_count")).toInt() !=
                static_cast<int>(expected.page_count) ||
            record_entry.value(QStringLiteral("sealed")).toBool(true)) {
            return fail(
                QStringLiteral("render/source/blob/record pin mismatch: %1").arg(output_path));
        }

        const auto source_text = QString::fromUtf8(source_bytes);
        if (!source_text.startsWith(banner + QLatin1Char('\n')) || source_text.count(banner) != 1) {
            return fail(QStringLiteral("source safety banner mismatch: %1").arg(source_path));
        }
        const auto lower_source = source_text.toLower();
        for (const auto& phrase : forbidden_source_phrases) {
            if (lower_source.contains(phrase.toLower())) {
                return fail(
                    QStringLiteral("temporal/meta source leak in %1: %2").arg(source_path, phrase));
            }
        }
        if (index < 19) {
            for (const auto& phrase : forbidden_batch_1_phrases) {
                if (lower_source.contains(phrase.toLower())) {
                    return fail(
                        QStringLiteral("successor fact leaked into immutable batch 1 %1: %2")
                            .arg(source_path, phrase));
                }
            }
        }
        const auto source_pages =
            source_text.split(QStringLiteral("<!-- PAGE BREAK -->"), Qt::KeepEmptyParts);
        if (source_pages.size() != static_cast<qsizetype>(expected.page_count)) {
            return fail(QStringLiteral("Markdown logical-page mismatch: %1").arg(source_path));
        }
        for (auto page : source_pages) {
            page.remove(banner);
            page = page.simplified();
            if (page.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size() <
                    45 ||
                distinct_source_pages.contains(page)) {
                return fail(QStringLiteral("thin/duplicate substantive Markdown page: %1")
                                .arg(source_path));
            }
            distinct_source_pages.insert(page);
        }

        QPdfDocument pdf;
        if (pdf.load(QDir(pack_root).filePath(output_path)) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready ||
            pdf.pageCount() != static_cast<int>(expected.page_count) ||
            pdf.metaData(QPdfDocument::MetaDataField::Title).toString() != title ||
            pdf.metaData(QPdfDocument::MetaDataField::Author).toString() !=
                QStringLiteral("Appellate Workbench synthetic content") ||
            pdf.metaData(QPdfDocument::MetaDataField::Creator).toString() !=
                QStringLiteral("Appellate Workbench Markdown PDF Renderer")) {
            return fail(QStringLiteral("PDF metadata/page-count mismatch: %1").arg(output_path));
        }
        for (int page_index = 0; page_index < pdf.pageCount(); ++page_index) {
            const auto expected_label = QStringLiteral("JA%1").arg(expected_ja);
            auto page_text = pdf.getAllText(page_index).text().simplified();
            if (page_text.size() < 250 || !page_text.contains(expected_label)) {
                return fail(QStringLiteral("thin/unlabeled searchable page %1 in %2")
                                .arg(expected_label, output_path));
            }
            for (const auto& claim : premature_appellate_claims) {
                if (page_text.contains(claim, Qt::CaseInsensitive)) {
                    return fail(QStringLiteral("appellate outcome leaked into rendered %1: %2")
                                    .arg(expected_label, claim));
                }
            }
            for (const auto& phrase : forbidden_source_phrases) {
                if (page_text.contains(phrase, Qt::CaseInsensitive)) {
                    return fail(QStringLiteral("temporal/meta leak in rendered %1: %2")
                                    .arg(expected_label, phrase));
                }
            }
            if (index < 19) {
                for (const auto& phrase : forbidden_batch_1_phrases) {
                    if (page_text.contains(phrase, Qt::CaseInsensitive)) {
                        return fail(
                            QStringLiteral("successor fact leaked into immutable PDF %1: %2")
                                .arg(expected_label, phrase));
                    }
                }
            }
            page_text.remove(any_page_label);
            page_text = page_text.simplified();
            if (distinct_pdf_pages.contains(page_text)) {
                return fail(QStringLiteral("duplicate substantive PDF page body at %1")
                                .arg(expected_label));
            }
            distinct_pdf_pages.insert(page_text);

            const auto anchor = anchors.at(expected_ja - 1).toObject();
            if (anchor.value(QStringLiteral("anchor_id")).toString() !=
                    QStringLiteral("ca4m4.benton.anchor.ja%1").arg(expected_ja) ||
                anchor.value(QStringLiteral("entry_id")).toString() !=
                    record_entry.value(QStringLiteral("entry_id")).toString() ||
                anchor.value(QStringLiteral("page_number")).toInt() != page_index + 1 ||
                anchor.value(QStringLiteral("citation_label")).toString() != expected_label) {
                return fail(QStringLiteral("record anchor mismatch at %1").arg(expected_label));
            }
            ++expected_ja;
        }
    }
    if (expected_ja != 263 || distinct_source_pages.size() != 262 ||
        distinct_pdf_pages.size() != 262) {
        return fail(QStringLiteral("JA/source/PDF substantive-page closure mismatch"));
    }

    const QJsonArray expected_seats{
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.benton.seat.rowan")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.rowan")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.benton.seat.alder")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.alder")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
        QJsonObject{{QStringLiteral("seat_id"), QStringLiteral("ca4m4.benton.seat.fen")},
                    {QStringLiteral("profile_id"), QStringLiteral("us.ca4.bench-profile.fen")},
                    {QStringLiteral("court_role"), QStringLiteral("appellate")}},
    };
    if (bench_resource->document.value(QStringLiteral("presiding_seat_id")).toString() !=
            QStringLiteral("ca4m4.benton.seat.rowan") ||
        bench_resource->document.value(QStringLiteral("seats")).toArray() != expected_seats) {
        return fail(QStringLiteral("Rowan/Alder/Fen bench contract mismatch"));
    }

    const auto routes =
        workflow_resource->document.value(QStringLiteral("filing_routes")).toArray();
    const auto operations =
        workflow_resource->document.value(QStringLiteral("operations")).toArray();
    const QJsonArray expected_stages{
        QStringLiteral("ca4m4.benton.stage.opened"),
        QStringLiteral("ca4m4.benton.stage.briefing"),
        QStringLiteral("ca4m4.benton.stage.submitted"),
        QStringLiteral("ca4m4.benton.stage.post-judgment"),
    };
    const auto notice_route = objectById(routes, QStringLiteral("filing_type_id"),
                                         QStringLiteral("us.ca4.filing.civil-notice-of-appeal"));
    const auto brief_route = objectById(routes, QStringLiteral("filing_type_id"),
                                        QStringLiteral("us.ca4.filing.principal-brief"));
    const auto rehearing_route = objectById(routes, QStringLiteral("filing_type_id"),
                                            QStringLiteral("us.ca4.filing.rehearing-petition"));
    const QJsonArray both_party_roles{
        QStringLiteral("us.ca4.role.initiating-party"),
        QStringLiteral("us.ca4.role.responding-party"),
    };
    if (workflow_resource->document.value(QStringLiteral("initial_stage_id")).toString() !=
            QStringLiteral("ca4m4.benton.stage.opened") ||
        workflow_resource->document.value(QStringLiteral("stages")).toArray() != expected_stages ||
        routes.size() != 3 || operations.size() != 28 || notice_route.isEmpty() ||
        brief_route.isEmpty() || rehearing_route.isEmpty() ||
        notice_route.contains(QStringLiteral("accepted_deadline")) ||
        notice_route.contains(QStringLiteral("advance_operation_id")) ||
        rehearing_route.contains(QStringLiteral("accepted_deadline")) ||
        rehearing_route.value(QStringLiteral("satisfies_deadline_id")).toString() !=
            QStringLiteral("ca4m4.benton.deadline.rehearing") ||
        !rehearing_route.value(QStringLiteral("reject_after_deadline")).toBool() ||
        brief_route.value(QStringLiteral("required_service_role_ids")).toArray() !=
            both_party_roles ||
        rehearing_route.value(QStringLiteral("required_service_role_ids")).toArray() !=
            both_party_roles) {
        return fail(QStringLiteral("filing-route/stage contract mismatch"));
    }

    const auto operation = [&](const char* id) {
        return objectById(operations, QStringLiteral("operation_id"), QString::fromLatin1(id));
    };
    const auto preconditionText = [&](const char* id) {
        return QString::fromUtf8(
            QJsonDocument(operation(id).value(QStringLiteral("preconditions")).toArray())
                .toJson(QJsonDocument::Compact));
    };
    const QJsonArray court_role{QStringLiteral("us.ca4.role.court")};
    const auto record_complete = operation("ca4m4.benton.operation.enter-record-complete");
    const auto advance_briefing = operation("ca4m4.benton.operation.advance-briefing");
    const auto briefing_complete = operation("ca4m4.benton.operation.enter-briefing-complete");
    const auto schedule_argument = operation("ca4m4.benton.operation.schedule-argument");
    const auto argument_held = operation("ca4m4.benton.operation.enter-argument-held");
    const auto advance_after_argument =
        operation("ca4m4.benton.operation.advance-submitted-after-argument");
    const auto advance_on_briefs = operation("ca4m4.benton.operation.advance-submitted-on-briefs");
    const auto judgment = operation("ca4m4.benton.operation.issue-judgment");
    const auto judgment_on_briefs = operation("ca4m4.benton.operation.issue-judgment-on-briefs");
    const auto calculate_rehearing = operation("ca4m4.benton.operation.calculate-rehearing");
    const auto calculate_mandate_time =
        operation("ca4m4.benton.operation.calculate-mandate-after-rehearing-time");
    const auto calculate_mandate_denial =
        operation("ca4m4.benton.operation.calculate-mandate-after-rehearing-denial");
    const auto issue_no_petition = operation("ca4m4.benton.operation.issue-mandate-no-petition");
    const auto issue_after_denial =
        operation("ca4m4.benton.operation.issue-mandate-after-rehearing-denial");
    const auto issue_shortened = operation("ca4m4.benton.operation.issue-mandate-shortened");
    const QJsonObject judgment_event_base{
        {QStringLiteral("kind"), QStringLiteral("judgment_occurred")}};
    const QJsonObject denial_event_base{
        {QStringLiteral("kind"), QStringLiteral("order_occurred")},
        {QStringLiteral("order_id"), QStringLiteral("ca4m4.benton.order.rehearing-disposition")},
        {QStringLiteral("operation_id"),
         QStringLiteral("ca4m4.benton.operation.enter-rehearing-disposition")}};
    if (record_complete.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        advance_briefing.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        briefing_complete.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        schedule_argument.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        argument_held.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        !operation("ca4m4.benton.operation.enter-submitted-on-briefs").isEmpty() ||
        advance_after_argument.value(QStringLiteral("authorized_role_ids")).toArray() !=
            court_role ||
        advance_on_briefs.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        judgment.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        judgment_on_briefs.value(QStringLiteral("authorized_role_ids")).toArray() != court_role ||
        calculate_rehearing.value(QStringLiteral("deadline_days")).toInt() != 14 ||
        calculate_rehearing.value(QStringLiteral("produced_deadline_id")).toString() !=
            QStringLiteral("ca4m4.benton.deadline.rehearing") ||
        calculate_rehearing.value(QStringLiteral("deadline_event_base")).toObject() !=
            judgment_event_base ||
        calculate_mandate_time.value(QStringLiteral("deadline_days")).toInt() != 7 ||
        calculate_mandate_time.value(QStringLiteral("deadline_base_id")).toString() !=
            QStringLiteral("ca4m4.benton.deadline.rehearing") ||
        calculate_mandate_time.value(QStringLiteral("produced_deadline_id")).toString() !=
            QStringLiteral("ca4m4.benton.deadline.mandate-no-petition") ||
        calculate_mandate_denial.value(QStringLiteral("deadline_days")).toInt() != 7 ||
        calculate_mandate_denial.value(QStringLiteral("produced_deadline_id")).toString() !=
            QStringLiteral("ca4m4.benton.deadline.mandate-after-rehearing-denial") ||
        calculate_mandate_denial.value(QStringLiteral("deadline_event_base")).toObject() !=
            denial_event_base ||
        !preconditionText("ca4m4.benton.operation.enter-record-complete")
             .contains(QStringLiteral("us.ca4.filing.civil-notice-of-appeal")) ||
        !preconditionText("ca4m4.benton.operation.advance-briefing")
             .contains(QStringLiteral("ca4m4.benton.order.record-complete")) ||
        !preconditionText("ca4m4.benton.operation.enter-briefing-complete")
             .contains(QStringLiteral("us.ca4.filing.principal-brief")) ||
        !preconditionText("ca4m4.benton.operation.schedule-argument")
             .contains(QStringLiteral("ca4m4.benton.order.briefing-complete")) ||
        !preconditionText("ca4m4.benton.operation.enter-argument-held")
             .contains(QStringLiteral("argument_scheduled")) ||
        !preconditionText("ca4m4.benton.operation.enter-argument-held")
             .contains(QStringLiteral("argument_date_status")) ||
        !preconditionText("ca4m4.benton.operation.advance-submitted-on-briefs")
             .contains(QStringLiteral("ca4m4.benton.order.briefing-complete")) ||
        !preconditionText("ca4m4.benton.operation.advance-submitted-on-briefs")
             .contains(QStringLiteral("\"scheduled\":false")) ||
        !preconditionText("ca4m4.benton.operation.issue-judgment")
             .contains(QStringLiteral("ca4m4.benton.order.argument-held")) ||
        !preconditionText("ca4m4.benton.operation.issue-judgment-on-briefs")
             .contains(QStringLiteral("\"scheduled\":false")) ||
        !preconditionText("ca4m4.benton.operation.calculate-rehearing")
             .contains(QStringLiteral("judgment_issued")) ||
        !preconditionText("ca4m4.benton.operation.calculate-mandate-after-rehearing-time")
             .contains(QStringLiteral("ca4m4.benton.deadline.rehearing")) ||
        !preconditionText("ca4m4.benton.operation.calculate-mandate-after-rehearing-time")
             .contains(QStringLiteral("\"status\":\"reached\"")) ||
        !preconditionText("ca4m4.benton.operation.calculate-mandate-after-rehearing-time")
             .contains(QStringLiteral("\"present\":false")) ||
        !preconditionText("ca4m4.benton.operation.calculate-mandate-after-rehearing-denial")
             .contains(QStringLiteral("ca4m4.benton.order.rehearing-disposition")) ||
        !preconditionText("ca4m4.benton.operation.calculate-mandate-after-rehearing-denial")
             .contains(QStringLiteral("\"present\":true")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-no-petition")
             .contains(QStringLiteral("ca4m4.benton.deadline.mandate-no-petition")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-no-petition")
             .contains(QStringLiteral("\"status\":\"reached\"")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-no-petition")
             .contains(QStringLiteral("\"present\":false")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-no-petition")
             .contains(QStringLiteral("ca4m4.benton.order.mandate-release")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-after-rehearing-denial")
             .contains(QStringLiteral("ca4m4.benton.deadline.mandate-after-rehearing-denial")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-after-rehearing-denial")
             .contains(QStringLiteral("ca4m4.benton.order.rehearing-disposition")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-after-rehearing-denial")
             .contains(QStringLiteral("\"present\":true")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-shortened")
             .contains(QStringLiteral("ca4m4.benton.order.mandate-shortening")) ||
        !preconditionText("ca4m4.benton.operation.issue-mandate-shortened")
             .contains(QStringLiteral("ca4m4.benton.order.mandate-release")) ||
        issue_no_petition.isEmpty() || issue_after_denial.isEmpty() || issue_shortened.isEmpty()) {
        return fail(QStringLiteral("workflow order/branch/deadline guard contract mismatch"));
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return fail(QStringLiteral("cannot create temporary validation directory"));
    }
    const auto archive_a = QDir(temporary.path()).filePath(QStringLiteral("benton-a.awpack"));
    const auto archive_b = QDir(temporary.path()).filePath(QStringLiteral("benton-b.awpack"));
    const auto exported_a = PackArchive::exportDirectory(pack_root, archive_a, {},
                                                         PackValidationScope::ResolvedClosure);
    const auto exported_b = PackArchive::exportDirectory(pack_root, archive_b, {},
                                                         PackValidationScope::ResolvedClosure);
    if (!exported_a || !exported_b || *exported_a != expected_root ||
        *exported_b != expected_root || readAll(archive_a).isEmpty() ||
        readAll(archive_a) != readAll(archive_b) ||
        sha256(readAll(archive_a)) != QByteArray::fromStdString(archive_digest)) {
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
        resolved->resourceOwner("us.federal.authorities.appellate-rules") !=
            std::optional<PackRevision>{expected_federal} ||
        resolved->resourceOwner("us.ca4.bench-profile.rowan") !=
            std::optional<PackRevision>{expected_bench} ||
        resolved->resourceOwner("us.ca4.bench-profile.alder") !=
            std::optional<PackRevision>{expected_bench} ||
        resolved->resourceOwner("us.ca4.bench-profile.fen") !=
            std::optional<PackRevision>{expected_bench} ||
        resolved->resourceOwner("ca4m4.benton.argument.actual-record") !=
            std::optional<PackRevision>{expected_root} ||
        resolved->resourceOwner("ca4m4.benton.argument.no-knowledge-counterfactual") !=
            std::optional<PackRevision>{expected_root} ||
        resolved->resourceOwner("ca4m4.benton.record") !=
            std::optional<PackRevision>{expected_root} ||
        resolved->resourceOwner("ca4m4.benton.workflow.civil-appeal") !=
            std::optional<PackRevision>{expected_root}) {
        return fail(QStringLiteral("resolved graph owner/pin contract mismatch"));
    }

    const auto runtime = packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != expected_root || runtime->cases.size() != std::size_t{1} ||
        runtime->cases.front().argument_configurations.size() != std::size_t{2} ||
        std::ranges::any_of(
            runtime->cases.front().argument_configurations, [](const auto& configuration) {
                return !configuration.grounded_question_bank.has_value() ||
                       configuration.permitted_issue_ids.size() != std::size_t{2} ||
                       configuration.grounded_question_bank->issue_topics.size() !=
                           std::size_t{2} ||
                       configuration.grounded_question_bank->questions.size() != std::size_t{12};
            })) {
        return fail(QStringLiteral("resolved Benton closure is not runtime-loadable"));
    }
    const auto& runtime_case = runtime->cases.front();
    const QSet<QString> bank_topics{
        QStringLiteral("workbench.topic.standard-of-review"),
        QStringLiteral("workbench.topic.preservation"),
        QStringLiteral("workbench.topic.record-support"),
        QStringLiteral("workbench.topic.governing-authority"),
        QStringLiteral("workbench.topic.merits"),
        QStringLiteral("workbench.topic.remedy"),
        QStringLiteral("workbench.topic.practical-consequences"),
    };
    for (const auto& configuration : runtime_case.argument_configurations) {
        if (configuration.bench.seats.size() != std::size_t{3}) {
            return fail(QStringLiteral("runtime bench does not have three seats"));
        }
        for (const auto& seat : configuration.bench.seats) {
            const auto intersects =
                std::ranges::any_of(seat.profile.interaction.issue_focus, [&](const auto& focus) {
                    return bank_topics.contains(QString::fromStdString(focus.topic_id));
                });
            if (!intersects ||
                seat.profile.profile_class != model::ProfileClass::FictionalComposite) {
                return fail(QStringLiteral("runtime bench focus/synthetic contract mismatch"));
            }
        }
    }

    const auto runCommand = [&](Run& run,
                                model::WorkflowCommand command) -> std::optional<QString> {
        const auto result = execute(runtime_case, run, std::move(command));
        if (!result) {
            return QString::fromStdString(result.error());
        }
        return std::nullopt;
    };
    const auto mustRun = [&](Run& run, model::WorkflowCommand command,
                             const QString& context) -> std::optional<QString> {
        const auto error = runCommand(run, std::move(command));
        if (error.has_value()) {
            return context + QStringLiteral(": ") + *error;
        }
        return std::nullopt;
    };
    const auto deadline = [](const Run& run, std::string_view id) {
        return std::ranges::find(run.state.deadlines, id, [](const auto& item) {
            return std::string_view(item.deadline_id.value);
        });
    };

    const std::string argued_session = "ca4m4.benton.session.argued-positive";
    auto argued = emptyRun(runtime_case, argued_session);
    if (const auto error = mustRun(argued,
                                   notice(argued_session, "ca4m4.benton.command.notice",
                                          "ca4m4.benton.filing.notice", date(2026, 1U, 16U)),
                                   QStringLiteral("accept notice"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::AdvanceWorkflowStage{
                     header(argued_session, "ca4m4.benton.command.early-briefing", clerk_actor,
                            date(2026, 1U, 16U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.advance-briefing"}}})) {
        return fail(QStringLiteral("briefing can begin before the record-complete order"));
    }
    if (const auto error =
            mustRun(argued,
                    model::CalculateWorkflowDeadline{
                        header(argued_session, "ca4m4.benton.command.calculate-docketing",
                               clerk_actor, date(2026, 1U, 20U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.calculate-docketing"},
                        model::WorkflowDeadlineId{"ca4m4.benton.deadline.docketing-statement"}},
                    QStringLiteral("calculate docketing deadline"));
        error.has_value()) {
        return fail(*error);
    }
    const auto docketing = deadline(argued, "ca4m4.benton.deadline.docketing-statement");
    if (docketing == argued.state.deadlines.end() || docketing->due_date != date(2026, 2U, 3U)) {
        return fail(QStringLiteral("docketing deadline does not run 14 days from court event"));
    }
    if (const auto error =
            mustRun(argued,
                    order(argued_session, "ca4m4.benton.command.record-complete", clerk_actor,
                          "ca4m4.benton.operation.enter-record-complete",
                          "ca4m4.benton.order.record-complete",
                          model::WorkflowOrderDisposition::Other, date(2026, 2U, 20U), 'c'),
                    QStringLiteral("enter record-complete order"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    model::AdvanceWorkflowStage{
                        header(argued_session, "ca4m4.benton.command.advance-briefing", clerk_actor,
                               date(2026, 2U, 20U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.advance-briefing"}},
                    QStringLiteral("advance to briefing"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    principalBrief(argued_session, "ca4m4.benton.command.opening-brief",
                                   "ca4m4.benton.filing.opening-brief", appellant_actor,
                                   appellee_actor, date(2026, 3U, 16U)),
                    QStringLiteral("accept opening brief"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    principalBrief(argued_session, "ca4m4.benton.command.response-brief",
                                   "ca4m4.benton.filing.response-brief", appellee_actor,
                                   appellant_actor, date(2026, 4U, 16U)),
                    QStringLiteral("accept response brief"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::ScheduleWorkflowArgument{
                     header(argued_session, "ca4m4.benton.command.early-schedule", clerk_actor,
                            date(2026, 4U, 16U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.schedule-argument"},
                     date(2026, 5U, 20U)}})) {
        return fail(QStringLiteral("argument can be scheduled before briefing-complete order"));
    }
    if (const auto error =
            mustRun(argued,
                    order(argued_session, "ca4m4.benton.command.briefing-complete", clerk_actor,
                          "ca4m4.benton.operation.enter-briefing-complete",
                          "ca4m4.benton.order.briefing-complete",
                          model::WorkflowOrderDisposition::Other, date(2026, 4U, 17U), 'd'),
                    QStringLiteral("enter briefing-complete order"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    model::ScheduleWorkflowArgument{
                        header(argued_session, "ca4m4.benton.command.schedule-argument",
                               clerk_actor, date(2026, 4U, 20U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.schedule-argument"},
                        date(2026, 5U, 20U)},
                    QStringLiteral("schedule argument"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::AdvanceWorkflowStage{
                     header(argued_session, "ca4m4.benton.command.early-submit", clerk_actor,
                            date(2026, 4U, 20U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.advance-submitted-after-argument"}}})) {
        return fail(QStringLiteral("scheduled argument bypasses argument-held order"));
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::AdvanceWorkflowStage{
                     header(argued_session, "ca4m4.benton.command.wrong-on-briefs-advance",
                            clerk_actor, date(2026, 4U, 20U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.advance-submitted-on-briefs"}}})) {
        return fail(QStringLiteral("scheduled argument permits submitted-on-briefs advance"));
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{
                     order(argued_session, "ca4m4.benton.command.day-before-argument-held",
                           panel_actor, "ca4m4.benton.operation.enter-argument-held",
                           "ca4m4.benton.order.argument-held",
                           model::WorkflowOrderDisposition::Other, date(2026, 5U, 19U), 'e')})) {
        return fail(QStringLiteral("argument-held marker is accepted before the scheduled date"));
    }
    if (const auto error = mustRun(
            argued,
            order(argued_session, "ca4m4.benton.command.argument-held", panel_actor,
                  "ca4m4.benton.operation.enter-argument-held", "ca4m4.benton.order.argument-held",
                  model::WorkflowOrderDisposition::Other, date(2026, 5U, 20U), 'e'),
            QStringLiteral("enter argument-held order"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    model::AdvanceWorkflowStage{
                        header(argued_session, "ca4m4.benton.command.advance-submitted",
                               clerk_actor, date(2026, 5U, 20U)),
                        model::WorkflowOperationId{
                            "ca4m4.benton.operation.advance-submitted-after-argument"}},
                    QStringLiteral("advance argued case to submission"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::IssueWorkflowJudgment{
                     header(argued_session, "ca4m4.benton.command.wrong-branch-judgment",
                            panel_actor, date(2026, 6U, 15U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-judgment-on-briefs"},
                     std::string(64, 'a'), std::string("wrong submission branch")}})) {
        return fail(QStringLiteral("argument-held case accepts on-briefs judgment operation"));
    }
    if (const auto error =
            mustRun(argued,
                    model::IssueWorkflowJudgment{
                        header(argued_session, "ca4m4.benton.command.issue-judgment", panel_actor,
                               date(2026, 6U, 15U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.issue-judgment"},
                        std::string(64, 'f'), std::string("Fictional exercise judgment")},
                    QStringLiteral("issue guarded judgment"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    model::AdvanceWorkflowStage{
                        header(argued_session, "ca4m4.benton.command.advance-post-judgment",
                               clerk_actor, date(2026, 6U, 15U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.advance-post-judgment"}},
                    QStringLiteral("advance to post-judgment"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error =
            mustRun(argued,
                    model::CalculateWorkflowDeadline{
                        header(argued_session, "ca4m4.benton.command.calculate-rehearing",
                               clerk_actor, date(2026, 6U, 20U)),
                        model::WorkflowOperationId{"ca4m4.benton.operation.calculate-rehearing"},
                        model::WorkflowDeadlineId{"ca4m4.benton.deadline.rehearing"}},
                    QStringLiteral("calculate rehearing deadline"));
        error.has_value()) {
        return fail(*error);
    }
    const auto rehearing = deadline(argued, "ca4m4.benton.deadline.rehearing");
    if (rehearing == argued.state.deadlines.end() || rehearing->due_date != date(2026, 6U, 29U)) {
        return fail(QStringLiteral("rehearing deadline does not run 14 days from judgment"));
    }
    auto post_judgment = argued;
    const auto post_judgment_state = post_judgment.state;
    const auto post_judgment_journal = post_judgment.journal;
    if (!isUnmet(runtime_case, post_judgment,
                 model::WorkflowCommand{
                     order(argued_session, "ca4m4.benton.command.denial-without-petition",
                           panel_actor, "ca4m4.benton.operation.enter-rehearing-disposition",
                           "ca4m4.benton.order.rehearing-disposition",
                           model::WorkflowOrderDisposition::Denied, date(2026, 6U, 21U), 'a')}) ||
        post_judgment.state != post_judgment_state ||
        post_judgment.journal != post_judgment_journal) {
        return fail(QStringLiteral("rehearing denial order is not blocked without a petition"));
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::CalculateWorkflowDeadline{
                     header(argued_session, "ca4m4.benton.command.early-mandate-clock", clerk_actor,
                            date(2026, 6U, 28U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.calculate-mandate-after-rehearing-time"},
                     model::WorkflowDeadlineId{"ca4m4.benton.deadline.mandate-no-petition"}}})) {
        return fail(QStringLiteral("mandate clock starts before rehearing boundary"));
    }

    auto boundary_petition = post_judgment;
    if (const auto error =
            mustRun(boundary_petition,
                    rehearingPetition(argued_session, "ca4m4.benton.command.boundary-petition",
                                      "ca4m4.benton.filing.boundary-petition", date(2026, 6U, 29U)),
                    QStringLiteral("accept rehearing petition on exact due date"));
        error.has_value()) {
        return fail(*error);
    }
    const auto boundary_rehearing = deadline(boundary_petition, "ca4m4.benton.deadline.rehearing");
    if (boundary_rehearing == boundary_petition.state.deadlines.end() ||
        boundary_rehearing->status != model::WorkflowDeadlineStatus::Satisfied) {
        return fail(
            QStringLiteral("exact-date rehearing petition does not satisfy named deadline"));
    }

    auto late_petition = post_judgment;
    if (const auto error =
            mustRun(late_petition,
                    rehearingPetition(argued_session, "ca4m4.benton.command.late-petition",
                                      "ca4m4.benton.filing.late-petition", date(2026, 6U, 30U)),
                    QStringLiteral("record late rehearing rejection"));
        error.has_value()) {
        return fail(*error);
    }
    const auto* late_rejection =
        late_petition.trace.empty()
            ? nullptr
            : std::get_if<model::WorkflowFilingRejected>(&late_petition.trace.back());
    if (late_rejection == nullptr ||
        late_rejection->reason != model::WorkflowFilingRejectionReason::DeadlineExpired ||
        late_petition.state.accepted_filings != post_judgment.state.accepted_filings) {
        return fail(QStringLiteral("day-late rehearing petition is not rejected as expired"));
    }

    auto no_petition = post_judgment;
    if (const auto error =
            mustRun(no_petition,
                    model::CalculateWorkflowDeadline{
                        header(argued_session, "ca4m4.benton.command.boundary-mandate-clock",
                               clerk_actor, date(2026, 6U, 29U)),
                        model::WorkflowOperationId{
                            "ca4m4.benton.operation.calculate-mandate-after-rehearing-time"},
                        model::WorkflowDeadlineId{"ca4m4.benton.deadline.mandate-no-petition"}},
                    QStringLiteral("calculate mandate at rehearing boundary"));
        error.has_value()) {
        return fail(*error);
    }
    const auto no_petition_mandate =
        deadline(no_petition, "ca4m4.benton.deadline.mandate-no-petition");
    if (no_petition_mandate == no_petition.state.deadlines.end() ||
        no_petition_mandate->due_date != date(2026, 7U, 6U)) {
        return fail(QStringLiteral("dependent mandate clock is not seven literal days from "
                                   "the rehearing due date"));
    }
    if (const auto error =
            mustRun(no_petition,
                    order(argued_session, "ca4m4.benton.command.no-petition-release", panel_actor,
                          "ca4m4.benton.operation.enter-mandate-release",
                          "ca4m4.benton.order.mandate-release",
                          model::WorkflowOrderDisposition::Granted, date(2026, 7U, 5U), 'c'),
                    QStringLiteral("release no-petition mandate"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, no_petition,
                 model::WorkflowCommand{model::IssueWorkflowMandate{
                     header(argued_session, "ca4m4.benton.command.day-before-mandate", clerk_actor,
                            date(2026, 7U, 5U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-no-petition"},
                     std::string(64, 'd')}})) {
        return fail(QStringLiteral("ordinary mandate issues one day before its reached boundary"));
    }
    if (const auto error = mustRun(
            no_petition,
            model::IssueWorkflowMandate{
                header(argued_session, "ca4m4.benton.command.boundary-mandate", clerk_actor,
                       date(2026, 7U, 6U)),
                model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-no-petition"},
                std::string(64, 'e')},
            QStringLiteral("issue no-petition mandate on reached boundary"));
        error.has_value()) {
        return fail(*error);
    }
    if (!no_petition.state.mandate_sha256.has_value() ||
        no_petition.journal.size() != std::size_t{16}) {
        return fail(QStringLiteral("no-petition dependent-deadline trace did not close"));
    }

    auto shortened = post_judgment;
    if (!isUnmet(runtime_case, shortened,
                 model::WorkflowCommand{model::IssueWorkflowMandate{
                     header(argued_session, "ca4m4.benton.command.shortened-without-orders",
                            clerk_actor, date(2026, 6U, 21U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-shortened"},
                     std::string(64, 'a')}})) {
        return fail(QStringLiteral("shortened mandate issues without shortening/release orders"));
    }
    if (const auto error =
            mustRun(shortened,
                    order(argued_session, "ca4m4.benton.command.shorten-mandate", panel_actor,
                          "ca4m4.benton.operation.enter-mandate-shortening",
                          "ca4m4.benton.order.mandate-shortening",
                          model::WorkflowOrderDisposition::Granted, date(2026, 6U, 21U), 'a'),
                    QStringLiteral("enter mandate-shortening order"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, shortened,
                 model::WorkflowCommand{model::IssueWorkflowMandate{
                     header(argued_session, "ca4m4.benton.command.shortened-without-release",
                            clerk_actor, date(2026, 6U, 21U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-shortened"},
                     std::string(64, 'b')}})) {
        return fail(QStringLiteral("shortened mandate issues without release order"));
    }
    if (const auto error =
            mustRun(shortened,
                    order(argued_session, "ca4m4.benton.command.release-shortened-mandate",
                          panel_actor, "ca4m4.benton.operation.enter-mandate-release",
                          "ca4m4.benton.order.mandate-release",
                          model::WorkflowOrderDisposition::Granted, date(2026, 6U, 21U), 'b'),
                    QStringLiteral("release shortened mandate"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = mustRun(
            shortened,
            model::IssueWorkflowMandate{
                header(argued_session, "ca4m4.benton.command.issue-shortened-mandate", clerk_actor,
                       date(2026, 6U, 21U)),
                model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-shortened"},
                std::string(64, 'c')},
            QStringLiteral("issue shortened mandate"));
        error.has_value()) {
        return fail(*error);
    }
    if (!shortened.state.mandate_sha256.has_value() ||
        shortened.journal.size() != std::size_t{16}) {
        return fail(QStringLiteral("shortened-mandate positive branch did not close"));
    }

    if (const auto error = mustRun(
            argued,
            rehearingPetition(argued_session, "ca4m4.benton.command.rehearing-petition",
                              "ca4m4.benton.filing.rehearing-petition", date(2026, 6U, 25U)),
            QStringLiteral("accept timely rehearing petition"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::CalculateWorkflowDeadline{
                     header(argued_session, "ca4m4.benton.command.petition-blocks-no-petition",
                            clerk_actor, date(2026, 6U, 29U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.calculate-mandate-after-rehearing-time"},
                     model::WorkflowDeadlineId{"ca4m4.benton.deadline.mandate-no-petition"}}})) {
        return fail(QStringLiteral("timely petition does not block no-petition mandate clock"));
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::CalculateWorkflowDeadline{
                     header(argued_session, "ca4m4.benton.command.denial-clock-before-order",
                            clerk_actor, date(2026, 6U, 29U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.calculate-mandate-after-rehearing-denial"},
                     model::WorkflowDeadlineId{
                         "ca4m4.benton.deadline.mandate-after-rehearing-denial"}}})) {
        return fail(QStringLiteral("denial mandate clock starts before denial order"));
    }
    if (const auto error =
            mustRun(argued,
                    order(argued_session, "ca4m4.benton.command.deny-rehearing", panel_actor,
                          "ca4m4.benton.operation.enter-rehearing-disposition",
                          "ca4m4.benton.order.rehearing-disposition",
                          model::WorkflowOrderDisposition::Denied, date(2026, 7U, 1U), 'b'),
                    QStringLiteral("enter rehearing denial"));
        error.has_value()) {
        return fail(*error);
    }
    if (const auto error = mustRun(
            argued,
            model::CalculateWorkflowDeadline{
                header(argued_session, "ca4m4.benton.command.calculate-mandate-denial", clerk_actor,
                       date(2026, 7U, 3U)),
                model::WorkflowOperationId{
                    "ca4m4.benton.operation.calculate-mandate-after-rehearing-denial"},
                model::WorkflowDeadlineId{"ca4m4.benton.deadline.mandate-after-rehearing-denial"}},
            QStringLiteral("calculate mandate after rehearing denial"));
        error.has_value()) {
        return fail(*error);
    }
    const auto mandate = deadline(argued, "ca4m4.benton.deadline.mandate-after-rehearing-denial");
    if (mandate == argued.state.deadlines.end() || mandate->due_date != date(2026, 7U, 8U)) {
        return fail(QStringLiteral("denial-triggered mandate deadline is not seven days"));
    }
    if (const auto error = mustRun(
            argued,
            order(argued_session, "ca4m4.benton.command.stay-mandate", panel_actor,
                  "ca4m4.benton.operation.enter-mandate-stay", "ca4m4.benton.order.mandate-stay",
                  model::WorkflowOrderDisposition::Granted, date(2026, 7U, 4U), 'c'),
            QStringLiteral("enter mandate stay"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::IssueWorkflowMandate{
                     header(argued_session, "ca4m4.benton.command.mandate-without-release",
                            clerk_actor, date(2026, 7U, 8U)),
                     model::WorkflowOperationId{
                         "ca4m4.benton.operation.issue-mandate-after-rehearing-denial"},
                     std::string(64, 'd')}})) {
        return fail(QStringLiteral("mandate issues while stay has no release order"));
    }
    if (const auto error =
            mustRun(argued,
                    order(argued_session, "ca4m4.benton.command.release-mandate", panel_actor,
                          "ca4m4.benton.operation.enter-mandate-release",
                          "ca4m4.benton.order.mandate-release",
                          model::WorkflowOrderDisposition::Granted, date(2026, 7U, 8U), 'd'),
                    QStringLiteral("enter mandate release"));
        error.has_value()) {
        return fail(*error);
    }
    if (!isUnmet(runtime_case, argued,
                 model::WorkflowCommand{model::IssueWorkflowMandate{
                     header(argued_session, "ca4m4.benton.command.wrong-no-petition-issue",
                            clerk_actor, date(2026, 7U, 8U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-mandate-no-petition"},
                     std::string(64, 'e')}})) {
        return fail(QStringLiteral("denial branch accepts no-petition mandate operation"));
    }
    if (const auto error =
            mustRun(argued,
                    model::IssueWorkflowMandate{
                        header(argued_session, "ca4m4.benton.command.issue-mandate", clerk_actor,
                               date(2026, 7U, 8U)),
                        model::WorkflowOperationId{
                            "ca4m4.benton.operation.issue-mandate-after-rehearing-denial"},
                        std::string(64, 'e')},
                    QStringLiteral("issue released mandate"));
        error.has_value()) {
        return fail(*error);
    }
    if (!argued.state.mandate_sha256.has_value() || argued.journal.size() != std::size_t{19}) {
        return fail(QStringLiteral("positive argued workflow trace did not close at mandate"));
    }

    const std::string briefs_session = "ca4m4.benton.session.briefs-positive";
    auto on_briefs = emptyRun(runtime_case, briefs_session);
    const std::array<std::pair<model::WorkflowCommand, QString>, 6> briefing_commands{
        std::pair{model::WorkflowCommand{
                      notice(briefs_session, "ca4m4.benton.command.briefs-notice",
                             "ca4m4.benton.filing.briefs-notice", date(2026, 1U, 16U))},
                  QStringLiteral("briefs branch notice")},
        std::pair{model::WorkflowCommand{
                      order(briefs_session, "ca4m4.benton.command.briefs-record", clerk_actor,
                            "ca4m4.benton.operation.enter-record-complete",
                            "ca4m4.benton.order.record-complete",
                            model::WorkflowOrderDisposition::Other, date(2026, 2U, 20U), 'a')},
                  QStringLiteral("briefs branch record order")},
        std::pair{model::WorkflowCommand{model::AdvanceWorkflowStage{
                      header(briefs_session, "ca4m4.benton.command.briefs-advance", clerk_actor,
                             date(2026, 2U, 20U)),
                      model::WorkflowOperationId{"ca4m4.benton.operation.advance-briefing"}}},
                  QStringLiteral("briefs branch advance")},
        std::pair{model::WorkflowCommand{
                      principalBrief(briefs_session, "ca4m4.benton.command.briefs-opening",
                                     "ca4m4.benton.filing.briefs-opening", appellant_actor,
                                     appellee_actor, date(2026, 3U, 16U))},
                  QStringLiteral("briefs branch opening")},
        std::pair{model::WorkflowCommand{
                      principalBrief(briefs_session, "ca4m4.benton.command.briefs-response",
                                     "ca4m4.benton.filing.briefs-response", appellee_actor,
                                     appellant_actor, date(2026, 4U, 16U))},
                  QStringLiteral("briefs branch response")},
        std::pair{model::WorkflowCommand{
                      order(briefs_session, "ca4m4.benton.command.briefs-complete", clerk_actor,
                            "ca4m4.benton.operation.enter-briefing-complete",
                            "ca4m4.benton.order.briefing-complete",
                            model::WorkflowOrderDisposition::Other, date(2026, 4U, 17U), 'b')},
                  QStringLiteral("briefs branch complete")},
    };
    for (const auto& [command, context] : briefing_commands) {
        if (const auto error = mustRun(on_briefs, command, context); error.has_value()) {
            return fail(*error);
        }
    }
    if (const auto error = mustRun(
            on_briefs,
            model::AdvanceWorkflowStage{
                header(briefs_session, "ca4m4.benton.command.advance-on-briefs", clerk_actor,
                       date(2026, 4U, 20U)),
                model::WorkflowOperationId{"ca4m4.benton.operation.advance-submitted-on-briefs"}},
            QStringLiteral("advance submitted-on-briefs branch"));
        error.has_value()) {
        return fail(*error);
    }
    const auto committed_on_briefs_state = on_briefs.state;
    const auto committed_on_briefs_journal = on_briefs.journal;
    if (!isRejectedWith(runtime_case, on_briefs,
                        model::WorkflowCommand{model::ScheduleWorkflowArgument{
                            header(briefs_session, "ca4m4.benton.command.schedule-after-on-briefs",
                                   clerk_actor, date(2026, 4U, 21U)),
                            model::WorkflowOperationId{"ca4m4.benton.operation.schedule-argument"},
                            date(2026, 5U, 20U)}},
                        engine::WorkflowErrorCode::InvalidCommand) ||
        on_briefs.state != committed_on_briefs_state ||
        on_briefs.journal != committed_on_briefs_journal) {
        return fail(QStringLiteral("on-briefs branch permits later argument scheduling"));
    }
    if (!isRejectedWith(
            runtime_case, on_briefs,
            model::WorkflowCommand{order(
                briefs_session, "ca4m4.benton.command.argument-held-after-on-briefs", panel_actor,
                "ca4m4.benton.operation.enter-argument-held", "ca4m4.benton.order.argument-held",
                model::WorkflowOrderDisposition::Other, date(2026, 5U, 20U), 'c')},
            engine::WorkflowErrorCode::InvalidCommand) ||
        on_briefs.state != committed_on_briefs_state ||
        on_briefs.journal != committed_on_briefs_journal) {
        return fail(QStringLiteral("on-briefs branch permits later argument-held marker"));
    }
    if (!isUnmet(runtime_case, on_briefs,
                 model::WorkflowCommand{model::IssueWorkflowJudgment{
                     header(briefs_session, "ca4m4.benton.command.wrong-argued-judgment",
                            panel_actor, date(2026, 5U, 1U)),
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-judgment"},
                     std::string(64, 'd'), std::string("wrong argued branch")}})) {
        return fail(QStringLiteral("submitted-on-briefs case accepts argument-held judgment"));
    }
    if (const auto error = mustRun(
            on_briefs,
            model::IssueWorkflowJudgment{
                header(briefs_session, "ca4m4.benton.command.briefs-judgment", panel_actor,
                       date(2026, 5U, 1U)),
                model::WorkflowOperationId{"ca4m4.benton.operation.issue-judgment-on-briefs"},
                std::string(64, 'e'), std::string("Fictional on-briefs exercise judgment")},
            QStringLiteral("issue on-briefs judgment"));
        error.has_value()) {
        return fail(*error);
    }
    if (!on_briefs.state.judgment_sha256.has_value() ||
        on_briefs.journal.size() != std::size_t{8}) {
        return fail(QStringLiteral("positive submitted-on-briefs trace did not reach judgment"));
    }

    std::cout << "Benton 1.1 integration contract passed: 37 PDFs, 262 unique searchable pages, "
                 "two accepted render inventories, two actual-exclusion grounded banks, guarded "
                 "dual submission paths and mandate, four exact revisions.\n";
    return 0;
}
