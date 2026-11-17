# Практический тест 01: Avatar WalkSpeed

## Цель
Проверить, использует ли пеший Avatar после перехода из HQ в Freedrive физические настройки `CITEntityAvatarSettings -> PlayerCharacterSetting -> DefaultAvatarPhysicDesc`.

## Единственное изменение
- Файл: `entity/generated/archetypes.entities.bin`
- Путь PitCrew: `85092:0:1`
- Поле: `WalkSpeed`
- Было: 2.5 (`00002040`)
- Тест: 5.0 (`0000A040`)

Путь подтверждён по индексу:
- `CITEntityAvatarSettings`, ID `1A42010000000008`, offset `0x18D1072`, root ordinal `85092`;
- `PlayerCharacterSetting`, child ordinal `0`;
- `DefaultAvatarPhysicDesc`, child ordinal `1`.

## Процедура
1. Импортировать ZIP в PitCrew.
2. Оставить включённым DZ.Blood Walkmod.
3. Скомпилировать моды.
4. Запустить игру обычным способом.
5. Войти в поддерживаемый HQ Rally или HQ GP и штатно перейти в Freedrive через Leave.
6. Сравнить скорость ходьбы на ровном участке с исходной игрой.

## Интерпретация
- Скорость заметно выросла: подтверждено, что данный Avatar path потребляет этот physics setting в проверенном режиме.
- Скорость не изменилась: сначала проверить успешность компиляции/загрузки патча; затем считать, что этот Avatar не использует данный блок либо WalkSpeed не определяет наблюдаемую скорость.

## Откат
Отключить Avatar Physics Test в PitCrew и пересобрать моды. DZ.Blood можно оставить включённым. Никакие EXE, DLL или BattlEye настройки не меняются.
