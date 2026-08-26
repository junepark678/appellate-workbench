#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_archive.hpp"
#include "appellate/storage/session_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace {

using appellate::storage::AssetReference;
using appellate::storage::AssetStore;
using appellate::storage::CommitBatch;
using appellate::storage::DocketWrite;
using appellate::storage::EventWrite;
using appellate::storage::RevisionPin;
using appellate::storage::SessionArchive;
using appellate::storage::SessionArchiveErrorCode;
using appellate::storage::SessionArchiveImportOptions;
using appellate::storage::SessionArchiveLimits;
using appellate::storage::SessionAuthorityContract;
using appellate::storage::SessionSnapshot;
using appellate::storage::SessionStore;
using appellate::storage::StoreErrorCode;

constexpr auto pin_digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const QByteArray document_bytes =
    QByteArrayLiteral("archive-document-body-unique-v1\0with-binary") + QByteArray(1, '\x7f');

[[nodiscard]] std::vector<RevisionPin> pins() {
    return {{QStringLiteral("example.archive.pack"), QStringLiteral("1.0.0"),
             QString::fromLatin1(pin_digest)}};
}

[[nodiscard]] CommitBatch initialBatch(QStringView suffix) {
    return CommitBatch{
        QStringLiteral("command.initial.%1").arg(suffix),
        QByteArrayLiteral(R"({"command":"initial"})"),
        QStringLiteral("2026-08-26T00:00:00Z"),
        {EventWrite{QStringLiteral("session.started"), QByteArrayLiteral(R"({"state":"started"})"),
                    QStringLiteral("archive.test")}},
        {DocketWrite{QStringLiteral("entry.initial.%1").arg(suffix), 0,
                     QStringLiteral("Initial entry"), QStringLiteral("entered")}},
        {},
    };
}

[[nodiscard]] CommitBatch documentBatch(QStringView suffix, const QString& digest) {
    return CommitBatch{
        QStringLiteral("command.document.%1").arg(suffix),
        QByteArrayLiteral(R"({"command":"attach_document"})"),
        QStringLiteral("2026-08-26T00:01:00Z"),
        {EventWrite{QStringLiteral("document.attached"),
                    QByteArrayLiteral(R"({"document":"archive"})"),
                    QStringLiteral("archive.test")}},
        {DocketWrite{QStringLiteral("entry.document.%1").arg(suffix), 0,
                     QStringLiteral("Archived document"), QStringLiteral("filed")}},
        {AssetReference{digest, QStringLiteral("filing-document")}},
    };
}

[[nodiscard]] QString seedSession(SessionStore& store, AssetStore& assets,
                                  const QString& session_id, QStringView suffix,
                                  std::optional<QByteArrayView> document = std::nullopt) {
    const auto created = store.createSessionWithInitialBatch(
        session_id, QStringLiteral("engine.archive.v1"), QStringLiteral("2026-08-26T00:00:00Z"),
        pins(), SessionAuthorityContract::CanonicalV2, initialBatch(suffix));
    if (!created || *created != 1) {
        return {};
    }
    if (!document) {
        return QString{};
    }
    auto staged = assets.stage(*document);
    if (!staged) {
        return {};
    }
    const auto digest = staged->sha256();
    const auto appended =
        store.appendWithStagedAsset(session_id, 1, documentBatch(suffix, digest), assets, *staged);
    return appended && *appended == 2 ? digest : QString{};
}

[[nodiscard]] bool snapshotsEqual(const SessionSnapshot& left, const SessionSnapshot& right) {
    return left.session_id == right.session_id && left.engine_revision == right.engine_revision &&
           left.authority_contract == right.authority_contract && left.sequence == right.sequence &&
           left.pins == right.pins && left.commands == right.commands &&
           left.events == right.events && left.docket == right.docket &&
           left.asset_references == right.asset_references &&
           left.created_at_utc == right.created_at_utc;
}

