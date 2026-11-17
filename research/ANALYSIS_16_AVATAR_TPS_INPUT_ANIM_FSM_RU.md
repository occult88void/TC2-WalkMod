# ANALYSIS 16: CAnimCtrl_AvatarTPS — Avatar-specific InputFSM and AnimFSM

Дата: 2026-11-17  
Источник: TC2_ENTITY_INDEX.sqlite, файл A, entities, typed_objects, objects, fields.

## 1. Male/Female Avatar settings прямо указывают на два animation-controller archetypes

В дочернем CITEntityAvatarSettings с ID 6642010000000008, offset 0x18BDB5D, объект PlayerCharacterSetting расположен по 0x18BDB7D.

Его поля:

| Поле | Значение | Разрешённый класс |
|---|---|---|
| AvatarAnimCtrlID_ThirdPerson_Male | C6BD040000000008 | CITEntityAnimCtrl |
| AvatarAnimCtrlID_ThirdPerson_Female | D45B040000000008 | CITEntityAnimCtrl |

Оба ID точно разрешаются в отдельные CITEntityAnimCtrl archetypes. Оба являются наследниками CITEntityAnimCtrl 5AE7030000000008, который в свою очередь наследуется от explicit CITEntityAnimCtrl EBA2000000000008.

## 2. В обоих archetypes находится CAnimCtrl_AvatarTPS

Для каждого из двух ID в typed_objects найден вложенный AnimCtrl с type CAnimCtrl_AvatarTPS:

- Male C6BD040000000008: typed object в 0x12A91A8;
- Female D45B040000000008: typed object в 0x12A7B23.

У обоих контроллеров есть две главные ветви:
- InputFSM;
- AnimFSM.

Это не просто ссылка на один animation clip. Это отдельная конфигурация двух state machines.

## 3. InputFSM: состояния движения

В Male/Female InputFSM обнаружены состояния:

- StartStateDesc;
- StartLeftStateDesc;
- StartRightStateDesc;
- StartBackStateDesc;
- StartRunStateDesc;
- StartRunLeftStateDesc;
- StartRunRightStateDesc;
- StartRunBackStateDesc;
- WalkStateDesc;
- RunStateDesc;
- UTurnStateDesc;
- UTurnRunStateDesc;
- StopStateDesc;
- StopRunStateDesc;
- DecelStateDesc;
- AccelStateDesc;
- EmoteSateDesc.

В state descriptors встречаются поля:
- InitialLinearVelocity / FinalLinearVelocity;
- InitialAngularVelocity / FinalAngularVelocity;
- LinearVelocity / AngularVelocity;
- Duration;
- QuickStopTimeMin / QuickStopTimeMax;
- BreakAnim.

Вложенный Input → DefaultInputDesc содержит UTurnError; в Male-варианте также InterpSpeed.

Эти данные прямо показывают, что Avatar animation controller имеет отдельную конфигурацию стартов, ходьбы, бега, поворотов, остановки, ускорения и замедления. Поля скорости используются как параметры state machine.

## 4. AnimFSM: соответствующие animation states

В AnimFSM обнаружены:
- IdleAnim;
- EmoteAnim;
- StartAnim и варианты StartLeft/Right/Back;
- StartRunAnim и варианты StartRunLeft/Right/Back;
- WalkAnim;
- RunAnim;
- UTurnAnimLeft/Right;
- UTurnRunAnimLeft/Right;
- StopLeft/Right;
- StopRunLeft/Right;
- DecelLeft/Right;
- AccelLeft/Right.

Эти узлы содержат AnimPathID, а в ряде случаев LeftAnimPathID, RightAnimPathID, Loop, ApplyAnimLinearVel, ApplyAnimAngularVel и QuickStopPathID. Таким образом, конфигурация связывает именованные состояния движения с конкретными animation asset IDs и переходами.

AnimFSM также содержит States и Transitions с переходами вида IdleToWalk, IdleToStart, StartToWalk, WalkToStop, WalkToRun, RunToWalk, RunToUTurn, StopToIdle и другие.

Male и Female варианты имеют одинаковую общую структуру состояний, но разные AnimPathID и некоторые параметры переходов. Это согласуется с отдельными мужским и женским animation-controller archetypes.

## 5. Граница доказательств

Подтверждено:
- Avatar settings указывают на отдельные Male/Female CITEntityAnimCtrl;
- оба используют CAnimCtrl_AvatarTPS;
- у контроллера есть InputFSM с состояниями walk/run/turn/start/stop/accel/decel;
- AnimFSM сопоставляет эти состояния с animation assets и transitions.

Не подтверждено:
- что InputFSM читает физические клавиши или gamepad напрямую;
- что InputFSM сам перемещает Avatar в мире;
- что эти state machines получают данные от локального Player input manager;
- что наличие CAnimCtrl_AvatarTPS означает, что Avatar уже назначен текущей управляемой сущностью;
- что этот контроллер активируется вне HQ/interior.

Следовательно, InputFSM пока трактуется как animation/input-state machine. Это не доказанный gameplay movement controller.

## 6. Почему это важная зацепка

Ранее было установлено, что AvatarTC2 archetypes не содержат отдельного serialized PlayerController или InputController component. Теперь найден конкретный Avatar-specific AnimCtrl, который уже знает состояния walk/run/turn/stop и имеет соответствующие animation transitions.

Это сужает native-поиск:
1. найти native type registration / StaticType для CAnimCtrl_AvatarTPS;
2. найти его constructor и update/tick path;
3. определить, откуда он получает velocity, angular velocity и movement-state inputs;
4. выяснить, кто обновляет transform/physics Avatar отдельно от анимации;
5. только затем искать handoff от LocalPlayer к Avatar и camera-owner activation.

## Вывод

CAnimCtrl_AvatarTPS — первая конкретная Avatar-specific state-machine структура, найденная в данных. Она подтверждает наличие настроенного цикла анимаций ходьбы и бега, но не доказывает физическое управление персонажем. Это теперь один из главных native targets наряду с resolver GetLocalPlayer/GetCurrentPlayer и entity registry.
