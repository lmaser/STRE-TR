#pragma once

#include "../../../TR-Shared/SimpleUIV2/TRSimpleUIV2.h"

#include <string>
#include <vector>

namespace TR::StreUIV2
{
static_assert(SimpleUIV2::TR_SHARED_UI_V2_API_VERSION == 0x00020024u,
              "STRE-TR requires TR SimpleUIV2 API 2.1");
inline constexpr std::string_view STRE_REQUIRED_SHARED_UI_V2_REVISION =
    "simple-ui-v2-family-equivalence-20260816";
static_assert(STRE_REQUIRED_SHARED_UI_V2_REVISION == SimpleUIV2::TR_SHARED_UI_V2_REVISION,
              "STRE-TR and TR SimpleUIV2 revisions are not coordinated");

const SimpleUIV2::SimplePluginDefinition& definition();
const std::vector<std::string>& retiredUiParameterIds();
}
