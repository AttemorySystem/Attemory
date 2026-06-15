#pragma once

#include <cstdint>

enum class AttemoryCommand {
    ADD_SYSTEM,
    ADD_MEMORY,
    RESTORE,
    CLEAR,
    CREATE_SESSION,
    DELETE_SESSION,
    SEARCH,
    NEXT_SEGMENT,
    SAVE_SESSION,
    INDEX,
    ONESHOT_SEARCH,
};

struct AttentionMemoryRef {
    int32_t memory_index = 0;
    int32_t segment_id = 0;
};
