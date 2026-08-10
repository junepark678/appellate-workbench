#pragma once

#include "appellate/packs/error.hpp"

#include <QByteArrayView>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringView>

#include <expected>

namespace appellate::packs {

struct JsonLimits final {
    qsizetype maximum_depth{64};
    qsizetype maximum_values{200'000};
};

// Validates only the deliberately supported, fail-closed JSON Schema 2020-12
// subset. Schemas are loaded from the application's Qt resources and refs are
// confined to that registry; validation never performs network or filesystem I/O.
class SchemaValidator final {
  public:
    [[nodiscard]] static std::expected<SchemaValidator, Error> fromBundledSchemas();

    [[nodiscard]] static std::expected<QJsonObject, Error>
    parseObject(QByteArrayView bytes, QStringView source_name, JsonLimits limits = {});

    [[nodiscard]] std::expected<void, Error> validate(QStringView schema_file,
                                                      const QJsonObject& instance) const;

  private:
    QHash<QString, QJsonObject> schemas_;
};

} // namespace appellate::packs
