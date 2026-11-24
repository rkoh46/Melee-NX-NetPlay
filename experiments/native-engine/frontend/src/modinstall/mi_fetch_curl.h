// The installer's Fetcher over libcurl (mi_fetch_curl.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mi_io.h"

namespace modinstall {
Fetcher* curl_fetcher();   // one shared instance
}
