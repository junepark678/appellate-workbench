#include "appellate/packs/pack_catalog.hpp"
#include "pack_catalog_p.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using appellate::packs::CatalogError;
using appellate::packs::CatalogErrorCode;
using appellate::packs::PackCatalog;
using appellate::packs::PackCatalogSnapshot;
using appellate::packs::detail::acquireSecureScratchContext;
using appellate::packs::detail::CatalogEvent;
using appellate::packs::detail::CatalogHooks;
using appellate::packs::detail::CatalogLockMode;
using appellate::packs::detail::CatalogOperandFailureCode;
using appellate::packs::detail::CatalogOperation;
using appellate::packs::detail::CatalogProtectedInputDirectory;
using appellate::packs::detail::CatalogReport;
using appellate::packs::detail::CatalogSubject;
using appellate::packs::detail::inspectProtectedCatalogInputs;
using appellate::packs::detail::PackCatalogFactory;
using appellate::packs::detail::PackCatalogSnapshotFactory;
using appellate::packs::detail::retainCatalogOperand;
using appellate::packs::detail::validateCatalogOperandSpelling;

#if defined(Q_OS_LINUX)

[[nodiscard]] auto directoryIdentity(const QString& path)
    -> std::expected<CatalogProtectedInputDirectory, int> {
    struct stat status{};
    const auto encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return std::unexpected(errno);
    }
    return CatalogProtectedInputDirectory{static_cast<std::uint64_t>(status.st_dev),
                                          static_cast<std::uint64_t>(status.st_ino)};
}

[[nodiscard]] bool createCatalogObjects(const QString& directory, std::size_t begin,
                                        std::size_t count, QByteArrayView suffix) {
    const auto encoded_directory = QFile::encodeName(directory);
    const auto parent =
        ::open(encoded_directory.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) {
        return false;
    }
    for (std::size_t offset = 0; offset < count; ++offset) {
        auto name =
            QByteArray::number(static_cast<qulonglong>(begin + offset), 16).rightJustified(64, '0');
        name.append(suffix);
        const auto descriptor = ::openat(
            parent, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444);
        if (descriptor < 0) {
            static_cast<void>(::close(parent));
            return false;
        }
        if (::close(descriptor) != 0) {
            static_cast<void>(::close(parent));
            return false;
        }
    }
    return ::close(parent) == 0;
}

#endif

class PackCatalogProtectedInputsTest final : public QObject {
    Q_OBJECT

  private slots:
    void retainsExactEncodedCatalogOperandBytes();
    void classifiesRelativeOperandFailures();
    void retainsOperandsBeforeConsumingScratch();
    void inspectsRetainedSnapshotAnchorWithOneBoundedInventory();
};

