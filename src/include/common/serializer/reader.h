#pragma once

#include <cstdint>

#include "common/cast.h"

namespace lbug {
namespace common {

class Reader {
public:
    virtual void read(uint8_t* data, uint64_t size) = 0;
    virtual ~Reader() = default;

    virtual bool finished() = 0;
    // Upper bound on the bytes this reader can still supply, or UINT64_MAX when
    // unknown. Persisted lengths and counts are validated against it before they
    // size an allocation.
    virtual uint64_t getRemainingBytes() const { return UINT64_MAX; }
    virtual void onObjectBegin() {};
    virtual void onObjectEnd() {};

    template<typename TARGET>
    TARGET* cast() {
        return common::ku_dynamic_cast<TARGET*>(this);
    }
};

} // namespace common
} // namespace lbug
