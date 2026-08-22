// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

namespace ShadNet {

// Server version, e.g. "0.0.14".
QString Version();

// When this build was compiled, as ISO 8601 local time: "2026-08-22T09:30:00".
QString BuildTimestamp();

// Just the date part of the build, "2026-08-22".
QString BuildDate();

// Just the time part of the build, "09:30:00".
QString BuildTime();

} // namespace ShadNet
