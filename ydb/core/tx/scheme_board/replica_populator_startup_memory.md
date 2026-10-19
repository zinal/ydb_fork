# Память `SCHEME_BOARD_REPLICA_POPULATOR_ACTOR` при массовом старте SchemeShard

Анализ версии YDB `stable-26-3-1` (`github.com/ydb-platform/ydb`). Логика `TReplicaPopulator` в этой ветке совпадает с `main` на момент разбора. Отличие `TPopulator` касается учёта кворума ack и на описываемый всплеск не влияет.

## Вывод

Всплеск — полная перезаливка схемы в SchemeBoard. Её начинает каждый tenant SchemeShard после своего старта. Актор `SCHEME_BOARD_REPLICA_POPULATOR_ACTOR` (`TReplicaPopulator`) держит окно из 100 полных `DescribeSchemeResult` на каждую реплику SchemeBoard.

На узле shared Dedicated базы Serverless таких окон одновременно столько, сколько tenant SchemeShard поднялось на узле, умножить на число реплик SchemeBoard. Пока реплики и interconnect не подтверждают пачки, окна остаются заполненными. Если узел переживает эту фазу, sync доходит до commit, новые большие `TEvUpdate` больше не создаются, очереди рассылаются, и память окна освобождается.

Ширина окна и состав описания путями конфигурации не задаются. Пик при рестарте узла можно только растянуть лимитом Hive на число одновременно стартующих таблеток.

## Условия, при которых это проявляется

1. База относится к коммунальному сервису YDB Serverless: общая shared Dedicated база, в которой живёт много serverless-тенантов.
2. Узел поднимает сразу много таблеток SchemeShard этих тенантов.
3. После рестарта поколение SchemeShard больше поколения, которое помнит реплика SchemeBoard, поэтому выполняется full sync, а не докачка.

На базе с одним-двумя SchemeShard тот же код даёт одно окно. На shared-узле множитель — плотность tenant SchemeShard.

## Хронология после старта узла

Несколько минут уходит на восстановление локальной базы таблетки. Популятора в это время ещё нет.

Дальше SchemeShard в `ActivateAfterInitialization` выполняет `TTxInitPopulator` (`ydb/core/tx/schemeshard/schemeshard_impl.cpp`). Транзакция обходит все живые пути и для каждого вызывает `DescribePath` (`ydb/core/tx/schemeshard/schemeshard__init_populator.cpp`).

В `Complete` этой транзакции:

1. Создаётся `SCHEME_BOARD_POPULATOR_ACTOR` со всем набором описаний.
2. Родитель резолвит SchemeBoard и на каждую реплику всех колец регистрирует `TReplicaPopulator` (`TMailboxType::ReadAsFilled`).
3. Сразу вызывается `SignalTabletActive`. Sync к этому моменту ещё не закончен: он идёт асинхронно.

Handshake сравнивает поколения. Ответ реплики содержит зафиксированное у неё поколение. Если оно меньше поколения SchemeShard, `TReplicaPopulator` пишет в лог `Start full sync` и заново шлёт все пути. Совпадение поколений бывает только у докачки того же поколения (`Resume sync` с `LastAckedPathId`). Рестарт таблетки поднимает поколение, поэтому после старта узла это full sync.

Пока SchemeBoard перегружен тем же стадом, ack приходят медленно, окна остаются полными. После commit окна схлопываются. Сам популятор остаётся жить вместе с таблеткой и дальше обслуживает инкрементальные публикации.

## Из чего состоит окно

Ограничение — 100 сообщений, не байт. `BatchSizeLimit` — `constexpr` в `TReplicaPopulator`:

```cpp
// ydb/core/tx/scheme_board/populator.cpp
static constexpr ui32 BatchSizeLimit = 100;
```

`ProcessSync` запрашивает следующее описание и шлёт `TEvUpdate`, пока в пачке меньше 100. У сотого сообщения ставится `NeedAck`. Реплика подтверждает только такие сообщения (`AckUpdate` выходит сразу, если `NeedAck` ложен). Следующая пачка запрашивается после ack с тем же `PathId`, что текущий `CurPathId`, и с нулевым cookie.

