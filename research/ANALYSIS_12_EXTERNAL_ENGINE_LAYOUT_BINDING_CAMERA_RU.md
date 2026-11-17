# ANALYSIS 12: независимая карта engine objects, Binding и camera owner

Дата: 2026-11-17  
Источники:
- публичный reverse-engineering проект `PhialsBasement/tc2vr`, `docs/ENGINE.md`, `src/game.hpp`, `src/game.cpp`;
- локальный `TheCrew2.exe`, timestamp `2025-09-22 22:37:44 UTC`;
- предыдущий runtime capture v09 и отчёты ветки `analysis-07-player-control-chain`.

Внешний проект описывает Steam-сборку сентября 2026 года. Локальный EXE проекта имеет timestamp сентября 2025 года. Поэтому его адреса RVA не переносятся на нашу сборку без отдельной проверки. Ниже используются только структурные совпадения как ориентиры, не как готовые адреса.

## 1. Новое подтверждение: Binding — компонент родительской привязки

В `tc2vr/docs/ENGINE.md` `CITEntityComponentBinding` описан как компонент parenting/attachment:

- `BindingType` включает `EntityLogical`, `EntityPhysical`, `CarPuzzleLink`, `EntityPhysicalLockAngle`;
- `+0x38` — `ParentEntityID` (`CITEntityHandle`);
- `+0x98` — `BoneName`;
- `+0x9c` — `PhysicsLinkType`;
- `+0xa0` — `CarPuzzleLinkType`;
- `+0xa8` — `Locator`;
- `+0xc0` — `LocalTransform`;
- также описаны флаги наследования transform/animation и ограничения parent.

Это существенно уточняет старую гипотезу. Общий `CITEntityComponentBinding` у `CITEntityHumanPlayer`, `CITEntityGhostPlayer` и `CITEntityAvatarTC2` не является сам по себе Player→Avatar мостом. Его документированная роль — привязка сущности к родителю, к кости/locator и локальному transform. Наличие одного и того же компонента у несвязанных классов естественно для общего механизма parenting.

В нашем индексе `BindingType=0` встречается у 641 компонентной записи в большом наборе несвязанных классов: транспорт, Avatar, Player, HUD, геометрия, точки спавна и другие. Это дополнительно исключает трактовку значения 0 как уникального признака локального игрока.

**Вывод:** Binding больше не рассматриваем как кандидат на регистрацию игрока или назначение управляемой сущности, пока не появятся отдельные данные о конкретном `ParentEntityID` runtime-экземпляра.

## 2. Компонентная модель движка и возможная переоценка v09 manager candidate

В независимых заметках указана следующая структура:

- Entity хранит массив компонентных указателей в `+0x288`, число компонентов в `+0x290`;
- компонент имеет vtable в `+0x00` и back-pointer на владельца в `+0x18`;
- `GetComponent<T>` обходит этот массив и выполняет RTTI-проверку;
- component factory зарегистрирована через hash map по адресу VA `0x1404EB7C0` (RVA `0x4EB7C0` в описанной сборке), ключ — `typeId ^ 0xDEADBEEF`.

Важно: это **фабрика компонентов**, не доказанная фабрика Entity. Её нельзя автоматически считать механизмом создания `CITEntityAvatarTC2`.

Там же глобальный объект `g_Game` описан как component host:

- глобальный pointer slot в той сборке: RVA `0x3480BB8`;
- массив компонентов: `g_Game + 0x1DD8`;
- индексная таблица: `g_Game + 0x1DE8`;
- известные component slots включают `0x1C` для camera owner и `0x1A` для render-target manager.

В нашем v09 capture объект, ранее названный manager candidate, имел поля по тем же смещениям `+0x1DD8` и `+0x1DE8`, а также компонентный объект по `+0x1E68`. Это сильное структурное совпадение с описанным component host, но пока не доказательство, что это именно `g_Game`: сборки разные, глобальные RVA различаются, а v09 снимок не содержит установленного RTTI-имени корневого объекта.

**Коррекция терминологии:** до установления vtable/type identity этот объект следует называть `component-host candidate`, а не Player Manager. Вполне возможно, что ранее мы смотрели на общий игровой component host.

## 3. Камера: это подсистема-владелец, а не отдельная camera entity

По `tc2vr/docs/ENGINE.md`:

- отдельной camera entity нет;
- камеры принадлежат camera-owner component на `g_Game`, slot `0x1C`;
- компонент хранит массив camera controllers и соответствующие `CameraData`;
- `+0x2F8` — активный `CameraData`;
- controllers имеют `Activate`, `Deactivate` и `Update`;
- известен `CameraControllerVehicleEntity`, плюс в этой сборке описано ещё 16 controller subclasses.

Это не доказывает наличие готового пешего controller. Но меняет направление поиска: camera handoff может происходить через camera-owner subsystem и смену/активацию controller, а не через создание отдельной camera entity. Для нашего проекта нужно найти в matching build список controller types и выяснить, есть ли Avatar/third-person пеший вариант.

## 4. Transform и world coordinates

Внешние заметки описывают transform как inline-часть `CIT3DEntity`, а не компонент:

- `+0x3A0` — integer world cell;
- `+0x3B0` — fractional position внутри cell;
- `+0x3C0`, `+0x3D0`, `+0x3E0` — строки rotation matrix;
- `+0x3F0` — world position;
- `+0x450` — rotation quaternion.

Мир Z-up. Это может быть полезно при будущем анализе transform пешего экземпляра, но наследование `CITEntityAvatarTC2` от конкретного `CIT3DEntity` в нашем индексе отдельно не доказано, поэтому offsets не следует применять к Avatar вслепую.

## 5. Сверка сборок

| Источник | Сборка |
|---|---|
| Наш `TheCrew2.exe` | PE timestamp `2025-09-22 22:37:44 UTC`, image base `0x140000000` |
| `tc2vr` | В `src/game.hpp` обозначена Steam-сборка сентября 2026 года |

Из-за разницы примерно в год нельзя переносить:
- глобальные RVA;
- адреса функций;
- vtable addresses;
- signatures и offsets, если они не подтверждены для нашей версии.

Структурные описания — компонентный массив, parent Binding, camera-owner architecture — можно использовать как исследовательские гипотезы и проверять на нашей сборке.

## 6. Следующие конкретные задачи

1. Для v09 component-host candidate установить RTTI/type name по vtable, если появится безопасный статический или уже имеющийся memory snapshot с областью `vtable[-1]`. Новых in-process hooks ради этого не ставить.
2. Проверить, соответствует ли slot `0x1C` camera owner в нашей сборке, и определить его component type по vtable/RTTI.
3. Найти список camera-controller subclasses именно в нашей сборке; отдельно искать пеший/Avatar controller и его activation path.
4. Отдельно искать **Entity factory/registry**, не смешивая её с описанной component factory.
5. Продолжить искать native implementation `GetLocalPlayer`/`GetCurrentPlayer` и setter текущей управляемой сущности.
6. Не использовать NPC, HQ characters или `AvatarNPC` как подтверждение player pipeline.

## Итог

Самый полезный результат внешнего исследования — уточнение роли `CITEntityComponentBinding`: это механизм родительской привязки, а не подтверждённый мост Player→Avatar. Второй результат — независимое описание component-host и camera-owner архитектуры. Оно позволяет переоценить v09 root как возможный общий игровой component host и задаёт конкретный следующий поиск по camera-owner slot и controller subclasses.

Ни один из этих результатов пока не доказывает создание пешего Avatar, передачу ему ввода или смену controlled entity. Для этого всё ещё нужны native реализации из нашей сборки.
