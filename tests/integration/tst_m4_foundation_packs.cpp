#include "appellate/model/resource.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name the content/foundations directory"
#endif

namespace {

using appellate::model::PackId;
using appellate::model::PackRevision;
using appellate::model::ResourceKind;
using appellate::packs::LoadedPack;
using appellate::packs::PackArchive;
using appellate::packs::PackCatalog;
using appellate::packs::PackGraphState;
using appellate::packs::PackReader;
using appellate::packs::PackValidationScope;

constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto federal_archive_sha256 =
    "69736648f78376a6d85cde32148337edbf5af2a289de6070734c5454cc6b411b";
constexpr auto ca4_archive_sha256 =
    "5c9098d76012891ab2cb1f04c48bdcb3101c64253fdaab1608de789d0f5aa6ef";
constexpr auto bench_archive_sha256 =
    "e2758217f5ba9b987cc9e9920af65f762263f420e1698b12732d4f02b0121137";

[[nodiscard]] QString foundationRoot(const QString& name) {
    return QDir(QStringLiteral(APPELLATE_M4_FOUNDATIONS)).filePath(name);
}

[[nodiscard]] QString foundation(const QString& name) {
    return QDir(foundationRoot(name)).filePath(QStringLiteral("pack"));
}

[[nodiscard]] QString checkedInArchive(const QString& name, const QString& archive_name) {
    return QDir(foundationRoot(name)).filePath(archive_name);
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] std::size_t countKind(const LoadedPack& pack, ResourceKind kind) {
    return static_cast<std::size_t>(std::ranges::count_if(
        pack.resources, [kind](const auto& resource) { return resource.descriptor.kind == kind; }));
}

[[nodiscard]] std::size_t authorityCount(const LoadedPack& pack) {
    std::size_t count = 0;
    for (const auto& resource : pack.resources) {
        if (resource.descriptor.kind == ResourceKind::AuthoritySet) {
            count += static_cast<std::size_t>(
                resource.document.value(QStringLiteral("authorities")).toArray().size());
        }
    }
    return count;
}

class M4FoundationPacksTest final : public QObject {
    Q_OBJECT

