# ANALYSIS 14: структура AvatarCameraSettings и граница между camera config и controller

Дата: 2026-11-17  
Источник: TC2_ENTITY_INDEX.sqlite, файл A (archetypes.entities.bin), объекты CITEntityAvatarSettings.

## 1. Путь к настройкам камеры аватара

В CITEntityAvatarSettings по адресу 0x18D1072 расположен дочерний объект PlayerCharacterSetting по 0x18D109C.

В нём:
- AvatarAnimCtrlID_ThirdPerson_Male = null;
- AvatarAnimCtrlID_ThirdPerson_Female = null;
- OverlapRange;
- FadeDuration;
- DriverAvatarFadeDuration;
- вложенный AvatarCameraSettings по 0x18D10CF;
- вложенный DefaultAvatarPhysicDesc по 0x18D141C.

Нулевые animation IDs относятся к базовой записи настроек. В дочернем archetype настроек ранее были найдены конкретные ID, поэтому этот null нельзя переносить на все варианты.

## 2. AvatarCameraSettings

В AvatarCameraSettings находятся параметры поворота, инерции, наклона головы, зума, FOV, proximity slowdown и переходов. Важные дочерние блоки:

- CameraOffset (0x18D11E9): OffsetForward, OffsetSide, OffsetUp;
- FocusPersonCamera (0x18D120A): BackOffset, UpOffset, YawSpeed, PitchSpeed, ZoomSpeed, ограничения pitch и FOV;
- ThirdPersonCamera (0x18D126A).

Внутри ThirdPersonCamera есть ThirdPersonSettings, далее:

| Блок | Настройки |
|---|---|
| Camera | CameraFOV, CameraRadius, CameraBackOffset, CameraPitchOffset, initial pitch/yaw |
| Target | TargetScreenOffset, TargetWorldOffset |
| AxisLink | AxisSpringStiffness2, AxisInertia |
| PitchLink | stiffness, acceleration, inertia, max speed, lower/upper limits, mouse speed |
| YawLink | stiffness, acceleration, inertia, max speed, mouse speed |
| Zoom | front/screen offsets, look factor, bike offset |
| ZoomVehicleTarget | таблица порогов высоты, FOV, world offset, interpolation speed |
| Collisions | small/medium collision size |

Это полноценная конфигурация third-person camera поведения для Avatar settings. Она даёт конкретные поля, которые можно искать в native-коде как входные данные camera controller.

## 3. Физика аватара в том же PlayerCharacterSetting

DefaultAvatarPhysicDesc содержит:
- CapsuleRadius = 0.25;
- CapsuleHeight = 1.85;
- Gravity = 16;
- Gain = 1;
- MaxSpeed = 5.5;
- MaxAcceleration = 100;
- WalkSpeed = 2.5;
- RunSpeedFactor = 3;
- RunThreshold = 0.5;
- MaxSlope = 60;
- support distance, strafe/backward factors и proximity slowdown параметры.

Таким образом, camera config и физическая конфигурация пешего аватара сосредоточены в одном PlayerCharacterSetting.

## 4. Чего здесь нет

В этой конфигурационной ветке не найдено:
- PlayerEntityID или AvatarEntityID;
- ссылки на CITEntityHumanPlayer или CITEntityGhostPlayer;
- поля выбора текущего controlled entity;
- явного имени native camera controller или функции активации;
- ссылки на input manager.

Следовательно, ThirdPersonCamera — это имя блока настроек, не доказательство существования или активации конкретного runtime controller.

## 5. Связь с внешними engine notes

В независимых заметках tc2vr/docs/ENGINE.md камера описана как subsystem, принадлежащая camera-owner component на игровом component host. У camera controllers есть отдельные Activate, Deactivate и Update методы.

Совместное чтение двух источников задаёт проверяемый путь:
1. Avatar settings предоставляют параметры AvatarCameraSettings.ThirdPersonCamera.
2. Native camera controller должен читать эти параметры.
3. Camera-owner subsystem хранит активный controller и его CameraData.
4. Переключение на пеший вид, если оно реализовано штатно, должно затрагивать активный controller/target.

Пункт 2–4 пока являются исследовательской гипотезой, а не установленной реализацией в нашей сборке.

## Вывод

У пешего Avatar есть детальная конфигурация third-person camera и физики. Это усиливает вывод, что игра содержит данные для полноценного пешего режима, но не показывает, как режим включается. Следующий конкретный native-поиск — потребители полей AvatarCameraSettings и типы camera controllers в matching build, а не дальнейшее изучение NPC/HQ archetypes.
