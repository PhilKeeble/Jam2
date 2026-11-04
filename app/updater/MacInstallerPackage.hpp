#pragma once

#include "UpdatePackage.hpp"
#include <QStringList>

namespace jam2::update {
struct MacToolResult { int exitCode; QString output; };
using MacToolRunner = std::function<MacToolResult(const QString&, const QStringList&)>;

// The runner seam exercises the actual verification/handoff policy without
// installing software or requiring Apple certificates on test machines.
void verifyMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner = {});
void prepareMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner = {});
Metadata verifyPreparedMacInstaller(const QString& root, const QString& version,
    const std::atomic_bool& cancelled, const MacToolRunner& runner = {});
void openMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner = {});
}