[[nodiscard]] qsizetype objectFileCount(const AssetStore& assets) {
    return QDir(assets.objectsDirectory()).entryList(QDir::Files | QDir::NoDotAndDotDot).size();
}

[[nodiscard]] QByteArray withRecomputedEnvelope(QByteArray archive) {
    if (archive.size() < 32) {
        return archive;
    }
    archive.chop(32);
    archive.append(QCryptographicHash::hash(QByteArrayView(archive), QCryptographicHash::Sha256));
    return archive;
}

class SessionArchiveTest final : public QObject {
    Q_OBJECT

  private slots:
    void exportsSelectedAndAllSessionsDeterministically();
    void roundTripsSessionsAndSharedAssets();
    void mergesIntoNonemptyPairAndRejectsConflictBeforePublication();
    void rejectsCorruptMalformedDuplicateAndLimitedArchives();
    void rejectsUnavailablePinsAndCorruptTargetWithoutMutation();
};

void SessionArchiveTest::exportsSelectedAndAllSessionsDeterministically() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto opened = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("source.db")));
    QVERIFY(opened.has_value());
    AssetStore assets(QDir(temporary.path()).filePath(QStringLiteral("assets")), 1024 * 1024);
    const auto first_digest =
        seedSession(**opened, assets, QStringLiteral("session-a"), u"a", document_bytes);
    QVERIFY(!first_digest.isEmpty());
    QCOMPARE(seedSession(**opened, assets, QStringLiteral("session-b"), u"b", document_bytes),
             first_digest);

    const auto all_first = SessionArchive::exportSessions(**opened, assets);
    QVERIFY(all_first.has_value());
    const auto all_second = SessionArchive::exportSessions(**opened, assets);
    QVERIFY(all_second.has_value());
    QCOMPARE(*all_second, *all_first);

    const auto selected = SessionArchive::exportSessions(
        **opened, assets, {QStringLiteral("session-b"), QStringLiteral("session-a")});
    QVERIFY(selected.has_value());
    QCOMPARE(*selected, *all_first);

    const auto manifest = SessionArchive::inspect(*all_first);
    QVERIFY(manifest.has_value());
    QCOMPARE(manifest->sessions.size(), std::size_t{2});
    QCOMPARE(manifest->sessions[0].session_id, QStringLiteral("session-a"));
    QCOMPARE(manifest->sessions[1].session_id, QStringLiteral("session-b"));
    QCOMPARE(manifest->sessions[0].pins, pins());
    QCOMPARE(manifest->sessions[0].command_count, std::size_t{2});
    QCOMPARE(manifest->sessions[0].event_count, std::size_t{2});
    QCOMPARE(manifest->sessions[0].docket_count, std::size_t{2});
    QCOMPARE(manifest->sessions[0].asset_reference_count, std::size_t{1});
    QCOMPARE(manifest->asset_digests, QStringList{first_digest});
    QCOMPARE(manifest->total_asset_bytes, static_cast<qint64>(document_bytes.size()));
    QCOMPARE(manifest->archive_sha256.size(), 64);

    const auto replay_contents = SessionArchive::readForReplay(*all_first);
    QVERIFY(replay_contents.has_value());
    QCOMPARE(replay_contents->manifest.archive_sha256, manifest->archive_sha256);
    QCOMPARE(replay_contents->sessions.size(), std::size_t{2});
    QCOMPARE(replay_contents->sessions[0].session_id, QStringLiteral("session-a"));
    QCOMPARE(replay_contents->sessions[0].commands.size(), std::size_t{2});
    QCOMPARE(replay_contents->sessions[0].events.size(), std::size_t{2});
    QCOMPARE(replay_contents->assets.size(), std::size_t{1});
    QCOMPARE(replay_contents->assets[0].digest, first_digest);
    QCOMPARE(replay_contents->assets[0].bytes, document_bytes);

    const auto one =
        SessionArchive::exportSessions(**opened, assets, {QStringLiteral("session-b")});
    QVERIFY(one.has_value());
    const auto one_manifest = SessionArchive::inspect(*one);
    QVERIFY(one_manifest.has_value());
    QCOMPARE(one_manifest->sessions.size(), std::size_t{1});
    QCOMPARE(one_manifest->sessions.front().session_id, QStringLiteral("session-b"));
    QCOMPARE(one_manifest->asset_digests, QStringList{first_digest});

    const auto duplicate = SessionArchive::exportSessions(
        **opened, assets, {QStringLiteral("session-a"), QStringLiteral("session-a")});
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, SessionArchiveErrorCode::InvalidArgument);
    const auto missing =
        SessionArchive::exportSessions(**opened, assets, {QStringLiteral("session-missing")});
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, SessionArchiveErrorCode::InvalidArgument);
}

