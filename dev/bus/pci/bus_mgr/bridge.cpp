/*
 * Copyright (c) 2021 Travis Geiseblrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include "bridge.h"

#include <sys/types.h>
#include <lk/cpp.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <dev/bus/pci.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <platform/interrupts.h>

#define LOCAL_TRACE 0

#include "bridge.h"
#include "bus.h"

namespace pci {

bridge::bridge(pci_location_t loc, bus *bus) : device(loc, bus) {}
bridge::~bridge() = default;

// examine the bridge device, figuring out the bus range it controls and recurse
status_t bridge::probe(pci_location_t loc, bus *parent_bus, bridge **out_bridge) {
    char str[14];
    LTRACEF("%s\n", pci_loc_string(loc, str));

    *out_bridge = nullptr;

    // read vendor id and see if this is a real device
    uint16_t vendor_id;
    status_t err = pci_read_config_half(loc, PCI_CONFIG_VENDOR_ID, &vendor_id);
    if (err != NO_ERROR) {
        return ERR_NOT_FOUND;
    }
    if (vendor_id == 0xffff) {
        return ERR_NOT_FOUND;
    }

    // read header type (0 or 1)
    uint8_t header_type;
    err = pci_read_config_byte(loc, PCI_CONFIG_HEADER_TYPE, &header_type);
    if (err != NO_ERROR) {
        return ERR_NOT_FOUND;
    }

    // we are a bridge to a new set of busses
    bridge *br = new bridge(loc, parent_bus);

    // we only grok type 1 headers here
    err = pci_read_config(loc, &br->config_);
    if (err < 0) {
        delete br;
        return err;
    }

    LTRACEF("primary bus %hhd secondary %hhd subordinate %hhd\n",
            br->primary_bus(), br->secondary_bus(), br->subordinate_bus());

    // probe the bridge's capabilities
    br->probe_capabilities();

    *out_bridge = br;

    if (br->secondary_bus() > 0 && br->subordinate_bus() >= br->secondary_bus()) {
        // start a scan of the secondary bus downstream of this.
        // via bridge devices on this bus, should find all of the subordinate busses.
        bus *new_bus;
        pci_location_t bus_location = {};
        bus_location.segment = loc.segment;
        bus_location.bus = br->config_.type1.secondary_bus;
        err = bus::probe(bus_location, br, &new_bus);
        if (err < 0) {
            return err;
        }

        // add the bus to our list of children
        DEBUG_ASSERT(new_bus);
        br->add_bus(new_bus);

        // add the bus to the global bus list
        new_bus->add_to_global_list();
    }

    return NO_ERROR;
}

void bridge::dump(size_t indent) {
    auto scoot = [&]() {
        for (size_t i = 0; i < indent; i++) {
            printf(" ");
        }

    };
    char str[14];
    scoot();
    printf("bridge %s %04hx:%04hx child busses [%d..%d]\n", pci_loc_string(loc_, str),
           config_.vendor_id, config_.device_id,
           config_.type1.secondary_bus, config_.type1.subordinate_bus);

    auto mr = mem_range();
    auto ir = io_range();
    auto pr = prefetch_range();
    scoot();
    printf("mem_range [%#x..%#x] io_range [%#x..%#x] pref_range [%#llx..%#llx] \n",
           mr.base, mr.limit, ir.base, ir.limit, pr.base, pr.limit);

    for (size_t b = 0; b < 2; b++) {
        if (bars_[b].valid) {
            for (size_t i = 0; i < indent + 1; i++) {
                printf(" ");
            }
            printf("BAR %zu: addr %#llx size %#zx io %d valid %d\n", b, bars_[b].addr, bars_[b].size, bars_[b].io, bars_[b].valid);
        }
    }

    if (secondary_bus_) {
        secondary_bus_->dump(indent + 1);
    }
}

// accessors to compute the io and memory range of the bridge
bridge::range<uint32_t> bridge::io_range() {
    if (config_.type1.io_limit < config_.type1.io_base) {
        return { 0, 0 };
    } else {
        // TODO: handle 32bit io (does this really exist?)
        return { ((uint32_t)config_.type1.io_base >> 4) << 12,
                 (((uint32_t)config_.type1.io_limit >> 4) << 12) | 0xfff };
    }
}

bridge::range<uint32_t> bridge::mem_range() {
    if (config_.type1.memory_limit < config_.type1.memory_base) {
        return { 0, 0 };
    } else {
        return { ((uint32_t)config_.type1.memory_base >> 4) << 20,
                 (((uint32_t)config_.type1.memory_limit >> 4) << 20) | 0xfffff };
    }
}

bridge::range<uint64_t> bridge::prefetch_range() {
    if (config_.type1.prefetchable_memory_limit < config_.type1.prefetchable_memory_base) {
        return { 0, 0 };
    } else {
        bool is_64 = (config_.type1.prefetchable_memory_base & 0xf) == 1;

        uint64_t base, limit;

        base = (((uint64_t)config_.type1.prefetchable_memory_base >> 4) << 20);
        if (is_64) {
            base |= (uint64_t)config_.type1.prefetchable_base_upper << 32;
        }
        limit = (((uint64_t)config_.type1.prefetchable_memory_limit >> 4) << 20) | 0xfffff;
        if (is_64) {
            limit |= (uint64_t)config_.type1.prefetchable_limit_upper << 32;
        }
        return { base, limit };
    }
}

} // namespace pci