Пока ack нет, до 100 полных описаний на этот `TReplicaPopulator` лежат в исходящих `TEvUpdate` и в очереди interconnect до сериализации и освобождения события.

Инкрементальные `TEvUpdate`, пришедшие до конца handshake, payload не копят. `EnqueueUpdate` сохраняет `pathId`, версию и cookie, а тело события уничтожается. Большая память — sync-окно, которое тело не выбрасывает.

## Оценка объёма

Живые байты на время синка:

```text
N_scheme_shard × N_реплик_SchemeBoard × 100 × средний размер DescribeSchemeResult
```

`N_реплик` — все реплики всех колец, не `NToSelect`. Актор создаётся в цикле по `RingGroups` → `Rings` → `Replicas`.

Каждый путь уходит на объединение двух выборок: по `CityHash64(path)` и по `pathId.Hash()`. На кольцевую группу это до `2 × NToSelect` реплик. `NToSelect` задаёт, сколько реплик хранит путь. Число `TReplicaPopulator` оно не уменьшает: синк всё равно идёт по всем репликам, каждая со своим окном 100.

Пример: 200 SchemeShard на узле, 40 реплик SchemeBoard, описание 30 КБ — около 24 ГБ только в окнах. Окна заполнены одновременно, потому что реплики SchemeBoard в эту минуту принимают тот же поток.

Описание большое, потому что `TTxInitPopulator` вызывает `DescribePath` без урезания опций (`ShowPrivateTable = true`, остальное — значения protobuf по умолчанию). У `TDescribeOptions` (`ydb/core/protos/flat_scheme_op.proto`) по умолчанию включены:

| Поле | Значение по умолчанию |
| --- | --- |
| `ReturnPartitioningInfo` | true |
| `ReturnPartitionConfig` | true |
| `ReturnChildren` | true |
| `ReturnRangeKey` | true |
| `ReturnPartitionStats` | false |
| `ReturnBoundaries` | false |

В blob попадают список партиций с ключами диапазонов, конфиг партиций, список детей директории и, для топиков, пресериализованный список партиций. Для тенанта с широкими топиками и таблицами это сотни килобайт на путь.

## Почему shared Serverless

На узле shared-базы Hive поднимает SchemeShard многих serverless-тенантов. У каждого свой full sync в один кластерный SchemeBoard. Общего байтового бюджета на узел у этого конвейера нет.

Постоянная копия описаний после синка не исчезает. `TPopulator::Descriptions` живёт, пока жива таблетка. Её строки собирает `MakeOpaquePathDescription` в конструкторе популятора, а конструктор вызывается из `TTxInitPopulator::Complete` на потоке SchemeShard. Эта копия — тег `FLAT_SCHEMESHARD_ACTOR`. Спадающий всплеск — окна `TReplicaPopulator`, не этот корпус.

## Теги аллокатора

Пока актор исполняется, executor ставит тег lfalloc в индекс его активности. Имя тега совпадает с именем активности.

| Тег | Что в нём на этом сценарии | После синка |
| --- | --- | --- |
| `FLAT_SCHEMESHARD_ACTOR` | Корпус `DescribeSchemeResult` в `TPopulator::Descriptions`, собранный в `TTxInitPopulator` | Остаётся |
| `SCHEME_BOARD_POPULATOR_ACTOR` | Копия описания в `TEvDescribeResult` на каждый элемент окна | Спадает по мере доставки |
| `SCHEME_BOARD_REPLICA_POPULATOR_ACTOR` | Сборка `TEvUpdate` в `ProcessSync`; буфер описания переносится через `SetDescribeSchemeResultSerialized(std::move(...))`. Тег держит ширину и время жизни окна | Спадает |
| сессия interconnect | Сериализованный буфер отправки, если событие уже забрано из очереди сессии | Спадает после отправки |

Имеет смысл смотреть эти теги вместе. `SCHEME_BOARD_REPLICA_POPULATOR_ACTOR` — активность, которая открывает окно и не отпускает его до ack. Байты самого blob появляются на шаге раньше, в копировании `TOpaquePathDescription` в `TEvDescribeResult`.

