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
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#ifndef APPELLATE_M4_BENTON_ROOT
#error "APPELLATE_M4_BENTON_ROOT must name content/m4/benton-retaliation"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

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

constexpr auto root_digest = "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto manifest_digest = "2705a970c34c83317a940e00dc11b51ac8ca205424f3a9a30df405bbcb27717a";
constexpr auto archive_digest = "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05";
constexpr auto archive_byte_size = std::uint64_t{3'408'701};
constexpr auto successor_inventory_digest =
    "c9887d3c15b51cf278d18e1c1c160f48c03b1153181465f493a9fbaf4ebaa972";
constexpr auto case_digest = "21b068c597c15399aaada912e1e653ca1f53f7325561955976e21fa02576ef8b";
constexpr auto record_digest = "191aac8abba8fb7817cf15c3019429e50dc70b0afa66dfd5e2f2df392a94e875";
constexpr auto workflow_digest = "b4f73bcb6b4451a06a1d1fb6b23fd94549d5968de7bb0f9f0a19bbba7a624e36";
constexpr auto realism_review_digest =
    "fd42415873af8558112a21312eaad87c0020da256f17178fa58554009328dff0";
constexpr auto evidence_closure_digest =
    "cf3538ecc449cc3e8a0a05220a1b8a741c636a17c05fec571c1851ea320aea43";
constexpr auto authored_disposition_digest =
    "40fd60e4fe24ddcbecfd61d72a39db361c2aebc82601521082d2e2e0e472b51e";
constexpr auto adverse_disposition_digest =
    "5a9f06407f6e82dc194f9ba74335573c600cb44a2b9dd5215dc84f90379b819b";
constexpr auto realism_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";
constexpr auto actual_bank_digest =
    "161431c279887ac0914029a8912515fa271a9c3a6d1957ab507f3b6facbf6ff6";
constexpr auto counterfactual_bank_digest =
    "ab366be43b263bff2f3951b6c793cbe10543358779820bda83e855cbf2765758";

constexpr auto retaliation_issue = "ca4m4.benton.issue.retaliation-summary-judgment";
constexpr auto exclusion_issue = "ca4m4.benton.issue.late-comparator-declaration-exclusion";
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

