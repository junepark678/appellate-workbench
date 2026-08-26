#include "independent_review_publisher_p.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using appellate::cli::detail::encodeIndependentReviewDestinationPath;
using appellate::cli::detail::encodeIndependentReviewHandoffPath;
using appellate::cli::detail::encodeIndependentReviewPathSpelling;
using appellate::cli::detail::IndependentReviewArtifactKind;
using appellate::cli::detail::IndependentReviewInputReaderHooks;
using appellate::cli::detail::IndependentReviewProtectedDirectory;
using appellate::cli::detail::IndependentReviewPublicationError;
using appellate::cli::detail::IndependentReviewPublicationErrorCode;
using appellate::cli::detail::IndependentReviewPublicationMember;
using appellate::cli::detail::IndependentReviewPublicationRequest;
using appellate::cli::detail::IndependentReviewPublisherEvent;
using appellate::cli::detail::IndependentReviewPublisherHooks;
using appellate::cli::detail::IndependentReviewPublisherInjectedAction;
using appellate::cli::detail::IndependentReviewPublisherInjectedOutcome;
using appellate::cli::detail::IndependentReviewPublisherObservation;
using appellate::cli::detail::IndependentReviewPublisherReport;
using appellate::cli::detail::IndependentReviewPublisherSyntheticNodeType;
using appellate::cli::detail::IndependentReviewPublisherSyntheticStat;
using appellate::cli::detail::IndependentReviewStagedValidationError;
using appellate::cli::detail::IndependentReviewStagedValidationErrorCode;
using appellate::cli::detail::publishIndependentReviewArtifacts;
using appellate::cli::detail::readIndependentReviewDeclaration;
using appellate::cli::detail::readIndependentReviewHandoffDirectory;
using appellate::cli::detail::readIndependentReviewStagedHandoffDirectory;

using PublicationResult = std::expected<void, IndependentReviewPublicationError>;

constexpr auto prepared_handoff_bytes = QByteArrayView{"{\"handoff\":true}\n"};
constexpr auto declaration_template_bytes = QByteArrayView{"{\"template\":true}\n"};
constexpr auto final_manifest_bytes = QByteArrayView{"{\"manifest\":true}\n"};
constexpr auto final_review_bytes = QByteArrayView{"{\"review\":true}\n"};

[[maybe_unused, nodiscard]] IndependentReviewPublisherSyntheticStat
syntheticStat(IndependentReviewPublisherSyntheticNodeType type, std::uint64_t owner,
              std::uint32_t mode, std::uint64_t link_count = 1) {
    IndependentReviewPublisherSyntheticStat result;
    result.type = type;
    result.owner = owner;
    result.mode = mode;
    result.link_count = link_count;
    return result;
}

class IndependentReviewPublisherTest final : public QObject {
    Q_OBJECT

  private slots:
    void publishesPreparedHandoffWithFixedTree();
    void publishesFinalizedPackWithFixedTree();
    void normalizesPublicationModesUnderHostileUmask();
    void preservesUnnormalizedStagingRoot();
    void retainsCreationDescriptorsThroughPublication();
    void preflightsModeNormalizationBeforeStaging();
    void rejectsExistingAndProtectedDestinations();
    void boundsProtectedInventoryBeforeStaging();
    void exhaustsStagingCollisionsWithoutResidue();
    void cleansEverySafelyRetainedPartialTree();
    void cleansEveryMidWritePrefix();
    void cleansInvalidStagedArtifact();
    void reportsCleanupFailureWithExactLedger();
    void reconcilesSourceRetainedRenameErrors();
    void preservesRacingDestinationAndCleansSource();
    void preservesMovedDestinationWhenRenameReportsError();
    void reportsPostRenameDurabilityFailure();
    void reportsIdentityFailureAfterDestinationMutation();
    void rejectsSpecialAndReboundReaderInputs();
    void rejectsEnvironmentallyInfeasibleRelativeHandoffBeforeAccess();
    void reusesEncodedOperandsAcrossEnvironmentalCapture();
    void preservesUnretainedStagingWithUnknownTelemetry();
    void classifiesStagedValidationFailures();
    void mapsInjectedControllerAclAndLeaseOutcomes();
    void retriesInjectedTransientSyscallOutcomes();
    void mapsInjectedMutationOutcomes();
    void exhaustsSyntheticControllerPolicy();
    void mapsSyntheticCreationMetadata();
    void rejectsSyntheticPostRecordMetadataChanges();
    void rejectsInjectedAndObservedEntryRebinding();
    void closesFinalTreeBindingWindow();
    void preservesCleanupSeamReplacements();
    void reportsInjectedCleanupTelemetryMatrix();
    void exhaustsSyntheticRenameReconciliationStates();
    void reconcilesInjectedRenameAndSyncOutcomes();
};

[[nodiscard]] auto validationFailure(QString message)
    -> std::unexpected<IndependentReviewStagedValidationError> {
    return std::unexpected(IndependentReviewStagedValidationError{std::move(message)});
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[maybe_unused, nodiscard]] bool writeNew(const QString& path, QByteArrayView bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes.data(), bytes.size()) == bytes.size() && file.flush();
}

[[nodiscard]] QStringList entriesAt(const QString& path) {
    return QDir(path).entryList(
        QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

#if defined(Q_OS_LINUX)
[[nodiscard]] std::optional<struct stat> statusAt(const QString& path) {
    struct stat status{};
    const auto encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &status) != 0) {
        return std::nullopt;
    }
    return status;
}

class ScopedUmask final {
  public:
    explicit ScopedUmask(mode_t mask) : original_(::umask(mask)) {}
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;
    ~ScopedUmask() { ::umask(original_); }

  private:
    mode_t original_{};
};

[[nodiscard]] bool createPreparedTree(const QString& root) {
    const auto handoff = QDir(root).filePath(QStringLiteral("handoff.json"));
    const auto declaration =
        QDir(root).filePath(QStringLiteral("review-declaration.template.json"));
    return QDir().mkdir(root) && ::chmod(QFile::encodeName(root).constData(), 0700) == 0 &&
           writeNew(handoff, prepared_handoff_bytes) &&
           ::chmod(QFile::encodeName(handoff).constData(), 0600) == 0 &&
           writeNew(declaration, declaration_template_bytes) &&
           ::chmod(QFile::encodeName(declaration).constData(), 0600) == 0;
}

[[nodiscard]] bool retainedCreationsAreExact(const IndependentReviewPublisherReport& report,
                                             IndependentReviewArtifactKind kind,
                                             std::size_t expected_count) {
    if (report.retained_creations.size() != expected_count) {
        return false;
    }
    std::vector<int> descriptors;
    descriptors.reserve(expected_count);
    for (const auto& creation : report.retained_creations) {
        if (creation.ordinal >= expected_count || creation.descriptor < 0 ||
            std::ranges::find(descriptors, creation.descriptor) != descriptors.end()) {
            return false;
        }
        struct stat status{};
        const auto flags = ::fcntl(creation.descriptor, F_GETFL);
        if (flags < 0 || ::fstat(creation.descriptor, &status) != 0 ||
            static_cast<std::uint64_t>(status.st_dev) != creation.device ||
            static_cast<std::uint64_t>(status.st_ino) != creation.inode) {
            return false;
        }
        const auto directory =
            creation.ordinal == 0 ||
            (kind == IndependentReviewArtifactKind::FinalizedPack && creation.ordinal == 1);
        if (directory != ((flags & O_PATH) != 0)) {
            return false;
        }
        descriptors.push_back(creation.descriptor);
    }
    return true;
}
#endif

[[nodiscard]] auto validateModeAndOwner(const QString& path, bool directory, std::uint32_t mode)
    -> std::expected<void, IndependentReviewStagedValidationError> {
#if defined(Q_OS_LINUX)
    const auto status = statusAt(path);
    if (!status) {
        return validationFailure(QStringLiteral("Missing staged path: %1").arg(path));
    }
    if ((directory && !S_ISDIR(status->st_mode)) || (!directory && !S_ISREG(status->st_mode)) ||
        static_cast<std::uint32_t>(status->st_mode & 07777) != mode ||
        status->st_uid != ::geteuid() || (!directory && status->st_nlink != 1)) {
        return validationFailure(QStringLiteral("Wrong staged metadata: %1").arg(path));
    }
    return {};
#else
    static_cast<void>(path);
    static_cast<void>(directory);
    static_cast<void>(mode);
    return validationFailure(QStringLiteral("Publisher metadata tests require Linux"));
#endif
}

[[nodiscard]] auto validatePreparedTree(const QString& root)
    -> std::expected<void, IndependentReviewStagedValidationError> {
    const auto expected_entries = QStringList{QStringLiteral("handoff.json"),
                                              QStringLiteral("review-declaration.template.json")};
    if (entriesAt(root) != expected_entries) {
        return validationFailure(QStringLiteral("Prepared inventory is not exact"));
    }
    if (const auto mode = validateModeAndOwner(root, true, 0700); !mode) {
        return mode;
    }
    const auto handoff = QDir(root).filePath(QStringLiteral("handoff.json"));
    const auto declaration =
        QDir(root).filePath(QStringLiteral("review-declaration.template.json"));
    if (const auto mode = validateModeAndOwner(handoff, false, 0600); !mode) {
        return mode;
    }
    if (const auto mode = validateModeAndOwner(declaration, false, 0600); !mode) {
        return mode;
    }
    if (readAll(handoff) != prepared_handoff_bytes ||
        readAll(declaration) != declaration_template_bytes) {
        return validationFailure(QStringLiteral("Prepared bytes are not exact"));
    }
    return {};
}

[[nodiscard]] auto validateFinalizedTree(const QString& root)
    -> std::expected<void, IndependentReviewStagedValidationError> {
    const auto expected_entries =
        QStringList{QStringLiteral("manifest.json"), QStringLiteral("resources")};
    if (entriesAt(root) != expected_entries) {
        return validationFailure(QStringLiteral("Finalized root inventory is not exact"));
    }
    const auto resources = QDir(root).filePath(QStringLiteral("resources"));
    if (entriesAt(resources) != QStringList{QStringLiteral("realism-review.json")}) {
        return validationFailure(QStringLiteral("Finalized resource inventory is not exact"));
    }
    const auto manifest = QDir(root).filePath(QStringLiteral("manifest.json"));
    const auto review = resources + QStringLiteral("/realism-review.json");
    if (const auto mode = validateModeAndOwner(root, true, 0700); !mode) {
        return mode;
    }
    if (const auto mode = validateModeAndOwner(resources, true, 0700); !mode) {
        return mode;
    }
    if (const auto mode = validateModeAndOwner(manifest, false, 0600); !mode) {
        return mode;
    }
    if (const auto mode = validateModeAndOwner(review, false, 0600); !mode) {
        return mode;
    }
    if (readAll(manifest) != final_manifest_bytes || readAll(review) != final_review_bytes) {
        return validationFailure(QStringLiteral("Finalized bytes are not exact"));
    }
    return {};
}

[[nodiscard]] std::vector<IndependentReviewPublicationMember>
membersFor(IndependentReviewArtifactKind kind) {
    if (kind == IndependentReviewArtifactKind::PreparedHandoff) {
        return {
            {QStringLiteral("handoff.json"), QByteArray{prepared_handoff_bytes}},
            {QStringLiteral("review-declaration.template.json"),
             QByteArray{declaration_template_bytes}},
        };
    }
    return {
        {QStringLiteral("manifest.json"), QByteArray{final_manifest_bytes}},
        {QStringLiteral("resources/realism-review.json"), QByteArray{final_review_bytes}},
    };
}

[[maybe_unused, nodiscard]] IndependentReviewPublicationRequest
requestFor(IndependentReviewArtifactKind kind, const QString& destination,
           qsizetype* validation_calls = nullptr) {
    IndependentReviewPublicationRequest request;
    request.kind = kind;
    request.destination_path = destination;
    request.members = membersFor(kind);
    request.validate_staged = [kind, validation_calls](const QString& root) {
        if (validation_calls != nullptr) {
            ++*validation_calls;
        }
        return kind == IndependentReviewArtifactKind::PreparedHandoff ? validatePreparedTree(root)
                                                                      : validateFinalizedTree(root);
    };
    return request;
}

template <typename Value>
void requireErrorCode(const std::expected<Value, IndependentReviewPublicationError>& result,
                      IndependentReviewPublicationErrorCode expected) {
    QVERIFY2(!result.has_value(), "Publication unexpectedly succeeded");
    if (result.has_value()) {
        return;
    }
    QCOMPARE(static_cast<int>(result.error().code), static_cast<int>(expected));
}

[[maybe_unused, nodiscard]] qsizetype countEvent(const IndependentReviewPublisherReport& report,
                                                 IndependentReviewPublisherEvent event) {
    return static_cast<qsizetype>(std::ranges::count_if(
        report.observations, [event](const IndependentReviewPublisherObservation& observation) {
            return observation.event == event;
        }));
}

