#pragma once

#include "context/kv/segment_kv_manager.h"
#include "context/search/search_context.h"

#include <string>
#include <vector>

namespace attemory::context {

SearchConfig apply_search_request_overrides(
    const SearchConfig & base,
    const SearchRequestOverrides & request_overrides);

CommandResult run_search(
    SearchRuntimeContext context,
    kv::SegmentKVManager & kv_manager,
    const std::string & session_id,
    const SearchInput & input,
    const SearchConfig & search_config);

CommandResult run_oneshot_search(
    SearchRuntimeContext context,
    kv::SegmentKVManager & kv_manager,
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const SearchInput & input,
    const SearchConfig & search_config);

} // namespace attemory::context
