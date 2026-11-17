# ANALYSIS 08: общие механизмы pedestrian, player и vehicle lifecycle

Дата: 2026-11-17  
Ветка: `analysis-07-player-control-chain`  
База: `TC2_ENTITY_INDEX.sqlite`, ранее сохранённые native code windows и результаты анализа mission graph.

## Главный вывод

Pedestrian в The Crew 2 нельзя рассматривать как одну сущность, которая сама по себе «становится игроком». В данных видны как минимум три раздельных слоя:

1. **Avatar entity** — модель персонажа, его части, анимационный контроллер и физическая оболочка.
2. **Player entity** — сущность игрока/участника, связанная с HUD-поведением и общими игровыми ссылками.
3. **Control/lifecycle manager** — native-механизм, который управляет текущим состоянием транспорта и avatar-mode. Его точный интерфейс передачи управления пока не восстановлен.

Это пока архитектурная модель, а не доказанная схема полного перехода. Но разделение подтверждается составом archetype-компонентов и ранее захваченным native-кодом.

## 1. Avatar и Player — разные роли, а не варианты одного класса

В базе отдельно существуют:

- `CITEntityAvatarTC2`
- `CITEntityAvatarInterior`
- `CITEntityAvatarNPC`
- `CITEntityHumanPlayer`
- `CITEntityGhostPlayer`
- `CITEntityAIPlayer`
- `CITEntityAIPlayerBotFreeride`

Компоненты распределены показательно:

| Класс | Binding | AvatarPuzzle | AnimCtrl | HudBehaviorAvatar | HudBehaviorPlayer |
|---|---:|---:|---:|---:|---:|
| AvatarTC2 | да | да | у части archetypes | нет | нет |
| AvatarInterior | да | да | да | да | нет |
| AvatarNPC | да | да | да | нет | нет |
| HumanPlayer | да | нет | нет | нет | да |
| GhostPlayer | да | нет | нет | нет | да |
| AIPlayerBotFreeride | да | нет | нет | нет | да |

Числа в таблице обозначают наличие компонента хотя бы у соответствующих archetypes, а не обязательное наличие у каждого экземпляра класса.

**Следствие:** `CITEntityAvatarTC2` не выглядит полноценным Player Controller. В нём нет `HudBehaviorPlayer`, а у `CITEntityHumanPlayer` нет AvatarPuzzle/AnimCtrl. Это сильный признак разделения визуально-физического аватара и player-сущности.

## 2. Общие компоненты: что они действительно связывают

### 2.1 Binding

`CITEntityComponentBinding` встречается у AvatarTC2, AvatarInterior, AvatarNPC, HumanPlayer, GhostPlayer, AIPlayer и у множества совершенно других игровых сущностей.

В доступных записях компонента встречаются поля:

- `BindingType`
- `ParentEntityID`
- `BoneName`
- `InheritParentVisibility`
- `UseLocalTransform`
- `UseParentAnimation`

Это общий механизм связывания сущностей/частей и наследования трансформации или анимации. Он может быть одним из мостов между avatar и другими сущностями, но сам по себе не доказывает передачу управления игроку.

### 2.2 AvatarPuzzle

У компонента `CITEntityComponentAvatarPuzzle` в исследованных данных обнаружены только варианты `BSPhereRadius`. Он относится к AvatarTC2, AvatarInterior и AvatarNPC.

Это больше похоже на часть avatar-физики/пространственного представления, чем на контроллер ввода.

### 2.3 AnimCtrl

`CITEntityComponentAnimCtrl` связывает AvatarTC2/Interior/NPC с объектами `CITEntityAnimCtrl`. В записях AvatarTC2 найдено 254 экземпляра компонента, у которых есть соответствующий AnimCtrl ID.

Это общий механизм анимационного состояния. Он нужен для ходьбы/бега/поз, но не отвечает на вопрос, кто получает input.

### 2.4 HUD behavior

`CITEntityComponentHudBehaviorAvatar` найден только у одного archetype AvatarInterior в текущем индексе. `CITEntityComponentHudBehaviorPlayer` встречается у HumanPlayer, GhostPlayer и AIPlayer-классов.

Это ещё одно подтверждение, что avatar и player представлены разными сущностями. HUD-компоненты не следует принимать за управление движением.

## 3. Связь с транспортом: AvatarDrivingConfig

`CITEntityAvatarDrivingConfig` содержит поля:

- `DriverAvatarArchetype`
- `DriverAnimCtrlArchetype`
- `SeatOffsetHeight`
- `SeatOffsetLength`
- `SecurityBeltArchetype`

В базе найдено 6 ссылок `DriverAvatarArchetype`, все ведут на archetypes класса `CITEntityAvatarTC2`. У этих шести avatar archetypes компонентный набор минимален: `CITEntityComponentAvatarPuzzle`; они наследуются от одного родителя `C092020000000008`.

Это важная связка: транспорт уже умеет ссылаться на avatar и отдельный AnimCtrl через собственную конфигурацию водителя. Но это **не** доказывает, что именно этот avatar можно переключить в свободное управление. Скорее, это механизм отображения/анимации водителя внутри транспорта.

Пока не найдено поле, которое связывает эти шесть DriverAvatar archetypes с `CITEntityHumanPlayer`, `CITEntityGhostPlayer` или native manager.