void SessionArchiveTest::roundTripsSessionsAndSharedAssets() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto source = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("source.db")));
    QVERIFY(source.has_value());
    AssetStore source_assets(QDir(temporary.path()).filePath(QStringLiteral("source-assets")),
                             1024 * 1024);
    const auto digest =
        seedSession(**source, source_assets, QStringLiteral("session-a"), u"a", document_bytes);
    QVERIFY(!digest.isEmpty());
    QCOMPARE(
        seedSession(**source, source_assets, QStringLiteral("session-b"), u"b", document_bytes),
        digest);
    const auto source_a = (*source)->loadSession(QStringLiteral("session-a"));
    const auto source_b = (*source)->loadSession(QStringLiteral("session-b"));
    QVERIFY(source_a.has_value());
    QVERIFY(source_b.has_value());
    const auto archive = SessionArchive::exportSessions(**source, source_assets);
    QVERIFY(archive.has_value());

    auto destination =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("destination.db")));
    QVERIFY(destination.has_value());
    AssetStore destination_assets(
        QDir(temporary.path()).filePath(QStringLiteral("destination-assets")), 1024 * 1024);
    const auto fail_closed =
        SessionArchive::importSessions(*archive, **destination, destination_assets);
    QVERIFY(!fail_closed.has_value());
    QCOMPARE(fail_closed.error().code, SessionArchiveErrorCode::IncompatibleRevisionPins);
    QVERIFY(!(*destination)->loadSession(QStringLiteral("session-a")).has_value());
    SessionArchiveImportOptions options;
    options.available_revision_pins = pins();
    options.available_revision_pins.push_back(
        RevisionPin{QStringLiteral("unrelated.pack"), QStringLiteral("9.0.0"), QString(64, u'a')});
    const auto imported =
        SessionArchive::importSessions(*archive, **destination, destination_assets, options);
    QVERIFY2(imported.has_value(), imported ? "" : imported.error().message.toUtf8().constData());

    const auto destination_a = (*destination)->loadSession(QStringLiteral("session-a"));
    const auto destination_b = (*destination)->loadSession(QStringLiteral("session-b"));
    QVERIFY(destination_a.has_value());
    QVERIFY(destination_b.has_value());
    QVERIFY(snapshotsEqual(*source_a, *destination_a));
    QVERIFY(snapshotsEqual(*source_b, *destination_b));
    const auto restored_asset = destination_assets.read(digest);
    QVERIFY(restored_asset.has_value());
    QCOMPARE(*restored_asset, document_bytes);
    QCOMPARE(objectFileCount(destination_assets), 1);

    const auto reexported = SessionArchive::exportSessions(**destination, destination_assets);
    QVERIFY(reexported.has_value());
    QCOMPARE(*reexported, *archive);
}