  private slots:
    void directoriesMatchFrozenContract();
    void archivesAreDeterministicAndResolveExactly();
};

void M4FoundationPacksTest::directoriesMatchFrozenContract() {
    const PackRevision expected_federal{PackId{"foundation.us-federal"}, "2025.12.01",
                                        federal_digest};
    const PackRevision expected_ca4{PackId{"foundation.us-ca4"}, "2026.03.23", ca4_digest};
    const PackRevision expected_bench{PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                                      bench_digest};

    const auto federal = PackReader::readDirectory(foundation(QStringLiteral("us-federal")));
    QVERIFY2(federal.has_value(), federal ? "" : qPrintable(federal.error().message));
    QCOMPARE(federal->revision, expected_federal);
    QCOMPARE(federal->resources.size(), std::size_t{4});
    QCOMPARE(countKind(*federal, ResourceKind::AuthoritySet), std::size_t{3});
    QCOMPARE(countKind(*federal, ResourceKind::Court), std::size_t{1});
    QCOMPARE(authorityCount(*federal), std::size_t{26});

    QSet<QString> federal_authority_ids;
    for (const auto& resource : federal->resources) {
        if (resource.descriptor.kind != ResourceKind::AuthoritySet) {
            continue;
        }
        const auto expected_source_version = QStringLiteral("2025-12-01");
        const auto expected_source_url =
            resource.descriptor.id == "us.federal.authorities.appellate-rules"
                ? QStringLiteral("https://www.uscourts.gov/sites/default/files/document/"
                                 "federal-rules-of-appellate-procedure.pdf")
            : resource.descriptor.id == "us.federal.authorities.civil-rules"
                ? QStringLiteral("https://www.uscourts.gov/sites/default/files/document/"
                                 "federal-rules-of-civil-procedure.pdf")
                : QStringLiteral("https://www.uscourts.gov/sites/default/files/document/"
                                 "federal-rules-of-criminal-procedure.pdf");
        for (const auto& authority_value :
             resource.document.value(QStringLiteral("authorities")).toArray()) {
            const auto authority = authority_value.toObject();
            federal_authority_ids.insert(
                authority.value(QStringLiteral("authority_id")).toString());
            QVERIFY(authority.value(QStringLiteral("official_source")).toBool());
            QCOMPARE(authority.value(QStringLiteral("source_version")).toString(),
                     expected_source_version);
            QCOMPARE(authority.value(QStringLiteral("checked_on")).toString(),
                     QStringLiteral("2026-08-11"));
            QCOMPARE(authority.value(QStringLiteral("source_url")).toString(), expected_source_url);
            QVERIFY(!authority.value(QStringLiteral("locator")).toString().isEmpty());
        }
    }
    QVERIFY(
        federal_authority_ids.contains(QStringLiteral("us.federal.authority.frap-40-rehearing")));
    QVERIFY(
        !federal_authority_ids.contains(QStringLiteral("us.federal.authority.frap-35-en-banc")));

    const auto bench =
        PackReader::readDirectory(foundation(QStringLiteral("us-ca4-fictional-bench")));
    QVERIFY2(bench.has_value(), bench ? "" : qPrintable(bench.error().message));
    QCOMPARE(bench->revision, expected_bench);
    QCOMPARE(bench->resources.size(), std::size_t{8});
    QCOMPARE(countKind(*bench, ResourceKind::JudgeProfile), std::size_t{8});
    QCOMPARE(bench->judge_profiles.size(), std::size_t{8});

    const QSet<QString> reusable_topics{
        QStringLiteral("workbench.topic.jurisdiction"),
        QStringLiteral("workbench.topic.preservation"),
        QStringLiteral("workbench.topic.standard-of-review"),
        QStringLiteral("workbench.topic.record-support"),
        QStringLiteral("workbench.topic.governing-authority"),
        QStringLiteral("workbench.topic.merits"),
        QStringLiteral("workbench.topic.remedy"),
        QStringLiteral("workbench.topic.practical-consequences"),
    };
    for (const auto& resource : bench->resources) {
        QCOMPARE(resource.document.value(QStringLiteral("profile_class")).toString(),
                 QStringLiteral("fictional_composite"));
        const auto compatibility =
            resource.document.value(QStringLiteral("compatibility")).toObject();
        QCOMPARE(compatibility.value(QStringLiteral("court_roles")).toArray(),
                 QJsonArray{QStringLiteral("appellate")});
        QCOMPARE(compatibility.value(QStringLiteral("jurisdiction_ids")).toArray(),
                 QJsonArray{QStringLiteral("us.ca4")});
        for (const auto& focus : resource.document.value(QStringLiteral("interaction"))
                                     .toObject()
                                     .value(QStringLiteral("issue_focus"))
                                     .toArray()) {
            QVERIFY(reusable_topics.contains(
                focus.toObject().value(QStringLiteral("topic_id")).toString()));
        }
    }

    const auto ca4 = PackReader::readDirectory(foundation(QStringLiteral("us-ca4")),
                                               PackValidationScope::ResolvedClosure);
    QVERIFY2(ca4.has_value(), ca4 ? "" : qPrintable(ca4.error().message));
    QCOMPARE(ca4->revision, expected_ca4);
    QCOMPARE(ca4->graph_state, PackGraphState::DeferredReferences);
    QCOMPARE(ca4->dependencies.size(), std::size_t{1});
    QCOMPARE(ca4->dependencies.front().revision, expected_federal);
    QCOMPARE(ca4->resources.size(), std::size_t{24});
    QCOMPARE(countKind(*ca4, ResourceKind::AuthoritySet), std::size_t{1});
    QCOMPARE(countKind(*ca4, ResourceKind::Court), std::size_t{1});
    QCOMPARE(countKind(*ca4, ResourceKind::FilingCatalog), std::size_t{1});
    QCOMPARE(countKind(*ca4, ResourceKind::Form), std::size_t{13});
    QCOMPARE(countKind(*ca4, ResourceKind::ProcedureProfile), std::size_t{4});
    QCOMPARE(countKind(*ca4, ResourceKind::Workflow), std::size_t{4});
    QCOMPARE(authorityCount(*ca4), std::size_t{17});

    const auto local_authorities = std::ranges::find_if(ca4->resources, [](const auto& resource) {
        return resource.descriptor.id == "us.ca4.authorities.local-procedure";
    });
    QVERIFY(local_authorities != ca4->resources.end());
    for (const auto& authority_value :
         local_authorities->document.value(QStringLiteral("authorities")).toArray()) {
        const auto authority = authority_value.toObject();
        const auto sealed_guide = authority.value(QStringLiteral("authority_id")).toString() ==
                                  QStringLiteral("us.ca4.authority.sealed-materials-guide");
        QVERIFY(authority.value(QStringLiteral("official_source")).toBool());
        QCOMPARE(authority.value(QStringLiteral("source_version")).toString(),
                 sealed_guide ? QStringLiteral("2026-08-11") : QStringLiteral("2026-03-23"));
        QCOMPARE(authority.value(QStringLiteral("checked_on")).toString(),
                 QStringLiteral("2026-08-11"));
        QCOMPARE(authority.value(QStringLiteral("source_url")).toString(),
                 sealed_guide
                     ? QStringLiteral("https://www.ca4.uscourts.gov/appellateprocedureguide/"
                                      "General_Provisions/SealedConfidMem.html")
                     : QStringLiteral("https://www.ca4.uscourts.gov/docs/pdfs/rules.pdf"));
        QVERIFY(!authority.value(QStringLiteral("locator")).toString().isEmpty());
    }

    struct ExpectedProcedure final {
        QString id;
        QString workflow_id;
        QJsonArray authority_set_ids;
        QJsonArray filing_route_ids;
    };
    const std::array expected_procedures{
        ExpectedProcedure{QStringLiteral("us.ca4.procedure.agency-review"),
                          QStringLiteral("us.ca4.workflow.agency-review"),
                          QJsonArray{QStringLiteral("us.federal.authorities.appellate-rules"),
                                     QStringLiteral("us.ca4.authorities.local-procedure")},
                          QJsonArray{QStringLiteral("us.ca4.filing.agency-petition-for-review"),
                                     QStringLiteral("us.ca4.filing.agency-record"),
                                     QStringLiteral("us.ca4.filing.principal-brief")}},
        ExpectedProcedure{QStringLiteral("us.ca4.procedure.civil-appeal"),
                          QStringLiteral("us.ca4.workflow.civil-appeal"),
                          QJsonArray{QStringLiteral("us.federal.authorities.appellate-rules"),
                                     QStringLiteral("us.federal.authorities.civil-rules"),
                                     QStringLiteral("us.ca4.authorities.local-procedure")},
                          QJsonArray{QStringLiteral("us.ca4.filing.civil-notice-of-appeal"),
                                     QStringLiteral("us.ca4.filing.principal-brief")}},
        ExpectedProcedure{QStringLiteral("us.ca4.procedure.criminal-appeal"),
                          QStringLiteral("us.ca4.workflow.criminal-appeal"),
                          QJsonArray{QStringLiteral("us.federal.authorities.appellate-rules"),
                                     QStringLiteral("us.federal.authorities.criminal-rules"),
                                     QStringLiteral("us.ca4.authorities.local-procedure")},
                          QJsonArray{QStringLiteral("us.ca4.filing.criminal-notice-of-appeal"),
                                     QStringLiteral("us.ca4.filing.principal-brief")}},
        ExpectedProcedure{QStringLiteral("us.ca4.procedure.original-writ"),
                          QStringLiteral("us.ca4.workflow.original-writ"),
                          QJsonArray{QStringLiteral("us.federal.authorities.appellate-rules"),
                                     QStringLiteral("us.ca4.authorities.local-procedure")},
                          QJsonArray{QStringLiteral("us.ca4.filing.writ-petition"),
                                     QStringLiteral("us.ca4.filing.writ-response"),
                                     QStringLiteral("us.ca4.filing.motion")}},
    };

    QSet<QString> routed_filing_ids;
    const QJsonArray all_party_service_roles{QStringLiteral("us.ca4.role.initiating-party"),
                                             QStringLiteral("us.ca4.role.responding-party")};
    int calculate_docketing_operation_count = 0;
    int explicit_stage_transition_count = 0;
    for (const auto& expected : expected_procedures) {
        const auto procedure = std::ranges::find_if(ca4->resources, [&](const auto& resource) {
            return QString::fromStdString(resource.descriptor.id) == expected.id;
        });
        QVERIFY2(procedure != ca4->resources.end(), qPrintable(expected.id));
        QCOMPARE(procedure->document.value(QStringLiteral("court_id")).toString(),
                 QStringLiteral("us.ca4.court.appeals"));
        QCOMPARE(procedure->document.value(QStringLiteral("filing_catalog_id")).toString(),
                 QStringLiteral("us.ca4.filing-catalog.shared"));
        QCOMPARE(procedure->document.value(QStringLiteral("workflow_id")).toString(),
                 expected.workflow_id);
        QCOMPARE(procedure->document.value(QStringLiteral("authority_set_ids")).toArray(),
                 expected.authority_set_ids);

        const auto workflow = std::ranges::find_if(ca4->resources, [&](const auto& resource) {
            return QString::fromStdString(resource.descriptor.id) == expected.workflow_id;
        });
        QVERIFY2(workflow != ca4->resources.end(), qPrintable(expected.workflow_id));
        if (expected.workflow_id == QStringLiteral("us.ca4.workflow.agency-review")) {
            QCOMPARE(workflow->document.value(QStringLiteral("stages")).toArray(),
                     QJsonArray({QStringLiteral("us.ca4.agency.stage.opened"),
                                 QStringLiteral("us.ca4.agency.stage.record"),
                                 QStringLiteral("us.ca4.agency.stage.briefing")}));
        }
        for (const auto& operation :
             workflow->document.value(QStringLiteral("operations")).toArray()) {
            const auto operation_object = operation.toObject();
            const auto operation_id =
                operation_object.value(QStringLiteral("operation_id")).toString();
            QVERIFY(!operation_id.contains(QStringLiteral("calculate-cure")));
            const auto opcode = operation_object.value(QStringLiteral("opcode")).toString();
            if (opcode == QStringLiteral("calculate_deadline")) {
                ++calculate_docketing_operation_count;
                QVERIFY(operation_id.endsWith(QStringLiteral("calculate-docketing")));
                QCOMPARE(operation_object.value(QStringLiteral("authorized_role_ids")).toArray(),
                         QJsonArray{QStringLiteral("us.ca4.role.court")});
                const auto authority =
                    operation_object.value(QStringLiteral("authority")).toObject();
                QCOMPARE(authority.value(QStringLiteral("primary_authority_id")).toString(),
                         QStringLiteral("us.ca4.authority.local-rule-3b-docketing"));
                QCOMPARE(operation_object.value(QStringLiteral("stage_id")).toString(),
                         expected.workflow_id == QStringLiteral("us.ca4.workflow.agency-review")
                             ? QStringLiteral("us.ca4.agency.stage.record")
                         : expected.workflow_id == QStringLiteral("us.ca4.workflow.civil-appeal")
                             ? QStringLiteral("us.ca4.civil.stage.opened")
                             : QStringLiteral("us.ca4.criminal.stage.opened"));
                const QJsonArray expected_preconditions{QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                    {QStringLiteral("filing_type_id"), expected.filing_route_ids.at(0).toString()},
                    {QStringLiteral("present"), true},
                }};
                QCOMPARE(operation_object.value(QStringLiteral("preconditions")).toArray(),
                         expected_preconditions);
            }
            if (opcode == QStringLiteral("advance_stage")) {
                ++explicit_stage_transition_count;
                const auto route_controlled =
                    operation_id == QStringLiteral("us.ca4.agency.operation.advance-record") ||
                    operation_id == QStringLiteral("us.ca4.writ.operation.advance-submitted");
                QCOMPARE(operation_object.value(QStringLiteral("authorized_role_ids")).toArray(),
                         route_controlled ? QJsonArray{}
                                          : QJsonArray{QStringLiteral("us.ca4.role.court")});
                const auto prerequisite_filing_id =
                    operation_id == QStringLiteral("us.ca4.agency.operation.advance-briefing")
                        ? QStringLiteral("us.ca4.filing.agency-record")
                        : expected.filing_route_ids.at(0).toString();
                const QJsonArray expected_preconditions{QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                    {QStringLiteral("filing_type_id"), prerequisite_filing_id},
                    {QStringLiteral("present"), true},
                }};
                QCOMPARE(operation_object.value(QStringLiteral("preconditions")).toArray(),
                         route_controlled ? QJsonArray{} : expected_preconditions);
                if (operation_id.endsWith(QStringLiteral("advance-briefing"))) {
                    const auto authority =
                        operation_object.value(QStringLiteral("authority")).toObject();
                    QCOMPARE(authority.value(QStringLiteral("primary_authority_id")).toString(),
                             QStringLiteral("us.ca4.authority.local-rule-31b-briefing-order"));
                } else if (operation_id ==
                           QStringLiteral("us.ca4.agency.operation.advance-record")) {
                    const auto authority =
                        operation_object.value(QStringLiteral("authority")).toObject();
                    QCOMPARE(authority.value(QStringLiteral("primary_authority_id")).toString(),
                             QStringLiteral("us.federal.authority.frap-17-agency-record"));
                }
            }
        }
        QJsonArray actual_route_ids;
        for (const auto& route :
             workflow->document.value(QStringLiteral("filing_routes")).toArray()) {
            const auto route_object = route.toObject();
            QVERIFY(!route_object.contains(QStringLiteral("deficiency_deadline")));
            QVERIFY(!route_object.contains(QStringLiteral("accepted_deadline")));
            const auto filing_id = route_object.value(QStringLiteral("filing_type_id")).toString();
            actual_route_ids.append(filing_id);
            routed_filing_ids.insert(filing_id);
            if (filing_id == QStringLiteral("us.ca4.filing.agency-petition-for-review")) {
                const QJsonArray expected_agency_fields{
                    QStringLiteral("us.ca4.field.agency-petition.caption"),
                    QStringLiteral("us.ca4.field.agency-petition.parties-seeking-review"),
                    QStringLiteral("us.ca4.field.agency-petition.agency"),
                    QStringLiteral("us.ca4.field.agency-petition.order-reference"),
                    QStringLiteral("us.ca4.field.agency-petition.order-copy-attached"),
                    QStringLiteral("us.ca4.field.agency-petition.respondent-names-addresses"),
                };
                QCOMPARE(route_object.value(QStringLiteral("required_field_ids")).toArray(),
                         expected_agency_fields);
                QCOMPARE(route_object.value(QStringLiteral("required_service_role_ids")).toArray(),
                         QJsonArray{QStringLiteral("us.ca4.role.responding-party")});
                QCOMPARE(route_object.value(QStringLiteral("stage_id")).toString(),
                         QStringLiteral("us.ca4.agency.stage.opened"));
                QCOMPARE(route_object.value(QStringLiteral("advance_operation_id")).toString(),
                         QStringLiteral("us.ca4.agency.operation.advance-record"));
            } else if (filing_id == QStringLiteral("us.ca4.filing.agency-record")) {
                QCOMPARE(route_object.value(QStringLiteral("stage_id")).toString(),
                         QStringLiteral("us.ca4.agency.stage.record"));
                QCOMPARE(route_object.value(QStringLiteral("required_service_role_ids")).toArray(),
                         all_party_service_roles);
                QVERIFY(!route_object.contains(QStringLiteral("advance_operation_id")));
            } else if (filing_id == QStringLiteral("us.ca4.filing.principal-brief") &&
                       expected.workflow_id == QStringLiteral("us.ca4.workflow.agency-review")) {
                QCOMPARE(route_object.value(QStringLiteral("stage_id")).toString(),
                         QStringLiteral("us.ca4.agency.stage.briefing"));
                QVERIFY(!route_object.contains(QStringLiteral("advance_operation_id")));
            } else if (filing_id == QStringLiteral("us.ca4.filing.civil-notice-of-appeal") ||
                       filing_id == QStringLiteral("us.ca4.filing.criminal-notice-of-appeal")) {
                const auto civil =
                    filing_id == QStringLiteral("us.ca4.filing.civil-notice-of-appeal");
                const auto field_prefix = civil ? QStringLiteral("us.ca4.field.civil-notice.")
                                                : QStringLiteral("us.ca4.field.criminal-notice.");
                const QJsonArray expected_notice_fields{
                    field_prefix + QStringLiteral("caption"),
                    field_prefix + QStringLiteral("appealing-parties"),
                    field_prefix + QStringLiteral("originating-docket"),
                    field_prefix + QStringLiteral("judgment-or-order"),
                    field_prefix +
                        (civil ? QStringLiteral("order-date") : QStringLiteral("judgment-date")),
                    field_prefix + QStringLiteral("destination-court"),
                };
                QCOMPARE(route_object.value(QStringLiteral("required_field_ids")).toArray(),
                         expected_notice_fields);
                QCOMPARE(route_object.value(QStringLiteral("required_service_role_ids")).toArray(),
                         QJsonArray{QStringLiteral("us.ca4.role.responding-party")});
                QVERIFY(!route_object.contains(QStringLiteral("advance_operation_id")));
            } else if (filing_id == QStringLiteral("us.ca4.filing.writ-petition")) {
                const QJsonArray expected_writ_fields{
                    QStringLiteral("us.ca4.field.writ-petition.caption"),
                    QStringLiteral("us.ca4.field.writ-petition.relief"),
                    QStringLiteral("us.ca4.field.writ-petition.issues"),
                    QStringLiteral("us.ca4.field.writ-petition.necessary-facts"),
                    QStringLiteral("us.ca4.field.writ-petition.reasons"),
                    QStringLiteral("us.ca4.field.writ-petition.essential-materials"),
                    QStringLiteral("us.ca4.field.writ-petition.trial-judge-copy-provided"),
                };
                QCOMPARE(route_object.value(QStringLiteral("required_field_ids")).toArray(),
                         expected_writ_fields);
                QCOMPARE(route_object.value(QStringLiteral("required_service_role_ids")).toArray(),
                         all_party_service_roles);
                QCOMPARE(route_object.value(QStringLiteral("advance_operation_id")).toString(),
                         QStringLiteral("us.ca4.writ.operation.advance-submitted"));
            } else {
                QVERIFY(!route_object.contains(QStringLiteral("advance_operation_id")));
            }
            if (filing_id == QStringLiteral("us.ca4.filing.principal-brief") ||
                filing_id == QStringLiteral("us.ca4.filing.writ-response") ||
                filing_id == QStringLiteral("us.ca4.filing.motion")) {
                QCOMPARE(route_object.value(QStringLiteral("required_service_role_ids")).toArray(),
                         all_party_service_roles);
            }
        }
        QCOMPARE(actual_route_ids, expected.filing_route_ids);
    }
    QCOMPARE(calculate_docketing_operation_count, 3);
    QCOMPARE(explicit_stage_transition_count, 5);

    const auto writ_workflow = std::ranges::find_if(ca4->resources, [](const auto& resource) {
        return resource.descriptor.id == "us.ca4.workflow.original-writ";
    });
    QVERIFY(writ_workflow != ca4->resources.end());
    QJsonObject order_response;
    QJsonObject accept_response;
    for (const auto& operation :
         writ_workflow->document.value(QStringLiteral("operations")).toArray()) {
        const auto object = operation.toObject();
        const auto id = object.value(QStringLiteral("operation_id")).toString();
        if (id == QStringLiteral("us.ca4.writ.operation.order-response")) {
            order_response = object;
        } else if (id == QStringLiteral("us.ca4.writ.operation.accept-response")) {
            accept_response = object;
        }
    }
    QVERIFY(!order_response.isEmpty());
    QCOMPARE(order_response.value(QStringLiteral("opcode")).toString(),
             QStringLiteral("enter_order"));
    QCOMPARE(order_response.value(QStringLiteral("authorized_role_ids")).toArray(),
             QJsonArray{QStringLiteral("us.ca4.role.court")});
    const auto order_authority = order_response.value(QStringLiteral("authority")).toObject();
    QCOMPARE(order_authority.value(QStringLiteral("primary_authority_id")).toString(),
             QStringLiteral("us.federal.authority.frap-21-extraordinary-writ"));
    QCOMPARE(order_authority.value(QStringLiteral("supporting_authority_ids")).toArray(),
             QJsonArray{QStringLiteral("us.ca4.authority.local-rule-21b-writs")});
    QVERIFY(!accept_response.isEmpty());
    const QJsonArray expected_response_preconditions{QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("order_disposition")},
        {QStringLiteral("order_id"), QStringLiteral("us.ca4.writ.order.response-requested")},
        {QStringLiteral("disposition"), QStringLiteral("granted")},
    }};
    QCOMPARE(accept_response.value(QStringLiteral("preconditions")).toArray(),
             expected_response_preconditions);

    const auto filing_catalog = std::ranges::find_if(ca4->resources, [](const auto& resource) {
        return resource.descriptor.kind == ResourceKind::FilingCatalog;
    });
    QVERIFY(filing_catalog != ca4->resources.end());
    QSet<QString> catalog_filing_ids;
    for (const auto& filing : filing_catalog->document.value(QStringLiteral("filings")).toArray()) {
        catalog_filing_ids.insert(filing.toObject().value(QStringLiteral("filing_id")).toString());
    }
    QCOMPARE(catalog_filing_ids.size(), 13);
    auto inventory_only_filing_ids = catalog_filing_ids;
    inventory_only_filing_ids.subtract(routed_filing_ids);
    const QSet<QString> expected_inventory_only{
        QStringLiteral("us.ca4.filing.appearance"),
        QStringLiteral("us.ca4.filing.appendix"),
        QStringLiteral("us.ca4.filing.docketing-statement"),
        QStringLiteral("us.ca4.filing.rehearing-petition"),
        QStringLiteral("us.ca4.filing.transcript-order"),
    };
    QVERIFY(inventory_only_filing_ids == expected_inventory_only);

    QSet<QString> proceeding_types;
    std::set<std::string> resource_ids;
    for (const auto* pack : {&*federal, &*ca4, &*bench}) {
        for (const auto& resource : pack->resources) {
            QVERIFY(resource_ids.insert(resource.descriptor.id).second);
            if (resource.descriptor.kind == ResourceKind::ProcedureProfile) {
                proceeding_types.insert(
                    resource.document.value(QStringLiteral("proceeding_type")).toString());
            }
        }
    }
    const QSet<QString> expected_proceeding_types{
        QStringLiteral("civil_appeal"), QStringLiteral("criminal_appeal"),
        QStringLiteral("agency_review"), QStringLiteral("original_writ")};
    QVERIFY(proceeding_types == expected_proceeding_types);
}

