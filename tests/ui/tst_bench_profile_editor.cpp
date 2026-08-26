#include "bench_profile_codec.hpp"
#include "bench_profile_editor.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
#include <QWidget>

#include <array>
#include <string>
#include <utility>

namespace {

class BenchProfileEditorTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        if (!qEnvironmentVariableIsSet("APPELLATE_TEST_REQUIRE_2X_DPR")) {
            return;
        }

        QWidget probe;
        QCOMPARE(probe.devicePixelRatioF(), 2.0);
    }

    void strictCodecRoundTripsSchemaV1();
    void cloneEditsEveryStructuredControl();
    void rejectsMalformedAndNonFictionalDocuments();
    void rejectsIncompatibleModelsWithoutMutation();
    void exportsAtomicallyWithoutOverwrite();
    void exposesAccessibleKeyboardControls();
    void previewChangesWithoutTouchingLegalState();
};

using appellate::model::CounselAddress;
using appellate::model::CourtRole;
using appellate::model::InteractionStyle;
using appellate::model::IssueFocus;
using appellate::model::JudgeProfile;
using appellate::model::ProfileClass;
using appellate::model::ProfileCompatibility;
using appellate::model::QuestionFraming;
using appellate::model::VoiceCadence;
using appellate::model::VoiceRegister;
using appellate::model::VoiceStyle;
using appellate::ui::BenchProfileCodec;
using appellate::ui::BenchProfileEditor;
using appellate::ui::BenchProfileErrorCode;
using appellate::ui::InteractionControl;

[[nodiscard]] JudgeProfile sampleProfile() {
    return JudgeProfile{
        "example.judge.cedar",
        "Judge Cedar (Composite)",
        ProfileClass::FictionalComposite,
        ProfileCompatibility{
            {CourtRole::Appellate},
            {"example.jurisdiction.fictional"},
        },
        InteractionStyle{
            0.75,
            0.70,
            0.45,
            0.35,
            0.80,
            0.55,
            0.90,
            0.88,
            0.65,
            {
                IssueFocus{"example.issue.preservation", 0.85},
                IssueFocus{"example.issue.standard-review", 0.60},
            },
        },
        VoiceStyle{
            VoiceRegister::Formal,
            VoiceCadence::Measured,
            QuestionFraming::Socratic,
            CounselAddress::Counsel,
            0.50,
            0.60,
            {"help the court with the rule", "identify the limiting principle"},
            {"pause at that premise", "before you continue"},
            {"clarify that answer", "state the distinction precisely"},
        },
    };
}

[[nodiscard]] QJsonObject encodedRoot() {
    const auto encoded = BenchProfileCodec::encode(sampleProfile());
    Q_ASSERT(encoded.has_value());
    return QJsonDocument::fromJson(*encoded).object();
}

void BenchProfileEditorTest::strictCodecRoundTripsSchemaV1() {
    const auto expected = sampleProfile();
    const auto first = BenchProfileCodec::encode(expected);
    QVERIFY(first.has_value());
    QVERIFY(first->contains("\"profile_class\": \"fictional_composite\""));
    QVERIFY(first->contains("\"resource_kind\": \"judge_profile\""));
    const auto second = BenchProfileCodec::encode(expected);
    QVERIFY(second.has_value());
    QCOMPARE(*second, *first);

    const auto decoded = BenchProfileCodec::decode(*first);
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, expected);

    BenchProfileEditor editor;
    QVERIFY(editor.loadProfile(expected).has_value());
    const auto edited = editor.profile();
    QVERIFY(edited.has_value());
    QCOMPARE(*edited, expected);
}

