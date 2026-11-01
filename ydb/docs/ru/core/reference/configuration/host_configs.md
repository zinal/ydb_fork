# host_configs

Кластер {{ ydb-short-name }} состоит из множества узлов, для развертывания которых обычно используется одна или несколько типовых конфигураций серверов. Для того чтобы не повторять её описание для каждого узла, в файле конфигурации существует раздел `host_configs`, в котором перечислены используемые конфигурации, и им присвоены идентификаторы.

## Синтаксис

```yaml
host_configs:
- host_config_id: 1
  drive:
  - path: <path_to_device>
    type: <type>
    disk_scope: <disk_scope>  # необязательный атрибут
  - path: ...
- host_config_id: 2
  ...
```

Атрибут `host_config_id` задает числовой идентификатор конфигурации. В атрибуте `drive` содержится коллекция описаний подключенных дисков. Каждое описание состоит из двух обязательных атрибутов:

- `path` : Путь к смонтированному блочному устройству, например `/dev/disk/by-partlabel/ydb_disk_ssd_01`
- `type` : Тип физического носителя устройства: `ssd`, `nvme` или `rot` (rotational - HDD)

Дополнительно может быть указан необязательный атрибут `disk_scope` — метка для вычисления домена отказа для некоторых упрощенных конфигураций, см. [Конфигурирование disk_scope](#disk-scope).

## Примеры

Одна конфигурация с идентификатором 1, с одним диском типа SSD, доступным по пути `/dev/disk/by-partlabel/ydb_disk_ssd_01`:

``` yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/disk/by-partlabel/ydb_disk_ssd_01
    type: SSD
```

Две конфигурации с идентификаторами 1 (два SSD диска) и 2 (три SSD диска):

``` yaml
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

## Конфигурирование disk_scope {#disk-scope}

`disk_scope` — необязательный строковый атрибут диска, задающий более детальную зону отказа внутри одного узла. Он учитывается при вычислении [доменов отказа](../../concepts/glossary.md#fail-domain) во время выбора дисков для размещения [VDisk'ов](../../concepts/glossary.md#vdisk) [групп хранения](../../concepts/glossary.md#storage-group).

Домен отказа определяется физическим расположением диска (дата-центр, стойка, сервер, физическое устройство), поэтому либо никакие два VDisk'а одной группы не могут быть размещены на одном узле (если домен отказа соответствует серверу или серверной стойке), либо несколько VDisk'ов одной группы может быть размещено на одном узле на разных дисках (если домен отказа соответствует отдельному физическому устройству). В первом случае невозможно создать конфигурацию в режиме `block-4-2` менее чем с 8 узлами хранения, а во втором случае конфигурация может не переживать отказ 1 узла. Чтобы ограничить количество VDisk'ов одной группы, размещаемых на одном узле, физические устройства можно пометить атрибутом `disk_scope` и включить расчёт доменов отказа по уровню `disk_scope`. Это позволит собирать некоторые отказоустойчивые конфигурации в инсталляциях, в которых серверов меньше, чем требуется доменов отказа для выбранного [режима отказоустойчивости](../../concepts/topology.md).

### Пример {#disk-scope-example}

Кластер из 4 серверов, на каждом сервере 4 диска в режиме отказоустойчивости `block-4-2`:

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

Режиму `block-4-2` нужны 8 доменов отказа, и он переживает потерю 2 из них. Две разные метки `disk_scope` на каждом сервере дают эти 8 доменов (4 сервера × 2). Отказ одного сервера тогда задевает ровно 2 домена. Третья отдельная метка на том же сервере заденет больше двух доменов, а этот режим такого отказа не переживает. Дополнительные диски добавляются к тем же двум меткам. У каждого сервера должен быть свой `location.body`. Одинаковая метка на разных серверах — это разные домены, потому что сервер входит в домен. Число стоек не важно.

Метка учитывается на уровне 50, сразу после сервера (`unit` / `location.body`, уровень 40). Отдельный диск — это уровень 255. Граница домена отказа поэтому `domain_level_end: 60`. Утилита `ydbd_slice` разворачивает шаблонный ключ `fail_domain_type: disk_scope` именно в эту геометрию (`ydb/tools/cfg/types.py`, `DistinctionLevels`) и для статической группы, и для пулов хранения.

В ConfigV2 значение `fail_domain_type: disk_scope` недопустимо. Поле принимает только `rack`, `body` и `disk`. `body` (граница 50) игнорирует `disk_scope`, а `disk` (граница 256) делает каждый диск отдельным доменом, и на одном сервере может оказаться больше двух VDisk одной группы. Геометрию нужно задать явно и одинаково для статической группы и для пулов хранения баз данных. Полный пример — [block-4-2-4-nodes.yaml](https://github.com/ydb-platform/ydb/blob/main/ydb/deploy/yaml_config_examples/block-4-2-4-nodes.yaml).

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

В шаблонах `ydbd_slice` по-прежнему пишут `fail_domain_type: disk_scope` и `domains[].storage_pool_kinds[].fail_domain_type: disk_scope`. Это ключи шаблона, а не файла ConfigV2. Утилита подставляет геометрию выше, когда собирает конфигурацию кластера.

При такой конфигурации на каждом сервере размещаются по два VDisk каждой группы хранения, и наибольший допустимый отказ — отказ одного сервера.

## Особенности Kubernetes {#host-configs-k8s}

{{ ydb-short-name }} Kubernetes operator монтирует NBS диски для Storage узлов на путь `/dev/kikimr_ssd_00`. Для их использования должна быть указана следующая конфигурация `host_configs`:

``` yaml
host_configs:
- host_config_id: 1
  drive:
  - path: /dev/kikimr_ssd_00
    type: SSD
```

Файлы с примерами конфигурации, поставляемые в составе {{ ydb-short-name }} Kubernetes operator, уже содержат такую секцию, и её не нужно менять.