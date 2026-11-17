# ANALYSIS 08B: границы доказательств между AvatarNPC, HQ Avatar и локальным Player

Дата: 2026-11-17  
Источник: `TC2_ENTITY_INDEX.sqlite`, представления `entities`, `entity_links`, `class_components`.

## Цель

Не переносить выводы о локальном управляемом персонаже с NPC-пешеходов, HQ/Housing персонажей или просто похожих archetypes. Этот документ фиксирует, что именно связывают доступные данные, а что остаётся неизвестным.

## 1. Классы действительно разделены

В основном archetype-файле (file=1) индекс показывает:

| Класс | Явные определения | Наследуемые определения |
|---|---:|---:|
| `CITEntityAvatarTC2` | 1 | 611 |
| `CITEntityAvatarInterior` | 1 | 2 |
| `CITEntityAvatarNPC` | 1 | 24 |
| `CITEntityHumanPlayer` | 1 | 4 |
| `CITEntityGhostPlayer` | 1 | 0 |

Это не взаимозаменяемые имена. У классов разные линии наследования и разные компонентные наборы.

Компоненты, агрегированные по сущностям класса:

| Класс | Компонент | Число сущностей с компонентом |
|---|---|---:|
| AvatarTC2 | AvatarPuzzle | 511 |
| AvatarTC2 | AnimCtrl | 254 |
| AvatarTC2 | Binding | 128 |
| AvatarInterior | AvatarPuzzle | 3 |
| AvatarInterior | AnimCtrl | 1 |
| AvatarInterior | Binding | 1 |
| AvatarInterior | HudBehaviorAvatar | 1 |
| AvatarNPC | AvatarPuzzle | 15 |
| AvatarNPC | AnimCtrl | 21 |
| AvatarNPC | Binding | 1 |
| HumanPlayer | HudBehaviorPlayer | 3 |
| HumanPlayer | Binding | 1 |
| HumanPlayer | AudioObject | 1 |
| GhostPlayer | HudBehaviorPlayer | 1 |
| GhostPlayer | Binding | 1 |

Количество означает число archetype-сущностей в индексе, у которых найден компонент, а не число runtime экземпляров.

**Ограничение:** общие компоненты показывают сходство структуры, но не доказывают общую runtime-роль. В частности, наличие AnimCtrl у AvatarNPC не означает, что NPC и локальный Avatar используют один control pipeline.

## 2. Прямые ссылки Player → Avatar в archetype-данных не обнаружены

У `CITEntityHumanPlayer` в entity_links кроме наследования обнаружены ссылки на:
- `Emote2DProjectedAssetBehaviorArchetype`
- `MinimapBehaviorArchetype`
- `ProjectedAssetBehaviorArchetype`

У `CITEntityGhostPlayer`:
- `MinimapBehaviorArchetype`
- `ProjectedAssetBehaviorArchetype`

В проиндексированных ссылках этих классов не обнаружена явная ссылка на `CITEntityAvatarTC2`, `CITEntityAvatarInterior` или `CITEntityAvatarNPC`.

Это ограниченный отрицательный результат: он относится к сериализованным ссылкам archetype, которые индексатор смог распознать. Runtime-связь может устанавливаться кодом, через Binding, mission graph, фабрику или динамическую регистрацию. Поэтому нельзя делать вывод, что связи Player и Avatar вообще не существует.

## 3. AvatarNPC имеет собственные типовые зависимости

У `CITEntityAvatarNPC` видны ссылки на:
- `AnimCtrlID` → `CITEntityAnimCtrl`
- `PartIDList` → `CITEntityAvatarPart` и `CITEntityAvatarRootPart`
- `CameraMovie` → `CITEntityMoviePlayer`
- `NewIcon` → `CITEntityTagSpot`

Это хорошо объясняет конфигурацию NPC-аватара как визуального/анимационного персонажа, связанного с частями модели и некоторыми presentation-объектами. Но в этих данных не обнаружена прямая ссылка NPC на HumanPlayer/GhostPlayer или на control manager.

**Вывод:** AvatarNPC нельзя использовать как доказательство того, что локальный player-avatar создаётся или управляется тем же способом.

