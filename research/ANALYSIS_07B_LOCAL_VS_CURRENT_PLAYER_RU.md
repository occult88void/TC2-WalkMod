# Analysis 07B: local player vs current player comparisons

Дата: 2026-11-17  
Ветка: `analysis-07-player-control-chain`

## Проверка

В `TC2_ENTITY_INDEX.sqlite` проверены все data-slot bindings для `CMissionAtomIsSameEntity`. Найдено 93 узла этого типа и 186 входных связей. В 51 узле (с учётом повторно используемых графов) одна сторона сравнения приходит из `CMissionAtomGetLocalPlayer`, другая из `CMissionAtomGetCurrentPlayer`.

Примеры подтверждённых связей:

| Graph | IsSameEntity atom | VarEntityID1 | VarEntityID2 |
|---|---:|---|---|
| `0x618505` | 8326 | GetCurrentPlayer 8329 | GetLocalPlayer 8330 |
| `0x618505` | 8337 | GetCurrentPlayer 8336 | GetLocalPlayer 8335 |
| `0x7CADB8` | 48420 | GetCurrentPlayer 48423 | GetLocalPlayer 48425 |
| `0x130389E` | 48042 | GetLocalPlayer 48044 | GetCurrentPlayer 48043 |
| `0x1E24C89` | 17339 | GetLocalPlayer 17338 | GetCurrentPlayer 17337 |
| `0x1EB0778` | 21435 | GetLocalPlayer 21436 | GetCurrentPlayer 21437 |

В графе `0x618505` четыре сравнения IsSameEntity подают результат в `CMissionAtomTestLogicANDEachFrame`. Одно из этих сравнений (atom 8971) через test-logic node 8970 имеет True flow к `CMissionAtomOperatorSetBool` 8979. Этот оператор получает две bool-переменные 8978 и 8977. Таким образом, сравнение LocalPlayer и CurrentPlayer действительно используется в исполняемой mission logic, а не только существует в словаре типов.

Та же физическая группа узлов многократно повторно используется в нескольких mission graph roots (`0x618505`, `0x62A54A`, `0x780FF3`, `0x1289DBF` и другие). Повторное использование важно: это может быть общий скриптовый компонент, а не независимые эксперименты миссий.

## Что это доказывает

1. `GetLocalPlayer` и `GetCurrentPlayer` возвращают ссылки, которые mission logic считает потенциально различными: игра содержит штатные проверки их идентичности.
2. Результат сравнения влияет на bool-логику миссии.
3. Значит, разница между LocalPlayer и CurrentPlayer является значимой для игрового кода. Это более сильная зацепка, чем просто наличие двух похожих mission atoms.

## Чего это не доказывает

- Не установлено, что `CurrentPlayer` означает «сущность, которой прямо сейчас управляет пользователь». Название подсказывает такую трактовку, но runtime-семантика ещё не подтверждена.
- Не установлено, что `LocalPlayer` обязательно является машиной, а `CurrentPlayer` — пешим аватаром, или наоборот.
- В данных нет setter-атома `SetCurrentPlayer` или явной команды possession.
- Сравнение идентичности не показывает, кто и где меняет одну ссылку.

## Практическое направление

Следующий поиск должен быть направлен на native implementation двух mission atoms:
- `CMissionAtomGetLocalPlayer`
- `CMissionAtomGetCurrentPlayer`

Нужно установить, из каких менеджеров/полей они получают ссылки. Затем сопоставить эти источники с lifecycle manager candidate из RVA `0x343C630` и с объектами `CITEntityHumanPlayer`, `CITEntityAvatarTC2`, `CITEntityAvatarInterior`.

Это может дать прямой мост между двумя уже существующими половинами исследования: avatar/vehicle lifecycle и player identity. Пока такой мост не найден, не следует проектировать ручную запись указателя или подмену player state.