[[maybe_unused, nodiscard]] QList<QByteArray>
creationComponents(const IndependentReviewPublisherReport& report) {
    QList<QByteArray> result;
    for (const auto& observation : report.observations) {
        if (observation.event == IndependentReviewPublisherEvent::StagingCreated ||
            observation.event == IndependentReviewPublisherEvent::DirectoryCreated ||
            observation.event == IndependentReviewPublisherEvent::FileCreated) {
            result.push_back(observation.component);
        }
    }
    return result;
}

[[maybe_unused]] void requireSuccessfulTree(const PublicationResult& result,
                                            const IndependentReviewPublisherReport& report,
                                            const QString& destination) {
    QVERIFY2(result.has_value(), result.has_value() ? "" : qPrintable(result.error().message));
    QCOMPARE(report.destination_path, destination);
    QVERIFY(!report.staging_path.isEmpty());
    QVERIFY(!QFileInfo::exists(report.staging_path));
    QVERIFY(report.remaining_ledger_paths.isEmpty());
}

void IndependentReviewPublisherTest::publishesPreparedHandoffWithFixedTree() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("handoff"));
    qsizetype validation_calls = 0;
    auto request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination, &validation_calls);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"abc123"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireSuccessfulTree(result, report, destination);
    QCOMPARE(validation_calls, 2);
    const auto validated = validatePreparedTree(destination);
    QVERIFY2(validated.has_value(),
             validated.has_value() ? "" : qPrintable(validated.error().message));
    QCOMPARE(entriesAt(parent.path()), QStringList{QStringLiteral("handoff")});
    QCOMPARE(creationComponents(report),
             (QList<QByteArray>{QByteArray{".handoff.appellate-independent-review-abc123"},
                                QByteArray{"handoff.json"},
                                QByteArray{"review-declaration.template.json"}}));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ModeNormalized), 3);
#endif
}

void IndependentReviewPublisherTest::publishesFinalizedPackWithFixedTree() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("review-pack"));
    qsizetype validation_calls = 0;
    auto request =
        requestFor(IndependentReviewArtifactKind::FinalizedPack, destination, &validation_calls);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"def456"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireSuccessfulTree(result, report, destination);
    QCOMPARE(validation_calls, 2);
    const auto validated = validateFinalizedTree(destination);
    QVERIFY2(validated.has_value(),
             validated.has_value() ? "" : qPrintable(validated.error().message));
    QCOMPARE(entriesAt(parent.path()), QStringList{QStringLiteral("review-pack")});
    QCOMPARE(creationComponents(report),
             (QList<QByteArray>{QByteArray{".review-pack.appellate-independent-review-def456"},
                                QByteArray{"resources"}, QByteArray{"manifest.json"},
                                QByteArray{"realism-review.json"}}));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ModeNormalized), 4);
#endif
}

void IndependentReviewPublisherTest::normalizesPublicationModesUnderHostileUmask() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const auto mask : {mode_t{0000}, mode_t{0777}}) {
        for (const auto kind : {IndependentReviewArtifactKind::PreparedHandoff,
                                IndependentReviewArtifactKind::FinalizedPack}) {
            QTemporaryDir parent;
            QVERIFY(parent.isValid());
            const auto leaf = kind == IndependentReviewArtifactKind::PreparedHandoff
                                  ? QStringLiteral("handoff")
                                  : QStringLiteral("review-pack");
            const auto destination = QDir(parent.path()).filePath(leaf);
            auto request = requestFor(kind, destination);
            IndependentReviewPublisherHooks hooks;
            hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"umask1"}; };

            PublicationResult result;
            {
                const ScopedUmask scoped_umask(mask);
                result = publishIndependentReviewArtifacts(request, hooks);
            }

            QVERIFY2(result.has_value(),
                     result.has_value() ? "" : qPrintable(result.error().message));
            const auto validated = kind == IndependentReviewArtifactKind::PreparedHandoff
                                       ? validatePreparedTree(destination)
                                       : validateFinalizedTree(destination);
            QVERIFY2(validated.has_value(),
                     validated.has_value() ? "" : qPrintable(validated.error().message));
            QCOMPARE(entriesAt(parent.path()), QStringList{leaf});
        }
    }
#endif
}

void IndependentReviewPublisherTest::preservesUnnormalizedStagingRoot() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"mode00"}; };
    hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        return observation.event == IndependentReviewPublisherEvent::ModeNormalizeAttempted &&
                       observation.ordinal == 0
                   ? std::optional{IndependentReviewPublisherInjectedOutcome{false, false, EIO}}
                   : std::nullopt;
    };
    hooks.report = &report;

    PublicationResult result;
    {
        const ScopedUmask scoped_umask(0777);
        result = publishIndependentReviewArtifacts(request, hooks);
    }

    requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
    QVERIFY(!result.has_value());
    if (!result.has_value()) {
        QCOMPARE(result.error().message,
                 QStringLiteral("original_staging_reachability=unknown;"
                                "cleanup_residue=present;parent_fsync=not_attempted"));
    }
    const auto staging_status = statusAt(report.staging_path);
    QVERIFY(staging_status.has_value());
    QCOMPARE(static_cast<std::uint32_t>(staging_status->st_mode & 07777), std::uint32_t{0000});
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CleanupRemoved), 0);
    QVERIFY(!QFileInfo::exists(destination));

    QCOMPARE(::chmod(QFile::encodeName(report.staging_path).constData(), 0700), 0);
    QVERIFY(QDir(report.staging_path).removeRecursively());

    QTemporaryDir exact_parent;
    QVERIFY(exact_parent.isValid());
    const auto exact_destination =
        QDir(exact_parent.path()).filePath(QStringLiteral("exact-output"));
    auto exact_request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, exact_destination);
    report = {};
    hooks.report = &report;
    {
        const ScopedUmask scoped_umask(0000);
        result = publishIndependentReviewArtifacts(exact_request, hooks);
    }

    requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
    QVERIFY(!QFileInfo::exists(exact_destination));
    QVERIFY(!QFileInfo::exists(report.staging_path));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CleanupRemoved), 1);
    QVERIFY(report.remaining_ledger_paths.isEmpty());
    QVERIFY(entriesAt(exact_parent.path()).isEmpty());
#endif
}

void IndependentReviewPublisherTest::retainsCreationDescriptorsThroughPublication() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const auto kind : {IndependentReviewArtifactKind::PreparedHandoff,
                            IndependentReviewArtifactKind::FinalizedPack}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(kind, destination);
        const auto expected_count = kind == IndependentReviewArtifactKind::PreparedHandoff
                                        ? std::size_t{3}
                                        : std::size_t{4};
        bool descriptors_exact = true;
        std::array<std::size_t, 4> checkpoint_counts{};
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"retain"}; };
        hooks.barrier = [&report, kind, expected_count, &descriptors_exact, &checkpoint_counts](
                            const IndependentReviewPublisherObservation& observation) {
            std::optional<std::size_t> checkpoint;
            switch (observation.event) {
            case IndependentReviewPublisherEvent::AfterStagedValidation:
                checkpoint = 0;
                break;
            case IndependentReviewPublisherEvent::BeforeRename:
                checkpoint = 1;
                break;
            case IndependentReviewPublisherEvent::Reconciled:
                checkpoint = 2;
                break;
            case IndependentReviewPublisherEvent::ParentSync:
                checkpoint = 3;
                break;
            default:
                break;
            }
            if (checkpoint.has_value()) {
                ++checkpoint_counts.at(*checkpoint);
                descriptors_exact =
                    descriptors_exact && retainedCreationsAreExact(report, kind, expected_count);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        QVERIFY(descriptors_exact);
        QVERIFY(report.retained_creations_verified_at_completion);
        QCOMPARE(checkpoint_counts.at(0), std::size_t{2});
        QCOMPARE(checkpoint_counts.at(1), std::size_t{1});
        QCOMPARE(checkpoint_counts.at(2), std::size_t{1});
        QCOMPARE(checkpoint_counts.at(3), std::size_t{1});
    }
#endif
}

void IndependentReviewPublisherTest::preflightsModeNormalizationBeforeStaging() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const auto native_error : {ENOSYS, EINVAL, EOPNOTSUPP, EIO}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.outcome = [native_error](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            return observation.event == IndependentReviewPublisherEvent::ModeNormalizePreflight
                       ? std::optional{IndependentReviewPublisherInjectedOutcome{false, false,
                                                                                 native_error}}
                       : std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result,
                         native_error == EIO
                             ? IndependentReviewPublicationErrorCode::CannotPublishDestination
                             : IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform);
        QVERIFY(report.staging_path.isEmpty());
        QVERIFY(entriesAt(parent.path()).isEmpty());
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ModeNormalizePreflight), 1);
    }

    QTemporaryDir retry_parent;
    QVERIFY(retry_parent.isValid());
    const auto retry_destination =
        QDir(retry_parent.path()).filePath(QStringLiteral("retry-output"));
    auto retry_request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, retry_destination);
    std::size_t attempts = 0;
    IndependentReviewPublisherHooks retry_hooks;
    retry_hooks.outcome = [&attempts](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        if (observation.event != IndependentReviewPublisherEvent::ModeNormalizePreflight ||
            attempts >= 2) {
            return std::nullopt;
        }
        return attempts++ == 0
                   ? std::optional{IndependentReviewPublisherInjectedOutcome{false, false, EINTR}}
                   : std::optional{IndependentReviewPublisherInjectedOutcome{true, false, 0}};
    };

    const auto retry_result = publishIndependentReviewArtifacts(retry_request, retry_hooks);

    QVERIFY2(retry_result.has_value(),
             retry_result.has_value() ? "" : qPrintable(retry_result.error().message));
    QCOMPARE(attempts, std::size_t{2});
    QVERIFY(QFileInfo(retry_destination).isDir());
#endif
}

void IndependentReviewPublisherTest::rejectsExistingAndProtectedDestinations() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const bool existing_directory : {false, true}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("existing"));
        if (existing_directory) {
            QVERIFY(QDir().mkdir(destination));
        } else {
            QVERIFY(writeNew(destination, QByteArrayView{"preserve me"}));
        }
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"exists"}; };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::DestinationExists);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::NameCandidate), 0);
        QVERIFY(QFileInfo::exists(destination));
        if (!existing_directory) {
            QCOMPARE(readAll(destination), QByteArray{"preserve me"});
        }
    }

    QTemporaryDir protected_root;
    QVERIFY(protected_root.isValid());
    const auto existing_precedence =
        QDir(protected_root.path()).filePath(QStringLiteral("already-there"));
    QVERIFY(writeNew(existing_precedence, QByteArrayView{"preserve"}));
    auto existing_request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, existing_precedence);
    existing_request.protected_directory_paths = {protected_root.path()};
    IndependentReviewPublisherReport existing_report;
    IndependentReviewPublisherHooks existing_hooks;
    existing_hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        if (observation.event != IndependentReviewPublisherEvent::ControllerOpened) {
            return std::nullopt;
        }
        IndependentReviewPublisherSyntheticStat unsafe;
        unsafe.owner = static_cast<std::uint64_t>(::geteuid()) + 1U;
        return IndependentReviewPublisherInjectedOutcome{true, false, 0, unsafe};
    };
    existing_hooks.report = &existing_report;

    const auto existing_precedence_result =
        publishIndependentReviewArtifacts(existing_request, existing_hooks);

    requireErrorCode(existing_precedence_result,
                     IndependentReviewPublicationErrorCode::DestinationExists);
    QCOMPARE(readAll(existing_precedence), QByteArray{"preserve"});
    QCOMPARE(countEvent(existing_report, IndependentReviewPublisherEvent::ProtectedInventory), 0);
    QVERIFY(QFile::remove(existing_precedence));

    const auto destination =
        QDir(protected_root.path()).filePath(QStringLiteral("must-not-publish-here"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    request.protected_directory_paths = {protected_root.path()};
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"inside"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result,
                     IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput);
    QVERIFY(!QFileInfo::exists(destination));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::NameCandidate), 0);
    QVERIFY(entriesAt(protected_root.path()).isEmpty());

    const auto protected_status = statusAt(protected_root.path());
    QVERIFY(protected_status.has_value());
    request.protected_directory_paths.clear();
    request.protected_directories = {
        IndependentReviewProtectedDirectory{static_cast<std::uint64_t>(protected_status->st_dev),
                                            static_cast<std::uint64_t>(protected_status->st_ino)}};
    report = {};
    const auto retained_identity_result = publishIndependentReviewArtifacts(request, hooks);
    requireErrorCode(retained_identity_result,
                     IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput);
    QVERIFY(!QFileInfo::exists(destination));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::NameCandidate), 0);

    report = {};
    hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        if (observation.event != IndependentReviewPublisherEvent::ControllerOpened) {
            return std::nullopt;
        }
        IndependentReviewPublisherSyntheticStat unsafe;
        unsafe.owner = static_cast<std::uint64_t>(::geteuid()) + 1U;
        return IndependentReviewPublisherInjectedOutcome{true, false, 0, unsafe};
    };
    const auto precedence_result = publishIndependentReviewArtifacts(request, hooks);
    requireErrorCode(precedence_result,
                     IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput);
    QVERIFY(!QFileInfo::exists(destination));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::NameCandidate), 0);
