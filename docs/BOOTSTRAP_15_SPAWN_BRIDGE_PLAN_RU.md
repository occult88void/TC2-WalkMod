# Bootstrap15 — Host pointer-depth, spawn bridge и подготовка к анализу переходов

## Цель

Bootstrap15 — узкий read-only диагностический проход после Bootstrap14. Он должен закрыть ошибки глубины указателей и связать уже найденные Host-компоненты с конфигурациями пешего персонажа и точками появления. Это не реализация выхода из транспорта.

## Сценарии

Сохранить ровно два сценария:
- Ctrl+Shift+F10 — Freedrive с транспортом.
- Ctrl+Shift+F11 — HQ с пешим персонажем.

Не добавлять Leave, открытый мир, дополнительные переходы или автоматические действия. Никаких хуков, патчей и записей в память игры.

## Обязательные исправления захвата

В Bootstrap14 CaptureObjectAndVtable трактовал первый qword каждого объекта как vtable. Для Host wrapper-полей это неверно: первый qword указывает на вложенный объект. В Bootstrap15 каждый уровень должен иметь отдельную запись и файл:

1. wrapper object bytes;
2. nested object bytes по wrapper.qword0;
3. true vtable по nested.qword0;
4. code pointers в wrapper/nested object с метками byte_offset, не vtable_slot.

Это относится к Host +0x650, +0x658, +0x6D0, +0x6D8. Для component[selector 0x03] +0xDF8 первый qword указывает обратно на компонент, поэтому его также нельзя называть vtable.

## Host и компоненты

- Host root: захватить 0x4000 байт, если диапазон доступен и читаем.
- Host wrapper-поля: +0x650, +0x658, +0x6D0, +0x6D8; захватить wrapper, nested object, true vtable.
- Host +0x1E68: сохранить корректный объект и его настоящую vtable.
- Логические selector offsets 0x03 и 0x11: расширить объектные окна до 0x1800, если регион памяти позволяет.
- Для selector 0x03 дополнительно захватить объект по component+0xDF8, его вложенные ссылки +0x98/+0xA8 и callbacks по известным RVA 0x780D70, 0x780D80, 0x780D90.
- Для selector 0x11 зафиксировать внутреннюю область component+0x1258.
- Дополнительно расширить логические слоты 0x06, 0x1C, 0x23 до 0x800 для сравнения.

## Кодовые цели

Снять окна известных thunk targets из Host+0x6D0 и их прямых целей, включая RVA:
- 0xB77FA0
- 0xB78010
- 0xB948E0
- 0xB78150
- 0xB786A0
- 0xB78710
- 0xB96590
- 0xECCEE0

Все адреса перепроверять относительно PE fingerprint текущей версии игры и executable section. Метки нейтральные, без приписывания неизвестным функциям семантики.

## Связь с точками появления

Из игровых данных ранее подтверждены архетипы:
- CITEntityHeadQuarterCharacterSpawnPoint
- CITEntityHeadQuarterExitPoint
- CITEntityHousingCharacterSpawnPoint
- CITEntityHousingExitSpawnPoint

Также найдены миссионные типы CMissionAtomSpawn и CMissionAtomTeleportEntity. Это подтверждает наличие в mission runtime операций создания сущности и перемещения существующей сущности, но не доказывает, что их можно вызывать из Freedrive или использовать для передачи управления локальному игроку.

Bootstrap15 не должен подменять это доказательство предположением. В его отчёте нужно явно разделять:
- archetype/config spawn points;
- runtime object references;
- world transform/coordinates;
- mission Spawn/Teleport operations;
- local-player control ownership.

## Формат отчёта

Для каждого объекта указывать:
- source field / logical selector offset;
- wrapper address и qword0;
- nested address и qword0;
- true vtable address;
- object/vtable/code files;
- стабильность повторного чтения;
- отличия Freedrive/HQ;
- неизвестные значения без интерпретации.

В конце дать mapping-таблицу «logical selector offset → component address → важные внутренние ссылки», а также отдельный раздел «что это НЕ доказывает».

## Следующий этап после Bootstrap15

Только после проверки результатов переходить к поиску перехода. Гипотеза для проверки: штатная смена контекста может включать выбор/создание пешего аватара, назначение spawn transform, регистрацию локального/текущего игрока, передачу камеры и управления, а также отдельное сохранение транспорта. Не считать телепортацию доказанной до обнаружения соответствующих transform/position полей или наблюдаемой последовательности.

## Критерий успеха Bootstrap15

Успех — корректно захваченные уровни указателей, расширенные объекты 0x03/0x11 и кодовые окна, которые позволяют связать Host lifecycle с avatar/spawn/control данными. Сам по себе успешный сбор не означает, что найден Leave или рабочая свободная ходьба.