void SessionArchiveTest::mergesIntoNonemptyPairAndRejectsConflictBeforePublication() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto source = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("source.db")));
    QVERIFY(source.has_value());
    AssetStore source_assets(QDir(temporary.path()).filePath(QStringLiteral("source-assets")),
                             1024 * 1024);
    const auto digest =
        seedSession(**source, source_assets, QStringLiteral("session-a"), u"a", document_bytes);
    QVERIFY(!digest.isEmpty());
    const auto archive = SessionArchive::exportSessions(**source, source_assets);
    QVERIFY(archive.has_value());

    auto merge = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("merge.db")));
    QVERIFY(merge.has_value());
    AssetStore merge_assets(QDir(temporary.path()).filePath(QStringLiteral("merge-assets")),
                            1024 * 1024);
    QCOMPARE(seedSession(**merge, merge_assets, QStringLiteral("session-existing"), u"e",
                         document_bytes),
             digest);
    QCOMPARE(objectFileCount(merge_assets), 1);
    SessionArchiveImportOptions compatible_options;
    compatible_options.available_revision_pins = pins();
    const auto merged =
        SessionArchive::importSessions(*archive, **merge, merge_assets, compatible_options);
    QVERIFY2(merged.has_value(), merged ? "" : merged.error().message.toUtf8().constData());
    QVERIFY((*merge)->loadSession(QStringLiteral("session-a")).has_value());
    QVERIFY((*merge)->loadSession(QStringLiteral("session-existing")).has_value());
    QCOMPARE(objectFileCount(merge_assets), 1);

    auto conflict =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("conflict.db")));
    QVERIFY(conflict.has_value());
    AssetStore conflict_assets(QDir(temporary.path()).filePath(QStringLiteral("conflict-assets")),
                               1024 * 1024);
    QVERIFY(seedSession(**conflict, conflict_assets, QStringLiteral("session-a"), u"c").isEmpty());
    const auto before = (*conflict)->loadSession(QStringLiteral("session-a"));
    QVERIFY(before.has_value());
    const auto rejected =
        SessionArchive::importSessions(*archive, **conflict, conflict_assets, compatible_options);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, SessionArchiveErrorCode::SessionConflict);
    const auto after = (*conflict)->loadSession(QStringLiteral("session-a"));
    QVERIFY(after.has_value());
    QVERIFY(snapshotsEqual(*before, *after));
    QCOMPARE(objectFileCount(conflict_assets), 0);
    const auto absent_asset = conflict_assets.read(digest);
    QVERIFY(!absent_asset.has_value());
}