#endif
}

void IndependentReviewPublisherTest::boundsProtectedInventoryBeforeStaging() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir protected_root;
    QTemporaryDir destination_parent;
    QVERIFY(protected_root.isValid());
    QVERIFY(destination_parent.isValid());
    const auto root_descriptor = ::open(QFile::encodeName(protected_root.path()).constData(),
                                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    QVERIFY(root_descriptor >= 0);
    constexpr std::size_t excessive_entry_count = 20'001;
    for (std::size_t index = 0; index < excessive_entry_count; ++index) {
        const auto name = QByteArray::number(static_cast<qulonglong>(index)).rightJustified(5, '0');
        const auto descriptor =
            ::openat(root_descriptor, name.constData(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        QVERIFY(descriptor >= 0);
        QCOMPARE(::close(descriptor), 0);
    }
    QCOMPARE(::close(root_descriptor), 0);
    const auto destination = QDir(destination_parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    request.protected_directory_paths = {protected_root.path()};
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result,
                     IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput);
    QVERIFY(report.staging_path.isEmpty());
    QVERIFY(!QFileInfo::exists(destination));

    QVERIFY(QFile::remove(
        QDir(protected_root.path())
            .filePath(QString::number(static_cast<qulonglong>(excessive_entry_count - 1))
                          .rightJustified(5, u'0'))));
    const auto boundary_destination =
        QDir(destination_parent.path()).filePath(QStringLiteral("boundary-output"));
    request.destination_path = boundary_destination;
    report = {};
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"bound1"}; };

    const auto boundary_result = publishIndependentReviewArtifacts(request, hooks);

    requireSuccessfulTree(boundary_result, report, boundary_destination);
#endif
}

void IndependentReviewPublisherTest::exhaustsStagingCollisionsWithoutResidue() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    const auto collision =
        QDir(parent.path()).filePath(QStringLiteral(".output.appellate-independent-review-repeat"));
    QVERIFY(QDir().mkdir(collision));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"repeat"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::NameCandidate), 128);
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::StagingCreated), 0);
    QVERIFY(!QFileInfo::exists(destination));
    QVERIFY(QFileInfo(collision).isDir());
    QVERIFY(entriesAt(collision).isEmpty());
    QCOMPARE(entriesAt(parent.path()),
             QStringList{QStringLiteral(".output.appellate-independent-review-repeat")});
    QVERIFY(report.remaining_ledger_paths.isEmpty());
#endif
}

void IndependentReviewPublisherTest::cleansEverySafelyRetainedPartialTree() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Fault final {
        IndependentReviewArtifactKind kind;
        IndependentReviewPublisherEvent event;
        std::size_t ordinal;
        IndependentReviewPublisherInjectedAction action;
    };
    const std::array faults{
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              IndependentReviewPublisherEvent::ModeNormalized, 0,
              IndependentReviewPublisherInjectedAction::FailAfter},
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              IndependentReviewPublisherEvent::NameCandidate, 0,
              IndependentReviewPublisherInjectedAction::FailAfter},
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              IndependentReviewPublisherEvent::FileWrite, 1,
              IndependentReviewPublisherInjectedAction::FailBefore},
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              IndependentReviewPublisherEvent::FileCreated, 2,
              IndependentReviewPublisherInjectedAction::FailBefore},
        Fault{IndependentReviewArtifactKind::FinalizedPack,
              IndependentReviewPublisherEvent::DirectoryCreated, 1,
              IndependentReviewPublisherInjectedAction::FailAfter},
        Fault{IndependentReviewArtifactKind::FinalizedPack,
              IndependentReviewPublisherEvent::FileWrite, 2,
              IndependentReviewPublisherInjectedAction::FailBefore},
        Fault{IndependentReviewArtifactKind::FinalizedPack,
              IndependentReviewPublisherEvent::FileWrite, 3,
              IndependentReviewPublisherInjectedAction::FailAfter},
    };
    for (std::size_t index = 0; index < faults.size(); ++index) {
        const auto& fault = faults.at(index);
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(fault.kind, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fault1"}; };
        hooks.inject = [fault](const IndependentReviewPublisherObservation& observation) {
            return observation.event == fault.event && observation.ordinal == fault.ordinal
                       ? fault.action
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY2(!result.has_value(),
                 qPrintable(QStringLiteral("Fault row %1 succeeded").arg(index)));
        if (!result.has_value()) {
            QCOMPARE(
                static_cast<int>(result.error().code),
                static_cast<int>(IndependentReviewPublicationErrorCode::CannotPublishDestination));
        }
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
        QVERIFY(entriesAt(parent.path()).isEmpty());
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CleanupSynced), 1);
    }
#endif
}

void IndependentReviewPublisherTest::cleansEveryMidWritePrefix() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Fault final {
        IndependentReviewArtifactKind kind;
        std::size_t ordinal;
        QByteArrayView expected_bytes;
        QStringList root_entries;
        QStringList resource_entries;
    };
    const std::array faults{
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              1,
              prepared_handoff_bytes,
              {QStringLiteral("handoff.json")},
              {}},
        Fault{IndependentReviewArtifactKind::PreparedHandoff,
              2,
              declaration_template_bytes,
              {QStringLiteral("handoff.json"), QStringLiteral("review-declaration.template.json")},
              {}},
        Fault{IndependentReviewArtifactKind::FinalizedPack,
              2,
              final_manifest_bytes,
              {QStringLiteral("manifest.json"), QStringLiteral("resources")},
              {}},
        Fault{IndependentReviewArtifactKind::FinalizedPack,
              3,
              final_review_bytes,
              {QStringLiteral("manifest.json"), QStringLiteral("resources")},
              {QStringLiteral("realism-review.json")}},
    };
    for (std::size_t row = 0; row < faults.size(); ++row) {
        const auto& fault = faults.at(row);
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(fault.kind, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"short1"}; };
        hooks.outcome = [fault](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event != IndependentReviewPublisherEvent::FileWrite ||
                observation.ordinal != fault.ordinal) {
                return std::nullopt;
            }
            IndependentReviewPublisherInjectedOutcome outcome{false, true, EIO};
            outcome.maximum_write_bytes = std::size_t{1};
            return outcome;
        };
        QByteArray observed_prefix;
        QStringList observed_root_entries;
        QStringList observed_resource_entries;
        hooks.observe = [&report, &observed_prefix, &observed_root_entries,
                         &observed_resource_entries,
                         fault](const IndependentReviewPublisherObservation& observation) {
            if (observation.event != IndependentReviewPublisherEvent::CleanupInspected) {
                return;
            }
            const auto relative_path =
                fault.kind == IndependentReviewArtifactKind::PreparedHandoff
                    ? (fault.ordinal == 1 ? QStringLiteral("handoff.json")
                                          : QStringLiteral("review-declaration.template.json"))
                    : (fault.ordinal == 2 ? QStringLiteral("manifest.json")
                                          : QStringLiteral("resources/realism-review.json"));
            observed_prefix = readAll(QDir(report.staging_path).filePath(relative_path));
            observed_root_entries = entriesAt(report.staging_path);
            const auto resources = QDir(report.staging_path).filePath(QStringLiteral("resources"));
            if (QFileInfo(resources).isDir()) {
                observed_resource_entries = entriesAt(resources);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QCOMPARE(observed_prefix, QByteArray{fault.expected_bytes.first(1)});
        QCOMPARE(observed_root_entries, fault.root_entries);
        QCOMPARE(observed_resource_entries, fault.resource_entries);

        std::vector<std::size_t> created_ordinals;
        QList<QByteArray> created_components;
        for (const auto& observation : report.observations) {
            if (observation.event == IndependentReviewPublisherEvent::StagingCreated ||
                observation.event == IndependentReviewPublisherEvent::DirectoryCreated ||
                observation.event == IndependentReviewPublisherEvent::FileCreated) {
                created_ordinals.push_back(observation.ordinal);
                created_components.push_back(observation.component);
            }
        }
        QVERIFY(!created_ordinals.empty());
        QCOMPARE(created_ordinals.back(), fault.ordinal);

        std::vector<std::size_t> cleanup_ordinals;
        std::vector<std::size_t> cleanup_sync_ordinals;
        bool cleaning = false;
        for (const auto& observation : report.observations) {
            cleaning =
                cleaning || observation.event == IndependentReviewPublisherEvent::CleanupInspected;
            if (!cleaning) {
                continue;
            }
            if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved) {
                cleanup_ordinals.push_back(observation.ordinal);
            } else if (observation.event == IndependentReviewPublisherEvent::DirectorySync) {
                cleanup_sync_ordinals.push_back(observation.ordinal);
            }
        }
        auto reversed_created_ordinals = created_ordinals;
        std::ranges::reverse(reversed_created_ordinals);
        QCOMPARE(cleanup_ordinals, reversed_created_ordinals);
        QCOMPARE(cleanup_sync_ordinals, reversed_created_ordinals);

        auto reversed_created_components = created_components;
        std::ranges::reverse(reversed_created_components);
        QList<QByteArray> removed_components;
        for (const auto& observation : report.observations) {
            if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved) {
                removed_components.push_back(observation.component);
            }
        }
        QCOMPARE(removed_components, reversed_created_components);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
        QVERIFY(entriesAt(parent.path()).isEmpty());
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CleanupSynced), 1);
    }
#endif
}

void IndependentReviewPublisherTest::cleansInvalidStagedArtifact() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    qsizetype validation_calls = 0;
    request.validate_staged = [&validation_calls](const QString&) {
        ++validation_calls;
        return std::expected<void, IndependentReviewStagedValidationError>{
            std::unexpected(IndependentReviewStagedValidationError{
                QStringLiteral("synthetic staged validation failure")})};
    };
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"badval"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QCOMPARE(validation_calls, 1);
    QVERIFY(!QFileInfo::exists(destination));
    QVERIFY(!QFileInfo::exists(report.staging_path));
    QVERIFY(report.remaining_ledger_paths.isEmpty());
    QVERIFY(entriesAt(parent.path()).isEmpty());
#endif
}

void IndependentReviewPublisherTest::reportsCleanupFailureWithExactLedger() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnfal"}; };
    hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
        if (observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
            observation.ordinal == 0) {
            return IndependentReviewPublisherInjectedAction::FailBefore;
        }
        if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
            observation.ordinal == 2) {
            return IndependentReviewPublisherInjectedAction::FailBefore;
        }
        return IndependentReviewPublisherInjectedAction::Continue;
    };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
    if (!result.has_value()) {
        QCOMPARE(result.error().message,
                 QStringLiteral("original_staging_reachability=reachable;"
                                "cleanup_residue=present;parent_fsync=not_attempted"));
    }
    const auto expected_remaining =
        QStringList{report.staging_path, report.staging_path + QStringLiteral("/handoff.json"),
                    report.staging_path + QStringLiteral("/review-declaration.template.json")};
    QCOMPARE(report.remaining_ledger_paths, expected_remaining);
    const auto validated = validatePreparedTree(report.staging_path);
    QVERIFY2(validated.has_value(),
             validated.has_value() ? "" : qPrintable(validated.error().message));
    QVERIFY(!QFileInfo::exists(destination));
    QCOMPARE(entriesAt(parent.path()), QStringList{QFileInfo(report.staging_path).fileName()});
#endif
}

void IndependentReviewPublisherTest::reconcilesSourceRetainedRenameErrors() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct RenameError final {
        int native_error;
        IndependentReviewPublicationErrorCode expected;
    };
    const std::array errors{
        RenameError{ENOSYS, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform},
        RenameError{EINVAL, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform},
        RenameError{EOPNOTSUPP,
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform},
        RenameError{EXDEV, IndependentReviewPublicationErrorCode::CannotPublishDestination},
        RenameError{EIO, IndependentReviewPublicationErrorCode::CannotPublishDestination},
    };
    for (const auto& row : errors) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"nomove"}; };
        hooks.outcome = [row](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event != IndependentReviewPublisherEvent::RenameAttempted) {
                return std::nullopt;
            }
            return IndependentReviewPublisherInjectedOutcome{false, false, row.native_error};
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, row.expected);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
        QVERIFY(entriesAt(parent.path()).isEmpty());
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::RenameAttempted), 1);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ParentSync), 0);
    }
#endif
}

void IndependentReviewPublisherTest::preservesRacingDestinationAndCleansSource() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    IndependentReviewPublisherReport report;
    bool raced = false;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"destrc"}; };
    hooks.barrier = [&destination,
                     &raced](const IndependentReviewPublisherObservation& observation) {
        if (observation.event == IndependentReviewPublisherEvent::RenameAttempted) {
            raced = QDir().mkdir(destination) &&
                    writeNew(QDir(destination).filePath(QStringLiteral("marker")),
                             QByteArrayView{"preserve"});
        }
    };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    QVERIFY(raced);
    requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
    QCOMPARE(entriesAt(destination), QStringList{QStringLiteral("marker")});
    QCOMPARE(readAll(QDir(destination).filePath(QStringLiteral("marker"))), QByteArray{"preserve"});
    QVERIFY(!QFileInfo::exists(report.staging_path));
    QVERIFY(report.remaining_ledger_paths.isEmpty());
    QCOMPARE(entriesAt(parent.path()), QStringList{QStringLiteral("output")});
