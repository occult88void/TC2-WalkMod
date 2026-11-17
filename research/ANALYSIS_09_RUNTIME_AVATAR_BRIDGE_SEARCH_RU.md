# ANALYSIS 09: поиск runtime-моста Player → Avatar

Дата: 2026-11-17  
Источник: `TC2_ENTITY_INDEX.sqlite` из handoff-пакета; таблицы `entity_component`, `entity_reference`, `entities`, `fields`, `mission_node`, `mission_bindings`, `symbol`.

## Цель

Найти конкретный механизм, который связывает локальную Player-сущность с runtime Avatar instance: фабрику/создатель, регистрацию, передачу идентификатора, назначение текущей управляемой сущности. NPC, AvatarNPC и HQ/Housing персонажи не используются как доказательство этого механизма.

## 1. Повторно используемый Binding не является доказанным мостом

У явных archetype-записей `CITEntityHumanPlayer`, `CITEntityGhostPlayer`, `CITEntityAvatarTC2`, `CITEntityAvatarInterior` и `CITEntityAvatarNPC` найден компонент `CITEntityComponentBinding`.

В записи компонентов этих базовых archetype все пять указывают на один сериализованный объект компонента по смещению `0x0A6C4C`. В нём поле `BindingType = 0`.

Это означает, что общий Binding в этих записях может быть результатом повторного использования одного archetype-компонента/шаблона. Он не содержит индивидуального Player ID, Avatar ID или явной ссылки между ними. Поэтому само наличие Binding у двух классов остаётся уровнем E0 (структурное сходство), а не доказательством регистрации или handoff.

Аналогично, `CITEntityAvatarNPC` повторно использует компонентные записи базового `CITEntityAvatarTC2` для AvatarPuzzle и AnimCtrl. Это наследование/переиспользование archetype-данных, а не подтверждение общего runtime control pipeline.

## 2. Прямые ссылки в базовых записях не дают Player → Avatar

Проверены распознанные `entity_reference` для явных базовых записей:

| Archetype | Найденные ссылки |
|---|---|
| `CITEntityHumanPlayer` | Прямых ссылок нет |
| `CITEntityGhostPlayer` | MinimapBehavior и ProjectedAssetBehavior |
| `CITEntityAvatarTC2` | Прямых ссылок нет |
| `CITEntityAvatarInterior` | ProjectedAssetBehavior |
| `CITEntityAvatarNPC` | Прямых ссылок нет |

Это отрицательный результат только для сериализованных ссылок, распознанных индексатором. Он не исключает динамическую регистрацию или связь, создаваемую native-кодом.

## 3. Что реально делают потребители GetLocalPlayer и GetCurrentPlayer в mission graph

Повторно агрегированы исходящие data-slot bindings. Числа ниже — подключения mission graph, а не количество уникальных native-функций или runtime-вызовов.

### GetLocalPlayer

| Получатель | Поле | Подключений |
|---|---|---:|
| `CMissionAtomPlaySound` | `VarLocationEntID` | 118 |
| `CMissionAtomIsSameEntity` | `VarEntityID2` | 55 |
| `CMissionAtomEntityToList` | `VarEntListID` | 36 |
| `CMissionAtomSetHUDBehavior` | `VarEntID` | 31 |
| `CMissionAtomIsSameEntity` | `VarEntityID1` | 15 |
| `CMissionAtomPostAudioEvent` | `VarLocationEntID` | 15 |
| `CMissionAtomDetectCollisionPlayerVsPlayer` | `VarPlayerEntityID` | 10 |
| `CMissionAtomTeleportEntity` | `VarEntID` | 4 |
| `CMissionAtomBlockCar` | `VarEntID` | 2 |

Связь с `SetHUDBehavior` в ранее проверенном графе разрешается в `CITEntityHudBehaviorMinimap`. Это HUD-настройка, не управление аватаром.

### GetCurrentPlayer