## 4. Глобальные настройки Avatar

`CITEntityWorldSettings` ссылается на:

- `AvatarArchetypeTC2` → `CITEntityAvatarTC2`
- `AvatarInteriorArchetype` → `CITEntityAvatarInterior`

`CITEntityAvatarSettings` содержит настройки головы, меню-анимации и third-person animation controller. В предыдущем исследовании также найдены `AvatarCameraSettings` и `DefaultAvatarPhysicDesc`.

Это задаёт типы и конфигурацию avatar, но не является доказательством runtime spawn или назначения avatar текущим игроком.

## 5. HQ/Housing spawn points — специализированная точка входа

В данных отдельно существуют:

- `CITEntityHeadQuarterCharacterSpawnPoint`
- `CITEntityHeadQuarterExitPoint`
- `CITEntityHousingCharacterSpawnPoint`
- `CITEntityHousingExitSpawnPoint`

У них есть `Angles`, `SpawnPolicy`, а у housing-вариантов также `Room` и `VehicleFamily`. Это специализированные world markers для HQ/Housing flow.

В исследованных записях сам spawn point не содержит явной ссылки на `CITEntityHumanPlayer` или универсальный runtime player controller. Значит, точка появления и создание/назначение управляемой сущности, вероятно, разделены: marker задаёт место, а native/mission flow выполняет остальную работу. Это гипотеза, которую нужно подтвердить кодом.

## 6. Freedrive player spawner — отдельная ветка

`CITEntityAiBotFreerideSpawner` содержит `SpawnEntries`, `playerArchetype` и `maxPlayerCount=8`. `playerArchetype` указывает на `CITEntityAIPlayerBotFreeride`, у которого есть Binding и HudBehaviorPlayer, а также `VehicleArchetypeToSpawn`.

Это подтверждает, что Freedrive имеет собственный механизм появления AI player entities и их транспорта. Но это не pedestrian spawner и не доказательство, что тот же spawner обслуживает локального игрока.

## 7. Как это стыкуется с native lifecycle

Ранее захваченные функции:

- `0x204800` — coordinator/update-процедура, которая читает manager global и координирует vehicle replica lifecycle;
- `0x1FA530` — создание/обслуживание replica vehicle в avatar-mode;
- `0x1FA430` — cleanup/state transition в avatar-mode;
- `0x841100` и `0x841780` — cleanup связанного vehicle reference.

В объекте manager candidate есть selector и таблица указателей, а в контексте lifecycle используются поля `+0x568` и `+0x570`. Их точная семантика пока не доказана.

Сопоставление с archetype-данными даёт важную рабочую модель:

- Avatar archetype описывает персонажа.
- Player archetype описывает игровую player-сущность.
- AvatarDrivingConfig описывает avatar, используемый как водитель транспорта.
- Native lifecycle manager обслуживает состояние транспорта/Avatar Mode.
- Отдельный механизм должен связать локального player entity с пешим avatar и переключить input/camera.

Последний пункт пока остаётся недостающим звеном. Нельзя объявлять его найденным только потому, что в данных существуют Avatar, HumanPlayer и менеджер.

## 8. Что исследовать дальше

Приоритеты теперь такие:

1. **Найти native consumers `CITEntityComponentBinding`, `CITEntityComponentAvatarPuzzle`, `CITEntityComponentAnimCtrl`** и понять, какие из них участвуют в runtime player/avatar activation, а какие только в рендере/анимации.
2. **Разобрать источники `GetLocalPlayer` и `GetCurrentPlayer`** на уровне native реализации. Особенно важно выяснить, возвращают ли они разные сущности во время avatar-mode и как их значения связаны с manager candidate.
3. **Связать AvatarDrivingConfig с lifecycle manager**: проверить, откуда native код берёт DriverAvatarArchetype и используется ли он только для seated driver.
4. **Найти место создания/регистрации AvatarInterior/AvatarTC2** в HQ/Housing flow и сравнить с Freedrive. Spawn marker, entity factory, player binding и input handoff нужно искать как отдельные этапы.
5. Только после нахождения цепочки создания и назначения управления проектировать собственный переход Vehicle → Pedestrian → Vehicle.

## Уровень достоверности

**Подтверждено данными:**
- Avatar и Player — отдельные классы с разными компонентами.
- Binding общий для avatar и player, но используется очень широко.
- AvatarDrivingConfig ссылается на AvatarTC2 и отдельный AnimCtrl.
- WorldSettings задаёт archetypes для AvatarTC2 и AvatarInterior.
- HQ/Housing используют отдельные spawn point archetypes.
- Freedrive AI spawner создаёт AIPlayerBotFreeride, а не pedestrian.

**Пока гипотезы:**
- Runtime flow создаёт/регистрирует avatar отдельно от player entity.
- Manager candidate участвует в handoff между vehicle и avatar.
- HQ/Housing flow может содержать повторно используемую часть общего avatar activation pipeline.

**Не найдено:**
- универсальный pedestrian spawner для Freedrive;
- явный native setter текущего управляемого avatar;
- доказанная связь `GetLocalPlayer`/`GetCurrentPlayer` с manager candidate;
- полный input/camera handoff.