#endif
}

void IndependentReviewPublisherTest::preservesMovedDestinationWhenRenameReportsError() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const bool parent_sync_succeeds : {true, false}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"moved1"}; };
        hooks.outcome =
            [parent_sync_succeeds](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::RenameAttempted) {
                return IndependentReviewPublisherInjectedOutcome{false, true, EIO};
            }
            if (observation.event == IndependentReviewPublisherEvent::ParentSync) {
                return IndependentReviewPublisherInjectedOutcome{parent_sync_succeeds, false,
                                                                 parent_sync_succeeds ? 0 : EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result,
                         IndependentReviewPublicationErrorCode::PublicationOutcomeUncertain);
        if (!result.has_value()) {
            QCOMPARE(
                result.error().message,
                QStringLiteral("original_staging_reachability=reachable;"
                               "cleanup_residue=absent;parent_fsync=%1")
                    .arg(parent_sync_succeeds ? QStringLiteral("ok") : QStringLiteral("failed")));
        }
        const auto validated = validatePreparedTree(destination);
        QVERIFY2(validated.has_value(),
                 validated.has_value() ? "" : qPrintable(validated.error().message));
        QVERIFY(!QFileInfo::exists(report.staging_path));

        const auto retry = publishIndependentReviewArtifacts(request);
        requireErrorCode(retry, IndependentReviewPublicationErrorCode::DestinationExists);
    }
#endif
}

void IndependentReviewPublisherTest::reportsPostRenameDurabilityFailure() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::FinalizedPack, destination);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"durabl"}; };
    hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        if (observation.event == IndependentReviewPublisherEvent::ParentSync) {
            return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
        }
        return std::nullopt;
    };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationDurabilityFailed);
    if (!result.has_value()) {
        QCOMPARE(result.error().message,
                 QStringLiteral("original_staging_reachability=reachable;"
                                "cleanup_residue=absent;parent_fsync=failed"));
    }
    const auto validated = validateFinalizedTree(destination);
    QVERIFY2(validated.has_value(),
             validated.has_value() ? "" : qPrintable(validated.error().message));
    QVERIFY(!QFileInfo::exists(report.staging_path));
    QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ParentSync), 1);
#endif
}

void IndependentReviewPublisherTest::reportsIdentityFailureAfterDestinationMutation() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    for (const bool parent_sync_succeeds : {true, false}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        bool mutated = false;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"identy"}; };
        hooks.barrier = [&destination,
                         &mutated](const IndependentReviewPublisherObservation& observation) {
            if (observation.event != IndependentReviewPublisherEvent::RenameReturned) {
                return;
            }
            QFile file(QDir(destination).filePath(QStringLiteral("handoff.json")));
            mutated = file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                      file.write("mutated\n") == 8 && file.flush();
        };
        hooks.outcome =
            [parent_sync_succeeds](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event != IndependentReviewPublisherEvent::ParentSync) {
                return std::nullopt;
            }
            return IndependentReviewPublisherInjectedOutcome{parent_sync_succeeds, false,
                                                             parent_sync_succeeds ? 0 : EIO};
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(mutated);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationIdentityFailed);
        if (!result.has_value()) {
            QCOMPARE(
                result.error().message,
                QStringLiteral("original_staging_reachability=unknown;"
                               "cleanup_residue=unknown;parent_fsync=%1")
                    .arg(parent_sync_succeeds ? QStringLiteral("ok") : QStringLiteral("failed")));
        }
        QVERIFY(QFileInfo(destination).isDir());
        QCOMPARE(readAll(QDir(destination).filePath(QStringLiteral("handoff.json"))),
                 QByteArray{"mutated\n"});
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ParentSync), 1);
    }
#endif
}

void IndependentReviewPublisherTest::rejectsSpecialAndReboundReaderInputs() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review input readers require Linux");
#else
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    const auto handoff_root = QDir(sandbox.path()).filePath(QStringLiteral("handoff"));
    QVERIFY(QDir().mkdir(handoff_root));
    const auto handoff_path = QDir(handoff_root).filePath(QStringLiteral("handoff.json"));
    const auto template_path =
        QDir(handoff_root).filePath(QStringLiteral("review-declaration.template.json"));
    const auto outside = QDir(sandbox.path()).filePath(QStringLiteral("outside.json"));
    QVERIFY(writeNew(handoff_path, prepared_handoff_bytes));
    QVERIFY(writeNew(template_path, declaration_template_bytes));
    QVERIFY(writeNew(outside, prepared_handoff_bytes));
    QVERIFY(readIndependentReviewHandoffDirectory(handoff_root).has_value());

    const auto extra_path = QDir(handoff_root).filePath(QStringLiteral("extra"));
    QVERIFY(writeNew(extra_path, QByteArrayView{"bounded\n"}));
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(QFile::remove(extra_path));

    QVERIFY(QFile::remove(handoff_path));
    const auto encoded_handoff = QFile::encodeName(handoff_path);
    QCOMPARE(::mkfifo(encoded_handoff.constData(), 0600), 0);
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);

    QVERIFY(QFile::remove(handoff_path));
    const auto encoded_outside = QFile::encodeName(outside);
    QCOMPARE(::symlink(encoded_outside.constData(), encoded_handoff.constData()), 0);
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);

    QVERIFY(QFile::remove(handoff_path));
    QCOMPARE(::link(encoded_outside.constData(), encoded_handoff.constData()), 0);
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(QFile::remove(handoff_path));
    QVERIFY(writeNew(handoff_path, prepared_handoff_bytes));

    bool handoff_replaced = false;
    const auto moved_handoff = QDir(sandbox.path()).filePath(QStringLiteral("moved-handoff.json"));
    IndependentReviewInputReaderHooks handoff_hooks;
    handoff_hooks.before_final_rebind = [&] {
        handoff_replaced = QFile::rename(handoff_path, moved_handoff) &&
                           writeNew(handoff_path, prepared_handoff_bytes);
    };
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root, handoff_hooks),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(handoff_replaced);

    bool handoff_root_replaced = false;
    const auto moved_handoff_root =
        QDir(sandbox.path()).filePath(QStringLiteral("moved-handoff-root"));
    IndependentReviewInputReaderHooks handoff_root_hooks;
    handoff_root_hooks.before_final_rebind = [&] {
        handoff_root_replaced = QFile::rename(handoff_root, moved_handoff_root) &&
                                QDir().mkdir(handoff_root) &&
                                writeNew(handoff_path, prepared_handoff_bytes) &&
                                writeNew(template_path, declaration_template_bytes);
    };
    requireErrorCode(readIndependentReviewHandoffDirectory(handoff_root, handoff_root_hooks),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(handoff_root_replaced);
    const auto replacement_handoff_root =
        QDir(sandbox.path()).filePath(QStringLiteral("replacement-handoff-root"));
    QVERIFY(QFile::rename(handoff_root, replacement_handoff_root));
    QVERIFY(QFile::rename(moved_handoff_root, handoff_root));

    QVERIFY(QFile::remove(handoff_path));
    QVERIFY(writeNew(handoff_path, prepared_handoff_bytes));
    const auto parent_descriptor =
        ::open(QFile::encodeName(sandbox.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(parent_descriptor >= 0);
    bool staged_replaced = false;
    const auto staged_moved =
        QDir(sandbox.path()).filePath(QStringLiteral("staged-moved-handoff.json"));
    IndependentReviewInputReaderHooks staged_hooks;
    staged_hooks.before_final_rebind = [&] {
        staged_replaced = QFile::rename(handoff_path, staged_moved) &&
                          writeNew(handoff_path, prepared_handoff_bytes);
    };
    const auto staged_root = QStringLiteral("/proc/self/fd/%1/handoff").arg(parent_descriptor);
    requireErrorCode(readIndependentReviewStagedHandoffDirectory(staged_root, staged_hooks),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(staged_replaced);

    bool staged_root_replaced = false;
    const auto staged_moved_root =
        QDir(sandbox.path()).filePath(QStringLiteral("staged-moved-root"));
    IndependentReviewInputReaderHooks staged_root_hooks;
    staged_root_hooks.before_final_rebind = [&] {
        staged_root_replaced = QFile::rename(handoff_root, staged_moved_root) &&
                               QDir().mkdir(handoff_root) &&
                               writeNew(handoff_path, prepared_handoff_bytes) &&
                               writeNew(template_path, declaration_template_bytes);
    };
    requireErrorCode(readIndependentReviewStagedHandoffDirectory(staged_root, staged_root_hooks),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(staged_root_replaced);
    QCOMPARE(::close(parent_descriptor), 0);

    const auto declaration_path =
        QDir(sandbox.path()).filePath(QStringLiteral("completed-declaration.json"));
    QVERIFY(writeNew(declaration_path, QByteArrayView{"{\"declaration\":true}\n"}));
    QVERIFY(readIndependentReviewDeclaration(declaration_path).has_value());
    const auto encoded_declaration = QFile::encodeName(declaration_path);

    QVERIFY(QFile::remove(declaration_path));
    QCOMPARE(::mkfifo(encoded_declaration.constData(), 0600), 0);
    requireErrorCode(readIndependentReviewDeclaration(declaration_path),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(QFile::remove(declaration_path));
    QCOMPARE(::symlink(encoded_outside.constData(), encoded_declaration.constData()), 0);
    requireErrorCode(readIndependentReviewDeclaration(declaration_path),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(QFile::remove(declaration_path));
    QCOMPARE(::link(encoded_outside.constData(), encoded_declaration.constData()), 0);
    requireErrorCode(readIndependentReviewDeclaration(declaration_path),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);

    QVERIFY(QFile::remove(declaration_path));
    QVERIFY(writeNew(declaration_path, QByteArrayView{"{\"declaration\":true}\n"}));
    bool declaration_replaced = false;
    const auto moved_declaration =
        QDir(sandbox.path()).filePath(QStringLiteral("moved-declaration.json"));
    IndependentReviewInputReaderHooks declaration_hooks;
    declaration_hooks.before_final_rebind = [&] {
        declaration_replaced =
            QFile::rename(declaration_path, moved_declaration) &&
            writeNew(declaration_path, QByteArrayView{"{\"declaration\":true}\n"});
    };
    requireErrorCode(readIndependentReviewDeclaration(declaration_path, declaration_hooks),
                     IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QVERIFY(declaration_replaced);
#endif
}

void IndependentReviewPublisherTest::rejectsEnvironmentallyInfeasibleRelativeHandoffBeforeAccess() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review input readers require Linux");
#else
    QTemporaryDir working_directory;
    QVERIFY(working_directory.isValid());
    const auto prior_working_directory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(working_directory.path()));
    const auto cwd_components = working_directory.path().sliced(1).split(u'/', Qt::SkipEmptyParts);
    constexpr std::size_t maximum_components = 128U;
    QVERIFY(static_cast<std::size_t>(cwd_components.size()) < maximum_components);
    const auto relative_count =
        maximum_components - static_cast<std::size_t>(cwd_components.size());
    QStringList relative_components;
    relative_components.fill(QStringLiteral("x"), static_cast<qsizetype>(relative_count));
    const auto relative_handoff = relative_components.join(u'/');

    const auto result = readIndependentReviewHandoffDirectory(relative_handoff);
    const auto restored = QDir::setCurrent(prior_working_directory);

    QVERIFY(restored);
    requireErrorCode(result, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform);
#endif
}

void IndependentReviewPublisherTest::reusesEncodedOperandsAcrossEnvironmentalCapture() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir lexical_directory;
    QTemporaryDir runtime_directory;
    QVERIFY(lexical_directory.isValid());
    QVERIFY(runtime_directory.isValid());
    const auto prior_working_directory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(lexical_directory.path()));
    auto destination_token = encodeIndependentReviewDestinationPath(
        QStringLiteral("output"), IndependentReviewArtifactKind::PreparedHandoff);
    QVERIFY(destination_token.has_value());
    auto request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, QStringLiteral("output"));
    request.destination_token = std::move(*destination_token);
    QVERIFY(QDir::setCurrent(runtime_directory.path()));
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"token1"}; };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);
    const auto restored = QDir::setCurrent(prior_working_directory);

    QVERIFY(restored);
    const auto destination = QDir(runtime_directory.path()).filePath(QStringLiteral("output"));
    requireSuccessfulTree(result, report, destination);
    QVERIFY(!QFileInfo::exists(QDir(lexical_directory.path()).filePath(QStringLiteral("output"))));

    auto handoff_token = encodeIndependentReviewHandoffPath(destination);
    QVERIFY(handoff_token.has_value());
    const auto handoff = readIndependentReviewHandoffDirectory(std::move(*handoff_token));
    QVERIFY(handoff.has_value());
    if (handoff.has_value()) {
        QCOMPARE(handoff->handoff_bytes, QByteArray{prepared_handoff_bytes});
        QCOMPARE(handoff->declaration_template_bytes, QByteArray{declaration_template_bytes});
    }

    const auto declaration_path =
        QDir(runtime_directory.path()).filePath(QStringLiteral("completed.json"));
    QVERIFY(writeNew(declaration_path, QByteArrayView{"{\"completed\":true}\n"}));
    auto declaration_token = encodeIndependentReviewPathSpelling(declaration_path, false);
    QVERIFY(declaration_token.has_value());
    const auto declaration = readIndependentReviewDeclaration(std::move(*declaration_token));
    QVERIFY(declaration.has_value());
    if (declaration.has_value()) {
        QCOMPARE(*declaration, QByteArray{"{\"completed\":true}\n"});
    }

    const auto rejected_destination =
        QDir(runtime_directory.path()).filePath(QStringLiteral("rejected"));
    auto rejected_token = encodeIndependentReviewDestinationPath(
        rejected_destination, IndependentReviewArtifactKind::PreparedHandoff);
    QVERIFY(rejected_token.has_value());
    rejected_token->supplied_native_path += 'x';
    auto rejected_request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, rejected_destination);
    rejected_request.destination_token = std::move(*rejected_token);
    const auto rejected = publishIndependentReviewArtifacts(rejected_request);
    requireErrorCode(rejected, IndependentReviewPublicationErrorCode::InvalidArguments);
    QVERIFY(!QFileInfo::exists(rejected_destination));