void SessionArchiveTest::rejectsCorruptMalformedDuplicateAndLimitedArchives() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto source = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("source.db")));
    QVERIFY(source.has_value());
    AssetStore assets(QDir(temporary.path()).filePath(QStringLiteral("assets")), 1024 * 1024);
    const auto digest =
        seedSession(**source, assets, QStringLiteral("session-a"), u"a", document_bytes);
    QVERIFY(!digest.isEmpty());
    QCOMPARE(seedSession(**source, assets, QStringLiteral("session-b"), u"b", document_bytes),
             digest);
    const auto exported = SessionArchive::exportSessions(**source, assets);
    QVERIFY(exported.has_value());

    auto duplicate_json = initialBatch(u"duplicate-json");
    duplicate_json.command_json = QByteArrayLiteral(R"({"command":"initial","command":"shadow"})");
    const auto duplicate_json_result = (*source)->createSessionWithInitialBatch(
        QStringLiteral("session-duplicate-json"), QStringLiteral("engine.archive.v1"),
        QStringLiteral("2026-08-26T00:00:00Z"), pins(), SessionAuthorityContract::CanonicalV2,
        duplicate_json);
    QVERIFY(!duplicate_json_result.has_value());
    QCOMPARE(duplicate_json_result.error().code, StoreErrorCode::InvalidArgument);

    auto raw_tamper = *exported;
    raw_tamper[raw_tamper.size() / 2] ^= 0x01;
    const auto raw_result = SessionArchive::inspect(raw_tamper);
    QVERIFY(!raw_result.has_value());
    QCOMPARE(raw_result.error().code, SessionArchiveErrorCode::DigestMismatch);

    auto noncanonical = *exported;
    noncanonical.chop(32);
    const auto first_id = noncanonical.indexOf(QByteArrayLiteral("session-a"));
    QVERIFY(first_id >= 0);
    noncanonical.replace(first_id, 9, QByteArrayLiteral("session-z"));
    noncanonical.append(
        QCryptographicHash::hash(QByteArrayView(noncanonical), QCryptographicHash::Sha256));
    const auto noncanonical_result = SessionArchive::inspect(noncanonical);
    QVERIFY(!noncanonical_result.has_value());
    QCOMPARE(noncanonical_result.error().code, SessionArchiveErrorCode::MalformedArchive);

    auto duplicate_session = *exported;
    duplicate_session.chop(32);
    const auto second_id = duplicate_session.indexOf(QByteArrayLiteral("session-b"));
    QVERIFY(second_id >= 0);
    duplicate_session.replace(second_id, 9, QByteArrayLiteral("session-a"));
    duplicate_session.append(
        QCryptographicHash::hash(QByteArrayView(duplicate_session), QCryptographicHash::Sha256));
    const auto duplicate_session_result = SessionArchive::inspect(duplicate_session);
    QVERIFY(!duplicate_session_result.has_value());
    QCOMPARE(duplicate_session_result.error().code, SessionArchiveErrorCode::MalformedArchive);

    auto equal_command_heads = *exported;
    equal_command_heads.chop(32);
    const auto second_command =
        equal_command_heads.indexOf(QByteArrayLiteral("command.document.a"));
    QVERIFY(second_command >= 0);
    const auto expected_sequence_offset =
        second_command + QByteArrayLiteral("command.document.a").size();
    QVERIFY(expected_sequence_offset + 8 <= equal_command_heads.size());
    std::fill_n(equal_command_heads.data() + expected_sequence_offset, 8, '\0');
    equal_command_heads.append(
        QCryptographicHash::hash(QByteArrayView(equal_command_heads), QCryptographicHash::Sha256));
    const auto equal_heads_result = SessionArchive::inspect(equal_command_heads);
    QVERIFY(!equal_heads_result.has_value());
    QCOMPARE(equal_heads_result.error().code, SessionArchiveErrorCode::MalformedArchive);

    auto duplicate_member_json = *exported;
    duplicate_member_json.chop(32);
    const auto original_json = QByteArrayLiteral(R"({"command":"initial"})");
    const auto forged_json = QByteArrayLiteral(R"({"x":1,"x":2})");
    const auto json_offset = duplicate_member_json.indexOf(original_json);
    QVERIFY(json_offset >= 4);
    const auto json_length_offset = json_offset - 4;
    duplicate_member_json[json_length_offset] = '\0';
    duplicate_member_json[json_length_offset + 1] = '\0';
    duplicate_member_json[json_length_offset + 2] = '\0';
    duplicate_member_json[json_length_offset + 3] = static_cast<char>(forged_json.size());
    duplicate_member_json.replace(json_offset, original_json.size(), forged_json);
    duplicate_member_json.append(QCryptographicHash::hash(QByteArrayView(duplicate_member_json),
                                                          QCryptographicHash::Sha256));
    const auto duplicate_member_result = SessionArchive::inspect(duplicate_member_json);
    QVERIFY(!duplicate_member_result.has_value());
    QCOMPARE(duplicate_member_result.error().code, SessionArchiveErrorCode::MalformedArchive);

    // Generic storage validation intentionally cannot decide engine semantics. It exposes exact
    // owned data so the product replay seam can reject a valid-shape forgery before import.
    auto semantic_forgery = *exported;
    semantic_forgery.chop(32);
    const auto accepted_semantics = QByteArrayLiteral(R"({"command":"attach_document"})");
    const auto forged_semantics = QByteArrayLiteral(R"({"command":"forged_document"})");
    QCOMPARE(accepted_semantics.size(), forged_semantics.size());
    const auto semantic_offset = semantic_forgery.indexOf(accepted_semantics);
    QVERIFY(semantic_offset >= 0);
    semantic_forgery.replace(semantic_offset, accepted_semantics.size(), forged_semantics);
    semantic_forgery.append(
        QCryptographicHash::hash(QByteArrayView(semantic_forgery), QCryptographicHash::Sha256));
    const auto replay_input = SessionArchive::readForReplay(semantic_forgery);
    QVERIFY(replay_input.has_value());
    const auto product_replay_accepts = std::ranges::none_of(
        replay_input->sessions, [&forged_semantics](const SessionSnapshot& snapshot) {
            return std::ranges::any_of(snapshot.commands, [&forged_semantics](const auto& command) {
                return command.payload_json == forged_semantics;
            });
        });
    QVERIFY(!product_replay_accepts);
    auto semantic_target =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("semantic-target.db")));
    QVERIFY(semantic_target.has_value());
    AssetStore semantic_target_assets(
        QDir(temporary.path()).filePath(QStringLiteral("semantic-target-assets")), 1024 * 1024);
    const auto semantic_absent = (*semantic_target)->loadSession(QStringLiteral("session-a"));
    QVERIFY(!semantic_absent.has_value());
    QCOMPARE(semantic_absent.error().code, StoreErrorCode::NotFound);
    QVERIFY(!QDir(semantic_target_assets.objectsDirectory()).exists());

    auto asset_tamper = *exported;
    asset_tamper.chop(32);
    const auto asset_offset = asset_tamper.indexOf(document_bytes);
    QVERIFY(asset_offset >= 0);
    asset_tamper[asset_offset] ^= 0x01;
    asset_tamper.append(
        QCryptographicHash::hash(QByteArrayView(asset_tamper), QCryptographicHash::Sha256));
    const auto asset_result = SessionArchive::inspect(asset_tamper);
    QVERIFY(!asset_result.has_value());
    QCOMPARE(asset_result.error().code, SessionArchiveErrorCode::DigestMismatch);

    auto duplicate_asset = *exported;
    duplicate_asset.chop(32);
    const auto digest_offset = duplicate_asset.lastIndexOf(digest.toLatin1());
    QVERIFY(digest_offset >= 8);
    const auto asset_frame_offset = digest_offset - 4;
    const auto asset_count_offset = asset_frame_offset - 4;
    QCOMPARE(static_cast<unsigned char>(duplicate_asset[asset_count_offset + 3]), 1U);
    const auto asset_frame = duplicate_asset.sliced(asset_frame_offset);
    duplicate_asset[asset_count_offset + 3] = 2;
    duplicate_asset.append(asset_frame);
    duplicate_asset.append(
        QCryptographicHash::hash(QByteArrayView(duplicate_asset), QCryptographicHash::Sha256));
    const auto duplicate_asset_result = SessionArchive::inspect(duplicate_asset);
    QVERIFY(!duplicate_asset_result.has_value());
    QCOMPARE(duplicate_asset_result.error().code, SessionArchiveErrorCode::MalformedArchive);

    SessionArchiveLimits limited;
    limited.maximum_sessions = 1;
    const auto limited_result = SessionArchive::inspect(*exported, limited);
    QVERIFY(!limited_result.has_value());
    QCOMPARE(limited_result.error().code, SessionArchiveErrorCode::LimitExceeded);

    SessionArchiveLimits aggregate_limited;
    aggregate_limited.maximum_total_rows = 10;
    const auto aggregate_result = SessionArchive::inspect(*exported, aggregate_limited);
    QVERIFY(!aggregate_result.has_value());
    QCOMPARE(aggregate_result.error().code, SessionArchiveErrorCode::LimitExceeded);

    auto malformed_magic = *exported;
    malformed_magic[0] = 'Z';
    malformed_magic = withRecomputedEnvelope(std::move(malformed_magic));
    const auto malformed_result = SessionArchive::inspect(malformed_magic);
    QVERIFY(!malformed_result.has_value());
    QCOMPARE(malformed_result.error().code, SessionArchiveErrorCode::MalformedArchive);
}

