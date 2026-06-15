#include "persistent/persistent.h"

namespace attemory::persistent {

static_assert(SessionStore::kCurrentSchemaVersion == 1, "unexpected session store schema version");
static_assert(CacheManifest::kCurrentSchemaVersion == 1, "unexpected cache manifest schema version");

} // namespace attemory::persistent