#endif
}

void IndependentReviewPublisherTest::preservesUnretainedStagingWithUnknownTelemetry() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"unheld"}; };
    hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
        return observation.event == IndependentReviewPublisherEvent::StagingRetainAttempted
                   ? IndependentReviewPublisherInjectedAction::FailBefore
                   : IndependentReviewPublisherInjectedAction::Continue;
    };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
    if (!result.has_value()) {
        QCOMPARE(result.error().message,
                 QStringLiteral("original_staging_reachability=unknown;"
                                "cleanup_residue=unknown;parent_fsync=not_attempted"));
    }
    QVERIFY(!QFileInfo::exists(destination));
    QVERIFY(QFileInfo(report.staging_path).isDir());
    QCOMPARE(entriesAt(parent.path()), QStringList{QFileInfo(report.staging_path).fileName()});
    QVERIFY(report.remaining_ledger_paths.isEmpty());
#endif
}

void IndependentReviewPublisherTest::classifiesStagedValidationFailures() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Row final {
        IndependentReviewStagedValidationErrorCode validation_code;
        IndependentReviewPublicationErrorCode publication_code;
    };
    const std::array rows{
        Row{IndependentReviewStagedValidationErrorCode::InvalidArtifact,
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact},
        Row{IndependentReviewStagedValidationErrorCode::PublicationMismatch,
            IndependentReviewPublicationErrorCode::CannotPublishDestination},
    };
    for (std::size_t index = 0; index < rows.size(); ++index) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        request.validate_staged = [row = rows.at(index)](const QString&) {
            return std::expected<void, IndependentReviewStagedValidationError>{
                std::unexpected(IndependentReviewStagedValidationError{
                    QStringLiteral("classified failure"), row.validation_code})};
        };

        const auto result = publishIndependentReviewArtifacts(request);

        requireErrorCode(result, rows.at(index).publication_code);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(entriesAt(parent.path()).isEmpty());
    }

    QTemporaryDir raced_parent;
    QVERIFY(raced_parent.isValid());
    const auto raced_destination =
        QDir(raced_parent.path()).filePath(QStringLiteral("raced-output"));
    auto raced_request =
        requestFor(IndependentReviewArtifactKind::PreparedHandoff, raced_destination);
    raced_request.validate_staged = [](const QString& staging_root) {
        QFile handoff(QDir(staging_root).filePath(QStringLiteral("handoff.json")));
        const auto changed = handoff.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                             handoff.write("changed\n") == 8 && handoff.flush();
        return std::expected<void, IndependentReviewStagedValidationError>{
            std::unexpected(IndependentReviewStagedValidationError{
                changed ? QStringLiteral("synthetic parser failure")
                        : QStringLiteral("could not create validator race")})};
    };

    const auto raced_result = publishIndependentReviewArtifacts(raced_request);

    requireErrorCode(raced_result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
    QVERIFY(!QFileInfo::exists(raced_destination));
    QVERIFY(entriesAt(raced_parent.path()).isEmpty());
#endif
}

void IndependentReviewPublisherTest::mapsInjectedControllerAclAndLeaseOutcomes() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.outcome = [&parent](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::ControllerRebound &&
                observation.absolute_path == parent.path()) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::UnsafeDestinationParent);
        QVERIFY(report.staging_path.isEmpty());
        QVERIFY(countEvent(report, IndependentReviewPublisherEvent::ControllerRebound) > 0);
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool policy_changed = false;
        IndependentReviewPublisherHooks hooks;
        hooks.barrier = [&parent, &policy_changed](
                            const IndependentReviewPublisherObservation& observation) {
            if (!policy_changed &&
                observation.event == IndependentReviewPublisherEvent::ControllerRebound &&
                observation.absolute_path == parent.path()) {
                policy_changed = ::chmod(QFile::encodeName(parent.path()).constData(), 0777) == 0;
            }
        };

        const auto result = publishIndependentReviewArtifacts(request, hooks);
        const auto restored = ::chmod(QFile::encodeName(parent.path()).constData(), 0700) == 0;

        QVERIFY(policy_changed);
        QVERIFY(restored);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::UnsafeDestinationParent);
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool policy_changed_during_probe = false;
        IndependentReviewPublisherHooks hooks;
        hooks.barrier = [&parent, &policy_changed_during_probe](
                            const IndependentReviewPublisherObservation& observation) {
            if (!policy_changed_during_probe &&
                observation.event == IndependentReviewPublisherEvent::AccessAclProbe &&
                observation.absolute_path == parent.path()) {
                policy_changed_during_probe =
                    ::chmod(QFile::encodeName(parent.path()).constData(), 0777) == 0;
            }
        };

        const auto result = publishIndependentReviewArtifacts(request, hooks);
        const auto restored = ::chmod(QFile::encodeName(parent.path()).constData(), 0700) == 0;

        QVERIFY(policy_changed_during_probe);
        QVERIFY(restored);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::UnsafeDestinationParent);
        QVERIFY(!QFileInfo::exists(destination));
    }

    struct AclRow final {
        IndependentReviewPublisherEvent event;
        IndependentReviewPublisherInjectedOutcome outcome;
        IndependentReviewPublicationErrorCode expected;
    };
    const std::array acl_rows{
        AclRow{IndependentReviewPublisherEvent::AccessAclProbe,
               IndependentReviewPublisherInjectedOutcome{true, false, 0},
               IndependentReviewPublicationErrorCode::UnsafeDestinationParent},
        AclRow{IndependentReviewPublisherEvent::DefaultAclProbe,
               IndependentReviewPublisherInjectedOutcome{false, false, ENOTSUP},
               IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform},
        AclRow{IndependentReviewPublisherEvent::AccessAclProbe,
               IndependentReviewPublisherInjectedOutcome{false, false, EIO},
               IndependentReviewPublicationErrorCode::CannotPublishDestination},
    };
    for (const auto& row : acl_rows) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.outcome = [&parent, row](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            return observation.event == row.event && observation.absolute_path == parent.path()
                       ? std::optional{row.outcome}
                       : std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, row.expected);
        QVERIFY(report.staging_path.isEmpty());
        QVERIFY(countEvent(report, row.event) > 0);
    }

    struct AclRetryRow final {
        int terminal_error;
        IndependentReviewPublicationErrorCode expected;
        bool succeeds;
    };
    std::vector<AclRetryRow> acl_retry_rows{
        {ENODATA, IndependentReviewPublicationErrorCode::CannotPublishDestination, true},
        {E2BIG, IndependentReviewPublicationErrorCode::UnsafeDestinationParent, false},
        {ERANGE, IndependentReviewPublicationErrorCode::UnsafeDestinationParent, false},
        {ENOTSUP, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform, false},
        {EOPNOTSUPP, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform, false},
        {ENOSYS, IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform, false},
        {EIO, IndependentReviewPublicationErrorCode::CannotPublishDestination, false},
    };
#if defined(ENOATTR) && ENOATTR != ENODATA
    acl_retry_rows.push_back(
        {ENOATTR, IndependentReviewPublicationErrorCode::CannotPublishDestination, true});
#endif
    for (const auto& row : acl_retry_rows) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        std::size_t injected_attempts = 0;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"acltry"}; };
        hooks.outcome = [&parent, &injected_attempts,
                         row](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event != IndependentReviewPublisherEvent::AccessAclProbe ||
                observation.absolute_path != parent.path() || injected_attempts >= 2) {
                return std::nullopt;
            }
            const auto native_error = injected_attempts++ == 0 ? EINTR : row.terminal_error;
            return IndependentReviewPublisherInjectedOutcome{false, false, native_error};
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QCOMPARE(injected_attempts, std::size_t{2});
        if (row.succeeds) {
            requireSuccessfulTree(result, report, destination);
        } else {
            requireErrorCode(result, row.expected);
            QVERIFY(!QFileInfo::exists(destination));
        }
    }

    for (const auto native_error : {0, ENOTSUP, EIO}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"aclrt1"}; };
        hooks.outcome = [&report,
                         native_error](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::AccessAclProbe &&
                !report.staging_path.isEmpty() &&
                observation.absolute_path == report.staging_path) {
                return IndependentReviewPublisherInjectedOutcome{native_error == 0, false,
                                                                 native_error};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        QVERIFY(QFileInfo(report.staging_path).isDir());
        QVERIFY(!QFileInfo::exists(destination));
    }

    for (const auto native_error : {0, ENOTSUP, EIO}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"aclmb1"}; };
        hooks.outcome = [native_error](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::AccessAclProbe &&
                observation.absolute_path.endsWith(QStringLiteral("/handoff.json"))) {
                return IndependentReviewPublisherInjectedOutcome{native_error == 0, false,
                                                                 native_error};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result,
                         native_error == ENOTSUP
                             ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                             : IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherHooks hooks;
        hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::ParentLeaseAttempted) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EWOULDBLOCK};
            }
            return std::nullopt;
        };

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QVERIFY(entriesAt(parent.path()).isEmpty());
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto prior_working_directory = QDir::currentPath();
        QVERIFY(QDir::setCurrent(parent.path()));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff,
                                  QStringLiteral("capture-output"));
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::CurrentDirectoryCaptured) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);
        const auto restored = QDir::setCurrent(prior_working_directory);

        QVERIFY(restored);
        requireErrorCode(result,
                         IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CurrentDirectoryCaptured), 1);
        QVERIFY(entriesAt(parent.path()).isEmpty());
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto parent_descriptor = ::open(QFile::encodeName(parent.path()).constData(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        QVERIFY(parent_descriptor >= 0);
        QCOMPARE(::flock(parent_descriptor, LOCK_EX | LOCK_NB), 0);
        const auto prior_working_directory = QDir::currentPath();
        QVERIFY(QDir::setCurrent(parent.path()));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff,
                                  QStringLiteral("relative-output"));
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);
        const auto restored = QDir::setCurrent(prior_working_directory);
        const auto unlocked = ::flock(parent_descriptor, LOCK_UN) == 0;
        const auto closed = ::close(parent_descriptor) == 0;

        QVERIFY(restored);
        QVERIFY(unlocked);
        QVERIFY(closed);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::CurrentDirectoryCaptured), 1);
        QVERIFY(entriesAt(parent.path()).isEmpty());
    }
#endif
}