void BenchProfileEditorTest::cloneEditsEveryStructuredControl() {
    BenchProfileEditor editor;
    const auto original = sampleProfile();
    QVERIFY(editor.loadProfile(original).has_value());

    const std::array edits{
        std::pair{InteractionControl::Directness, 0.11},
        std::pair{InteractionControl::Formality, 0.22},
        std::pair{InteractionControl::QuestionLength, 0.33},
        std::pair{InteractionControl::InterruptionFrequency, 0.44},
        std::pair{InteractionControl::FollowUpDepth, 0.55},
        std::pair{InteractionControl::HypotheticalFrequency, 0.66},
        std::pair{InteractionControl::ConcessionRecall, 0.77},
        std::pair{InteractionControl::RecordPinDemand, 0.79},
        std::pair{InteractionControl::TimeStrictness, 0.88},
    };
    for (const auto& [control, value] : edits) {
        QVERIFY(editor.interactionControl(control) != nullptr);
        editor.interactionControl(control)->setValue(value);
    }
    editor.voiceRegisterControl()->setCurrentIndex(
        editor.voiceRegisterControl()->findData(static_cast<int>(VoiceRegister::Technical)));
    editor.voiceCadenceControl()->setCurrentIndex(
        editor.voiceCadenceControl()->findData(static_cast<int>(VoiceCadence::Expansive)));
    editor.questionFramingControl()->setCurrentIndex(
        editor.questionFramingControl()->findData(static_cast<int>(QuestionFraming::Narrative)));
    editor.addressConventionControl()->setCurrentIndex(
        editor.addressConventionControl()->findData(static_cast<int>(CounselAddress::Advocate)));
    editor.verbosityControl()->setValue(0.23);
    editor.sentenceComplexityControl()->setValue(0.91);
    editor.questionPhrasesControl()->setPlainText(
        QStringLiteral("develop the principle\nlocate the record support"));
    editor.interruptionPhrasesControl()->setPlainText(QStringLiteral("hold that thought"));
    editor.clarificationPhrasesControl()->setPlainText(
        QStringLiteral("draw the distinction\nanswer the precise point"));
    auto* preservation = editor.issueFocusControl("example.issue.preservation");
    auto* review = editor.issueFocusControl("example.issue.standard-review");
    QVERIFY(preservation != nullptr);
    QVERIFY(review != nullptr);
    preservation->setValue(0.31);
    review->setValue(0.92);

    QVERIFY(editor.cloneProfile("user.judge.cedar-variant", "Judge Cedar Variant (Composite)")
                .has_value());
    const auto clone = editor.profile();
    QVERIFY(clone.has_value());
    QCOMPARE(clone->id, std::string("user.judge.cedar-variant"));
    QCOMPARE(clone->display_name, std::string("Judge Cedar Variant (Composite)"));
    QCOMPARE(clone->profile_class, ProfileClass::FictionalComposite);
    QCOMPARE(clone->compatibility, original.compatibility);
    QCOMPARE(clone->interaction.directness, 0.11);
    QCOMPARE(clone->interaction.formality, 0.22);
    QCOMPARE(clone->interaction.question_length, 0.33);
    QCOMPARE(clone->interaction.interruption_frequency, 0.44);
    QCOMPARE(clone->interaction.follow_up_depth, 0.55);
    QCOMPARE(clone->interaction.hypothetical_frequency, 0.66);
    QCOMPARE(clone->interaction.concession_recall, 0.77);
    QCOMPARE(clone->interaction.record_pin_demand, 0.79);
    QCOMPARE(clone->interaction.time_strictness, 0.88);
    QCOMPARE(clone->voice.register_style, VoiceRegister::Technical);
    QCOMPARE(clone->voice.cadence, VoiceCadence::Expansive);
    QCOMPARE(clone->voice.question_framing, QuestionFraming::Narrative);
    QCOMPARE(clone->voice.address_convention, CounselAddress::Advocate);
    QCOMPARE(clone->voice.verbosity, 0.23);
    QCOMPARE(clone->voice.sentence_complexity, 0.91);
    QCOMPARE(clone->voice.question_phrases,
             std::vector<std::string>({"develop the principle", "locate the record support"}));
    QCOMPARE(clone->voice.interruption_phrases, std::vector<std::string>({"hold that thought"}));
    QCOMPARE(clone->voice.clarification_phrases,
             std::vector<std::string>({"draw the distinction", "answer the precise point"}));
    QCOMPARE(clone->interaction.issue_focus.at(0).weight, 0.31);
    QCOMPARE(clone->interaction.issue_focus.at(1).weight, 0.92);

    const auto before_bad_clone = *clone;
    const auto bad_clone = editor.cloneProfile("not_namespaced", "Invalid clone");
    QVERIFY(!bad_clone.has_value());
    const auto after_bad_clone = editor.profile();
    QVERIFY(after_bad_clone.has_value());
    QCOMPARE(*after_bad_clone, before_bad_clone);
}