| Получатель | Поле | Подключений |
|---|---|---:|
| `CMissionAtomIsSameEntity` | `VarEntityID1` | 72 |
| `CMissionAtomIsBot` | `VarEntityID` | 38 |
| `CMissionAtomBlockCar` | `VarEntID` | 34 |
| `CMissionAtomEntityToList` | `VarEntListID` | 28 |
| `CMissionAtomIsSameEntity` | `VarEntityID2` | 21 |
| `CMissionAtomIsEntityInList` | `VarEntityID` | 3 |
| `CMissionAtomTeleportEntity` | `VarEntID` | 2 |
| `CMissionAtomTeleportEntity` | `VarTeleportPositionID` | 2 |

Эти потребители показывают, что две mission-переменные используются в разных контекстах. Но они не раскрывают, какую runtime-сущность возвращает каждая из них. В частности, `IsBot` и `BlockCar` не являются доказательством, что `CurrentPlayer` — пеший аватар или что он обозначает активный input target.

В двух графах `TeleportEntity` один и тот же `GetCurrentPlayer` подключён одновременно к `VarEntID` и `VarTeleportPositionID`. Без реализации mission atom это нельзя трактовать как переключение управления; это только зафиксированная структура слотов.

## 4. Другие кандидаты не дали подтверждённого handoff

- `CMissionAtomWaitLoseControl` остаётся узлом ожидания/потока. В его найденных полях нет явного Player ID, Avatar ID или архетипа создаваемой сущности.
- `CMissionAtomSyncPlayers` и `CMissionAtomMissionPlayerType` существуют в mission graph, но само название и наличие узла не устанавливают связь Player с Avatar.
- `SpawnAvatarDesc` и `CITEntityPlayerSave` присутствуют в таблице символов, но поиск по текущему индексу не дал соответствующих физических объектов/полей, которые можно было бы связать с фабрикой или runtime-регистрацией.
- Единственный физический `CMissionAtomSpawn`, изученный ранее, относится к narrative-video spawn. Он не является доказательством создания управляемого AvatarTC2.

## 5. Уровни доказательств

| Утверждение | Уровень | Статус |
|---|---|---|
| Player и Avatar имеют общий Binding component template | E0 | Подтверждено структурой, не является мостом |
| HumanPlayer прямо ссылается на AvatarTC2 в своей archetype-записи | E1 | Не найдено |
| Mission graph передаёт результат GetLocalPlayer в SetHUDBehavior | E2 | Подтверждено, но это Minimap HUD |
| GetCurrentPlayer является активным управляемым Avatar | E3/E4 | Не установлено |
| Native-фабрика создаёт Avatar и регистрирует его у LocalPlayer | E3 | Не найдена |
| Смена CurrentPlayer выполняет handoff транспорта и Avatar | E3/E4 | Не установлено |

## 6. Следующий конкретный поиск

Следующий этап должен быть направлен не на новые NPC/HQ archetypes, а на native-путь:

1. Найти runtime-реализации mission atom `GetLocalPlayer` и `GetCurrentPlayer` либо их общую функцию разрешения Player reference.
2. Найти код, который читает/записывает сущность, возвращаемую этими операциями, и проверить, есть ли setter для current/controlled entity.
3. Проверить, где native-код читает `CITEntityComponentBinding` и что именно означает `BindingType`; не предполагать, что это Player registration.
4. Найти общую фабрику/registry API, вызываемую для Player и Avatar, и только после этого сопоставлять их runtime handles.
5. Отдельно восстановить vehicle lifecycle; найденные ранее функции Avatar Mode относятся к реплике/очистке транспорта и пока не показывают создание пешехода.

Ограничение источника: текущий индекс archetype/mission data не содержит runtime-адресов native-реализаций. На диске у TheCrew2.exe отсутствует обычный raw `.text`; прежние runtime hook-тесты вызвали integrity warning, поэтому новые in-process hooks и патчи не являются следующим шагом.

## Итог

Пока не найдено доказанного моста `Local Player entity → runtime Avatar instance`. Общий Binding и общие компоненты AvatarNPC/AvatarTC2 объясняются повторным использованием archetype-записей и не должны использоваться как аргумент в пользу общего управления. Следующая задача — получить и исследовать native-реализации player-reference resolution, entity registry/factory и current-control assignment без новых рискованных вмешательств в процесс игры.