void IndependentReviewPublisherTest::retriesInjectedTransientSyscallOutcomes() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    const auto runRetry = [](IndependentReviewPublisherEvent event, QByteArray suffix,
                             std::optional<std::size_t> ordinal,
                             IndependentReviewArtifactKind kind) {
        QTemporaryDir parent;
        if (!parent.isValid()) {
            return std::pair{PublicationResult{std::unexpected(IndependentReviewPublicationError{
                                 IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                 QStringLiteral("temporary directory is invalid")})},
                             std::size_t{0}};
        }
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(kind, destination);
        std::size_t attempts = 0;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [suffix = std::move(suffix)](std::size_t) { return suffix; };
        hooks.outcome = [event, ordinal,
                         &attempts](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (attempts >= 2 || observation.event != event ||
                (ordinal.has_value() && observation.ordinal != *ordinal)) {
                return std::nullopt;
            }
            ++attempts;
            return attempts == 1 ? IndependentReviewPublisherInjectedOutcome{false, false, EINTR}
                                 : IndependentReviewPublisherInjectedOutcome{true, false, 0};
        };
        return std::pair{publishIndependentReviewArtifacts(request, hooks), attempts};
    };

    struct RetryTarget final {
        IndependentReviewPublisherEvent event;
        QByteArray suffix;
        std::optional<std::size_t> ordinal;
    };
    const std::array retry_targets{
        RetryTarget{IndependentReviewPublisherEvent::ParentLeaseAttempted, QByteArray{"rlock1"},
                    std::nullopt},
        RetryTarget{IndependentReviewPublisherEvent::DirectorySync, QByteArray{"rsync1"},
                    std::nullopt},
        RetryTarget{IndependentReviewPublisherEvent::ControllerRebound, QByteArray{"rstat1"},
                    std::nullopt},
        RetryTarget{IndependentReviewPublisherEvent::EntryRebound, QByteArray{"rbind1"}, 1},
        RetryTarget{IndependentReviewPublisherEvent::StagingCreateAttempted, QByteArray{"rmkdir"},
                    0},
        RetryTarget{IndependentReviewPublisherEvent::StagingRetainAttempted, QByteArray{"rhold1"},
                    0},
        RetryTarget{IndependentReviewPublisherEvent::DirectoryRetainAttempted, QByteArray{"rhold2"},
                    1},
        RetryTarget{IndependentReviewPublisherEvent::DirectoryUsableRetainAttempted,
                    QByteArray{"rhold3"}, 0},
        RetryTarget{IndependentReviewPublisherEvent::FileCreateAttempted, QByteArray{"ropen1"}, 1},
        RetryTarget{IndependentReviewPublisherEvent::ModeNormalizeAttempted, QByteArray{"rchmod"},
                    1},
        RetryTarget{IndependentReviewPublisherEvent::FileWrite, QByteArray{"rwrite"}, 1},
    };
    for (const auto& row : retry_targets) {
        const auto kind = row.event == IndependentReviewPublisherEvent::DirectoryRetainAttempted
                              ? IndependentReviewArtifactKind::FinalizedPack
                              : IndependentReviewArtifactKind::PreparedHandoff;
        auto [result, attempts] = runRetry(row.event, row.suffix, row.ordinal, kind);
        QVERIFY2(result.has_value(), result.has_value() ? "" : qPrintable(result.error().message));
        QCOMPARE(attempts, std::size_t{2});
    }

    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
    auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
    request.validate_staged = [](const QString&) {
        return validationFailure(QStringLiteral("force checked cleanup"));
    };
    std::size_t remove_attempts = 0;
    IndependentReviewPublisherReport report;
    IndependentReviewPublisherHooks hooks;
    hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"rremov"}; };
    hooks.outcome = [&remove_attempts](const IndependentReviewPublisherObservation& observation)
        -> std::optional<IndependentReviewPublisherInjectedOutcome> {
        if (observation.event != IndependentReviewPublisherEvent::CleanupRemoved ||
            observation.ordinal != 2 || remove_attempts >= 2) {
            return std::nullopt;
        }
        ++remove_attempts;
        return remove_attempts == 1 ? IndependentReviewPublisherInjectedOutcome{false, false, EINTR}
                                    : IndependentReviewPublisherInjectedOutcome{true, true, 0};
    };
    hooks.report = &report;

    const auto result = publishIndependentReviewArtifacts(request, hooks);

    requireErrorCode(result, IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    QCOMPARE(remove_attempts, std::size_t{2});
    QVERIFY(!QFileInfo::exists(report.staging_path));
#endif
}

void IndependentReviewPublisherTest::mapsInjectedMutationOutcomes() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Target final {
        IndependentReviewPublisherEvent event;
        IndependentReviewArtifactKind kind;
        std::size_t ordinal;
    };
    const std::array targets{
        Target{IndependentReviewPublisherEvent::StagingCreateAttempted,
               IndependentReviewArtifactKind::PreparedHandoff, 0},
        Target{IndependentReviewPublisherEvent::DirectoryCreateAttempted,
               IndependentReviewArtifactKind::FinalizedPack, 1},
        Target{IndependentReviewPublisherEvent::FileCreateAttempted,
               IndependentReviewArtifactKind::PreparedHandoff, 1},
        Target{IndependentReviewPublisherEvent::ModeNormalizeAttempted,
               IndependentReviewArtifactKind::FinalizedPack, 1},
        Target{IndependentReviewPublisherEvent::ModeNormalizeAttempted,
               IndependentReviewArtifactKind::PreparedHandoff, 1},
        Target{IndependentReviewPublisherEvent::FileWrite,
               IndependentReviewArtifactKind::PreparedHandoff, 1},
    };
    for (const auto& target : targets) {
        for (const bool state_change_applied : {false, true}) {
            QTemporaryDir parent;
            QVERIFY(parent.isValid());
            const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
            auto request = requestFor(target.kind, destination);
            IndependentReviewPublisherReport report;
            IndependentReviewPublisherHooks hooks;
            hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"mutate"}; };
            hooks.outcome = [state_change_applied,
                             target](const IndependentReviewPublisherObservation& observation)
                -> std::optional<IndependentReviewPublisherInjectedOutcome> {
                if (observation.event == target.event && observation.ordinal == target.ordinal) {
                    return IndependentReviewPublisherInjectedOutcome{false, state_change_applied,
                                                                     EIO};
                }
                return std::nullopt;
            };
            hooks.report = &report;

            const auto result = publishIndependentReviewArtifacts(request, hooks);

            requireErrorCode(result,
                             IndependentReviewPublicationErrorCode::CannotPublishDestination);
            QCOMPARE(std::ranges::count_if(
                         report.observations,
                         [&target](const IndependentReviewPublisherObservation& observation) {
                             return observation.event == target.event &&
                                    observation.ordinal == target.ordinal;
                         }),
                     std::ptrdiff_t{1});
            QVERIFY(!QFileInfo::exists(destination));
            QVERIFY(!QFileInfo::exists(report.staging_path));
            QVERIFY(report.remaining_ledger_paths.isEmpty());
            QVERIFY(entriesAt(parent.path()).isEmpty());
        }
    }
#endif
}

void IndependentReviewPublisherTest::exhaustsSyntheticControllerPolicy() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    const auto current_owner = static_cast<std::uint64_t>(::geteuid());
    const auto other_owner = current_owner == std::numeric_limits<std::uint64_t>::max()
                                 ? current_owner - 1
                                 : current_owner + 1;
    const std::array owners{current_owner, std::uint64_t{0}, other_owner};
    for (const bool target_grandparent : {false, true}) {
        for (const auto owner : owners) {
            for (const bool writable : {false, true}) {
                for (const bool sticky : {false, true}) {
                    const auto child_owners =
                        target_grandparent
                            ? owners
                            : std::array{current_owner, current_owner, current_owner};
                    for (std::size_t child_row = 0; child_row < child_owners.size(); ++child_row) {
                        if (!target_grandparent && child_row != 0) {
                            continue;
                        }
                        QTemporaryDir sandbox;
                        QVERIFY(sandbox.isValid());
                        const auto grandparent =
                            QDir(sandbox.path()).filePath(QStringLiteral("grandparent"));
                        const auto parent = QDir(grandparent).filePath(QStringLiteral("parent"));
                        QVERIFY(QDir().mkdir(grandparent));
                        QVERIFY(QDir().mkdir(parent));
                        const auto destination = QDir(parent).filePath(QStringLiteral("output"));
                        auto request =
                            requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
                        const auto target_path = target_grandparent ? grandparent : parent;
                        const auto target_mode = static_cast<std::uint32_t>(
                            0700 | (writable ? 0020 : 0) | (sticky ? S_ISVTX : 0));
                        const auto target_stat =
                            syntheticStat(IndependentReviewPublisherSyntheticNodeType::Directory,
                                          owner, target_mode);
                        const auto child_stat =
                            syntheticStat(IndependentReviewPublisherSyntheticNodeType::Directory,
                                          child_owners.at(child_row), 0700);
                        IndependentReviewPublisherReport report;
                        IndependentReviewPublisherHooks hooks;
                        hooks.staging_suffix_source = [](std::size_t) {
                            return QByteArray{"polrow"};
                        };
                        hooks.outcome =
                            [target_path, parent, target_grandparent, target_stat,
                             child_stat](const IndependentReviewPublisherObservation& observation)
                            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
                            if (observation.event !=
                                    IndependentReviewPublisherEvent::ControllerOpened &&
                                observation.event !=
                                    IndependentReviewPublisherEvent::ControllerRebound) {
                                return std::nullopt;
                            }
                            const auto* selected =
                                observation.absolute_path == target_path ? &target_stat
                                : target_grandparent && observation.absolute_path == parent
                                    ? &child_stat
                                    : nullptr;
                            if (selected == nullptr) {
                                return std::nullopt;
                            }
                            return IndependentReviewPublisherInjectedOutcome{true, false, 0,
                                                                             *selected, *selected};
                        };
                        hooks.report = &report;

                        const auto result = publishIndependentReviewArtifacts(request, hooks);

                        const auto owner_allowed = owner == current_owner || owner == 0;
                        const auto mode_allowed = !writable || sticky;
                        const auto child_allowed = !target_grandparent ||
                                                   child_owners.at(child_row) == current_owner ||
                                                   child_owners.at(child_row) == 0;
                        if (owner_allowed && mode_allowed && child_allowed) {
                            requireSuccessfulTree(result, report, destination);
                        } else {
                            requireErrorCode(
                                result,
                                IndependentReviewPublicationErrorCode::UnsafeDestinationParent);
                            QVERIFY(report.staging_path.isEmpty());
                            QVERIFY(!QFileInfo::exists(destination));
                        }
                    }
                }
            }
        }
    }
#endif
}

void IndependentReviewPublisherTest::mapsSyntheticCreationMetadata() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Target final {
        IndependentReviewArtifactKind kind;
        std::size_t ordinal;
        bool directory;
        bool staging_root;
    };
    const std::array targets{
        Target{IndependentReviewArtifactKind::PreparedHandoff, 0, true, true},
        Target{IndependentReviewArtifactKind::FinalizedPack, 1, true, false},
        Target{IndependentReviewArtifactKind::PreparedHandoff, 1, false, false},
        Target{IndependentReviewArtifactKind::PreparedHandoff, 2, false, false},
        Target{IndependentReviewArtifactKind::FinalizedPack, 2, false, false},
        Target{IndependentReviewArtifactKind::FinalizedPack, 3, false, false},
    };
    enum class Defect {
        Owner,
        Mode,
        Type,
        LinkCount,
    };
    const auto current_owner = static_cast<std::uint64_t>(::geteuid());
    const auto other_owner = current_owner == std::numeric_limits<std::uint64_t>::max()
                                 ? current_owner - 1
                                 : current_owner + 1;
    for (const auto& target : targets) {
        for (const auto defect : {Defect::Owner, Defect::Mode, Defect::Type, Defect::LinkCount}) {
            if (target.directory && defect == Defect::LinkCount) {
                continue;
            }
            QTemporaryDir parent;
            QVERIFY(parent.isValid());
            const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
            auto request = requestFor(target.kind, destination);
            auto metadata = syntheticStat(
                target.directory ? IndependentReviewPublisherSyntheticNodeType::Directory
                                 : IndependentReviewPublisherSyntheticNodeType::RegularFile,
                current_owner, target.directory ? 0700 : 0600);
            switch (defect) {
            case Defect::Owner:
                metadata.owner = other_owner;
                break;
            case Defect::Mode:
                metadata.mode = 0777;
                break;
            case Defect::Type:
                metadata.type = IndependentReviewPublisherSyntheticNodeType::Other;
                break;
            case Defect::LinkCount:
                metadata.link_count = 2;
                break;
            }
            const auto target_event =
                defect == Defect::Mode ? IndependentReviewPublisherEvent::ModeNormalized
                : target.staging_root  ? IndependentReviewPublisherEvent::StagingCreated
                : target.directory     ? IndependentReviewPublisherEvent::DirectoryCreated
                                       : IndependentReviewPublisherEvent::FileCreated;
            IndependentReviewPublisherReport report;
            IndependentReviewPublisherHooks hooks;
            hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"metada"}; };
            hooks.outcome = [metadata, target_event,
                             target](const IndependentReviewPublisherObservation& observation)
                -> std::optional<IndependentReviewPublisherInjectedOutcome> {
                if (observation.event == target_event && observation.ordinal == target.ordinal) {
                    return IndependentReviewPublisherInjectedOutcome{true, false, 0, metadata,
                                                                     metadata};
                }
                return std::nullopt;
            };
            hooks.report = &report;

            const auto result = publishIndependentReviewArtifacts(request, hooks);

            requireErrorCode(result,
                             target.staging_root
                                 ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                                 : IndependentReviewPublicationErrorCode::CannotPublishDestination);
            QVERIFY(!QFileInfo::exists(destination));
            if (target.staging_root) {
                QVERIFY(QFileInfo(report.staging_path).isDir());
            } else {
                QVERIFY(!QFileInfo::exists(report.staging_path));
                QVERIFY(report.remaining_ledger_paths.isEmpty());
            }
            QVERIFY(countEvent(report, target_event) > 0);
        }
    }
#endif
}

