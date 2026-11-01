# host_configs

A {{ ydb-short-name }} cluster consists of multiple nodes, and one or more typical server configurations are usually used for their deployment. To avoid repeating their description for each node, there is a `host_configs` section in the configuration file that lists the used configurations and assigned IDs.

## Syntax


```yaml
host_configs:
- host_config_id: 1
  drive:
  - path: <path_to_device>
    type: <type>
    disk_scope: <disk_scope>  # optional attribute
  - path: ...
- host_config_id: 2
  ...
```


The `host_config_id` attribute specifies a numeric configuration ID. The `drive` attribute contains a collection of descriptions of connected drives. Each description consists of two mandatory attributes:

- `path`: Path to the mounted block device, for example, `/dev/disk/by-partlabel/ydb_disk_ssd_01`
- `type`: Type of the device's physical media: `ssd`, `nvme`, or `rot` (rotational - HDD)

Additionally, an optional `disk_scope` attribute can be specified — a label for calculating fail domains for some reduced configurations, see [Configuring disk_scope](#disk-scope).

## Examples

One configuration with ID 1 and one SSD disk accessible via `/dev/disk/by-partlabel/ydb_disk_ssd_01`:


```yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
    type: SSD
```


Two configurations with IDs 1 (two SSD disks) and 2 (three SSD disks):


```yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
    type: SSD
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_02
    type: SSD
- host_config_id: 2
  drive:
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
    type: SSD
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_02
    type: SSD
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_03
    type: SSD
```


## Configuring disk_scope {#disk-scope}

`disk_scope` is an optional string attribute of a disk that defines a finer-grained failure zone within a single node. It is taken into account when calculating [fail domains](../../concepts/glossary.md#fail-domain) during the selection of disks for placing [VDisks](../../concepts/glossary.md#vdisk) of [storage groups](../../concepts/glossary.md#storage-group).

A fail domain is determined by the physical location of a disk (data center, rack, server, physical device), so either no two VDisks of the same group can be placed on the same node (if the fail domain corresponds to a server or a server rack), or several VDisks of the same group can be placed on the same node on different disks (if the fail domain corresponds to an individual physical device). In the first case, it is impossible to create a configuration in `block-4-2` mode with fewer than 8 storage nodes, and in the second case the configuration might not stand the failure of a single node. To limit the number of VDisks of the same group placed on a single node, physical devices can be labeled with the `disk_scope` attribute, and fail domain calculation can be enabled at the `disk_scope` level. This makes it possible to build some fault-tolerant configurations in installations that have fewer servers than the number of fail domains required by the chosen [fault tolerance mode](../../concepts/topology.md).

### Example {#disk-scope-example}

A cluster of 4 servers with 4 disks on each server, in the `block-4-2` fault tolerance mode:


``` yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
    type: SSD
    disk_scope: fail-domain-1
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_02
    type: SSD
    disk_scope: fail-domain-1
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_03
    type: SSD
    disk_scope: fail-domain-2
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_04
    type: SSD
    disk_scope: fail-domain-2
```


`block-4-2` needs 8 fail domains and tolerates the loss of 2 of them. Two distinct `disk_scope` labels on every server produce those 8 domains (4 servers × 2). One server failure then removes exactly 2 domains. A third distinct label on the same server would remove more than 2 domains, which this erasure does not tolerate. Extra disks are added under the same two labels. Each host needs its own `location.body`. The same label on different hosts is a different domain, because the host is part of the domain. The number of racks does not matter.

The label is taken into account at level 50, immediately after the host (`unit` / `location.body`, level 40). An individual disk is level 255. The fail-domain boundary is therefore `domain_level_end: 60`. `ydbd_slice` expands a template key `fail_domain_type: disk_scope` into this geometry (`ydb/tools/cfg/types.py`, `DistinctionLevels`) for both the static group and storage pools.

ConfigV2 does not accept `fail_domain_type: disk_scope`. That field allows only `rack`, `body` and `disk`. `body` (boundary 50) ignores `disk_scope`, and `disk` (boundary 256) makes every drive its own domain, so more than two VDisks of one group can land on one server. Set the geometry explicitly, and set the same geometry for the static group and for database storage pools. A full example is [block-4-2-4-nodes.yaml](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/yaml_config_examples/block-4-2-4-nodes.yaml).

```yaml
metadata:
  kind: MainConfig
  cluster: ""
  version: 0
config:
  erasure: block-4-2
  default_disk_type: SSD
  self_management_config:
    enabled: true
    geometry: &disk_scope_geometry
      realm_level_begin: 10
      realm_level_end: 20
      domain_level_begin: 10
      domain_level_end: 60
      num_fail_realms: 1
      num_fail_domains_per_fail_realm: 8
      num_vdisks_per_fail_domain: 1
  storage_pool_types:
  - kind: ssd
    pool_config:
      box_id: 1
      erasure_species: block-4-2
      kind: ssd
      vdisk_kind: Default
      geometry: *disk_scope_geometry
      pdisk_filter:
      - property:
        - type: SSD
```

`ydbd_slice` templates still use `fail_domain_type: disk_scope` and `domains[].storage_pool_kinds[].fail_domain_type: disk_scope`. Those keys belong to the template, not to a ConfigV2 file. The tool substitutes the geometry above when it builds the cluster configuration.

With this configuration, two VDisks of each storage group are placed on each server, and the largest tolerated failure is the failure of one server.

## Kubernetes Features {#host-configs-k8s}

The {{ ydb-short-name }} Kubernetes operator mounts NBS disks for Storage nodes at the path `/dev/kikimr_ssd_00`. To use them, the following `host_configs` configuration must be specified:


```yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/kikimr_ssd_00
    type: SSD
```


The example configuration files provided with the {{ ydb-short-name }} Kubernetes operator contain this section, and it does not need to be changed.