void SessionArchiveTest::rejectsUnavailablePinsAndCorruptTargetWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto source = SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("source.db")));
    QVERIFY(source.has_value());
    AssetStore source_assets(QDir(temporary.path()).filePath(QStringLiteral("source-assets")),
                             1024 * 1024);
    const auto digest =
        seedSession(**source, source_assets, QStringLiteral("session-a"), u"a", document_bytes);
    QVERIFY(!digest.isEmpty());
    const auto archive = SessionArchive::exportSessions(**source, source_assets);
    QVERIFY(archive.has_value());

    auto incompatible =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("incompatible.db")));
    QVERIFY(incompatible.has_value());
    AssetStore incompatible_assets(
        QDir(temporary.path()).filePath(QStringLiteral("incompatible-assets")), 1024 * 1024);
    SessionArchiveImportOptions incompatible_options;
    incompatible_options.available_revision_pins = std::vector<RevisionPin>{RevisionPin{
        QStringLiteral("example.archive.pack"), QStringLiteral("1.0.0"), QString(64, u'f')}};
    const auto incompatible_result = SessionArchive::importSessions(
        *archive, **incompatible, incompatible_assets, incompatible_options);
    QVERIFY(!incompatible_result.has_value());
    QCOMPARE(incompatible_result.error().code, SessionArchiveErrorCode::IncompatibleRevisionPins);
    const auto absent = (*incompatible)->loadSession(QStringLiteral("session-a"));
    QVERIFY(!absent.has_value());
    QCOMPARE(absent.error().code, StoreErrorCode::NotFound);
    QVERIFY(!QDir(incompatible_assets.objectsDirectory()).exists());

    auto asset_free_source =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("asset-free-source.db")));
    QVERIFY(asset_free_source.has_value());
    AssetStore asset_free_source_assets(
        QDir(temporary.path()).filePath(QStringLiteral("asset-free-source-assets")), 1024 * 1024);
    QVERIFY(seedSession(**asset_free_source, asset_free_source_assets,
                        QStringLiteral("session-asset-free"), u"f")
                .isEmpty());
    const auto asset_free_archive =
        SessionArchive::exportSessions(**asset_free_source, asset_free_source_assets);
    QVERIFY(asset_free_archive.has_value());

    auto corrupt_target =
        SessionStore::open(QDir(temporary.path()).filePath(QStringLiteral("corrupt-target.db")));
    QVERIFY(corrupt_target.has_value());
    AssetStore corrupt_target_assets(
        QDir(temporary.path()).filePath(QStringLiteral("corrupt-target-assets")), 1024 * 1024);
    const auto corrupt_digest =
        seedSession(**corrupt_target, corrupt_target_assets, QStringLiteral("session-existing"),
                    u"e", document_bytes);
    QVERIFY(!corrupt_digest.isEmpty());
    QVERIFY(QFile::remove(QDir(corrupt_target_assets.objectsDirectory()).filePath(corrupt_digest)));
    SessionArchiveImportOptions compatible_options;
    compatible_options.available_revision_pins = pins();
    const auto corrupt_result = SessionArchive::importSessions(
        *asset_free_archive, **corrupt_target, corrupt_target_assets, compatible_options);
    QVERIFY(!corrupt_result.has_value());
    QCOMPARE(corrupt_result.error().code, SessionArchiveErrorCode::AssetStoreFailure);
    const auto not_imported = (*corrupt_target)->loadSession(QStringLiteral("session-asset-free"));
    QVERIFY(!not_imported.has_value());
    QCOMPARE(not_imported.error().code, StoreErrorCode::NotFound);
}

} // namespace

QTEST_MAIN(SessionArchiveTest)
#include "tst_session_archive.moc"