void IndependentReviewPublisherTest::rejectsSyntheticPostRecordMetadataChanges() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    struct Target final {
        IndependentReviewArtifactKind kind;
        std::size_t ordinal;
        bool directory;
    };
    const std::array targets{
        Target{IndependentReviewArtifactKind::PreparedHandoff, 0, true},
        Target{IndependentReviewArtifactKind::FinalizedPack, 1, true},
        Target{IndependentReviewArtifactKind::PreparedHandoff, 1, false},
        Target{IndependentReviewArtifactKind::PreparedHandoff, 2, false},
        Target{IndependentReviewArtifactKind::FinalizedPack, 2, false},
        Target{IndependentReviewArtifactKind::FinalizedPack, 3, false},
    };
    const auto current_owner = static_cast<std::uint64_t>(::geteuid());
    const auto other_owner = current_owner == std::numeric_limits<std::uint64_t>::max()
                                 ? current_owner - 1
                                 : current_owner + 1;
    for (const auto& target : targets) {
        for (const bool change_owner : {false, true}) {
            QTemporaryDir parent;
            QVERIFY(parent.isValid());
            const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
            auto request = requestFor(target.kind, destination);
            bool tree_recorded = false;
            auto changed = syntheticStat(
                target.directory ? IndependentReviewPublisherSyntheticNodeType::Directory
                                 : IndependentReviewPublisherSyntheticNodeType::RegularFile,
                change_owner ? other_owner : current_owner,
                change_owner ? (target.directory ? 0700U : 0600U) : 0777U);
            IndependentReviewPublisherReport report;
            IndependentReviewPublisherHooks hooks;
            hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"record"}; };
            hooks.observe = [&tree_recorded,
                             target](const IndependentReviewPublisherObservation& observation) {
                const auto last_ordinal =
                    target.kind == IndependentReviewArtifactKind::PreparedHandoff ? 2U : 3U;
                if (observation.event == IndependentReviewPublisherEvent::BytesChecked &&
                    observation.ordinal == last_ordinal) {
                    tree_recorded = true;
                }
            };
            hooks.outcome = [&changed, &tree_recorded,
                             target](const IndependentReviewPublisherObservation& observation)
                -> std::optional<IndependentReviewPublisherInjectedOutcome> {
                if (tree_recorded &&
                    observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                    observation.ordinal == target.ordinal) {
                    return IndependentReviewPublisherInjectedOutcome{true, false, 0, changed,
                                                                     changed};
                }
                return std::nullopt;
            };
            hooks.report = &report;

            const auto result = publishIndependentReviewArtifacts(request, hooks);

            QVERIFY(tree_recorded);
            requireErrorCode(result,
                             IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
            QVERIFY(QFileInfo(report.staging_path).isDir());
            QVERIFY(!QFileInfo::exists(destination));
            QVERIFY(countEvent(report, IndependentReviewPublisherEvent::EntryRebound) > 0);
        }
    }
#endif
}

void IndependentReviewPublisherTest::rejectsInjectedAndObservedEntryRebinding() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool injected = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"statfl"}; };
        hooks.outcome = [&injected](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (!injected && observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                observation.ordinal == 1) {
                injected = true;
                return IndependentReviewPublisherInjectedOutcome{false, false, ESTALE};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(injected);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto escaped = QDir(parent.path()).filePath(QStringLiteral("escaped-handoff.json"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"membsw"}; };
        hooks.barrier = [&escaped,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                observation.ordinal == 1) {
                swapped = QFile::rename(observation.absolute_path, escaped) &&
                          writeNew(observation.absolute_path, QByteArrayView{"replacement\n"});
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
        }
        QVERIFY(QFileInfo::exists(escaped));
        QCOMPARE(readAll(QDir(report.staging_path).filePath(QStringLiteral("handoff.json"))),
                 QByteArray{"replacement\n"});
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool swapped = false;
        QString moved_root;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"rootsw"}; };
        hooks.barrier = [&moved_root,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                observation.ordinal == 0) {
                moved_root = observation.absolute_path + QStringLiteral(".moved");
                swapped = QDir().rename(observation.absolute_path, moved_root) &&
                          QDir().mkdir(observation.absolute_path);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(QFileInfo(report.staging_path).isDir());
        QVERIFY(QFileInfo(moved_root).isDir());
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool policy_changed = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"polic1"}; };
        hooks.barrier = [&parent, &policy_changed](
                            const IndependentReviewPublisherObservation& observation) {
            if (!policy_changed &&
                observation.event == IndependentReviewPublisherEvent::BeforeRename) {
                policy_changed = ::chmod(QFile::encodeName(parent.path()).constData(), 0777) == 0;
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);
        const auto restored = ::chmod(QFile::encodeName(parent.path()).constData(), 0700) == 0;

        QVERIFY(policy_changed);
        QVERIFY(restored);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(QFileInfo(report.staging_path).isDir());
        QVERIFY(!QFileInfo::exists(destination));
    }
#endif
}

void IndependentReviewPublisherTest::closesFinalTreeBindingWindow() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool before_rename = false;
        bool swapped = false;
        QString retained_tree;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fbind1"}; };
        hooks.observe = [&before_rename](const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::BeforeRename) {
                before_rename = true;
            }
        };
        hooks.barrier = [&before_rename, &report, &retained_tree,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && before_rename &&
                observation.event == IndependentReviewPublisherEvent::BeforeFinalTreeBinding) {
                retained_tree = report.staging_path + QStringLiteral(".retained");
                swapped = QFile::rename(report.staging_path, retained_tree) &&
                          createPreparedTree(report.staging_path);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(validatePreparedTree(retained_tree).has_value());
        QVERIFY(validatePreparedTree(report.staging_path).has_value());
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto retained_tree = QDir(parent.path()).filePath(QStringLiteral("retained-output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool reconciling = false;
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fbind2"}; };
        hooks.observe = [&reconciling](const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::Reconciled) {
                reconciling = true;
            }
        };
        hooks.barrier = [&destination, &reconciling, &retained_tree,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && reconciling &&
                observation.event == IndependentReviewPublisherEvent::BeforeFinalTreeBinding &&
                observation.absolute_path == destination) {
                swapped =
                    QFile::rename(destination, retained_tree) && createPreparedTree(destination);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationIdentityFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=ok"));
        }
        QVERIFY(validatePreparedTree(retained_tree).has_value());
        QVERIFY(validatePreparedTree(destination).has_value());
        QVERIFY(!QFileInfo::exists(report.staging_path));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto retained_tree =
            QDir(parent.path()).filePath(QStringLiteral("retained-member-seam-output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool reconciling = false;
        bool closing_sweep = false;
        std::size_t target_rebinds = 0;
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fbind3"}; };
        hooks.observe = [&closing_sweep, &destination, &reconciling, &target_rebinds](
                            const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::Reconciled) {
                reconciling = true;
            } else if (reconciling &&
                       observation.event ==
                           IndependentReviewPublisherEvent::BeforeFinalTreeBinding &&
                       observation.absolute_path == destination) {
                closing_sweep = true;
            } else if (closing_sweep &&
                       observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                       observation.ordinal == 2 &&
                       observation.absolute_path.startsWith(destination)) {
                ++target_rebinds;
            }
        };
        hooks.barrier = [&closing_sweep, &destination, &retained_tree, &swapped, &target_rebinds](
                            const IndependentReviewPublisherObservation& observation) {
            if (!swapped && closing_sweep &&
                observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                observation.ordinal == 2 && observation.absolute_path.startsWith(destination) &&
                target_rebinds == 4) {
                swapped =
                    QFile::rename(destination, retained_tree) && createPreparedTree(destination);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationIdentityFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=ok"));
        }
        QVERIFY(validatePreparedTree(retained_tree).has_value());
        QVERIFY(validatePreparedTree(destination).has_value());
        QVERIFY(!QFileInfo::exists(report.staging_path));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto retained_resources =
            QDir(parent.path()).filePath(QStringLiteral("retained-closing-resources"));
        auto request = requestFor(IndependentReviewArtifactKind::FinalizedPack, destination);
        bool reconciling = false;
        bool closing_sweep = false;
        std::size_t target_rebinds = 0;
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fbind4"}; };
        hooks.observe = [&closing_sweep, &destination, &reconciling, &target_rebinds](
                            const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::Reconciled) {
                reconciling = true;
            } else if (reconciling &&
                       observation.event ==
                           IndependentReviewPublisherEvent::BeforeFinalTreeBinding &&
                       observation.absolute_path == destination) {
                closing_sweep = true;
            } else if (closing_sweep &&
                       observation.event == IndependentReviewPublisherEvent::EntryRebound &&
                       observation.ordinal == 3 &&
                       observation.absolute_path.startsWith(destination)) {
                ++target_rebinds;
            }
        };
        hooks.barrier =
            [&closing_sweep, &destination, &retained_resources, &swapped,
             &target_rebinds](const IndependentReviewPublisherObservation& observation) {
                if (swapped || !closing_sweep ||
                    observation.event != IndependentReviewPublisherEvent::EntryRebound ||
                    observation.ordinal != 3 ||
                    !observation.absolute_path.startsWith(destination) || target_rebinds != 4) {
                    return;
                }
                const auto resources = QDir(destination).filePath(QStringLiteral("resources"));
                const auto replacement_review =
                    QDir(resources).filePath(QStringLiteral("realism-review.json"));
                swapped = QFile::rename(resources, retained_resources) && QDir().mkdir(resources) &&
                          ::chmod(QFile::encodeName(resources).constData(), 0700) == 0 &&
                          writeNew(replacement_review, final_review_bytes) &&
                          ::chmod(QFile::encodeName(replacement_review).constData(), 0600) == 0;
            };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationIdentityFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=ok"));
        }
        QCOMPARE(readAll(QDir(retained_resources).filePath(QStringLiteral("realism-review.json"))),
                 QByteArray{final_review_bytes});
        QCOMPARE(
            readAll(QDir(destination).filePath(QStringLiteral("resources/realism-review.json"))),
            QByteArray{final_review_bytes});
        QCOMPARE(readAll(QDir(destination).filePath(QStringLiteral("manifest.json"))),
                 QByteArray{final_manifest_bytes});
        QVERIFY(!QFileInfo::exists(report.staging_path));
    }
#endif
}