void BenchProfileEditorTest::rejectsMalformedAndNonFictionalDocuments() {
    const auto malformed = BenchProfileCodec::decode(QByteArrayView("{", 1));
    QVERIFY(!malformed.has_value());
    QCOMPARE(malformed.error().code, BenchProfileErrorCode::InvalidJson);

    auto outcome_root = encodedRoot();
    outcome_root.insert(QStringLiteral("authored_outcome"), QStringLiteral("affirmed"));
    const auto outcome =
        BenchProfileCodec::decode(QJsonDocument(outcome_root).toJson(QJsonDocument::Compact));
    QVERIFY(!outcome.has_value());
    QCOMPARE(outcome.error().code, BenchProfileErrorCode::UnexpectedField);

    const auto encoded = BenchProfileCodec::encode(sampleProfile());
    QVERIFY(encoded.has_value());
    auto duplicate = *encoded;
    const auto replaced = duplicate.replace("\"schema_version\": 1,",
                                            "\"schema_version\": 1, \"schema_\\u0076ersion\": 1,");
    QVERIFY(replaced.contains("schema_\\u0076ersion"));
    const auto duplicate_result = BenchProfileCodec::decode(replaced);
    QVERIFY(!duplicate_result.has_value());
    QCOMPARE(duplicate_result.error().code, BenchProfileErrorCode::DuplicateJsonKey);

    auto named_root = encodedRoot();
    named_root.insert(QStringLiteral("profile_class"), QStringLiteral("real_named_judge"));
    const auto named =
        BenchProfileCodec::decode(QJsonDocument(named_root).toJson(QJsonDocument::Compact));
    QVERIFY(!named.has_value());
    QCOMPARE(named.error().code, BenchProfileErrorCode::IncompatibleProfile);

    auto version_root = encodedRoot();
    version_root.insert(QStringLiteral("schema_version"), 2);
    const auto version =
        BenchProfileCodec::decode(QJsonDocument(version_root).toJson(QJsonDocument::Compact));
    QVERIFY(!version.has_value());
    QCOMPARE(version.error().code, BenchProfileErrorCode::UnsupportedSchema);

    auto range_root = encodedRoot();
    auto interaction = range_root.value(QStringLiteral("interaction")).toObject();
    interaction.insert(QStringLiteral("directness"), 1.001);
    range_root.insert(QStringLiteral("interaction"), interaction);
    const auto range =
        BenchProfileCodec::decode(QJsonDocument(range_root).toJson(QJsonDocument::Compact));
    QVERIFY(!range.has_value());
    QCOMPARE(range.error().code, BenchProfileErrorCode::OutOfRange);

    auto id_root = encodedRoot();
    id_root.insert(QStringLiteral("resource_id"), QStringLiteral("not_namespaced"));
    const auto invalid_id =
        BenchProfileCodec::decode(QJsonDocument(id_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_id.has_value());
    QCOMPARE(invalid_id.error().code, BenchProfileErrorCode::InvalidField);

    auto voice_root = encodedRoot();
    auto voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("cadence"), QStringLiteral("impersonated"));
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto invalid_voice =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_voice.has_value());
    QCOMPARE(invalid_voice.error().code, BenchProfileErrorCode::IncompatibleProfile);

    voice_root = encodedRoot();
    voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("question_framing"), QStringLiteral("rhetorical"));
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto invalid_framing =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_framing.has_value());
    QCOMPARE(invalid_framing.error().code, BenchProfileErrorCode::IncompatibleProfile);

    voice_root = encodedRoot();
    voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("address_convention"), QStringLiteral("named_person"));
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto invalid_address =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_address.has_value());
    QCOMPARE(invalid_address.error().code, BenchProfileErrorCode::IncompatibleProfile);

    voice_root = encodedRoot();
    voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("question_phrases"),
                 QJsonArray{QStringLiteral("repeat"), QStringLiteral("repeat")});
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto duplicate_phrase =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!duplicate_phrase.has_value());
    QCOMPARE(duplicate_phrase.error().code, BenchProfileErrorCode::IncompatibleProfile);

    voice_root = encodedRoot();
    voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("interruption_phrases"),
                 QJsonArray{QStringLiteral("unknown {profile_name} template")});
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto invalid_template =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_template.has_value());
    QCOMPARE(invalid_template.error().code, BenchProfileErrorCode::InvalidField);

    voice_root = encodedRoot();
    voice = voice_root.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("unbounded_prose"), QStringLiteral("not accepted"));
    voice_root.insert(QStringLiteral("voice"), voice);
    const auto unknown_voice_field =
        BenchProfileCodec::decode(QJsonDocument(voice_root).toJson(QJsonDocument::Compact));
    QVERIFY(!unknown_voice_field.has_value());
    QCOMPARE(unknown_voice_field.error().code, BenchProfileErrorCode::UnexpectedField);

    auto array_root = encodedRoot();
    auto compatibility = array_root.value(QStringLiteral("compatibility")).toObject();
    compatibility.insert(QStringLiteral("court_roles"), QJsonArray{});
    array_root.insert(QStringLiteral("compatibility"), compatibility);
    const auto invalid_array =
        BenchProfileCodec::decode(QJsonDocument(array_root).toJson(QJsonDocument::Compact));
    QVERIFY(!invalid_array.has_value());
    QCOMPARE(invalid_array.error().code, BenchProfileErrorCode::OutOfRange);

    const QByteArray oversized(1024 * 1024 + 1, ' ');
    const auto too_large = BenchProfileCodec::decode(oversized);
    QVERIFY(!too_large.has_value());
    QCOMPARE(too_large.error().code, BenchProfileErrorCode::InputTooLarge);
}

