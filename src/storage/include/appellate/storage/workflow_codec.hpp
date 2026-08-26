#pragma once

#include "appellate/model/workflow_command.hpp"
#include "appellate/model/workflow_event.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>

namespace appellate::storage {

enum class WorkflowCodecErrorCode {
    InvalidJson,
    DuplicateMember,
    PayloadTooLarge,
    UnsupportedVersion,
    UnknownCommandType,
    UnknownEventType,
    MissingField,
    UnexpectedField,
    InvalidField,
    OutOfRange,
    IncompleteAuthority,
};

struct WorkflowCodecError final {
    WorkflowCodecErrorCode code;
    QString message;

    friend bool operator==(const WorkflowCodecError&, const WorkflowCodecError&) = default;
};

[[nodiscard]] std::expected<QByteArray, WorkflowCodecError>
encodeWorkflowCommand(const model::WorkflowCommand& command);

[[nodiscard]] std::expected<model::WorkflowCommand, WorkflowCodecError>
decodeWorkflowCommand(QByteArrayView encoded);

[[nodiscard]] QString workflowCommandType(const model::WorkflowCommand& command);

[[nodiscard]] std::expected<QByteArray, WorkflowCodecError>
encodeWorkflowEvent(const model::WorkflowEvent& event);

[[nodiscard]] std::expected<model::WorkflowEvent, WorkflowCodecError>
decodeWorkflowEvent(QByteArrayView encoded);

[[nodiscard]] QString workflowEventType(const model::WorkflowEvent& event);

[[nodiscard]] QString workflowPrimaryAuthorityId(const model::WorkflowEvent& event);

} // namespace appellate::storage