void PackCatalogProtectedInputsTest::retainsExactEncodedCatalogOperandBytes() {
#if !defined(Q_OS_LINUX)
    QSKIP("Retained catalog operands require Linux");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto previous_directory = QDir::currentPath();
    auto restore_directory = qScopeGuard(
        [&previous_directory] { static_cast<void>(QDir::setCurrent(previous_directory)); });
    QVERIFY(QDir::setCurrent(temporary.path()));

    auto supplied = QString::fromUtf8("검토-\xF0\x9F\xA7\xAA/catalogu\xC3\xA9");
    const auto original_supplied = supplied;
    const auto expected_supplied_bytes = QFile::encodeName(supplied);
    {
        auto catalog = PackCatalog::open(supplied);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    }
    auto spelling = validateCatalogOperandSpelling(supplied);
    QVERIFY2(spelling.has_value(), spelling ? "" : qPrintable(spelling.error().message));
    auto retained = retainCatalogOperand(std::move(*spelling));
    QVERIFY2(retained.has_value(), retained ? "" : qPrintable(retained.error().message));
    QVERIFY(!spelling->isValid());
    QCOMPARE(retained->encodedSuppliedPath(), expected_supplied_bytes);
    QCOMPARE(retained->encodedAbsolutePath(), QFile::encodeName(retained->immutableAbsolutePath()));

    const auto diagnostic_components =
        retained->immutableAbsolutePath().sliced(1).split(u'/', Qt::KeepEmptyParts);
    const auto& encoded_components = retained->encodedAbsoluteComponents();
    QCOMPARE(encoded_components.size(), static_cast<std::size_t>(diagnostic_components.size()));
    for (qsizetype index = 0; index < diagnostic_components.size(); ++index) {
        const auto& encoded = encoded_components.at(static_cast<std::size_t>(index));
        QCOMPARE(encoded, QFile::encodeName(diagnostic_components.at(index)));
        QCOMPARE(QFile::decodeName(encoded), diagnostic_components.at(index));
    }

    supplied.fill(u'x');
    QCOMPARE(retained->encodedSuppliedPath(), expected_supplied_bytes);
    QVERIFY(QDir::setCurrent(previous_directory));
    restore_directory.dismiss();

    auto scratch = acquireSecureScratchContext();
    QVERIFY2(scratch.has_value(), scratch ? "" : qPrintable(scratch.error().message));
    const auto attached = std::move(*retained).attachToSecureScratch(*scratch);
    QVERIFY2(attached.has_value(), attached ? "" : qPrintable(attached.error().message));
    const auto mismatched = PackCatalogSnapshotFactory::openExisting(supplied, std::move(*scratch));
    QVERIFY(!mismatched.has_value());
    QCOMPARE(mismatched.error().code, CatalogErrorCode::InvalidConfiguration);
    QVERIFY(scratch->isValid());
    const auto snapshot =
        PackCatalogSnapshotFactory::openExisting(original_supplied, std::move(*scratch));
    QVERIFY2(snapshot.has_value(), snapshot ? "" : qPrintable(snapshot.error().message));
    QVERIFY(!retained->isValid());

    auto second_scratch = acquireSecureScratchContext();
    QVERIFY2(second_scratch.has_value(),
             second_scratch ? "" : qPrintable(second_scratch.error().message));
    CatalogReport second_report;
    CatalogHooks second_hooks;
    second_hooks.report = &second_report;
    const auto consumed =
        std::move(*retained).attachToSecureScratch(*second_scratch, std::move(second_hooks));
    QVERIFY(!consumed.has_value());
    QCOMPARE(consumed.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(second_scratch->isValid());
    QCOMPARE(second_report.scratch_acquisitions, std::size_t{0});
#endif
}

void PackCatalogProtectedInputsTest::classifiesRelativeOperandFailures() {
#if !defined(Q_OS_LINUX)
    QSKIP("Retained catalog operands require Linux");
#else
    const auto malformed = retainCatalogOperand(QStringLiteral("relative//catalog"));
    QVERIFY(!malformed.has_value());
    QCOMPARE(malformed.error().code, CatalogOperandFailureCode::InvalidArguments);

    QStringList maximum_relative_components;
    maximum_relative_components.reserve(128);
    for (std::size_t index = 0; index < 128; ++index) {
        maximum_relative_components.push_back(QStringLiteral("a"));
    }
    const auto valid_relative_spelling = maximum_relative_components.join(u'/');
    auto retained_spelling = validateCatalogOperandSpelling(valid_relative_spelling);
    QVERIFY(retained_spelling.has_value());
    const auto environmentally_infeasible = retainCatalogOperand(std::move(*retained_spelling));
    QVERIFY(!environmentally_infeasible.has_value());
    QVERIFY(!retained_spelling->isValid());
    QCOMPARE(environmentally_infeasible.error().code,
             CatalogOperandFailureCode::UnsupportedEnvironment);
#endif
}

void PackCatalogProtectedInputsTest::retainsOperandsBeforeConsumingScratch() {
#if !defined(Q_OS_LINUX)
    QSKIP("Retained catalog operands require Linux");
#else
    const auto invalid_absolute = QStringLiteral("/") + QString(256, u'a');
    QStringList maximum_relative_components;
    maximum_relative_components.reserve(128);
    for (std::size_t index = 0; index < 128; ++index) {
        maximum_relative_components.push_back(QStringLiteral("a"));
    }
    const auto environmentally_infeasible = maximum_relative_components.join(u'/');

    const auto rejects_without_consuming_scratch = [](const QString& operand,
                                                      CatalogErrorCode expected, auto open) {
        auto scratch = acquireSecureScratchContext();
        QVERIFY2(scratch.has_value(), scratch ? "" : qPrintable(scratch.error().message));
        CatalogReport report;
        CatalogHooks hooks;
        hooks.report = &report;
        const auto opened = open(operand, std::move(*scratch), std::move(hooks));
        QVERIFY(!opened.has_value());
        QCOMPARE(opened.error().code, expected);
        QVERIFY(scratch->isValid());
        QCOMPARE(report.scratch_acquisitions, std::size_t{0});
    };
    const auto open_writable = [](const QString& operand, auto&& scratch, CatalogHooks hooks) {
        using Result = std::expected<std::unique_ptr<PackCatalog>, CatalogError>;
        auto retained = retainCatalogOperand(operand);
        if (!retained) {
            const auto code = retained.error().code == CatalogOperandFailureCode::InvalidArguments
                                  ? CatalogErrorCode::InvalidConfiguration
                                  : CatalogErrorCode::CannotOpen;
            return Result(std::unexpected(CatalogError{code, retained.error().message}));
        }
        const auto attached = std::move(*retained).attachToSecureScratch(scratch);
        if (!attached) {
            return Result(std::unexpected(attached.error()));
        }
        return PackCatalogFactory::open(operand, std::move(scratch), std::move(hooks));
    };
    const auto open_snapshot = [](const QString& operand, auto&& scratch, CatalogHooks hooks) {
        using Result = std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>;
        auto retained = retainCatalogOperand(operand);
        if (!retained) {
            const auto code = retained.error().code == CatalogOperandFailureCode::InvalidArguments
                                  ? CatalogErrorCode::InvalidConfiguration
                                  : CatalogErrorCode::CannotOpen;
            return Result(std::unexpected(CatalogError{code, retained.error().message}));
        }
        const auto attached = std::move(*retained).attachToSecureScratch(scratch, std::move(hooks));
        if (!attached) {
            return Result(std::unexpected(attached.error()));
        }
        return PackCatalogSnapshotFactory::openExisting(operand, std::move(scratch));
    };
    rejects_without_consuming_scratch(invalid_absolute, CatalogErrorCode::InvalidConfiguration,
                                      open_writable);
    rejects_without_consuming_scratch(invalid_absolute, CatalogErrorCode::InvalidConfiguration,
                                      open_snapshot);
    rejects_without_consuming_scratch(environmentally_infeasible, CatalogErrorCode::CannotOpen,
                                      open_writable);
    rejects_without_consuming_scratch(environmentally_infeasible, CatalogErrorCode::CannotOpen,
                                      open_snapshot);

    const auto invalid_writable = PackCatalog::open(invalid_absolute);
    QVERIFY(!invalid_writable.has_value());
    QCOMPARE(invalid_writable.error().code, CatalogErrorCode::InvalidConfiguration);
    const auto invalid_snapshot = PackCatalogSnapshot::openExisting(invalid_absolute);
    QVERIFY(!invalid_snapshot.has_value());
    QCOMPARE(invalid_snapshot.error().code, CatalogErrorCode::InvalidConfiguration);
    const auto infeasible_writable = PackCatalog::open(environmentally_infeasible);
    QVERIFY(!infeasible_writable.has_value());
    QCOMPARE(infeasible_writable.error().code, CatalogErrorCode::CannotOpen);
    const auto infeasible_snapshot = PackCatalogSnapshot::openExisting(environmentally_infeasible);
    QVERIFY(!infeasible_snapshot.has_value());
    QCOMPARE(infeasible_snapshot.error().code, CatalogErrorCode::CannotOpen);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto retained_parent = QDir(temporary.path()).filePath(QStringLiteral("retained"));
    const auto decoy_parent = QDir(temporary.path()).filePath(QStringLiteral("decoy"));
    QVERIFY(QDir{}.mkpath(retained_parent));
    QVERIFY(QDir{}.mkpath(decoy_parent));
    const auto previous_directory = QDir::currentPath();
    auto restore_directory = qScopeGuard(
        [&previous_directory] { static_cast<void>(QDir::setCurrent(previous_directory)); });
    QVERIFY(QDir::setCurrent(retained_parent));
    auto retained = retainCatalogOperand(QStringLiteral("catalog"));
    QVERIFY2(retained.has_value(), retained ? "" : qPrintable(retained.error().message));
    QVERIFY(QDir::setCurrent(decoy_parent));

    auto scratch = acquireSecureScratchContext();
    QVERIFY2(scratch.has_value(), scratch ? "" : qPrintable(scratch.error().message));
    CatalogReport report;
    CatalogHooks hooks;
    hooks.report = &report;
    const auto attached = std::move(*retained).attachToSecureScratch(*scratch);
    QVERIFY2(attached.has_value(), attached ? "" : qPrintable(attached.error().message));
    auto opened =
        PackCatalogFactory::open(QStringLiteral("catalog"), std::move(*scratch), std::move(hooks));
    QVERIFY2(opened.has_value(), opened ? "" : qPrintable(opened.error().message));
    QVERIFY(!retained->isValid());
    QVERIFY(!scratch->isValid());
    QCOMPARE(report.scratch_acquisitions, std::size_t{1});
    QCOMPARE((*opened)->rootDirectory(), QDir(retained_parent).filePath(QStringLiteral("catalog")));
    QVERIFY(!QFileInfo::exists(QDir(decoy_parent).filePath(QStringLiteral("catalog"))));
    opened->reset();

    auto second_scratch = acquireSecureScratchContext();
    QVERIFY2(second_scratch.has_value(),
             second_scratch ? "" : qPrintable(second_scratch.error().message));
    CatalogReport second_report;
    CatalogHooks second_hooks;
    second_hooks.report = &second_report;
    const auto consumed =
        std::move(*retained).attachToSecureScratch(*second_scratch, std::move(second_hooks));
    QVERIFY(!consumed.has_value());
    QCOMPARE(consumed.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(second_scratch->isValid());
    QCOMPARE(second_report.scratch_acquisitions, std::size_t{0});
#endif
}

void PackCatalogProtectedInputsTest::inspectsRetainedSnapshotAnchorWithOneBoundedInventory() {
#if !defined(Q_OS_LINUX)
    QSKIP("Protected catalog inspection requires Linux");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    }
    auto snapshot = PackCatalogSnapshot::openExisting(catalog_root);
    QVERIFY2(snapshot.has_value(), snapshot ? "" : qPrintable(snapshot.error().message));

    CatalogReport report;
    CatalogHooks hooks;
    hooks.report = &report;
    const auto protected_inputs = inspectProtectedCatalogInputs(**snapshot, std::move(hooks));
    QVERIFY2(protected_inputs.has_value(),
             protected_inputs ? "" : qPrintable(protected_inputs.error().message));
    QCOMPARE(protected_inputs->immutable_root_path, catalog_root);
    QCOMPARE(protected_inputs->directories.size(), std::size_t{3});
    const auto root_identity = directoryIdentity(catalog_root);
    const auto archives_identity =
        directoryIdentity(QDir(catalog_root).filePath(QStringLiteral("archives")));
    const auto blobs_identity =
        directoryIdentity(QDir(catalog_root).filePath(QStringLiteral("blobs")));
    QVERIFY(root_identity.has_value());
    QVERIFY(archives_identity.has_value());
    QVERIFY(blobs_identity.has_value());
    const std::array expected_directories{*root_identity, *archives_identity, *blobs_identity};
    QVERIFY(std::ranges::equal(protected_inputs->directories, expected_directories));
    QCOMPARE(report.lock_attempts, std::size_t{1});
    const auto observed_lock = [&report](CatalogEvent event) {
        return std::ranges::any_of(report.observations, [event](const auto& observation) {
            return observation.event == event &&
                   observation.subject == CatalogSubject::CatalogRoot &&
                   observation.operation == CatalogOperation::ProtectedInputInspection &&
                   observation.lock_mode == CatalogLockMode::Shared;
        });
    };
    QVERIFY(observed_lock(CatalogEvent::RootLockAttempted));
    QVERIFY(observed_lock(CatalogEvent::RootLockAcquired));
    QVERIFY(observed_lock(CatalogEvent::RootLockReleased));

    const auto root_entry_count = protected_inputs->aggregate_entry_count;
    QVERIFY(root_entry_count < std::size_t{20'000});
    const auto object_count = std::size_t{20'000} - root_entry_count;
    const auto archive_count = object_count / 2U;
    const auto blob_count = object_count - archive_count;
    QVERIFY(createCatalogObjects(QDir(catalog_root).filePath(QStringLiteral("archives")), 0,
                                 archive_count, QByteArrayView(".awpack")));
    QVERIFY(createCatalogObjects(QDir(catalog_root).filePath(QStringLiteral("blobs")),
                                 archive_count, blob_count, {}));
    const auto at_budget = inspectProtectedCatalogInputs(**snapshot);
    QVERIFY2(at_budget.has_value(), at_budget ? "" : qPrintable(at_budget.error().message));
    QCOMPARE(at_budget->aggregate_entry_count, std::size_t{20'000});
    QVERIFY(at_budget->directories == protected_inputs->directories);

    QVERIFY(createCatalogObjects(QDir(catalog_root).filePath(QStringLiteral("blobs")),
                                 archive_count + blob_count, 1, {}));
    const auto beyond_budget = inspectProtectedCatalogInputs(**snapshot);
    QVERIFY(!beyond_budget.has_value());
    QCOMPARE(beyond_budget.error().code, CatalogErrorCode::CorruptCatalog);

    const auto root_descriptor = ::open(QFile::encodeName(catalog_root).constData(),
                                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    QVERIFY(root_descriptor >= 0);
    QVERIFY(::flock(root_descriptor, LOCK_EX | LOCK_NB) == 0);
    const auto busy = inspectProtectedCatalogInputs(**snapshot);
    QVERIFY(!busy.has_value());
    QCOMPARE(busy.error().code, CatalogErrorCode::CatalogBusy);
    QVERIFY(::flock(root_descriptor, LOCK_UN) == 0);
    QVERIFY(::close(root_descriptor) == 0);

    const auto replacement_root =
        QDir(temporary.path()).filePath(QStringLiteral("replacement-catalog"));
    {
        auto replacement = PackCatalog::open(replacement_root);
        QVERIFY2(replacement.has_value(),
                 replacement ? "" : qPrintable(replacement.error().message));
    }
    const auto moved_root = QDir(temporary.path()).filePath(QStringLiteral("moved-catalog"));
    bool rebound{};
    CatalogHooks rebind_hooks;
    rebind_hooks.observe = [&](const auto& observation) {
        if (!rebound && observation.event == CatalogEvent::RootLockAcquired &&
            observation.operation == CatalogOperation::ProtectedInputInspection) {
            rebound = QDir{}.rename(catalog_root, moved_root) &&
                      QDir{}.rename(replacement_root, catalog_root);
        }
    };
    const auto replaced = inspectProtectedCatalogInputs(**snapshot, std::move(rebind_hooks));
    QVERIFY(rebound);
    QVERIFY(!replaced.has_value());
    QCOMPARE(replaced.error().code, CatalogErrorCode::CannotOpen);
#endif
}

} // namespace

QTEST_MAIN(PackCatalogProtectedInputsTest)

#include "tst_pack_catalog_protected_inputs.moc"
