#include "storage/redis/drain_worker.hpp"

namespace run::storage::redis
{

template class GenericDrainWorker<DeferredTrackEvent>;

} // namespace run::storage::redis
