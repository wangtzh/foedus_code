/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_SNAPSHOT_SNAPSHOT_OPTIONS_HPP_
#define FOEDUS_SNAPSHOT_SNAPSHOT_OPTIONS_HPP_
#include <foedus/cxx11.hpp>
#include <foedus/externalize/externalizable.hpp>
#include <foedus/fs/device_emulation_options.hpp>
#include <string>
#include <vector>
namespace foedus {
namespace snapshot {
/**
 * @brief Set of options for snapshot manager.
 * @ingroup SNAPSHOT
 * This is a POD struct. Default destructor/copy-constructor/assignment operator work fine.
 */
struct SnapshotOptions CXX11_FINAL : public virtual externalize::Externalizable {
    enum Constants {
        DEFAULT_SNAPSHOT_TRIGGER_PAGE_POOL_PERCENT = 100,
        DEFAULT_SNAPSHOT_INTERVAL_MILLISECONDS = 60000,
    };

    /**
     * Constructs option values with default values.
     */
    SnapshotOptions();

    /**
     * @brief String pattern of path of snapshot folders in each NUMA node.
     * @details
     * This specifies the path of the folders to contain snapshot files in each NUMA node.
     * Two special placeholders can be used; $NODE$ and $PARTITION$.
     * $NODE$ is replaced with the NUMA node number.
     * $PARTITION$ is replaced with the partition in the node (0 to partitions_per_node_ - 1).
     * For example,
     * \li "/data/node_$NODE$/part_$PARTITION$" becomes "/data/node_1/part_0" on node-1 and part-0.
     * \li "/data/folder_$INDEX$" becomes "/data/folder_1" on any node and partition-1.
     *
     * Both are optional. You can specify a fixed path without the patterns, which means you will
     * use the same folder for multiple partitions and nodes.
     * Even in that case, snapshot file names include uniquefiers, so it wouldn't cause any data
     * corruption. It just makes things harder for poor sysadmins.
     *
     * The snapshot folders are also the granularity of partitioning.
     * Each snapshot phase starts with partitioning of logs using random samples, then
     * scatter-gather log entries to assigned partitions like Map-Reduce.
     *
     * The default value is "snapshots/node_$NODE$/partition_$PARTITION$".
     */
    std::string                         folder_path_pattern_;

    /**
     * @brief Number of snapshot folders (ie partitions) per NUMA node.
     * @details
     * This value must be at least 1 (which is also default).
     * A larger value might be able to employ more CPU power during snapshot construction,
     * but makes the scatter-gather more fine grained, potentially making it slower.
     */
    uint16_t                            partitions_per_node_;

    /**
     * When the main page pool runs under this percent (roughly calculated) of free pages,
     * snapshot manager starts snapshotting to drop volatile pages even before the interval.
     * Default is 100 (no check).
     */
    uint16_t                            snapshot_trigger_page_pool_percent_;

    /**
     * Interval in milliseconds to take snapshots.
     * Default is one minute.
     */
    uint32_t                            snapshot_interval_milliseconds_;

    /** Settings to emulate slower data device. */
    foedus::fs::DeviceEmulationOptions  emulation_;

    EXTERNALIZABLE(SnapshotOptions);
};
}  // namespace snapshot
}  // namespace foedus
#endif  // FOEDUS_SNAPSHOT_SNAPSHOT_OPTIONS_HPP_
