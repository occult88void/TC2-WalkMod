# ANALYSIS 08C: проверка SetHUDBehavior — это Minimap, не доказательство avatar handoff

Дата: 2026-11-17  
Источник: `TC2_ENTITY_INDEX.sqlite`, таблицы `mission_node`, `mission_use`, `fields`, `payload`, `entities`.

## Почему проверяли

В предыдущем отчёте `GetLocalPlayer → SetHUDBehavior` был выделен как потенциально важная связь. Но имя SetHUDBehavior слишком общее: нужно проверить конкретное поле archetype, которое получает атом, иначе легко приписать ему передачу управления.

## Что показал граф

В mission graph с root `0x618505`:
- `CMissionAtomGetLocalPlayer` ID 7236;
- `CMissionAtomSetHUDBehavior` ID 7238;
- поле `VarEntID` SetHUDBehavior получает выход GetLocalPlayer;
- поле `MinimapArchetypeID` получает `CMissionAtomVariableArchetype` ID 7237;
- Entity этой переменной = `01EEF20300000000`;
- ID разрешается в `CITEntityHudBehaviorMinimap`.

Таким образом, этот конкретный узел назначает LocalPlayer minimap HUD behavior. Это не назначение Avatar, не player controller и не input handoff.

## Масштаб совпадения

По агрегированным bindings:
- 31 graph occurrence связывает `GetLocalPlayer` с `SetHUDBehavior` через `VarEntID` (это число из предыдущего анализа).
- Более широкий просмотр SetHUDBehavior/MinimapArchetypeID обнаружил 113 graph occurrences с variable-archetype входом; 102 разрешаются в `CITEntityHudBehaviorMinimap`, 11 имеют null/неразрешённую сущность.
- Среди проверенных variable archetypes присутствуют ID, которые разрешаются в `CITEntityHudBehaviorMinimap`.

Эти числа относятся к graph occurrences, а не к уникальным runtime сущностям. Один физический mission node может использоваться несколькими graph roots.

## Коррекция интерпретации

Предыдущую формулировку «SetHUDBehavior — интересный мост между LocalPlayer и avatar/player setup» следует сузить:

- **Подтверждено:** LocalPlayer используется как target для назначения HUD behavior; в проверенном примере назначаемый archetype — Minimap HUD.
- **Не подтверждено:** что этот SetHUDBehavior назначает pedestrian behavior, avatar controller или передаёт управление.
- **Следствие:** SetHUDBehavior не является приоритетной уликой для walking control chain, пока не найден другой конкретный тип behavior или отдельный native consumer.

Это полезный отрицательный результат: он убирает ложный путь из основного поиска.

## Следующий фокус

Для поиска реального control handoff важнее:
1. найти native implementation `GetLocalPlayer` и `GetCurrentPlayer`;
2. выяснить, кто обновляет их значения и при каких состояниях;
3. искать отдельные операции, связанные с possession/control, input context, camera target или player entity binding;
4. не приравнивать HUD behavior к управлению персонажем.