## 4. AvatarInterior — отдельный HQ/Housing-кандидат, но не доказанный runtime player

У `CITEntityAvatarInterior` найдены:
- `PartIDList` → `CITEntityAvatarRootPart`
- `ProjectedAssetBehaviorArchetype` → HUD projected asset behavior
- компоненты AvatarPuzzle, AnimCtrl, Binding и HudBehaviorAvatar.

WorldSettings содержит ссылку на `AvatarInteriorArchetype`, а Housing settings содержит `HouseAvatarArchetype`. Это подтверждает, что Interior avatar archetype используется в конфигурации мира/жилья.

Но само по себе это не показывает:
- кто создаёт его runtime экземпляр;
- получает ли он input;
- становится ли он CurrentPlayer;
- связан ли он с HumanPlayer;
- используется ли тот же pipeline при выходе из HQ Rally/GP.

Поэтому корректная формулировка: **AvatarInterior — archetype, явно задействованный в настройках интерьера/жилья; его роль как управляемого локального персонажа требует отдельного подтверждения.**

## 5. HQ/Housing spawn points не содержат явной ссылки на Player или Avatar

Для классов:
- `CITEntityHeadQuarterCharacterSpawnPoint`
- `CITEntityHeadQuarterExitPoint`
- `CITEntityHousingCharacterSpawnPoint`
- `CITEntityHousingExitSpawnPoint`

В entity_links обнаружены только ссылки наследования `FatherArchetypeID`. В распознанных сериализованных ссылках самих spawn-point классов нет прямого target на Player или Avatar archetype.

Это не означает, что spawn point не запускает создание персонажа. Он может быть найден по типу/тегу, а дальнейшее действие выполняется кодом или mission graph. Но данные не дают права называть spawn point прямой ссылкой на управляемого avatar.

## 6. Что остаётся валидным по общим механизмам

Некоторые подсистемы действительно пересекают несколько классов:
- Binding присутствует у AvatarTC2, AvatarInterior, AvatarNPC, HumanPlayer и GhostPlayer.
- AnimCtrl присутствует у AvatarTC2, AvatarInterior и AvatarNPC.
- AvatarPuzzle присутствует у трёх avatar-классов.
- Player HUD behavior находится на HumanPlayer/GhostPlayer, а Avatar HUD behavior найден на AvatarInterior.

Эти пересечения годятся как направления поиска native consumers. Но до нахождения конкретного consumer callsite они не дают права утверждать, что это одна система создания или передачи управления.

## 7. Строгая шкала доказательств для следующих исследований

Каждую будущую корреляцию помечать одним из уровней:

- **E0 — сходство:** одинаковые имена, компоненты, анимации или похожие archetypes.
- **E1 — статическая ссылка:** сериализованное поле одного archetype указывает на другой класс/ID.
- **E2 — графовая связь:** mission graph передаёт сущность/ID от одного узла к другому.
- **E3 — native call/dataflow:** код вызывает конкретную функцию или передаёт объект/ID между подсистемами.
- **E4 — runtime подтверждение:** штатный сценарий подтверждает, что конкретная сущность получает управление или участвует в handoff.

Нельзя повышать уровень доказательства по косвенным признакам. Например, Binding у NPC и Player — E0, не E1. Ссылка WorldSettings на AvatarInterior — E1 для конфигурации archetype, но не E4 для управления игроком.

## Итог

В текущем индексе нет прямого доказательства, что AvatarNPC, AvatarInterior и AvatarTC2 являются одним runtime-пайплайном или что любой из них напрямую связан с HumanPlayer/GhostPlayer.

Подтверждено лишь то, что:
- это отдельные archetype-классы;
- у них есть частично общие компоненты;
- AvatarInterior указан в world/housing settings;
- HQ/Housing spawn-point классы существуют отдельно;
- статические ссылки Player → Avatar в archetype-записях не обнаружены.

Следующий шаг — искать конкретные native consumers и mission graph dataflow, которые соединяют Player entity с Avatar instance. Пока такой мост не найден, NPC и HQ avatar остаются отдельными исследовательскими ветками, а не доказательством механики локального пешехода.
