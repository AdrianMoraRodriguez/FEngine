#pragma once

/**
 * @file ImportControl.h
 * @brief Shared state between an import running on a background thread and
 *        the code that started it.
 */

#include <atomic>

namespace fe {

/**
 * @brief Lets the editor watch and cancel a model import.
 *
 * One instance is shared (through a `std::shared_ptr`) by the thread that
 * runs the import and the thread that started it. Both fields are atomics,
 * so neither side needs a lock.
 */
struct ImportControl {
    /// How far the import has got, from 0 to 1. Written by the importer.
    /// It is an estimate: Assimp reports its own progress while reading and
    /// post-processing, and the conversion to engine data fills the rest.
    std::atomic<float> progress{0.0f};

    /// Set by the editor to ask the importer to stop as soon as it can.
    std::atomic<bool> cancelRequested{false};
};

} // namespace fe