void BenchProfileEditorTest::rejectsIncompatibleModelsWithoutMutation() {
    BenchProfileEditor editor;
    const auto valid = sampleProfile();
    QVERIFY(editor.loadProfile(valid).has_value());

    auto non_fictional = valid;
    non_fictional.profile_class = static_cast<ProfileClass>(99);
    const auto class_result = editor.loadProfile(non_fictional);
    QVERIFY(!class_result.has_value());
    QCOMPARE(class_result.error().code, BenchProfileErrorCode::IncompatibleProfile);
    const auto after_class_rejection = editor.profile();
    QVERIFY(after_class_rejection.has_value());
    QCOMPARE(*after_class_rejection, valid);

    auto incompatible = valid;
    incompatible.compatibility.jurisdiction_ids = {"not_namespaced"};
    const auto compatibility_result = editor.loadProfile(incompatible);
    QVERIFY(!compatibility_result.has_value());
    QCOMPARE(compatibility_result.error().code, BenchProfileErrorCode::IncompatibleProfile);
    const auto after_compatibility_rejection = editor.profile();
    QVERIFY(after_compatibility_rejection.has_value());
    QCOMPARE(*after_compatibility_rejection, valid);
}

void BenchProfileEditorTest::exportsAtomicallyWithoutOverwrite() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto exported_path =
        QDir(directory.path()).filePath(QStringLiteral("fictional-profile.json"));
    BenchProfileEditor source_editor;
    QVERIFY(source_editor.loadProfile(sampleProfile()).has_value());
    QVERIFY(source_editor.exportProfile(exported_path).has_value());
    const QFileInfo exported_info(exported_path);
    QVERIFY(exported_info.isFile());
    QVERIFY(!exported_info.isSymLink());
    BenchProfileEditor imported_editor;
    QVERIFY(imported_editor.importProfile(exported_path).has_value());
    const auto imported = imported_editor.profile();
    QVERIFY(imported.has_value());
    QCOMPARE(*imported, sampleProfile());

    QFile exported(exported_path);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto original_contents = exported.readAll();
    exported.close();
    auto changed = sampleProfile();
    changed.interaction.directness = 0.01;
    const auto overwrite = BenchProfileCodec::exportFile(changed, exported_path);
    QVERIFY(!overwrite.has_value());
    QCOMPARE(overwrite.error().code, BenchProfileErrorCode::AlreadyExists);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), original_contents);

    const auto unsafe_import = BenchProfileCodec::importFile(directory.path());
    QVERIFY(!unsafe_import.has_value());
    QCOMPARE(unsafe_import.error().code, BenchProfileErrorCode::UnsafePath);

    const auto oversized_path =
        QDir(directory.path()).filePath(QStringLiteral("oversized-profile.json"));
    QFile oversized(oversized_path);
    QVERIFY(oversized.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(oversized.write(QByteArray(1024 * 1024 + 1, 'x')), qint64{1024 * 1024 + 1});
    oversized.close();
    const auto oversized_import = BenchProfileCodec::importFile(oversized_path);
    QVERIFY(!oversized_import.has_value());
    QCOMPARE(oversized_import.error().code, BenchProfileErrorCode::InputTooLarge);
}

