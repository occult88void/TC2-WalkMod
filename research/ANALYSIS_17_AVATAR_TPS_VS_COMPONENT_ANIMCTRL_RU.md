# ANALYSIS 17: разделение Avatar TPS controller и AnimCtrlID компонента Avatar

Дата: 2026-11-17  
Источник: TC2_ENTITY_INDEX.sqlite, файл A, entity_components, fields, entities, typed_objects.

## 1. Два разных поля, которые нельзя смешивать

В проекте есть два разных пути к animation controller:

1. PlayerCharacterSetting.AvatarAnimCtrlID_ThirdPerson_Male/Female — ссылки на CAnimCtrl_AvatarTPS, разобранные в ANALYSIS 16.
2. CITEntityComponentAnimCtrl.AnimCtrlID — поле компонента, присутствующего в AvatarTC2 archetypes.

Это разные поля с разными наборами target IDs.

## 2. Что показывает AnimCtrlID в AvatarTC2 archetypes

По всем 612 entity records класса CITEntityAvatarTC2 агрегированы поля AnimCtrlID в их CITEntityComponentAnimCtrl:

| AnimCtrlID target | Количество | Разрешённый тип |
|---|---:|---|
| 1D34020000000008 | 200 | CAnimCtrl_SingleAnim |
| E6AB020000000008 | 9 | CAnimCtrl_SingleAnim |
| 6720010000000008 | 3 | CAnimCtrl_SingleAnimAndFB |
| 4D52040000000008 | 3 | CAnimCtrl_SingleAnimAndFB |
| 2F5B040000000008 | 2 | CAnimCtrl_AnimSequence |
| 2E5B040000000008 | 2 | CAnimCtrl_AnimSequence |
| остальные target IDs | по 1 | SingleAnim / AnimSequence / другие найденные CITEntityAnimCtrl |

Всего у AvatarTC2 найдено 31 уникальное значение AnimCtrlID, включая null в одной базовой записи. Среди этих component AnimCtrlID нет Male/Female TPS targets C6BD040000000008 и D45B040000000008.

## 3. Что это значит

Подтверждено, что:
- AvatarAnimCtrlID_ThirdPerson_Male/Female ведут на CAnimCtrl_AvatarTPS с InputFSM и AnimFSM;
- AnimCtrlID в CITEntityComponentAnimCtrl у AvatarTC2 ведёт на отдельные CITEntityAnimCtrl targets, чьи типы включают CAnimCtrl_SingleAnim, CAnimCtrl_SingleAnimAndFB и CAnimCtrl_AnimSequence;
- эти два механизма не являются одним и тем же полем или одной и той же ссылкой.

Не установлено, для чего конкретно используется каждый из per-archetype AnimCtrlID. Нельзя утверждать, что это обязательно косметические клипы или что TPS controller всегда является активным locomotion controller. Это требует native call/dataflow.

## 4. Практическое следствие для native-поиска

При поиске потребителей Avatar animation data нужно отдельно отслеживать:
- чтение PlayerCharacterSetting.AvatarAnimCtrlID_ThirdPerson_Male/Female;
- чтение CITEntityComponentAnimCtrl.AnimCtrlID;
- регистрацию/создание объекта CAnimCtrl_AvatarTPS;
- источник velocity/angular velocity для его InputFSM.

Иначе легко принять общий AnimCtrl component за TPS locomotion controller или наоборот.

## Вывод

Найдены два независимых serialized пути к animation controllers у Avatar. TPS locomotion state machine задаётся через PlayerCharacterSetting; компонентный AnimCtrlID в AvatarTC2 records указывает на другие controller archetypes. Их runtime-взаимодействие пока неизвестно.