На странице SchemeBoard у replica populator в этот момент `BatchSize` доходит до 100, `CurPathId` уходит вперёд, `LastAckedPathId` отстаёт. Когда `CurPathId` доходит до `MaxPathId` и уходит commit, окна схлопываются. `DescriptionsCount` родительского популятора остаётся равным числу путей.

## Настройки

Окно и состав `DescribePath` при инициализации конфигом не задаются. `BatchSizeLimit` и вызов `DescribePath` из `TTxInitPopulator` — константы кода.

### Что растягивает пик при рестарте узла

`HiveConfig.MaxTabletsScheduled` (по умолчанию 100) — сколько таблеток на одном узле Hive держит в состоянии старта (`TABLET_VOLATILE_STATE_STARTING`). Пока SchemeShard восстанавливает локальную базу и выполняет `TTxInitPopulator`, он занимает слот. Меньшее значение не даёт сотне тенантов одновременно открыть окна.

Ограничение неполное. `SignalTabletActive` вызывается в момент создания популятора. После этого таблетка для Hive уже запущена (`GetTabletsScheduled` считает только `STARTING`), слот освобождается, а `TReplicaPopulator` продолжает слать описания. Если sync длиннее, чем подъём следующего SchemeShard, окна соседних волн пересекаются. Лимит общий для всех типов таблеток: слишком малое значение замедлит и DataShard на этом узле.

`BootStrategy = HIVE_BOOT_STRATEGY_BALANCED` (значение по умолчанию) при достижении лимита на узле останавливает старт на всех узлах. На пик одного узла это не влияет.

### Что не снижает стадо уже размещённых SchemeShard

`HiveConfig.DefaultTabletLimit` и узелный `TabletAvailability` для типа `SchemeShard` ограничивают новое размещение. Для таблетки, которая уже числится на узле, `TNodeInfo::IsAbleToRunTablet` возвращается раньше проверки `MaxCount`:

```cpp
// ydb/core/mind/hive/node_info.cpp
if (tablet.IsPresentOnLocal(Local)) {
    return !(IsOverloaded() && tablet.HasAllowedMetric(EResourceToBalance::ComputeResources));
}
```

Повторный старт узла эти лимиты не прореживают. Они уменьшают плотность при следующих назначениях.

### Топология SchemeBoard

Число реплик уменьшает второй множитель линейно: популятор создаётся на каждую реплику каждого кольца. `NToSelect` число популяторов не сокращает. Это изменение кворума кластера.

### Что на ширину окна не влияет

- `HiveConfig.MaxBootBatchSize` (по умолчанию 1000) — сколько записей boot queue Hive обрабатывает за один проход. Число одновременно стартующих таблеток задаёт `MaxTabletsScheduled`.
- `WarmUpEnabled` / `MaxWarmUpPeriod` — пауза перед стартом таблеток, пока не подключатся узлы. К моменту старта стадо остаётся тем же.
- `interconnect_config.EnablePreserializeEvents` (по умолчанию false) — момент сериализации события в сессии interconnect. Объём окна задаёт популятор.

## Исходные места

| Что | Где |
| --- | --- |
| Окно 100 и full sync | `ydb/core/tx/scheme_board/populator.cpp`, `TReplicaPopulator::ProcessSync` |
| Популятор на каждую реплику | там же, обработка `TEvListSchemeBoardResult` |
| Двойная выборка реплик | `TPopulator::SelectReplicas` |
| Сбор описаний и `SignalTabletActive` | `ydb/core/tx/schemeshard/schemeshard__init_populator.cpp` |
| Запуск транзакции после инициализации | `TSchemeShard::ActivateAfterInitialization` |
| Опции describe по умолчанию | `TDescribeOptions` в `ydb/core/protos/flat_scheme_op.proto` |
| Лимит одновременно стартующих таблеток | `THiveConfig.MaxTabletsScheduled` в `ydb/core/protos/config.proto` |