void BenchProfileEditorTest::exposesAccessibleKeyboardControls() {
    BenchProfileEditor editor;
    editor.resize(800, 720);
    QVERIFY(editor.loadProfile(sampleProfile()).has_value());
    editor.show();
    editor.activateWindow();
    QApplication::processEvents();

    QVERIFY(editor.fictionalCompositeLabel()->isVisible());
    QVERIFY(
        editor.fictionalCompositeLabel()->text().contains(QStringLiteral("Fictional/composite")));
    QVERIFY(!editor.fictionalCompositeLabel()->accessibleName().isEmpty());
    QVERIFY(!editor.previewLabel()->accessibleName().isEmpty());

    const std::array interaction_controls{
        InteractionControl::Directness,       InteractionControl::Formality,
        InteractionControl::QuestionLength,   InteractionControl::InterruptionFrequency,
        InteractionControl::FollowUpDepth,    InteractionControl::HypotheticalFrequency,
        InteractionControl::ConcessionRecall, InteractionControl::RecordPinDemand,
        InteractionControl::TimeStrictness,
    };
    for (const auto identifier : interaction_controls) {
        auto* control = editor.interactionControl(identifier);
        QVERIFY(control != nullptr);
        QVERIFY(!control->accessibleName().isEmpty());
        QCOMPARE(control->focusPolicy(), Qt::StrongFocus);
    }
    QVERIFY(!editor.voiceRegisterControl()->accessibleName().isEmpty());
    QVERIFY(!editor.voiceCadenceControl()->accessibleName().isEmpty());
    QVERIFY(!editor.questionFramingControl()->accessibleName().isEmpty());
    QVERIFY(!editor.addressConventionControl()->accessibleName().isEmpty());
    QVERIFY(!editor.verbosityControl()->accessibleName().isEmpty());
    QVERIFY(!editor.sentenceComplexityControl()->accessibleName().isEmpty());
    for (auto* control : {editor.questionPhrasesControl(), editor.interruptionPhrasesControl(),
                          editor.clarificationPhrasesControl()}) {
        QVERIFY(!control->accessibleName().isEmpty());
        QVERIFY(!control->accessibleDescription().isEmpty());
        QCOMPARE(control->focusPolicy(), Qt::StrongFocus);
        QVERIFY(control->tabChangesFocus());
    }
    QVERIFY(!editor.issueFocusControl("example.issue.preservation")->accessibleName().isEmpty());

    auto* directness = editor.interactionControl(InteractionControl::Directness);
    auto* formality = editor.interactionControl(InteractionControl::Formality);
    directness->setFocus();
    QTRY_VERIFY(directness->hasFocus());
    QTest::keyClick(directness, Qt::Key_Tab);
    QTRY_VERIFY(formality->hasFocus());

    auto* record_pin = editor.interactionControl(InteractionControl::RecordPinDemand);
    auto* time_strictness = editor.interactionControl(InteractionControl::TimeStrictness);
    record_pin->setFocus();
    QTRY_VERIFY(record_pin->hasFocus());
    QTest::keyClick(record_pin, Qt::Key_Tab);
    QTRY_VERIFY(time_strictness->hasFocus());

    editor.questionFramingControl()->setFocus();
    QTRY_VERIFY(editor.questionFramingControl()->hasFocus());
    QTest::keyClick(editor.questionFramingControl(), Qt::Key_Tab);
    QTRY_VERIFY(editor.addressConventionControl()->hasFocus());

    editor.questionPhrasesControl()->setFocus();
    QTRY_VERIFY(editor.questionPhrasesControl()->hasFocus());
    QTest::keyClick(editor.questionPhrasesControl(), Qt::Key_Tab);
    QTRY_VERIFY(editor.interruptionPhrasesControl()->hasFocus());

    const auto* directness_label =
        editor.findChild<QLabel*>(QStringLiteral("directnessControlLabel"));
    QVERIFY(directness_label != nullptr);
    QCOMPARE(directness_label->buddy(), directness);
    const auto* framing_label = editor.findChild<QLabel*>(QStringLiteral("questionFramingLabel"));
    const auto* phrases_label =
        editor.findChild<QLabel*>(QStringLiteral("questionPhrasesControlLabel"));
    QVERIFY(framing_label != nullptr);
    QVERIFY(phrases_label != nullptr);
    QCOMPARE(framing_label->buddy(), editor.questionFramingControl());
    QCOMPARE(phrases_label->buddy(), editor.questionPhrasesControl());
}