void M4FoundationPacksTest::archivesAreDeterministicAndResolveExactly() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto checked_federal = checkedInArchive(
        QStringLiteral("us-federal"), QStringLiteral("foundation-us-federal-2025.12.01.awpack"));
    const auto checked_ca4 = checkedInArchive(
        QStringLiteral("us-ca4"), QStringLiteral("foundation-us-ca4-2026.03.23.awpack"));
    const auto checked_bench =
        checkedInArchive(QStringLiteral("us-ca4-fictional-bench"),
                         QStringLiteral("foundation-us-ca4-fictional-bench-1.0.0.awpack"));
    QCOMPARE(QCryptographicHash::hash(readAll(checked_federal), QCryptographicHash::Sha256).toHex(),
             QByteArray(federal_archive_sha256));
    QCOMPARE(QCryptographicHash::hash(readAll(checked_ca4), QCryptographicHash::Sha256).toHex(),
             QByteArray(ca4_archive_sha256));
    QCOMPARE(QCryptographicHash::hash(readAll(checked_bench), QCryptographicHash::Sha256).toHex(),
             QByteArray(bench_archive_sha256));
    const auto federal_a = QDir(output.path()).filePath(QStringLiteral("federal-a.awpack"));
    const auto federal_b = QDir(output.path()).filePath(QStringLiteral("federal-b.awpack"));
    const auto ca4_a = QDir(output.path()).filePath(QStringLiteral("ca4-a.awpack"));
    const auto ca4_b = QDir(output.path()).filePath(QStringLiteral("ca4-b.awpack"));
    const auto bench_a = QDir(output.path()).filePath(QStringLiteral("bench-a.awpack"));
    const auto bench_b = QDir(output.path()).filePath(QStringLiteral("bench-b.awpack"));

    const auto exported_federal_a =
        PackArchive::exportDirectory(foundation(QStringLiteral("us-federal")), federal_a);
    QVERIFY2(exported_federal_a.has_value(),
             exported_federal_a ? "" : qPrintable(exported_federal_a.error().message));
    const auto exported_federal_b =
        PackArchive::exportDirectory(foundation(QStringLiteral("us-federal")), federal_b);
    QVERIFY2(exported_federal_b.has_value(),
             exported_federal_b ? "" : qPrintable(exported_federal_b.error().message));
    QCOMPARE(*exported_federal_a, *exported_federal_b);
    QCOMPARE(readAll(federal_a), readAll(federal_b));
    QVERIFY(!readAll(checked_federal).isEmpty());
    QCOMPARE(readAll(federal_a), readAll(checked_federal));

    const auto exported_ca4_a = PackArchive::exportDirectory(
        foundation(QStringLiteral("us-ca4")), ca4_a, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_ca4_a.has_value(),
             exported_ca4_a ? "" : qPrintable(exported_ca4_a.error().message));
    const auto exported_ca4_b = PackArchive::exportDirectory(
        foundation(QStringLiteral("us-ca4")), ca4_b, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_ca4_b.has_value(),
             exported_ca4_b ? "" : qPrintable(exported_ca4_b.error().message));
    QCOMPARE(*exported_ca4_a, *exported_ca4_b);
    QCOMPARE(readAll(ca4_a), readAll(ca4_b));
    QVERIFY(!readAll(checked_ca4).isEmpty());
    QCOMPARE(readAll(ca4_a), readAll(checked_ca4));

    const auto exported_bench_a =
        PackArchive::exportDirectory(foundation(QStringLiteral("us-ca4-fictional-bench")), bench_a);
    QVERIFY2(exported_bench_a.has_value(),
             exported_bench_a ? "" : qPrintable(exported_bench_a.error().message));
    const auto exported_bench_b =
        PackArchive::exportDirectory(foundation(QStringLiteral("us-ca4-fictional-bench")), bench_b);
    QVERIFY2(exported_bench_b.has_value(),
             exported_bench_b ? "" : qPrintable(exported_bench_b.error().message));
    QCOMPARE(*exported_bench_a, *exported_bench_b);
    QCOMPARE(readAll(bench_a), readAll(bench_b));
    QVERIFY(!readAll(checked_bench).isEmpty());
    QCOMPARE(readAll(bench_a), readAll(checked_bench));

    const auto imported_federal = PackArchive::importArchive(checked_federal);
    QVERIFY2(imported_federal.has_value(),
             imported_federal ? "" : qPrintable(imported_federal.error().message));
    QCOMPARE(imported_federal->revision, *exported_federal_a);
    const auto imported_ca4 =
        PackArchive::importArchive(checked_ca4, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(imported_ca4.has_value(),
             imported_ca4 ? "" : qPrintable(imported_ca4.error().message));
    QCOMPARE(imported_ca4->revision, *exported_ca4_a);
    const auto imported_bench = PackArchive::importArchive(checked_bench);
    QVERIFY2(imported_bench.has_value(),
             imported_bench ? "" : qPrintable(imported_bench.error().message));
    QCOMPARE(imported_bench->revision, *exported_bench_a);

    const auto directory_federal =
        PackReader::readDirectory(foundation(QStringLiteral("us-federal")));
    QVERIFY2(directory_federal.has_value(),
             directory_federal ? "" : qPrintable(directory_federal.error().message));
    QCOMPARE(imported_federal->revision, directory_federal->revision);
    const auto directory_ca4 = PackReader::readDirectory(foundation(QStringLiteral("us-ca4")),
                                                         PackValidationScope::ResolvedClosure);
    QVERIFY2(directory_ca4.has_value(),
             directory_ca4 ? "" : qPrintable(directory_ca4.error().message));
    QCOMPARE(imported_ca4->revision, directory_ca4->revision);
    const auto directory_bench =
        PackReader::readDirectory(foundation(QStringLiteral("us-ca4-fictional-bench")));
    QVERIFY2(directory_bench.has_value(),
             directory_bench ? "" : qPrintable(directory_bench.error().message));
    QCOMPARE(imported_bench->revision, directory_bench->revision);

    const auto catalog_path = QDir(output.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_path);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto installed_federal =
        (*catalog)->installArchive(checked_federal, QStringLiteral("2026-08-11T00:00:01Z"));
    QVERIFY2(installed_federal.has_value(),
             installed_federal ? "" : qPrintable(installed_federal.error().message));
    const auto installed_bench =
        (*catalog)->installArchive(checked_bench, QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY2(installed_bench.has_value(),
             installed_bench ? "" : qPrintable(installed_bench.error().message));
    const auto installed_ca4 =
        (*catalog)->installArchive(checked_ca4, QStringLiteral("2026-08-11T00:00:03Z"));
    QVERIFY2(installed_ca4.has_value(),
             installed_ca4 ? "" : qPrintable(installed_ca4.error().message));

    const auto resolved = (*catalog)->loadResolved(*exported_ca4_a);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    QCOMPARE(resolved->root().revision, *exported_ca4_a);
    QCOMPARE(resolved->dependenciesDependencyFirst().size(), std::size_t{1});
    QCOMPARE(resolved->dependenciesDependencyFirst().front().revision, *exported_federal_a);
    QCOMPARE(resolved->revisionsByPackId().size(), std::size_t{2});
    QCOMPARE(resolved->resourceOwner("us.federal.authorities.appellate-rules"),
             std::optional<PackRevision>(*exported_federal_a));
    QCOMPARE(resolved->resourceOwner("us.ca4.procedure.original-writ"),
             std::optional<PackRevision>(*exported_ca4_a));
}

} // namespace

QTEST_GUILESS_MAIN(M4FoundationPacksTest)

#include "tst_m4_foundation_packs.moc"