void IndependentReviewPublisherTest::preservesCleanupSeamReplacements() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto escaped =
            QDir(parent.path()).filePath(QStringLiteral("escaped-declaration.json"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnrp1"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                           observation.ordinal == 0
                       ? IndependentReviewPublisherInjectedAction::FailBefore
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&escaped,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 2) {
                swapped = QFile::rename(observation.absolute_path, escaped) &&
                          writeNew(observation.absolute_path, declaration_template_bytes);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
        }
        const auto replacement =
            QDir(report.staging_path).filePath(QStringLiteral("review-declaration.template.json"));
        QCOMPARE(readAll(replacement), QByteArray{declaration_template_bytes});
        QCOMPARE(readAll(escaped), QByteArray{declaration_template_bytes});
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto escaped_root =
            QDir(parent.path()).filePath(QStringLiteral("escaped-child-removal-root"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnrp3"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                           observation.ordinal == 0
                       ? IndependentReviewPublisherInjectedAction::FailBefore
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&escaped_root, &report,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 2) {
                swapped = QFile::rename(report.staging_path, escaped_root) &&
                          createPreparedTree(report.staging_path);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(validatePreparedTree(escaped_root).has_value());
        QVERIFY(validatePreparedTree(report.staging_path).has_value());
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto escaped_root =
            QDir(parent.path()).filePath(QStringLiteral("escaped-retried-cleanup-root"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        std::size_t removal_callbacks = 0;
        bool retry_injected = false;
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnrp5"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                           observation.ordinal == 0
                       ? IndependentReviewPublisherInjectedAction::FailBefore
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&escaped_root, &report, &removal_callbacks,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (observation.event != IndependentReviewPublisherEvent::CleanupRemoved ||
                observation.ordinal != 2) {
                return;
            }
            ++removal_callbacks;
            if (removal_callbacks == 2) {
                swapped = QFile::rename(report.staging_path, escaped_root) &&
                          createPreparedTree(report.staging_path);
            }
        };
        hooks.outcome = [&removal_callbacks,
                         &retry_injected](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (!retry_injected &&
                observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 2 && removal_callbacks == 1) {
                retry_injected = true;
                return IndependentReviewPublisherInjectedOutcome{false, false, EINTR};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(retry_injected);
        QCOMPARE(removal_callbacks, std::size_t{2});
        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(validatePreparedTree(escaped_root).has_value());
        QVERIFY(validatePreparedTree(report.staging_path).has_value());
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        const auto escaped_resources =
            QDir(parent.path()).filePath(QStringLiteral("escaped-resources"));
        auto request = requestFor(IndependentReviewArtifactKind::FinalizedPack, destination);
        bool swapped = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnrp4"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                           observation.ordinal == 0
                       ? IndependentReviewPublisherInjectedAction::FailBefore
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&escaped_resources, &report,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (swapped || observation.event != IndependentReviewPublisherEvent::CleanupRemoved ||
                observation.ordinal != 3) {
                return;
            }
            const auto resources = QDir(report.staging_path).filePath(QStringLiteral("resources"));
            const auto replacement_review =
                QDir(resources).filePath(QStringLiteral("realism-review.json"));
            swapped = QFile::rename(resources, escaped_resources) && QDir().mkdir(resources) &&
                      ::chmod(QFile::encodeName(resources).constData(), 0700) == 0 &&
                      writeNew(replacement_review, final_review_bytes) &&
                      ::chmod(QFile::encodeName(replacement_review).constData(), 0600) == 0;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
        }
        const auto replacement_review =
            QDir(report.staging_path).filePath(QStringLiteral("resources/realism-review.json"));
        const auto retained_review =
            QDir(escaped_resources).filePath(QStringLiteral("realism-review.json"));
        QCOMPARE(readAll(replacement_review), QByteArray{final_review_bytes});
        QCOMPARE(readAll(retained_review), QByteArray{final_review_bytes});
        QCOMPARE(readAll(QDir(report.staging_path).filePath(QStringLiteral("manifest.json"))),
                 QByteArray{final_manifest_bytes});
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool swapped = false;
        QString escaped_root;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"clnrp2"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                           observation.ordinal == 0
                       ? IndependentReviewPublisherInjectedAction::FailBefore
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&escaped_root,
                         &swapped](const IndependentReviewPublisherObservation& observation) {
            if (!swapped && observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 0) {
                escaped_root = observation.absolute_path + QStringLiteral(".escaped");
                swapped =
                    QFile::rename(observation.absolute_path, escaped_root) &&
                    QDir().mkdir(observation.absolute_path) &&
                    ::chmod(QFile::encodeName(observation.absolute_path).constData(), 0700) == 0;
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(swapped);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=not_attempted"));
        }
        QVERIFY(QFileInfo(report.staging_path).isDir());
        QVERIFY(entriesAt(report.staging_path).isEmpty());
        QVERIFY(QFileInfo(escaped_root).isDir());
        QVERIFY(entriesAt(escaped_root).isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
    }
#endif
}

void IndependentReviewPublisherTest::reportsInjectedCleanupTelemetryMatrix() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    enum class CleanupFault {
        ProperSubset,
        AmbiguousRemoval,
        RootRemovedReportedFailure,
        DeletedThenSyncFailed,
        SyncedThenAmbiguous,
    };
    for (const auto fault :
         {CleanupFault::ProperSubset, CleanupFault::AmbiguousRemoval,
          CleanupFault::RootRemovedReportedFailure, CleanupFault::DeletedThenSyncFailed,
          CleanupFault::SyncedThenAmbiguous}) {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool root_removal_attempted = false;
        bool cleanup_synced = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"telem1"}; };
        hooks.inject = [fault, &root_removal_attempted,
                        &cleanup_synced](const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                observation.ordinal == 0) {
                return IndependentReviewPublisherInjectedAction::FailBefore;
            }
            if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 0) {
                root_removal_attempted = true;
            }
            if (fault == CleanupFault::SyncedThenAmbiguous &&
                observation.event == IndependentReviewPublisherEvent::CleanupSynced) {
                cleanup_synced = true;
                return IndependentReviewPublisherInjectedAction::FailBefore;
            }
            return IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.outcome = [fault, &root_removal_attempted,
                         &cleanup_synced](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 2) {
                if (fault == CleanupFault::ProperSubset) {
                    return IndependentReviewPublisherInjectedOutcome{false, true, EIO};
                }
                if (fault == CleanupFault::AmbiguousRemoval) {
                    return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
                }
            }
            if (fault == CleanupFault::RootRemovedReportedFailure &&
                observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 0) {
                return IndependentReviewPublisherInjectedOutcome{false, true, EIO};
            }
            if (fault == CleanupFault::DeletedThenSyncFailed && root_removal_attempted &&
                observation.event == IndependentReviewPublisherEvent::DirectorySync) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            if (fault == CleanupFault::SyncedThenAmbiguous && cleanup_synced &&
                observation.event == IndependentReviewPublisherEvent::EntryRebound) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        QVERIFY(!result.has_value());
        if (result.has_value()) {
            continue;
        }
        if (fault == CleanupFault::ProperSubset) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
            QVERIFY(QFileInfo(report.staging_path).isDir());
            QVERIFY(!QFileInfo::exists(
                QDir(report.staging_path)
                    .filePath(QStringLiteral("review-declaration.template.json"))));
            QVERIFY(QFileInfo::exists(
                QDir(report.staging_path).filePath(QStringLiteral("handoff.json"))));
        } else if (fault == CleanupFault::AmbiguousRemoval) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=reachable;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
            QVERIFY(validatePreparedTree(report.staging_path).has_value());
        } else if (fault == CleanupFault::RootRemovedReportedFailure) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unreachable;"
                                    "cleanup_residue=absent;parent_fsync=not_attempted"));
            QVERIFY(!QFileInfo::exists(report.staging_path));
            QVERIFY(root_removal_attempted);
        } else if (fault == CleanupFault::DeletedThenSyncFailed) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unreachable;"
                                    "cleanup_residue=absent;parent_fsync=failed"));
            QVERIFY(!QFileInfo::exists(report.staging_path));
            QVERIFY(report.remaining_ledger_paths.isEmpty());
            QVERIFY(root_removal_attempted);
        } else {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=unknown;"
                                    "cleanup_residue=unknown;parent_fsync=ok"));
            QVERIFY(!QFileInfo::exists(report.staging_path));
            QVERIFY(report.remaining_ledger_paths.isEmpty());
            QVERIFY(root_removal_attempted);
            QVERIFY(cleanup_synced);
        }
        QVERIFY(!QFileInfo::exists(destination));
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        bool cleanup_failed = false;
        bool moved_before_final_read = false;
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"telem2"}; };
        hooks.inject = [&cleanup_failed](const IndependentReviewPublisherObservation& observation) {
            if (observation.event == IndependentReviewPublisherEvent::AfterStagedValidation &&
                observation.ordinal == 0) {
                return IndependentReviewPublisherInjectedAction::FailBefore;
            }
            if (observation.event == IndependentReviewPublisherEvent::CleanupRemoved &&
                observation.ordinal == 2) {
                cleanup_failed = true;
                return IndependentReviewPublisherInjectedAction::FailBefore;
            }
            return IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.barrier = [&cleanup_failed, &destination, &moved_before_final_read,
                         &report](const IndependentReviewPublisherObservation& observation) {
            if (cleanup_failed && !moved_before_final_read &&
                observation.event == IndependentReviewPublisherEvent::ControllerRebound) {
                moved_before_final_read = QDir().rename(report.staging_path, destination);
            }
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        QVERIFY(cleanup_failed);
        QVERIFY(moved_before_final_read);
        requireErrorCode(result, IndependentReviewPublicationErrorCode::PublicationCleanupFailed);
        if (!result.has_value()) {
            QCOMPARE(result.error().message,
                     QStringLiteral("original_staging_reachability=reachable;"
                                    "cleanup_residue=present;parent_fsync=not_attempted"));
        }
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(validatePreparedTree(destination).has_value());
    }
#endif
}

void IndependentReviewPublisherTest::exhaustsSyntheticRenameReconciliationStates() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    enum class State {
        Absent,
        Exact,
        Other,
        Ambiguous,
    };
    const std::array states{State::Absent, State::Exact, State::Other, State::Ambiguous};
    const auto outcomeFor = [](State state) {
        switch (state) {
        case State::Absent:
            return IndependentReviewPublisherInjectedOutcome{false, false, ENOENT};
        case State::Exact:
            return IndependentReviewPublisherInjectedOutcome{true, false, 0};
        case State::Other: {
            IndependentReviewPublisherSyntheticStat named;
            named.inode = 0;
            return IndependentReviewPublisherInjectedOutcome{true, false, 0, std::nullopt, named};
        }
        case State::Ambiguous:
            return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
        }
        Q_UNREACHABLE_RETURN(IndependentReviewPublisherInjectedOutcome{});
    };

    for (const auto source_state : states) {
        for (const auto destination_state : states) {
            QTemporaryDir parent;
            QVERIFY(parent.isValid());
            const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
            auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
            bool reconciling = false;
            IndependentReviewPublisherReport report;
            IndependentReviewPublisherHooks hooks;
            hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"states"}; };
            hooks.observe =
                [&reconciling](const IndependentReviewPublisherObservation& observation) {
                    if (observation.event == IndependentReviewPublisherEvent::Reconciled) {
                        reconciling = true;
                    }
                };
            hooks.outcome =
                [&destination, &outcomeFor, &reconciling, &report, source_state,
                 destination_state](const IndependentReviewPublisherObservation& observation)
                -> std::optional<IndependentReviewPublisherInjectedOutcome> {
                if (observation.event == IndependentReviewPublisherEvent::RenameAttempted) {
                    return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
                }
                if (!reconciling ||
                    observation.event != IndependentReviewPublisherEvent::EntryRebound ||
                    observation.ordinal != 0) {
                    return std::nullopt;
                }
                if (observation.absolute_path == report.staging_path) {
                    return outcomeFor(source_state);
                }
                if (observation.absolute_path == destination) {
                    return outcomeFor(destination_state);
                }
                return std::nullopt;
            };
            hooks.report = &report;

            const auto result = publishIndependentReviewArtifacts(request, hooks);

            const auto expected =
                source_state == State::Exact && destination_state == State::Absent
                    ? IndependentReviewPublicationErrorCode::CannotPublishDestination
                : source_state == State::Exact && destination_state == State::Other
                    ? IndependentReviewPublicationErrorCode::CannotPublishDestination
                : source_state == State::Absent && destination_state == State::Exact
                    ? IndependentReviewPublicationErrorCode::PublicationOutcomeUncertain
                    : IndependentReviewPublicationErrorCode::PublicationIdentityFailed;
            requireErrorCode(result, expected);
            QVERIFY(!QFileInfo::exists(destination));
            const auto cleanup_expected =
                source_state == State::Exact &&
                (destination_state == State::Absent || destination_state == State::Other);
            QCOMPARE(QFileInfo::exists(report.staging_path), !cleanup_expected);
        }
    }
#endif
}

void IndependentReviewPublisherTest::reconcilesInjectedRenameAndSyncOutcomes() {
#if !defined(Q_OS_LINUX)
    QSKIP("Independent-review publication requires Linux");
#else
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"renok1"}; };
        hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::RenameAttempted) {
                return IndependentReviewPublisherInjectedOutcome{true, true, 0};
            }
            if (observation.event == IndependentReviewPublisherEvent::ParentSync) {
                return IndependentReviewPublisherInjectedOutcome{true, false, 0};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireSuccessfulTree(result, report, destination);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::RenameAttempted), 1);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::RenameReturned), 1);
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::ParentSync), 1);
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"renaft"}; };
        hooks.inject = [](const IndependentReviewPublisherObservation& observation) {
            return observation.event == IndependentReviewPublisherEvent::RenameAttempted
                       ? IndependentReviewPublisherInjectedAction::FailAfter
                       : IndependentReviewPublisherInjectedAction::Continue;
        };
        hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::ParentSync) {
                return IndependentReviewPublisherInjectedOutcome{true, false, 0};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result,
                         IndependentReviewPublicationErrorCode::PublicationOutcomeUncertain);
        QVERIFY(validatePreparedTree(destination).has_value());
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QCOMPARE(countEvent(report, IndependentReviewPublisherEvent::RenameAttempted), 1);
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"fsync1"}; };
        hooks.outcome = [](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::FileSync &&
                observation.ordinal == 1) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
        QVERIFY(countEvent(report, IndependentReviewPublisherEvent::FileSync) > 0);
    }

    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto destination = QDir(parent.path()).filePath(QStringLiteral("output"));
        auto request = requestFor(IndependentReviewArtifactKind::PreparedHandoff, destination);
        IndependentReviewPublisherReport report;
        IndependentReviewPublisherHooks hooks;
        hooks.staging_suffix_source = [](std::size_t) { return QByteArray{"dsync1"}; };
        hooks.outcome = [&report](const IndependentReviewPublisherObservation& observation)
            -> std::optional<IndependentReviewPublisherInjectedOutcome> {
            if (observation.event == IndependentReviewPublisherEvent::DirectorySync &&
                !report.staging_path.isEmpty() &&
                observation.absolute_path == report.staging_path) {
                return IndependentReviewPublisherInjectedOutcome{false, false, EIO};
            }
            return std::nullopt;
        };
        hooks.report = &report;

        const auto result = publishIndependentReviewArtifacts(request, hooks);

        requireErrorCode(result, IndependentReviewPublicationErrorCode::CannotPublishDestination);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(report.staging_path));
        QVERIFY(report.remaining_ledger_paths.isEmpty());
        QVERIFY(countEvent(report, IndependentReviewPublisherEvent::DirectorySync) > 0);
    }
#endif
}

} // namespace

QTEST_MAIN(IndependentReviewPublisherTest)
#include "tst_independent_review_publisher.moc"