void BenchProfileEditorTest::previewChangesWithoutTouchingLegalState() {
    constexpr auto sentinel = "facts=fixed;deadline=fixed;authored-outcome=fixed";
    BenchProfileEditor editor;
    editor.setProperty("legalStateSentinel", QString::fromLatin1(sentinel));
    QVERIFY(editor.loadProfile(sampleProfile()).has_value());
    const auto first_preview = editor.previewText();
    QVERIFY(first_preview.contains(QStringLiteral("Fictional/composite")));

    editor.interactionControl(InteractionControl::Directness)->setValue(0.12);
    const auto second_preview = editor.previewText();
    QVERIFY(second_preview != first_preview);
    editor.voiceRegisterControl()->setCurrentIndex(
        editor.voiceRegisterControl()->findData(static_cast<int>(VoiceRegister::Technical)));
    const auto third_preview = editor.previewText();
    QVERIFY(third_preview != second_preview);
    editor.questionPhrasesControl()->setPlainText(QStringLiteral("test the exact premise"));
    const auto fourth_preview = editor.previewText();
    QVERIFY(fourth_preview != third_preview);
    QVERIFY(fourth_preview.contains(QStringLiteral("test the exact premise")));
    QCOMPARE(editor.property("legalStateSentinel").toString(), QString::fromLatin1(sentinel));

    BenchProfileEditor identical;
    QVERIFY(identical.loadProfile(*editor.profile()).has_value());
    QCOMPARE(identical.previewText(), editor.previewText());
}

} // namespace

QTEST_MAIN(BenchProfileEditorTest)

#include "tst_bench_profile_editor.moc"
