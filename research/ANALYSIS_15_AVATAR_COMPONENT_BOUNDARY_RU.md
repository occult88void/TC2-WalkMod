# ANALYSIS 15: фактический состав AvatarTC2 и отсутствие serialized movement component

Дата: 2026-11-17  
Источник: TC2_ENTITY_INDEX.sqlite, файл A, entity_components, objects, fields.

## 1. Состав базовых Player и Avatar archetypes

Компоненты explicit base archetypes:

| Archetype | Компоненты |
|---|---|
| CITEntityAvatarTC2 | CITEntityComponentBinding, CITEntityComponentAvatarPuzzle, CITEntityComponentAnimCtrl |
| CITEntityAvatarInterior | те же три + CITEntityComponentHudBehaviorAvatar |
| CITEntityHumanPlayer | CITEntityComponentBinding, CITEntityComponentHudBehaviorPlayer, CITEntityComponentAudioObject |
| CITEntityGhostPlayer | CITEntityComponentBinding, CITEntityComponentHudBehaviorPlayer |

Это подтверждает, что Avatar и HumanPlayer — разные serialized archetypes с разными наборами компонентов. Общим остаётся Binding, роль которого уточнена в ANALYSIS 12 как parent/attachment.

## 2. Что содержат Avatar-компоненты

### CITEntityComponentAvatarPuzzle

В explicit AvatarTC2 component:
- поле BSPhereRadius;
- дочерний DefaultHead;
- дальнейшая структура AvatarPuzzle содержит DefaultUpperBody, DefaultLowerBody, DefaultHands, DefaultFeets, DefaultHelmet, DefaultSuit и другие элементы.

Это конфигурация частей/внешнего вида персонажа, не найденный control manager.

### CITEntityComponentAnimCtrl

В explicit AvatarTC2:
- поле AnimCtrlID = null в базовой записи.

У дочерних Avatar settings ранее найдены конкретные animation IDs. Значит, базовое null не доказывает отсутствие анимации в конечном наследуемом archetype. Но AnimCtrl сам по себе — конфигурация анимации, не доказательство назначения ввода.

### CITEntityComponentHudBehaviorAvatar

Есть у AvatarInterior, но отсутствует в explicit base AvatarTC2. Его поля задают TagBehavior, MinimapBehavior, ProjectedAssetBehavior и Emote2DProjectedAssetBehavior. Это HUD/projected assets, не движение.

## 3. Что отсутствует в этих serialized component lists

В явных базовых списках AvatarTC2/AvatarInterior не обнаружены отдельные компоненты с очевидными именами:
- PlayerController;
- InputController;
- CharacterMovement;
- AvatarPhysicsController;
- CameraController.

Это только отсутствие таких компонентов в этих archetype records. Оно не доказывает отсутствие movement/control кода: он может быть реализован в native классе сущности, в глобальном manager/component host или подключаться динамически.

Отдельно важно: DefaultAvatarPhysicDesc и AvatarCameraSettings находятся в CITEntityAvatarSettings → PlayerCharacterSetting, то есть это внешняя конфигурация. В явном наборе компонентов AvatarTC2 нет компонента с этими именами.

## 4. Что это означает для архитектуры мода

Сейчас конфигурационные данные показывают три слоя:
1. Avatar archetype: Binding + AvatarPuzzle + AnimCtrl.
2. Avatar settings: параметры физики и камеры.
3. Runtime native path, который должен создать/зарегистрировать Avatar, связать его с нужными настройками и передать ему input/camera ownership.

Первые два слоя подтверждены. Третий пока не найден.

Не следует пытаться компенсировать отсутствие serialized movement component добавлением похожего NPC или HUD-компонента: в данных нет доказательства, что это создаст управляемого локального игрока.


## 5. Проверка всех наследуемых AvatarTC2 archetypes

Дополнительно агрегированы component records по всем 612 entity records класса CITEntityAvatarTC2, а не только по explicit base:

| Компонент | Количество записей |
|---|---:|
| CITEntityComponentAvatarPuzzle | 511 |
| CITEntityComponentAnimCtrl | 254 |
| CITEntityComponentBinding | 128 |

Других типов компонентов в этих 612 записях индекс не показывает. Следовательно, в полном наборе сериализованных AvatarTC2 archetypes не обнаружен отдельный movement/input/controller component.

Ограничение: счётчики относятся к записям компонентов в archetype data. Они не учитывают native fields базового C++ класса, глобальные manager components и компоненты, которые могут добавляться динамически во время исполнения.

## Вывод

Базовый AvatarTC2 не несёт очевидного serialized PlayerController/InputController компонента. Его видимые компоненты отвечают за привязку, внешний вид и анимацию; физика и камера вынесены в Avatar settings. Это делает native entity/controller path центральной недостающей частью расследования.
