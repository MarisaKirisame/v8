#include <nlohmann/json.hpp>
#include "v8.h"

namespace v8 {
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GCRecord, before_memory, after_memory, before_time, after_time, is_major_gc)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GCHistory, records, heap_birth_time)
}
