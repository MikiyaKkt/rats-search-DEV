#ifndef RATS_DOMAIN_HASH_STATE_H
#define RATS_DOMAIN_HASH_STATE_H
namespace rats::domain {
enum class HashState { Known, Missing, Incomplete, Collision, Failed };
}
#endif
