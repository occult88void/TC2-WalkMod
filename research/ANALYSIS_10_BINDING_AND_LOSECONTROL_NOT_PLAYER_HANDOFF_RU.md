# Анализ 10: Binding и LoseControl не подтверждают передачу управления Player → Avatar

## Цель

Проверить два набора символов, которые по названиям могли выглядеть как часть механизма смены управляемой сущности:

- `CMissionAtomBinding`
- `CMissionAtomLoseControlSet` / `CMissionAtomWaitLoseControl`

Проверка выполнена по локальному `TC2_ENTITY_INDEX.sqlite`, собранному из сериализованных entity/mission данных. Это анализ конфигурационных данных, а не дизассемблирование native-реализаций.

## 1. CMissionAtomBinding

В таблице `symbol` присутствует `CMissionAtomBinding` (hash `0x65107189`).

Найдены 5 mission-node экземпляров. Их поля:

- `VarEntIDBind`
- `VarEntIDToBind`
- `VarBoneIndex`

Во всех пяти графах обе сущности поступают из `CMissionAtomEntityToList`, а индекс кости — из `CMissionAtomVariableNumber`.

Это указывает на операцию привязки одной сущности к другой с указанием кости. Но конфигурационные данные не показывают, что операция регистрирует локального игрока, назначает текущую управляемую сущность или переключает ввод/камеру.

**Вывод:** `CMissionAtomBinding` не является доказанным Player → Avatar bridge. По доступным слотам он выглядит как entity-to-entity/bone binding.

## 2. CMissionAtomLoseControlSet

Символ `CMissionAtomLoseControlSet` присутствует в symbol table (hash `0x51CF518D`), однако в текущем индексе:

- нет mission-node экземпляров этого типа;
- нет typed-object экземпляров этого типа;
- нет сериализованных полей, раскрывающих его назначение.

Отсутствие сериализованного экземпляра не доказывает отсутствие native-класса или его runtime-вызовов. Оно лишь не даёт использовать этот символ как подтверждение механизма переключения Player/Avatar.

## 3. CMissionAtomWaitLoseControl

Для `CMissionAtomWaitLoseControl` обнаружено 9 typed-object записей и 10 mission-node появлений.

В проверенных конфигурациях узел представлен как flow/wait-узел. В его собственных полях нет явного входа с ID Player/Avatar или архетипом сущности.

Следовательно, имя может описывать ожидание завершения состояния LoseControl, но не устанавливает, какая сущность теряет управление и как назначается новая.

## 4. CITEntityLoseControlSettings

В данных найден один typed-object `CITEntityLoseControlSettings` (файл server data, offset `0x3CFCD9`).

Поля включают:

- `Enablelosecontrol = 1`
- `Enablecrashcamera = 1`
- `AlwaysUseFade = 0`
- `PositionMode = 0`
- `UseDynamicBackOnTrackInfos = 0`

Рядом расположен `CITLibraryEntity_LoseControlConfig`, у которого есть `DefaultSettingsID), ссылающийся на настройки LoseControl.

Это конфигурационный контекст потери управления/камеры, связанный с игровым транспортным поведением. Он не содержит подтверждённой ссылки на `CITEntityAvatarTC2`, `CITEntityHumanPlayer` или `CITEntityGhostPlayer`.

**Вывод:** этот LoseControl-контекст нельзя приравнивать к передаче управления пешему аватару.

## 5. Что это меняет в основной гипотезе

Пока не найдено подтверждение, что:

1. локальная Player-сущность создаёт или получает ссылку на runtime Avatar;
2. Avatar регистрируется в общем entity registry как кандидат на управление;
3. отдельный менеджер назначает Avatar текущей управляемой сущностью;
4. input и camera переключаются на Avatar;
5. обратный переход возвращает управление исходному транспортному объекту.

`CMissionAtomBinding`, `CMissionAtomWaitLoseControl` и `CITEntityLoseControlSettings` не закрывают ни один из этих пунктов.

## 6. Следующий статический поиск

Продолжать нужно по конкретным native-зависимостям:

1. найти обработчики/реализации `CMissionAtomGetLocalPlayer` и `CMissionAtomGetCurrentPlayer`;
2. выяснить, какой runtime resolver стоит за их выходами;
3. найти записи в указатель/handle текущей управляемой сущности;
4. отдельно проследить entity factory/registry и компонентные запросы;
5. только после появления доказанной связи сопоставлять Player и Avatar.

NPC, HQ-персонажи и `CITEntityAvatarNPC` остаются отдельными ветками анализа. Совпадение компонентов или archetype inheritance само по себе не считается доказательством общего control pipeline.

## Ограничения

- Native-реализации этих классов в текущем индексе не представлены.
- Отсутствие конфигурационных экземпляров не означает отсутствия runtime-кода.
- Никаких runtime hooks, DLL-инъекций или изменений игрового процесса в этом исследовании не выполнялось.
