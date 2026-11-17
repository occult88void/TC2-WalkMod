# ANALYSIS 13: LocalPlayerID и RemotePlayerID — варианты HumanPlayer, не Avatar

Дата: 2026-11-17  
Источник: `TC2_ENTITY_INDEX.sqlite`, файл A (`archetypes.entities.bin`), таблицы `entities`, `fields`, `entity_components`.

## 1. Найдена явная конфигурация LocalPlayer / RemotePlayer

У дочерней записи `CITEntityInterProjectSettings`:

- Entity ID: `978C040000000008`
- offset: `0x1013FD8`
- FatherArchetypeID: `968C040000000008`

два поля содержат конкретные ID archetype:

| Поле | Значение ID | Разрешённый класс |
|---|---|---|
| `LocalPlayerID` | `144F840700000000` | `CITEntityHumanPlayer` |
| `RemotePlayerID` | `F40F190200000000` | `CITEntityHumanPlayer` |

Это не просто совпадение имени поля: оба ID точно разрешаются в реальные записи `CITEntityHumanPlayer` в A.

## 2. Цепочки наследования

### LocalPlayer

`144F840700000000`  
- запись `CITEntityHumanPlayer` в `0x1F97B93`;
- FatherArchetypeID = `60CD760100000000`;
- промежуточная запись `CITEntityHumanPlayer` в `0x1FC1B26`;
- её FatherArchetypeID = базовый `DA93760100000000`.

### RemotePlayer

`F40F190200000000`  
- запись `CITEntityHumanPlayer` в `0x1F97B6E`;
- FatherArchetypeID = `61CD760100000000`;
- промежуточная запись `CITEntityHumanPlayer` в `0x1FC1B65`;
- её FatherArchetypeID = тот же базовый `DA93760100000000`.

Обе цепочки заканчиваются на одном explicit base archetype `CITEntityHumanPlayer`.

## 3. В чём реально различаются Local и Remote

У промежуточных archetype есть компонент `CITEntityComponentHudBehaviorPlayer`, но его HUD-настройки отличаются:

| Поле | Local intermediate `60CD...` | Remote intermediate `61CD...` |
|---|---|---|
| `MinimapBehaviorArchetype` | `3947F10300000000` | `3A47F10300000000` |
| `ProjectedAssetBehaviorArchetype` | отсутствует | `BA1E010000000008` |
| `Emote2DProjectedAssetBehaviorArchetype` | `1C1C040000000008` | отсутствует |

У обоих дочерних archetypes поле `SpawnPolicy = 0`. Они наследуют базовый `CITEntityHumanPlayer`, у которого есть `Binding`, `HudBehaviorPlayer` и `AudioObject`.

Различия, обнаруженные в этой цепочке, находятся в конфигурации HUD/ProjectedAsset. Никакого прямого поля на `CITEntityAvatarTC2`, `CITEntityAvatarInterior` или Avatar ID в этих цепочках нет.

## 4. Что это даёт для Player → Avatar

Подтверждено:
- конфигурация игры отдельно задаёт archetype локального и удалённого `HumanPlayer`;
- оба являются вариантами одного базового `CITEntityHumanPlayer`;
- различие между промежуточными вариантами в изученных компонентах относится к HUD/ProjectedAsset.

Не подтверждено:
- что runtime-результат `CMissionAtomGetLocalPlayer` всегда является экземпляром именно `144F8407...`;
- что `CMissionAtomGetCurrentPlayer` возвращает другой объект;
- что любой из этих archetype хранит ссылку на пеший Avatar;
- что выбор LocalPlayer archetype выполняет передачу управления.

Важно не превращать имена `LocalPlayerID` и `RemotePlayerID` в доказательство того, что найден runtime control target. Это конфигурационные archetype IDs, а не наблюдение живых сущностей.

## 5. Следующий поиск

1. Проверить, где native-код читает `CITEntityInterProjectSettings.LocalPlayerID` и `RemotePlayerID`.
2. Проследить, как выбранный archetype превращается в runtime entity handle.
3. Сопоставить resolver `GetLocalPlayer` с этим конфигурационным путём, не предполагая их эквивалентность.
4. Продолжить отдельно искать current controlled entity setter и Avatar controller activation.

## Вывод

Найден конкретный источник конфигурации локального Player: `CITEntityInterProjectSettings.LocalPlayerID` указывает на вариант `CITEntityHumanPlayer`. Это сужает область поиска Player archetype, но не раскрывает Player→Avatar handoff. По имеющимся полям Local/Remote различаются HUD-настройками, а не пешим Avatar.
