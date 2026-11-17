# ANALYSIS 08A: фактические потребители GetLocalPlayer и GetCurrentPlayer

Дата: 2026-11-17  
Ветка: `analysis-07-player-control-chain`  
Источник: таблица `mission_bindings` в `TC2_ENTITY_INDEX.sqlite`.

## Зачем это важно

Наличие двух атомов `CMissionAtomGetLocalPlayer` и `CMissionAtomGetCurrentPlayer` само по себе ничего не объясняет. Их реальные потребители в mission graphs дают более полезную картину: какие подсистемы запрашивают каждый тип ссылки.

## 1. Куда передаётся GetLocalPlayer

В агрегированных mission bindings обнаружены следующие потребители:

| Потребитель | Поле-приёмник | Число уникальных узлов |
|---|---|---:|
| `CMissionAtomPlaySound` | `VarLocationEntID` | 118 |
| `CMissionAtomIsSameEntity` | `VarEntityID2` | 55 |
| `CMissionAtomEntityToList` | `VarEntListID` | 36 |
| `CMissionAtomSetHUDBehavior` | `VarEntID` | 31 |
| `CMissionAtomIsSameEntity` | `VarEntityID1` | 15 |
| `CMissionAtomPostAudioEvent` | `VarLocationEntID` | 15 |
| `CMissionAtomDetectCollisionPlayerVsPlayer` | `VarPlayerEntityID` | 10 |
| `CMissionAtomTeleportEntity` | `VarEntID` | 4 |
| `CMissionAtomBlockCar` | `VarEntID` | 2 |

Наиболее интересны не аудио-вызовы, а три группы:

- `SetHUDBehavior`: LocalPlayer используется как сущность, которой назначается HUD behavior.
- `DetectCollisionPlayerVsPlayer`: LocalPlayer подаётся именно в поле `VarPlayerEntityID`.
- `TeleportEntity`: LocalPlayer подаётся как сущность для телепортации в одном из графов.

Это показывает, что LocalPlayer используется в операциях над player entity, а не только как абстрактная ссылка для звука.

## 2. Куда передаётся GetCurrentPlayer

| Потребитель | Поле-приёмник | Число уникальных узлов |
|---|---|---:|
| `CMissionAtomIsSameEntity` | `VarEntityID1` | 72 |
| `CMissionAtomIsBot` | `VarEntityID` | 38 |
| `CMissionAtomBlockCar` | `VarEntID` | 34 |
| `CMissionAtomEntityToList` | `VarEntListID` | 28 |
| `CMissionAtomIsSameEntity` | `VarEntityID2` | 21 |
| `CMissionAtomIsEntityInList` | `VarEntityID` | 3 |
| `CMissionAtomTeleportEntity` | `VarEntID` | 2 |
| `CMissionAtomTeleportEntity` | `VarTeleportPositionID` | 2 |

Здесь заметны:

- `IsBot`: CurrentPlayer проверяется на принадлежность к bot-категории.
- `BlockCar`: CurrentPlayer используется в операции, связанной с блокировкой автомобиля.
- `TeleportEntity`: в двух графах один и тот же CurrentPlayer подаётся одновременно в `VarEntID` и `VarTeleportPositionID`; это необычная конфигурация, но без native-семантики атома нельзя заключать, что она означает.

## 3. Что это говорит о различии ссылок

**Подтверждено:** LocalPlayer и CurrentPlayer имеют разные наборы потребителей. В частности, LocalPlayer используется в `SetHUDBehavior` и `DetectCollisionPlayerVsPlayer`, а CurrentPlayer — в `IsBot` и `BlockCar`. Кроме того, в 70 узлах `IsSameEntity` эти две ссылки сравниваются друг с другом.

**Осторожная интерпретация:** вероятно, это две разные семантические роли ссылки на сущность. Одна может обозначать локальную player entity, другая — текущую сущность/представление, участвующее в игровом управлении или Freedrive. Но точное значение слова Current в движке не установлено.

Нельзя пока утверждать, что:
- LocalPlayer всегда является `CITEntityHumanPlayer`;
- CurrentPlayer всегда является машиной;
- CurrentPlayer становится pedestrian после выхода из транспорта;
- один из атомов возвращает AvatarTC2.

Для этого нужно установить native implementation соответствующих атомов или получить наблюдения из безопасного, штатного игрового сценария. Инжектированные hooks не использовать: Bootstrap 10 уже показал signature mismatch и запуск игры с DLL сопровождался integrity warning.

## 4. Практическое значение для walking mod

Теперь поиск следует вести не только от pedestrian archetypes, но и от операций, которые принимают эти две ссылки:

1. Native implementation `GetLocalPlayer` и `GetCurrentPlayer`.
2. Native implementation `SetHUDBehavior` и `IsBot`.
3. Код, который вызывает `BlockCar` и `DetectCollisionPlayerVsPlayer` в общих player flows.
4. Код, который обновляет ссылки, сравниваемые через `IsSameEntity`, при переходах между состояниями.

Особенно полезен будет момент, когда LocalPlayer и CurrentPlayer перестают быть равны или снова становятся равны. Это может дать наблюдаемый маркер handoff, если удастся найти соответствующую штатную последовательность в mission graph или native lifecycle.

## Вывод

Mission data уже показывает, что LocalPlayer и CurrentPlayer не являются взаимозаменяемыми именами. Их потребители различаются по функциям. Это одна из самых перспективных связок для восстановления control handoff, но она пока не раскрывает точные типы сущностей за этими ссылками.