constexpr std::array<std::string_view, 66> retaliation_anchors{
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
    "ca4m4.benton.anchor.ja246", "ca4m4.benton.anchor.ja247", "ca4m4.benton.anchor.ja248",
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

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto authoring_root = QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT));
    const auto pack_root = authoring_root.filePath(QStringLiteral("pack"));
    const auto foundations_root = QDir(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    const PackRevision expected_root{PackId{"us.ca4.m4.benton-retaliation"}, "1.2.0", root_digest};
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
    if (sha256(manifest_bytes) != QByteArray(manifest_digest)) {
        return fail(QStringLiteral("frozen manifest digest mismatch"));
    }
    if (source->revision != expected_root ||
        source->graph_state != PackGraphState::DeferredReferences ||
        source->dependencies.size() != std::size_t{3} ||
        source->required_capabilities.size() != std::size_t{14} ||
        source->resources.size() != std::size_t{9} || source->blobs.size() != std::size_t{67}) {
        return fail(QStringLiteral("source pack revision/count contract mismatch"));
    }

    const auto readme =
        QString::fromUtf8(readAll(authoring_root.filePath(QStringLiteral("README.md"))))
            .simplified();
    if (!readme.contains(QStringLiteral("us.ca4.m4.benton-retaliation@1.2.0")) ||
        !readme.contains(QStringLiteral("installable schema-v2 root")) ||
        !readme.contains(QStringLiteral("67 unique substantive")) ||
        !readme.contains(QStringLiteral("389 page anchors")) ||
        !readme.contains(QStringLiteral("37 PDFs and 262 continuous JA pages")) ||
        !readme.contains(QStringLiteral("13 actual appellate PDFs at PA1–PA70")) ||
        !readme.contains(QStringLiteral("17 separately docketed")) ||
        !readme.contains(QStringLiteral("PA71–PA127")) ||
        !readme.contains(QStringLiteral("13-stage, 53-operation workflow")) ||
        !readme.contains(QStringLiteral("Seventeen operations bind")) ||
        !readme.contains(QStringLiteral("both judgment operations additionally bind")) ||
        !readme.contains(QStringLiteral("seven checked-in production journals")) ||
        !readme.contains(QStringLiteral("independent_review_pending")) ||
        !readme.contains(QStringLiteral("actual judgment affirms the narrow exclusion")) ||
        !readme.contains(QStringLiteral("does not rely on a sham-affidavit rule")) ||
        !readme.contains(QStringLiteral("vacates the retaliation summary judgment")) ||
        !readme.contains(QStringLiteral("B04 rehearing tender")) ||
        !readme.contains(QStringLiteral("Blue Cedar owns the response brief")) ||
        !readme.contains(QStringLiteral("B14 is a joint motion"))) {
        return fail(QStringLiteral("README does not describe the finalized 1.2 boundary"));
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
        QStringLiteral("workbench.pack.operation-document-bindings@1"),
        QStringLiteral("workbench.pack.operation-disposition-bindings@1"),
        QStringLiteral("workbench.pack.structured-disposition@1"),
        QStringLiteral("workbench.pack.realism-evidence@1"),
    };
    if (actual_capabilities != expected_capabilities) {
        return fail(QStringLiteral("exact 1.2 capability contract mismatch"));
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
    const auto* realism_resource =
        findResource(source->resources, "ca4m4.benton.review.authoring-2026-08-12");
    if (case_resource == nullptr || record_resource == nullptr || authority_resource == nullptr ||
        workflow_resource == nullptr || bench_resource == nullptr || actual_argument == nullptr ||
        counterfactual_argument == nullptr || realism_resource == nullptr ||
        case_resource->descriptor.kind != ResourceKind::Case ||
        record_resource->descriptor.kind != ResourceKind::Record ||
        actual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        counterfactual_argument->descriptor.kind != ResourceKind::ArgumentConfig ||
        realism_resource->descriptor.kind != ResourceKind::RealismReview ||
        case_resource->descriptor.sha256 != case_digest ||
        record_resource->descriptor.sha256 != record_digest ||
        workflow_resource->descriptor.sha256 != workflow_digest ||
        realism_resource->descriptor.sha256 != realism_review_digest) {
        return fail(QStringLiteral("required Benton resources are absent"));
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
    const auto disposition_plans =
        case_resource->document.value(QStringLiteral("disposition_plans")).toArray();
    const auto authored_plan = objectById(disposition_plans, QStringLiteral("plan_id"),
                                          QStringLiteral("ca4m4.benton.disposition.authored"));
    const auto adverse_plan =
        objectById(disposition_plans, QStringLiteral("plan_id"),
                   QStringLiteral("ca4m4.benton.disposition.counterfactual-adverse"));
    const auto authored_components = authored_plan.value(QStringLiteral("components")).toArray();
    const auto adverse_components = adverse_plan.value(QStringLiteral("components")).toArray();
    const auto authored_retaliation = objectById(authored_components, QStringLiteral("issue_id"),
                                                 QString::fromLatin1(retaliation_issue));
    const auto authored_exclusion = objectById(authored_components, QStringLiteral("issue_id"),
                                               QString::fromLatin1(exclusion_issue));
    const auto adverse_retaliation = objectById(adverse_components, QStringLiteral("issue_id"),
                                                QString::fromLatin1(retaliation_issue));
    const auto adverse_exclusion = objectById(adverse_components, QStringLiteral("issue_id"),
                                              QString::fromLatin1(exclusion_issue));
    if (disposition_plans.size() != 2 ||
        case_resource->document.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.benton.disposition.authored") ||
        case_resource->document.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.benton.operation.issue-judgment") ||
        authored_plan.value(QStringLiteral("finality")).toString() != QStringLiteral("final") ||
        adverse_plan.value(QStringLiteral("finality")).toString() != QStringLiteral("final") ||
        authored_plan.value(QStringLiteral("digest")).toString() !=
            QString::fromLatin1(authored_disposition_digest) ||
        adverse_plan.value(QStringLiteral("digest")).toString() !=
            QString::fromLatin1(adverse_disposition_digest) ||
        authored_components.size() != 2 || adverse_components.size() != 2 ||
        authored_retaliation.value(QStringLiteral("target_id")).toString() !=
            QStringLiteral("ca4m4.benton.target.retaliation-summary-judgment") ||
        authored_retaliation.value(QStringLiteral("action")).toString() !=
            QStringLiteral("vacate") ||
        !authored_retaliation.value(QStringLiteral("remand")).toBool() ||
        authored_exclusion.value(QStringLiteral("target_id")).toString() !=
            QStringLiteral("ca4m4.benton.target.wynn-declaration-exclusion") ||
        authored_exclusion.value(QStringLiteral("action")).toString() != QStringLiteral("affirm") ||
        authored_exclusion.value(QStringLiteral("remand")).toBool() ||
        adverse_retaliation.value(QStringLiteral("action")).toString() !=
            QStringLiteral("affirm") ||
        adverse_retaliation.value(QStringLiteral("remand")).toBool() ||
        adverse_exclusion.value(QStringLiteral("action")).toString() != QStringLiteral("affirm") ||
        adverse_exclusion.value(QStringLiteral("remand")).toBool()) {
        return fail(QStringLiteral("two-plan structured disposition contract mismatch"));
    }
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
             .contains(QStringLiteral("Wynn's materially new March 7 statement")) ||
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
        !fact_canon.contains(QStringLiteral("authored actual appellate disposition")) ||
        !fact_canon.contains(QStringLiteral("vacates that summary judgment and remands")) ||
        !fact_canon.contains(QStringLiteral("separate adverse disposition affirms both targets")) ||
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
    const auto current_semantics = record_source_semantics;
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
    if (record_resource->document.value(QStringLiteral("dockets")).toArray().size() != 3 ||
        record_entries.size() != 67 || anchors.size() != 389) {
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

    const auto successor_plan_bytes =
        readAll(authoring_root.filePath(QStringLiteral("render-plan-successor.json")));
    const auto successor_inventory_bytes = readAll(
        authoring_root.filePath(QStringLiteral("metadata/render-inventory-successor.json")));
    const auto successor_plan = QJsonDocument::fromJson(successor_plan_bytes).object();
    const auto successor_inventory = QJsonDocument::fromJson(successor_inventory_bytes).object();
    const auto successor_plan_entries = successor_plan.value(QStringLiteral("entries")).toArray();
    const auto successor_rendered_entries =
        successor_inventory.value(QStringLiteral("entries")).toArray();
    const auto successor_plan_digest = QString::fromLatin1(sha256(successor_plan_bytes));
    if (sha256(successor_inventory_bytes) != QByteArray(successor_inventory_digest) ||
        successor_plan.value(QStringLiteral("schema_version")).toInt() != 1 ||
        successor_inventory.value(QStringLiteral("schema_version")).toInt() != 1 ||
        successor_plan_entries.size() != 30 || successor_rendered_entries.size() != 30 ||
        successor_inventory.value(QStringLiteral("plan_sha256")).toString() !=
            successor_plan_digest ||
        successor_inventory.value(QStringLiteral("ordering")).toString() !=
            QStringLiteral("output_path_casefolded_then_codepoint") ||
        successor_inventory.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
        successor_inventory.value(QStringLiteral("renderer_contract")).toString() !=
            QStringLiteral("appellate.markdown-pdf.semantic-layout.v2")) {
        return fail(QStringLiteral("successor render plan/inventory envelope mismatch"));
    }

    int expected_pa = 1;
    int actual_pa_pages = 0;
    int branch_pa_pages = 0;
    for (int index = 0; index < successor_plan_entries.size(); ++index) {
        const auto plan = successor_plan_entries.at(index).toObject();
        const auto rendered = successor_rendered_entries.at(index).toObject();
        const auto record_entry = record_entries.at(37 + index).toObject();
        const auto source_path = plan.value(QStringLiteral("source_path")).toString();
        const auto output_path = plan.value(QStringLiteral("output_path")).toString();
        const auto title = plan.value(QStringLiteral("title")).toString();
        const auto source_bytes = readAll(authoring_root.filePath(source_path));
        const auto pdf_bytes = readAll(QDir(pack_root).filePath(output_path));
        const auto assembly = rendered.value(QStringLiteral("assembly_provenance")).toObject();
        const auto labels = rendered.value(QStringLiteral("page_labels")).toObject();
        const auto page_count = rendered.value(QStringLiteral("page_count")).toInt();
        const auto source_digest = QString::fromLatin1(sha256(source_bytes));
        const auto pdf_digest = QString::fromLatin1(sha256(pdf_bytes));
        const auto blob = std::ranges::find(source->blobs, output_path.toStdString(),
                                            &model::BlobDescriptor::path);
        const bool actual = index < 13;
        const auto expected_docket =
            actual ? QStringLiteral("ca4m4.benton.docket.appellate")
                   : QStringLiteral("ca4m4.benton.docket.counterfactual-branches");
        const auto expected_tag = actual ? QStringLiteral("actual_appellate_docket")
                                         : QStringLiteral("counterfactual_appellate_branch");
        const auto record_tags = strings(record_entry.value(QStringLiteral("tags")).toArray());
        if (source_bytes.isEmpty() || pdf_bytes.isEmpty() ||
            plan.value(QStringLiteral("page_label_prefix")).toString() != QStringLiteral("PA") ||
            plan.value(QStringLiteral("page_label_start")).toInt() != expected_pa ||
            rendered.value(QStringLiteral("output_path")).toString() != output_path ||
            rendered.value(QStringLiteral("title")).toString() != title ||
            rendered.value(QStringLiteral("source_sha256")).toString() != source_digest ||
            rendered.value(QStringLiteral("pdf_sha256")).toString() != pdf_digest ||
            rendered.value(QStringLiteral("byte_size")).toInteger() != pdf_bytes.size() ||
            rendered.value(QStringLiteral("pdf_byte_deterministic")).toBool(true) ||
            rendered.value(QStringLiteral("renderer_contract")).toString() !=
                QStringLiteral("appellate.markdown-pdf.semantic-layout.v2") ||
            assembly.value(QStringLiteral("assembly_contract")).toString() !=
                QStringLiteral("appellate.markdown-assembly.v1") ||
            assembly.value(QStringLiteral("kind")).toString() != QStringLiteral("single_source") ||
            assembly.value(QStringLiteral("source_path")).toString() != source_path ||
            assembly.value(QStringLiteral("source_sha256")).toString() != source_digest ||
            assembly.value(QStringLiteral("logical_page_count")).toInt() != page_count ||
            labels.value(QStringLiteral("prefix")).toString() != QStringLiteral("PA") ||
            labels.value(QStringLiteral("first_number")).toInt() != expected_pa ||
            labels.value(QStringLiteral("last_number")).toInt() != expected_pa + page_count - 1 ||
            blob == source->blobs.end() || blob->sha256 != pdf_digest.toStdString() ||
            blob->byte_size != static_cast<std::uint64_t>(pdf_bytes.size()) ||
            record_entry.value(QStringLiteral("entry_number")).toInt() != 38 + index ||
            record_entry.value(QStringLiteral("docket_id")).toString() != expected_docket ||
            record_entry.value(QStringLiteral("asset_path")).toString() != output_path ||
            record_entry.value(QStringLiteral("asset_sha256")).toString() != pdf_digest ||
            record_entry.value(QStringLiteral("page_count")).toInt() != page_count ||
            record_entry.value(QStringLiteral("sealed")).toBool(true) ||
            !record_tags.contains(expected_tag) ||
            (!actual && !record_tags.contains(QStringLiteral("never_occurred_on_actual_docket")))) {
            return fail(
                QStringLiteral("successor source/PDF/blob/record mismatch: %1").arg(output_path));
        }

        const auto source_text = QString::fromUtf8(source_bytes);
        const auto expected_banner =
            actual ? QStringLiteral("SYNTHETIC APPELLATE DOCKET — NOT FILED — ALL FACTS AND "
                                    "IDENTIFIERS ARE FICTIONAL")
                   : QStringLiteral("SYNTHETIC COUNTERFACTUAL APPELLATE BRANCH — NEVER OCCURRED ON "
                                    "THE ACTUAL DOCKET — ALL FACTS AND IDENTIFIERS ARE FICTIONAL");
        const auto source_pages =
            source_text.split(QStringLiteral("<!-- PAGE BREAK -->"), Qt::KeepEmptyParts);
        if (!source_text.startsWith(expected_banner) || source_pages.size() != page_count) {
            return fail(
                QStringLiteral("successor source banner/page mismatch: %1").arg(source_path));
        }
        for (auto page : source_pages) {
            page = page.simplified();
            if (page.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size() <
                    30 ||
                distinct_source_pages.contains(page)) {
                return fail(
                    QStringLiteral("thin/duplicate successor Markdown page: %1").arg(source_path));
            }
            distinct_source_pages.insert(page);
        }

        QPdfDocument pdf;
        if (pdf.load(QDir(pack_root).filePath(output_path)) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready || pdf.pageCount() != page_count ||
            pdf.metaData(QPdfDocument::MetaDataField::Title).toString() != title ||
            pdf.metaData(QPdfDocument::MetaDataField::Author).toString() !=
                QStringLiteral("Appellate Workbench synthetic content") ||
            pdf.metaData(QPdfDocument::MetaDataField::Creator).toString() !=
                QStringLiteral("Appellate Workbench Markdown PDF Renderer")) {
            return fail(
                QStringLiteral("successor PDF metadata/page mismatch: %1").arg(output_path));
        }
        for (int page_index = 0; page_index < page_count; ++page_index) {
            const auto expected_label = QStringLiteral("PA%1").arg(expected_pa);
            auto page_text = pdf.getAllText(page_index).text().simplified();
            if (page_text.size() < 200 || !page_text.contains(expected_label)) {
                return fail(QStringLiteral("thin/unlabeled successor page %1 in %2")
                                .arg(expected_label, output_path));
            }
            page_text.remove(QRegularExpression(QStringLiteral("\\bPA\\d+\\b")));
            page_text = page_text.simplified();
            if (distinct_pdf_pages.contains(page_text)) {
                return fail(
                    QStringLiteral("duplicate successor PDF page at %1").arg(expected_label));
            }
            distinct_pdf_pages.insert(page_text);
            const auto anchor = anchors.at(262 + expected_pa - 1).toObject();
            if (anchor.value(QStringLiteral("anchor_id")).toString() !=
                    QStringLiteral("ca4m4.benton.anchor.pa%1").arg(expected_pa) ||
                anchor.value(QStringLiteral("entry_id")).toString() !=
                    record_entry.value(QStringLiteral("entry_id")).toString() ||
                anchor.value(QStringLiteral("page_number")).toInt() != page_index + 1 ||
                anchor.value(QStringLiteral("citation_label")).toString() != expected_label) {
                return fail(
                    QStringLiteral("successor record anchor mismatch at %1").arg(expected_label));
            }
            ++expected_pa;
        }
        if (actual) {
            actual_pa_pages += page_count;
        } else {
            branch_pa_pages += page_count;
        }
    }
    if (expected_pa != 128 || actual_pa_pages != 70 || branch_pa_pages != 57 ||
        distinct_source_pages.size() != 389 || distinct_pdf_pages.size() != 389) {
        return fail(QStringLiteral("37/262 JA + 13/70 actual PA + 17/57 branch PA mismatch"));
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

    QHash<QString, QJsonObject> final_record_entry_by_id;
    QHash<QString, QString> final_record_entry_id_by_sha;
    for (const auto& value : record_entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("entry_id")).toString();
        const auto digest = entry.value(QStringLiteral("asset_sha256")).toString();
        if (id.isEmpty() || digest.isEmpty() || final_record_entry_by_id.contains(id) ||
            final_record_entry_id_by_sha.contains(digest)) {
            return fail(QStringLiteral("record entry identity is empty or duplicated"));
        }
        final_record_entry_by_id.insert(id, entry);
        final_record_entry_id_by_sha.insert(digest, id);
    }

    const auto final_stages = workflow_resource->document.value(QStringLiteral("stages")).toArray();
    const auto final_routes =
        workflow_resource->document.value(QStringLiteral("filing_routes")).toArray();
    const auto final_operations =
        workflow_resource->document.value(QStringLiteral("operations")).toArray();
    QSet<QString> final_workflow_operation_ids;
    int final_document_bindings = 0;
    int final_disposition_bindings = 0;
    bool final_workflow_bindings_valid = true;
    for (const auto& value : final_operations) {
        const auto operation_value = value.toObject();
        const auto operation_id = operation_value.value(QStringLiteral("operation_id")).toString();
        if (operation_id.isEmpty() || final_workflow_operation_ids.contains(operation_id)) {
            final_workflow_bindings_valid = false;
            continue;
        }
        final_workflow_operation_ids.insert(operation_id);
        const auto binding = operation_value.value(QStringLiteral("document_binding")).toObject();
        if (!binding.isEmpty()) {
            ++final_document_bindings;
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = final_record_entry_by_id.value(entry_id);
            final_workflow_bindings_valid =
                final_workflow_bindings_valid && !entry.isEmpty() &&
                binding.value(QStringLiteral("document_sha256")).toString() ==
                    entry.value(QStringLiteral("asset_sha256")).toString() &&
                binding.value(QStringLiteral("expected_court_date")).toString() ==
                    entry.value(QStringLiteral("filed_on")).toString();
        }
        const auto disposition_plan_id =
            operation_value.value(QStringLiteral("disposition_plan_id")).toString();
        if (!disposition_plan_id.isEmpty()) {
            ++final_disposition_bindings;
            if (operation_id == QStringLiteral("ca4m4.benton.operation.issue-judgment")) {
                final_workflow_bindings_valid =
                    final_workflow_bindings_valid &&
                    disposition_plan_id == QStringLiteral("ca4m4.benton.disposition.authored") &&
                    binding.value(QStringLiteral("record_entry_id")).toString() ==
                        QStringLiteral("ca4m4.benton.record.entry.a11");
            } else if (operation_id ==
                       QStringLiteral("ca4m4.benton.operation.issue-judgment-on-briefs")) {
                final_workflow_bindings_valid =
                    final_workflow_bindings_valid &&
                    disposition_plan_id ==
                        QStringLiteral("ca4m4.benton.disposition.counterfactual-adverse") &&
                    binding.value(QStringLiteral("record_entry_id")).toString() ==
                        QStringLiteral("ca4m4.benton.record.entry.b03");
            } else {
                final_workflow_bindings_valid = false;
            }
        }
    }
    if (workflow_resource->document.value(QStringLiteral("initial_stage_id")).toString() !=
            QStringLiteral("ca4m4.benton.stage.opened") ||
        final_stages.size() != 13 || final_operations.size() != 53 || final_routes.size() != 6 ||
        final_workflow_operation_ids.size() != 53 || final_document_bindings != 17 ||
        final_disposition_bindings != 2 || !final_workflow_bindings_valid) {
        return fail(QStringLiteral("13/53/6 workflow or 17/2 binding contract mismatch"));
    }

    const QHash<QString, QString> expected_selector_actors{
        {QStringLiteral("ca4m4.benton.filing.notice-of-appeal"),
         QStringLiteral("ca4m4.benton.actor.leora-benton")},
        {QStringLiteral("ca4m4.benton.filing.a02-docketing-statement"),
         QStringLiteral("ca4m4.benton.actor.leora-benton")},
        {QStringLiteral("ca4m4.benton.filing.a04-opening-brief"),
         QStringLiteral("ca4m4.benton.actor.leora-benton")},
        {QStringLiteral("ca4m4.benton.filing.a05-response-brief"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar")},
        {QStringLiteral("ca4m4.benton.filing.b06-rehearing-petition"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar")},
        {QStringLiteral("ca4m4.benton.filing.b10-stay-motion"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar")},
        {QStringLiteral("ca4m4.benton.filing.b14-joint-shortening-motion"),
         QStringLiteral("ca4m4.benton.actor.leora-benton")},
    };
    int selector_occurrences = 0;
    QSet<QString> selector_filing_ids;
    bool selectors_valid = true;
    std::function<void(const QJsonValue&)> visit_workflow_value;
    visit_workflow_value = [&](const QJsonValue& value) {
        if (value.isArray()) {
            for (const auto& child : value.toArray()) {
                visit_workflow_value(child);
            }
            return;
        }
        if (!value.isObject()) {
            return;
        }
        const auto object = value.toObject();
        if (object.value(QStringLiteral("kind")).toString() == QStringLiteral("filing_instance")) {
            ++selector_occurrences;
            const auto filing_id = object.value(QStringLiteral("filing_id")).toString();
            const auto entry = final_record_entry_by_id.value(
                object.value(QStringLiteral("record_entry_id")).toString());
            selector_filing_ids.insert(filing_id);
            selectors_valid = selectors_valid && expected_selector_actors.contains(filing_id) &&
                              object.value(QStringLiteral("actor_id")).toString() ==
                                  expected_selector_actors.value(filing_id) &&
                              !entry.isEmpty() &&
                              object.value(QStringLiteral("document_sha256")).toString() ==
                                  entry.value(QStringLiteral("asset_sha256")).toString();
        }
        for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
            visit_workflow_value(iterator.value());
        }
    };
    visit_workflow_value(workflow_resource->document);
    if (!selectors_valid || selector_occurrences != 19 || selector_filing_ids.size() != 7) {
        return fail(QStringLiteral("workflow filing-instance identity contract mismatch"));
    }

    const auto final_review = realism_resource->document;
    const auto final_dimensions = final_review.value(QStringLiteral("dimensions")).toObject();
    const auto final_evidence = final_review.value(QStringLiteral("evidence")).toObject();
    const auto final_evidence_packs = final_evidence.value(QStringLiteral("packs")).toArray();
    const auto final_evidence_resources =
        final_evidence.value(QStringLiteral("resources")).toArray();
    const auto final_evidence_blobs = final_evidence.value(QStringLiteral("blobs")).toArray();
    const auto final_evidence_traces = final_evidence.value(QStringLiteral("traces")).toArray();
    const auto final_evidence_record_checks =
        final_evidence.value(QStringLiteral("record_checks")).toArray();
    const auto final_evidence_authorities =
        final_evidence.value(QStringLiteral("authorities")).toArray();
    const auto final_dimension_evidence =
        final_evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const QSet<QString> final_expected_dimensions{
        QStringLiteral("procedural_law"),     QStringLiteral("deadlines_authority"),
        QStringLiteral("record_consistency"), QStringLiteral("bench_differentiation"),
        QStringLiteral("oral_argument"),      QStringLiteral("consequences"),
        QStringLiteral("provenance"),
    };
    QSet<QString> final_dimension_keys;
    bool final_dimensions_are_two = true;
    for (auto iterator = final_dimensions.constBegin(); iterator != final_dimensions.constEnd();
         ++iterator) {
        final_dimension_keys.insert(iterator.key());
        final_dimensions_are_two = final_dimensions_are_two && iterator.value().toInt() == 2;
    }
    QSet<QString> final_dimension_evidence_keys;
    for (auto iterator = final_dimension_evidence.constBegin();
         iterator != final_dimension_evidence.constEnd(); ++iterator) {
        final_dimension_evidence_keys.insert(iterator.key());
    }
    if (final_review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        final_review.value(QStringLiteral("reviewed_on")).toString() !=
            QStringLiteral("2026-08-12") ||
        final_dimension_keys != final_expected_dimensions ||
        final_dimension_evidence_keys != final_expected_dimensions || !final_dimensions_are_two ||
        final_evidence_packs.size() != 4 || final_evidence_resources.size() != 44 ||
        final_evidence_blobs.size() != 67 || final_evidence_traces.size() != 7 ||
        final_evidence_record_checks.size() != 2 || final_evidence_authorities.size() != 28 ||
        final_evidence.value(QStringLiteral("closure_digest")).toString() !=
            QString::fromLatin1(evidence_closure_digest)) {
        return fail(QStringLiteral("realism evidence 4/44/67/7/2/28 envelope mismatch"));
    }
    const QSet<QString> expected_uncertainty_ids{
        QStringLiteral("ca4m4.benton.uncertainty.qualified-review-pending"),
        QStringLiteral("ca4m4.benton.uncertainty.automated-legal-realism-limit"),
        QStringLiteral("ca4m4.benton.uncertainty.exact-document-classification-scope"),
        QStringLiteral("ca4m4.benton.uncertainty.authored-deadline-bases"),
        QStringLiteral("ca4m4.benton.uncertainty.counterfactual-never-filed-isolation"),
        QStringLiteral("ca4m4.benton.uncertainty.synthetic-bench-oral-limit"),
        QStringLiteral("ca4m4.benton.uncertainty.generated-pdf-provenance-limit"),
    };
    QSet<QString> final_uncertainty_ids;
    for (const auto& value : final_review.value(QStringLiteral("known_uncertainty")).toArray()) {
        const auto uncertainty = value.toObject();
        const auto id = uncertainty.value(QStringLiteral("uncertainty_id")).toString();
        if (id.isEmpty() || final_uncertainty_ids.contains(id) ||
            uncertainty.value(QStringLiteral("blocking")).toBool()) {
            return fail(QStringLiteral("pending realism uncertainty contract mismatch"));
        }
        final_uncertainty_ids.insert(id);
    }
    if (final_uncertainty_ids != expected_uncertainty_ids) {
        return fail(QStringLiteral("pending realism uncertainty IDs drifted"));
    }
    QSet<QString> final_evidence_ids;
    const std::array final_evidence_groups{final_evidence_resources, final_evidence_blobs,
                                           final_evidence_traces, final_evidence_record_checks,
                                           final_evidence_authorities};
    for (const auto& group : final_evidence_groups) {
        for (const auto& value : group) {
            const auto id = value.toObject().value(QStringLiteral("evidence_id")).toString();
            if (id.isEmpty() || final_evidence_ids.contains(id)) {
                return fail(QStringLiteral("realism evidence IDs are empty or duplicated"));
            }
            final_evidence_ids.insert(id);
        }
    }
    if (final_evidence_ids.size() != 148 ||
        std::ranges::any_of(final_evidence_resources, [](const auto& value) {
            return value.toObject().value(QStringLiteral("resource_kind")).toString() ==
                   QStringLiteral("realism_review");
        })) {
        return fail(QStringLiteral("realism review exclusion or 148-ID closure mismatch"));
    }
    const QHash<QString, int> expected_dimension_evidence_counts{
        {QStringLiteral("procedural_law"), 43},     {QStringLiteral("deadlines_authority"), 23},
        {QStringLiteral("record_consistency"), 70}, {QStringLiteral("consequences"), 29},
        {QStringLiteral("oral_argument"), 14},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 102},
    };
    for (const auto& dimension : final_expected_dimensions) {
        const auto references = final_dimension_evidence.value(dimension).toArray();
        const auto unique_references = strings(references);
        if (references.size() != expected_dimension_evidence_counts.value(dimension) ||
            unique_references.size() != references.size()) {
            return fail(QStringLiteral("realism dimension evidence cardinality drifted: %1")
                            .arg(dimension));
        }
        for (const auto& reference : unique_references) {
            if (!final_evidence_ids.contains(reference)) {
                return fail(QStringLiteral("realism dimension has an unresolved evidence ID"));
            }
        }
    }

    struct FinalTraceSpec final {
        QString file_name;
        QString sha256;
        QString trace_id;
        QString evidence_id;
        QString terminal_stage_id;
        int command_count{};
        int event_count{};
        QSet<QString> branch_entries;
    };
    const std::array final_trace_specs{
        FinalTraceSpec{
            QStringLiteral("actual-argued-no-petition-mandate.json"),
            QStringLiteral("0ea935423c4b4a80201b36e803d09fa0dc87d6b0ad2e510cc2f64be7c02bfc99"),
            QStringLiteral("ca4m4.benton.trace.actual-argued-no-petition-mandate"),
            QStringLiteral("ca4m4.benton.evidence.trace.actual-argued-no-petition-mandate"),
            QStringLiteral("ca4m4.benton.stage.terminated"),
            26,
            27,
            {}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-on-briefs-adverse-judgment.json"),
            QStringLiteral("793bb9deb256ac13c5db5f931fe407c4324b76a393f349ca135fc567500d57ba"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-on-briefs-adverse-judgment"),
            QStringLiteral("ca4m4.benton.evidence.trace.counterfactual-on-briefs-adverse-judgment"),
            QStringLiteral("ca4m4.benton.stage.post-judgment"),
            18,
            19,
            {QStringLiteral("ca4m4.benton.record.entry.b03")}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-day-late-rehearing-rejected.json"),
            QStringLiteral("aee9cd480c90b677e8fbe3e64013aeba607b9b42fc2af5bf726a0e1828b2d56a"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-day-late-rehearing-rejected"),
            QStringLiteral(
                "ca4m4.benton.evidence.trace.counterfactual-day-late-rehearing-rejected"),
            QStringLiteral("ca4m4.benton.stage.post-judgment"),
            22,
            23,
            {QStringLiteral("ca4m4.benton.record.entry.b04")}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-timely-rehearing-denied-mandate.json"),
            QStringLiteral("3f8548ca2f012bcc920ebbceef96877a9e2a1585f497ae7020ece0a826a25284"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-timely-rehearing-denied-mandate"),
            QStringLiteral(
                "ca4m4.benton.evidence.trace.counterfactual-timely-rehearing-denied-mandate"),
            QStringLiteral("ca4m4.benton.stage.terminated"),
            29,
            30,
            {QStringLiteral("ca4m4.benton.record.entry.b06"),
             QStringLiteral("ca4m4.benton.record.entry.b07"),
             QStringLiteral("ca4m4.benton.record.entry.b08"),
             QStringLiteral("ca4m4.benton.record.entry.b09")}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-stay-granted-blocks-mandate.json"),
            QStringLiteral("7a25758d4f35b80b5177960283edead8eac3a903b003ef3b54fdf455d09f3be2"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-stay-granted-blocks-mandate"),
            QStringLiteral(
                "ca4m4.benton.evidence.trace.counterfactual-stay-granted-blocks-mandate"),
            QStringLiteral("ca4m4.benton.stage.mandate-stayed"),
            28,
            29,
            {QStringLiteral("ca4m4.benton.record.entry.b06"),
             QStringLiteral("ca4m4.benton.record.entry.b07"),
             QStringLiteral("ca4m4.benton.record.entry.b10"),
             QStringLiteral("ca4m4.benton.record.entry.b11")}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-stay-released-mandate.json"),
            QStringLiteral("02add46e3b624dc1a329ea6fdde486de2476c9d77ae6f0095cb62b98dd8a80d8"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-stay-released-mandate"),
            QStringLiteral("ca4m4.benton.evidence.trace.counterfactual-stay-released-mandate"),
            QStringLiteral("ca4m4.benton.stage.terminated"),
            32,
            33,
            {QStringLiteral("ca4m4.benton.record.entry.b06"),
             QStringLiteral("ca4m4.benton.record.entry.b07"),
             QStringLiteral("ca4m4.benton.record.entry.b10"),
             QStringLiteral("ca4m4.benton.record.entry.b11"),
             QStringLiteral("ca4m4.benton.record.entry.b12"),
             QStringLiteral("ca4m4.benton.record.entry.b13")}},
        FinalTraceSpec{
            QStringLiteral("counterfactual-shortened-mandate.json"),
            QStringLiteral("735c93fddbf7846be6e8e1726e991d06cce2f1e797ea6b04cb4a48d05e31209f"),
            QStringLiteral("ca4m4.benton.trace.counterfactual-shortened-mandate"),
            QStringLiteral("ca4m4.benton.evidence.trace.counterfactual-shortened-mandate"),
            QStringLiteral("ca4m4.benton.stage.terminated"),
            27,
            28,
            {QStringLiteral("ca4m4.benton.record.entry.b14"),
             QStringLiteral("ca4m4.benton.record.entry.b15"),
             QStringLiteral("ca4m4.benton.record.entry.b16"),
             QStringLiteral("ca4m4.benton.record.entry.b17")}},
    };
    QHash<QString, QJsonObject> final_embedded_trace_by_id;
    for (const auto& value : final_evidence_traces) {
        const auto trace = value.toObject();
        final_embedded_trace_by_id.insert(trace.value(QStringLiteral("trace_id")).toString(),
                                          trace);
    }
    const auto final_traces_root = QDir(authoring_root.filePath(QStringLiteral("traces")));
    if (final_traces_root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name).size() !=
        7) {
        return fail(QStringLiteral("canonical trace source count mismatch"));
    }

    const QHash<QString, QString> expected_filing_semantics{
        {QStringLiteral("ca4m4.benton.filing.notice-of-appeal"),
         QStringLiteral("ca4m4.benton.actor.leora-benton|2026-01-16|"
                        "3e75e87fd04fd52e35865085dbd2f13c48c0f92e92354c34eb8ba30c09432012")},
        {QStringLiteral("ca4m4.benton.filing.a02-docketing-statement"),
         QStringLiteral("ca4m4.benton.actor.leora-benton|2026-02-03|"
                        "305437ad29730589f849255cb14d9ea5ed58eebf349e74865fe36fb9d37f59d0")},
        {QStringLiteral("ca4m4.benton.filing.a04-opening-brief"),
         QStringLiteral("ca4m4.benton.actor.leora-benton|2026-03-16|"
                        "515683e33f7e6d3a4c962e798de078a0114d31b1b592a84f6a5c6d22e40c7090")},
        {QStringLiteral("ca4m4.benton.filing.a05-response-brief"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar|2026-04-15|"
                        "75ed6e9aa8508730b0b8757c1151cd52c2985b030ffde7bbabf8c03bdaae55fa")},
        {QStringLiteral("ca4m4.benton.filing.b04-late-rehearing-petition"),
         QStringLiteral("ca4m4.benton.actor.leora-benton|2026-06-30|"
                        "48973cbefb83fd92622087d0e7a697f6021fa320f2cd786d855b50c0a9eb2065")},
        {QStringLiteral("ca4m4.benton.filing.b06-rehearing-petition"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar|2026-06-25|"
                        "cf4e03eb0fcdf6852ab7f7642d924ab14f35048c587dc2399e367de3fd9bc8d4")},
        {QStringLiteral("ca4m4.benton.filing.b10-stay-motion"),
         QStringLiteral("ca4m4.benton.actor.blue-cedar|2026-07-02|"
                        "51a1fa1554916d59c84c259c8604c4ef9e143c9afbc9eda8e65eb0f8bbfe2810")},
        {QStringLiteral("ca4m4.benton.filing.b14-joint-shortening-motion"),
         QStringLiteral("ca4m4.benton.actor.leora-benton|2026-06-18|"
                        "f3418e35e9d9a4197df11b85659140137d7f8829716ff18b894dce58391dc81f")},
    };
    const QHash<QString, int> expected_filing_counts{
        {QStringLiteral("ca4m4.benton.filing.notice-of-appeal"), 7},
        {QStringLiteral("ca4m4.benton.filing.a02-docketing-statement"), 7},
        {QStringLiteral("ca4m4.benton.filing.a04-opening-brief"), 7},
        {QStringLiteral("ca4m4.benton.filing.a05-response-brief"), 7},
        {QStringLiteral("ca4m4.benton.filing.b04-late-rehearing-petition"), 1},
        {QStringLiteral("ca4m4.benton.filing.b06-rehearing-petition"), 3},
        {QStringLiteral("ca4m4.benton.filing.b10-stay-motion"), 2},
        {QStringLiteral("ca4m4.benton.filing.b14-joint-shortening-motion"), 1},
    };
    const QSet<QString> expected_panel_operations{
        QStringLiteral("ca4m4.benton.operation.enter-argument-held"),
        QStringLiteral("ca4m4.benton.operation.issue-judgment"),
        QStringLiteral("ca4m4.benton.operation.issue-judgment-on-briefs"),
        QStringLiteral("ca4m4.benton.operation.enter-rehearing-denial"),
        QStringLiteral("ca4m4.benton.operation.enter-mandate-stay-grant"),
        QStringLiteral("ca4m4.benton.operation.enter-stay-dissolution-release"),
        QStringLiteral("ca4m4.benton.operation.enter-mandate-shortening"),
    };
    QHash<QString, int> actual_filing_counts;
    QSet<QString> final_seen_trace_ids;
    QSet<QString> final_seen_trace_evidence_ids;
    QSet<QString> final_executed_operation_ids;
    bool saw_late_rejection = false;
    bool saw_joint_shortening_semantics = false;
    for (const auto& specification : final_trace_specs) {
        const auto trace_bytes = readAll(final_traces_root.filePath(specification.file_name));
        const auto trace = QJsonDocument::fromJson(trace_bytes).object();
        const auto trace_id = trace.value(QStringLiteral("trace_id")).toString();
        const auto trace_evidence_id = trace.value(QStringLiteral("evidence_id")).toString();
        const auto journal = trace.value(QStringLiteral("journal")).toArray();
        const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
        if (sha256(trace_bytes) != specification.sha256.toLatin1() ||
            final_embedded_trace_by_id.value(trace_id) != trace ||
            final_seen_trace_ids.contains(trace_id) ||
            final_seen_trace_evidence_ids.contains(trace_evidence_id) ||
            trace_id != specification.trace_id || trace_evidence_id != specification.evidence_id ||
            trace.value(QStringLiteral("workflow_id")).toString() !=
                QStringLiteral("ca4m4.benton.workflow.civil-appeal") ||
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
            realismTraceDigest(QStringLiteral("ca4m4.case.benton-retaliation"), trace) !=
                trace.value(QStringLiteral("digest")).toString()) {
            return fail(QStringLiteral("canonical trace envelope/digest mismatch: %1")
                            .arg(specification.file_name));
        }
        final_seen_trace_ids.insert(trace_id);
        final_seen_trace_evidence_ids.insert(trace_evidence_id);
        final_executed_operation_ids.unite(strings(operation_ids));
        QSet<QString> used_branch_entries;
        QJsonArray decoded_operation_ids;
        int decoded_event_count = 0;
        int notice_count = 0;
        int january_twentieth_operations = 0;
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
            const auto command = command_document.object();
            const auto payload = command.value(QStringLiteral("payload")).toObject();
            const auto document_sha = payload.value(QStringLiteral("document_sha256")).toString();
            if (!document_sha.isEmpty()) {
                const auto entry_id = final_record_entry_id_by_sha.value(document_sha);
                const auto entry = final_record_entry_by_id.value(entry_id);
                if (entry_id.isEmpty() || entry.value(QStringLiteral("filed_on")).toString() !=
                                              payload.value(QStringLiteral("occurred_at"))
                                                  .toObject()
                                                  .value(QStringLiteral("court_date"))
                                                  .toString()) {
                    return fail(QStringLiteral("trace command document SHA/date does not resolve"));
                }
                if (entry.value(QStringLiteral("docket_id")).toString() ==
                    QStringLiteral("ca4m4.benton.docket.counterfactual-branches")) {
                    used_branch_entries.insert(entry_id);
                }
            }
            const auto filing_id = payload.value(QStringLiteral("filing_id")).toString();
            if (!filing_id.isEmpty()) {
                const auto actual_semantics = payload.value(QStringLiteral("actor_id")).toString() +
                                              QLatin1Char('|') +
                                              payload.value(QStringLiteral("occurred_at"))
                                                  .toObject()
                                                  .value(QStringLiteral("court_date"))
                                                  .toString() +
                                              QLatin1Char('|') + document_sha;
                if (!expected_filing_semantics.contains(filing_id) ||
                    actual_semantics != expected_filing_semantics.value(filing_id)) {
                    return fail(QStringLiteral("trace filing actor/date/document drifted: %1")
                                    .arg(filing_id));
                }
                ++actual_filing_counts[filing_id];
                if (filing_id == QStringLiteral("ca4m4.benton.filing.notice-of-appeal")) {
                    ++notice_count;
                }
                if (filing_id ==
                    QStringLiteral("ca4m4.benton.filing.b14-joint-shortening-motion")) {
                    const auto served = payload.value(QStringLiteral("served_actors")).toArray();
                    const auto fields = QString::fromUtf8(
                        QJsonDocument(payload.value(QStringLiteral("fields")).toArray())
                            .toJson(QJsonDocument::Compact));
                    saw_joint_shortening_semantics =
                        served == QJsonArray{QStringLiteral("ca4m4.benton.actor.blue-cedar")} &&
                        fields.contains(QStringLiteral("Joint motion by Benton and Blue Cedar")) &&
                        fields.contains(QStringLiteral("Joint good-cause request"));
                }
            }
            const auto operation_id = payload.value(QStringLiteral("operation_id")).toString();
            if (!operation_id.isEmpty()) {
                const auto actor_id = payload.value(QStringLiteral("actor_id")).toString();
                const auto expected_actor =
                    expected_panel_operations.contains(operation_id)
                        ? QStringLiteral("ca4m4.benton.actor.composite-panel")
                        : QStringLiteral("ca4m4.benton.actor.ca4-clerk");
                if (actor_id != expected_actor) {
                    return fail(
                        QStringLiteral("trace clerk/panel actor mismatch: %1").arg(operation_id));
                }
                if (operation_id ==
                        QStringLiteral("ca4m4.benton.operation.advance-opened-to-record") ||
                    operation_id ==
                        QStringLiteral("ca4m4.benton.operation.calculate-docketing-statement")) {
                    if (payload.value(QStringLiteral("occurred_at"))
                            .toObject()
                            .value(QStringLiteral("court_date"))
                            .toString() != QStringLiteral("2026-01-20")) {
                        return fail(QStringLiteral("Jan. 20 docket operation date drifted"));
                    }
                    ++january_twentieth_operations;
                }
            }
            for (const auto& event_value :
                 journal_entry.value(QStringLiteral("events_base64")).toArray()) {
                ++decoded_event_count;
                const auto event_encoded = event_value.toString().toLatin1();
                const auto event_bytes = QByteArray::fromBase64(event_encoded);
                const auto event_document = QJsonDocument::fromJson(event_bytes);
                if (event_bytes.isEmpty() || event_bytes.toBase64() != event_encoded ||
                    !event_document.isObject() ||
                    event_document.toJson(QJsonDocument::Compact) != event_bytes) {
                    return fail(QStringLiteral("trace event is not canonical base64 JSON"));
                }
                const auto event = event_document.object();
                const auto event_payload = event.value(QStringLiteral("payload")).toObject();
                decoded_operation_ids.push_back(
                    event_payload.value(QStringLiteral("operation_id")).toString());
                if (event.value(QStringLiteral("event_type")).toString() ==
                    QStringLiteral("filing.rejected")) {
                    saw_late_rejection =
                        event_payload.value(QStringLiteral("filing_id")).toString() ==
                            QStringLiteral("ca4m4.benton.filing.b04-late-rehearing-petition") &&
                        event_payload.value(QStringLiteral("actor_id")).toString() ==
                            QStringLiteral("ca4m4.benton.actor.leora-benton") &&
                        event_payload.value(QStringLiteral("operation_id")).toString() ==
                            QStringLiteral("ca4m4.benton.operation.reject-post-judgment") &&
                        event_payload.value(QStringLiteral("reason")).toString() ==
                            QStringLiteral("deadline_expired") &&
                        event_payload.value(QStringLiteral("occurred_at"))
                                .toObject()
                                .value(QStringLiteral("court_date"))
                                .toString() == QStringLiteral("2026-06-30");
                }
            }
        }
        if (decoded_event_count != specification.event_count ||
            decoded_operation_ids != operation_ids ||
            used_branch_entries != specification.branch_entries || notice_count != 1 ||
            january_twentieth_operations != 2) {
            return fail(QStringLiteral("trace replay operation/document/date mismatch: %1")
                            .arg(specification.file_name));
        }
    }
    const QSet<QString> intentionally_unexecuted_operations{
        QStringLiteral("ca4m4.benton.operation.issue-notice-deficiency"),
        QStringLiteral("ca4m4.benton.operation.reject-opened"),
        QStringLiteral("ca4m4.benton.operation.reject-record"),
        QStringLiteral("ca4m4.benton.operation.reject-opening-brief"),
        QStringLiteral("ca4m4.benton.operation.reject-response-brief"),
    };
    if (actual_filing_counts != expected_filing_counts || !saw_late_rejection ||
        !saw_joint_shortening_semantics || final_executed_operation_ids.size() != 48 ||
        !(final_executed_operation_ids - final_workflow_operation_ids).isEmpty() ||
        final_workflow_operation_ids - final_executed_operation_ids !=
            intentionally_unexecuted_operations) {
        return fail(QStringLiteral("seven-trace actor/nonmutation/operation coverage drifted"));
    }

    QTemporaryDir final_temporary;
    if (!final_temporary.isValid()) {
        return fail(QStringLiteral("cannot create temporary validation directory"));
    }
    const auto final_archive_a =
        QDir(final_temporary.path()).filePath(QStringLiteral("benton-a.awpack"));
    const auto final_archive_b =
        QDir(final_temporary.path()).filePath(QStringLiteral("benton-b.awpack"));
    const auto final_exported_a = PackArchive::exportDirectory(
        pack_root, final_archive_a, {}, PackValidationScope::ResolvedClosure);
    const auto final_exported_b = PackArchive::exportDirectory(
        pack_root, final_archive_b, {}, PackValidationScope::ResolvedClosure);
    const auto final_archive_bytes = readAll(final_archive_a);
    if (!final_exported_a || !final_exported_b || *final_exported_a != expected_root ||
        *final_exported_b != expected_root || final_archive_bytes.isEmpty() ||
        final_archive_bytes != readAll(final_archive_b) ||
        static_cast<std::uint64_t>(final_archive_bytes.size()) != archive_byte_size ||
        sha256(final_archive_bytes) != QByteArray(archive_digest)) {
        return fail(QStringLiteral("deferred archive export is not stable"));
    }
    const auto final_imported =
        PackArchive::importArchive(final_archive_a, {}, PackValidationScope::ResolvedClosure);
    if (!final_imported || final_imported->revision != source->revision ||
        final_imported->resources.size() != source->resources.size() ||
        final_imported->blobs != source->blobs) {
        return fail(QStringLiteral("directory/archive descriptor equality mismatch"));
    }

    const auto final_catalog_result =
        PackCatalog::open(QDir(final_temporary.path()).filePath(QStringLiteral("catalog")));
    if (!final_catalog_result) {
        return fail(QStringLiteral("catalog open: %1").arg(final_catalog_result.error().message));
    }
    auto& final_catalog = *final_catalog_result;
    const auto final_federal_archive = foundations_root.filePath(
        QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack"));
    const auto final_ca4_archive =
        foundations_root.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack"));
    const auto final_bench_archive = foundations_root.filePath(
        QStringLiteral("us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack"));
    const auto final_installed_federal = final_catalog->installArchive(
        final_federal_archive, QStringLiteral("2026-08-12T00:00:00Z"));
    const auto final_installed_ca4 =
        final_catalog->installArchive(final_ca4_archive, QStringLiteral("2026-08-12T00:00:01Z"));
    const auto final_installed_bench =
        final_catalog->installArchive(final_bench_archive, QStringLiteral("2026-08-12T00:00:02Z"));
    const auto final_installed_root =
        final_catalog->installArchive(final_archive_a, QStringLiteral("2026-08-12T00:00:03Z"));
    if (!final_installed_federal || !final_installed_ca4 || !final_installed_bench ||
        !final_installed_root || final_installed_federal->revision != expected_federal ||
        final_installed_ca4->revision != expected_ca4 ||
        final_installed_bench->revision != expected_bench ||
        final_installed_root->revision != expected_root) {
        return fail(QStringLiteral("exact catalog installation failed"));
    }

    const auto final_resolved = final_catalog->loadResolved(expected_root);
    if (!final_resolved || final_resolved->root().revision != expected_root ||
        final_resolved->revisionsByPackId().size() != std::size_t{4} ||
        final_resolved->resourceOwner("us.ca4.court.appeals") !=
            std::optional<PackRevision>{expected_ca4} ||
        final_resolved->resourceOwner("us.federal.authorities.appellate-rules") !=
            std::optional<PackRevision>{expected_federal} ||
        final_resolved->resourceOwner("us.ca4.bench-profile.rowan") !=
            std::optional<PackRevision>{expected_bench} ||
        final_resolved->resourceOwner("ca4m4.benton.record") !=
            std::optional<PackRevision>{expected_root} ||
        final_resolved->resourceOwner("ca4m4.benton.review.authoring-2026-08-12") !=
            std::optional<PackRevision>{expected_root}) {
        return fail(QStringLiteral("resolved graph owner/pin contract mismatch"));
    }

    std::vector<const packs::LoadedPack*> final_resolved_dependencies;
    final_resolved_dependencies.reserve(final_resolved->dependenciesDependencyFirst().size());
    for (const auto& dependency : final_resolved->dependenciesDependencyFirst()) {
        final_resolved_dependencies.push_back(&dependency);
    }
    for (std::size_t trace_index = 0; trace_index < final_trace_specs.size(); ++trace_index) {
        auto candidate = final_resolved->root();
        const auto candidate_review = std::ranges::find(
            candidate.resources, std::string_view("ca4m4.benton.review.authoring-2026-08-12"),
            [](const auto& resource) { return std::string_view(resource.descriptor.id); });
        if (candidate_review == candidate.resources.end()) {
            return fail(QStringLiteral("resolved root lost its realism review"));
        }
        auto candidate_evidence =
            candidate_review->document.value(QStringLiteral("evidence")).toObject();
        auto candidate_traces = candidate_evidence.value(QStringLiteral("traces")).toArray();
        auto candidate_trace = candidate_traces.at(static_cast<qsizetype>(trace_index)).toObject();
        auto candidate_journal = candidate_trace.value(QStringLiteral("journal")).toArray();
        auto candidate_entry = candidate_journal.at(0).toObject();
        auto candidate_events = candidate_entry.value(QStringLiteral("events_base64")).toArray();
        auto event_document = QJsonDocument::fromJson(
            QByteArray::fromBase64(candidate_events.at(0).toString().toLatin1()));
        auto event_object = event_document.object();
        auto event_payload = event_object.value(QStringLiteral("payload")).toObject();
        event_payload.insert(
            QStringLiteral("sequence"),
            QString::number(
                event_payload.value(QStringLiteral("sequence")).toString().toULongLong() + 1000U));
        event_object.insert(QStringLiteral("payload"), event_payload);
        candidate_events.replace(
            0, QString::fromLatin1(
                   QJsonDocument(event_object).toJson(QJsonDocument::Compact).toBase64()));
        candidate_entry.insert(QStringLiteral("events_base64"), candidate_events);
        candidate_journal.replace(0, candidate_entry);
        candidate_trace.insert(QStringLiteral("journal"), candidate_journal);
        candidate_traces.replace(static_cast<qsizetype>(trace_index), candidate_trace);
        candidate_evidence.insert(QStringLiteral("traces"), candidate_traces);
        candidate_review->document.insert(QStringLiteral("evidence"), candidate_evidence);
        const auto validation =
            PackReader::validateResolvedGraph(candidate, final_resolved_dependencies);
        if (validation || validation.error().code != packs::ErrorCode::CrossReferenceFailure) {
            return fail(QStringLiteral("trace tamper did not fail closed: %1")
                            .arg(final_trace_specs.at(trace_index).file_name));
        }
    }

    const auto final_runtime = packs::loadRuntimePack(*final_resolved);
    if (!final_runtime || final_runtime->revision != expected_root ||
        final_runtime->cases.size() != std::size_t{1} ||
        final_runtime->cases.front().argument_configurations.size() != std::size_t{2} ||
        std::ranges::any_of(
            final_runtime->cases.front().argument_configurations, [](const auto& configuration) {
                return !configuration.grounded_question_bank.has_value() ||
                       configuration.permitted_issue_ids.size() != std::size_t{2} ||
                       configuration.grounded_question_bank->issue_topics.size() !=
                           std::size_t{2} ||
                       configuration.grounded_question_bank->questions.size() != std::size_t{12};
            })) {
        return fail(QStringLiteral("catalog-valid Benton 1.2 closure is not runtime-loadable"));
    }

    std::cout << "Benton 1.2 integration contract passed: 67 PDFs / 389 pages "
                 "(JA1-JA262 and PA1-PA127), two grounded banks, two disposition plans, "
                 "13-stage/53-operation workflow, seven replayed traces, deterministic "
                 "archive, and four exact revisions.\n";
    return 0;
}
