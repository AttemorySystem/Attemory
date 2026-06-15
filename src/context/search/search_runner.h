#pragma once

#include "attemory-core/attemory-core.h"
#include "context/kv/segment_kv_manager.h"
#include "context/search/search_context.h"

#include <string>
#include <vector>

namespace attemory::context {

class SegmentSearchRunner {
public:
    SegmentSearchRunner(
        SearchRuntimeContext context,
        kv::SegmentKVManager & kv_manager,
        Session & session,
        const atmcore::SearchQueryTokens & query_stage_tokens);

    bool run_cached(
        std::vector<AttentionMemoryRef> & ordered_memories,
        std::string & error);

    bool run_ephemeral(
        const std::string & system,
        std::vector<AttentionMemoryRef> & ordered_memories,
        std::string & error);

private:
    SearchRuntimeContext context_;
    kv::SegmentKVManager & kv_manager_;
    Session & session_;
    const atmcore::SearchQueryTokens & query_stage_tokens_;
};

} // namespace attemory::context
