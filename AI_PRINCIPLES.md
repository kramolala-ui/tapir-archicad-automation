# AI Principles — работа с форком Tapir-archicad-automation

**Назначение.** Стартовый контекст для LLM, которая работает над
нашим форком Tapir-аддона. Прочитай целиком до того, как предлагать
правки.

**Аудитория.** LLM через AI Bridge (`===AI_REQUEST===`,
`===AI_CHANGES===`), работающая в ветке `fresh`.

---

## 0. Как подключить этот файл к промпту AI Bridge

Этот файл — часть рабочего контекста. Чтобы Bridge сам подтягивал
его в начале сессии, надо сделать две вещи (обе — однократно).

### 0.1. Пользователь: вкладка «Промпт» → тип «GitHub»

В AI Bridge есть вкладка **«Промпт»** и переключатель типа промпта
(`main` / `GitHub` / `donor`). Для работы с аддоном выбери тип
**GitHub** и в поле «Дополнительные инструкции» (или в конце
github-промпта) добавь строку:

```
Перед первым ответом в новой сессии прочитай файл:
  github://kramolala-ui/tapir-archicad-automation@fresh/AI_PRINCIPLES.md
и держи его в контексте до конца сессии.
```

Сохрани промпт. Со следующего запуска Bridge он будет в промпте.

### 0.2. Пользователь: вкладка «GitHub» → репозиторий и токен

- **Репозиторий:** `kramolala-ui/tapir-archicad-automation`
- **Токен:** Personal Access Token со scope `repo` (или
  `Contents: Read+Write`). Хранится в QSettings.
- **Рабочая ветка:** должна отображаться как `fresh` — это значит,
  что `WORK_BRANCH` в `agent_tools/github_provider.py` равен `"fresh"`.
  Если показывает `ai_bridge/work` — правь константу и перезапускай
  Bridge (см. п. 4a ниже про перезапуск).

### 0.3. LLM: первый ход в новой сессии

**Первый ответ в сессии — всегда `===AI_REQUEST===` с одним файлом:**

```json
[{"action": "request",
  "files": ["github://kramolala-ui/tapir-archicad-automation@fresh/AI_PRINCIPLES.md"],
  "reason": "стартовый контекст перед работой"}]
```

После получения файла — держи его в уме, ссылайся на разделы
(«см. раздел 4a про узкие места ACAPI») в ответах. Не перечитывай
каждый раз, если работаешь в той же сессии.

---

## 1. Что это за проект

Форк [ENZYME-APD/tapir-archicad-automation](https://github.com/ENZYME-APD/tapir-archicad-automation)
от `kramolala-ui`.

- **Upstream** — 105 команд, JSON API к Archicad через Graphisoft add-on
  механизм (`ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler`).
- **Наш форк** — 260+ команд: MEP, IFC, слои, аннотации, solid
  operations, rotate elements, копирование/поворот, операции с зонами,
  ключевые ноты, скрипт-UI, теперь ещё bulk-команды.
- **Зачем форк:** upstream не двигается, а нам нужны команды, которых
  там нет, и оптимизации, на которые upstream не пойдёт.

**Где живёт истина:** рабочая ветка — **`fresh`**. Это не `work`
(там upstream + 1 наша команда), не `main` (upstream), не
`cpp_bridge/work`.

**Локальная копия:** `C:\Python_projects\tapir-custom\archicad-addon\`.
Перед работой уточняй у пользователя, куда писать: через AI Bridge —
в `@fresh`; через локальный AI_PROGRAM — в `tapir-custom`.

---

## 2. Workflow правок

```
LLM (AI Bridge, @fresh)
  → правки в GitHub-ветку fresh
  → GitHub Actions собирает аддон (автоматически)
  → пользователь скачивает .apx (Win) / .bundle (Mac)
  → устанавливает в Archicad
  → тестирует живой проект
  → синхронизирует с локальным tapir-custom (иногда)
```

### ⚠ Сборка — ТОЛЬКО через GitHub Actions

**На машине разработчика НЕТ локального API Dev Kit.**

- Не пытайся искать `ACAPinc.h`, `ACAPI*.hpp` или `API Dev Kit`
  на диске — их нет ни в `C:\Program Files\GRAPHISOFT`, ни в
  `C:\DevKit`, ни где-либо ещё.
- Не предлагай «сначала проверим сигнатуру в DevKit» — это невозможно.
- Не строй планы, требующие локальной компиляции.
- Не спрашивай путь к DevKit — его нет.

**Что это значит для правок:**

1. Точные сигнатуры ACAPI-функций проверяются **только через сборку
   в GitHub Actions**. Если компиляция падает — лог присылает
   пользователь, правка делается по ошибке компилятора.
2. Любая новая ACAPI-функция (особенно пакетные варианты) — это
   **итеративная работа**: 1-3 ребилда, пока сигнатура не сойдётся.
   Это нормально.
3. Если можешь избежать нового ACAPI-вызова — избегай. Работай с
   тем, что уже скомпилировано, или с библиотеками через
   `FetchContent` / `find_package` (их API известен по документации).

### Правила правок

1. Всегда проверяй, что путь в `@fresh`, а не `@work` / `@main`.
2. До правки — `stat` или `read`, чтобы убедиться, что файл не
   изменился с прошлой сессии.
3. Новый `.hpp/.cpp` в `Sources/` подхватывается CMake-глобом
   автоматически. Не надо править `CMakeLists.txt` — он тривиален
   и делегирует всё в `Tools/CMakeCommon.cmake`.
4. Регистрация команды — всегда в `AddOnMain.cpp::Initialize`.
   Место вставки: **до** блока «Loading the palette singleton…»
   (баг #516 — если палитра упадёт, команды после неё не зарегаются).
5. Один блок `AI_CHANGES` = 3-5 правок. `write` большого файла —
   отдельным блоком.
6. **Не используй `replace` для файла, только что созданного `write`
   в той же сессии.** GitHub raw-кэш не успевает, `fetch_file`
   отдаёт 404, весь батч откатывается. Правь через повторный `write`.
7. **Bridge пишет только в ветку `WORK_BRANCH`.** Даже если в пути
   стоит `@fresh`, при `WORK_BRANCH="fresh"` всё уходит в fresh.
   Алиас `@work` = `WORK_BRANCH` (текущее значение). Алиас `@main`
   идёт ровно в main (только чтение, по политике).

---

## 3. Архитектура аддона

### CommandBase

Базовый класс — `Sources/CommandBase.hpp`. Наследники реализуют:

- `GetName()` — имя команды как видит клиент (без namespace).
- `GetInputParametersSchema()` — JSON-схема входных параметров.
- `GetResponseSchema()` — JSON-схема ответа.
- `Execute(params, processControl)` — сама работа.
- `GetNamespace()` — `final` в базовом, всегда `TapirCommand`.

**Важно:** `namespace` и `execution policy` — `final`. Их нельзя
переопределить. Всё, что делает аддон, — это стандартные API-команды
Archicad. Свой HTTP-сервер / endpoint — невозможен через API-команды.
Транспорт закрыт Graphisoft'ом.

### Поток данных

```
Python-клиент → JSON → Graphisoft runtime → GS::ObjectState
                  → CommandBase::Execute(params) → ответ ObjectState
                  → Graphisoft runtime → JSON → Python
```

**Пункт «Graphisoft runtime парсит JSON в ObjectState» — самый
медленный.** Он не наш, заменить нельзя. Единственный способ обойти —
упаковать данные в **одну строку** (`payload_b64`) и парсить её самим
внутри `Execute`.

### Группы команд

`CommandGroup` — организационная сущность. Создаётся в `Initialize`,
через `RegisterCommand<Cmd>(group, version, description)` складываются
команды. Порядок — не важен, но по стилю идут группами: Application,
Project, Element, Element Creation, Attribute, Property,
Classification, IFC, MEP, SolidElementOperation, ScriptUI, Developer,
Bulk.

---

## 4. Что делаем сейчас: bulk-транспорт

### Проблема

Массовые операции (3000 свойств × 20000 объектов) упираются в:

1. **Graphisoft JSON-парсер** — сходит с ума на больших JSON.
2. **Лимит батча ~500 элементов** — 40 HTTP-раундов.
3. **Одиночные ACAPI-вызовы в цикле** (см. раздел 4a).

### Решение: base64(msgpack(zstd(data)))

Не меняем транспорт Graphisoft. Оборачиваем тяжёлые данные в одну
JSON-строку `payload_b64`:

```
Python:
  data → msgpack → zstd → base64 → "payload_b64": "..."

C++ (в Execute):
  payload_b64 → base64-decode → zstd-decompress
              → msgpack-parse → обработка порциями ВНУТРИ одного вызова
```

**Что это даёт:**

- Один HTTP-запрос вместо 40.
- Парсинг внутри аддона — на своей стороне, быстро.
- zstd — 5-10× по объёму.
- base64 съедает 33%, но это меньше выигрыша.

### Порядок внедрения

| # | Шаг | Статус |
|---|---|---|
| 1 | `BulkPing` — base64 only | **сделано** |
| 2 | Подключить zstd через FetchContent | TODO |
| 3 | Подключить msgpack-cxx (header-only) | TODO |
| 4 | `Bulk.GetPropertyValues` на существующем ACAPI | TODO |
| 5 | Пакетный ACAPI в `PropertyCommands.cpp` | TODO |
| 6 | Остальные bulk-команды (set, gdl, details) | TODO |

**Не начинать шаг N+1, пока не проверен на живом шаг N.**

---

## 4a. Известные узкие места ACAPI

Места, где аддон делает по одному ACAPI-вызову на элемент там, где
теоретически может быть пакетный. **Не оптимизировать до того, как
bulk-транспорт заработает** — иначе непонятно, что дало выигрыш.

| Файл / метод | Что не так |
|---|---|
| `PropertyCommands.cpp::GetPropertyValuesOfElementsCommand` | `for (element)` + `ACAPI_Element_GetPropertyValuesByGuid` — 500 вызовов на 500 элементов. Пакетный аналог: `ACAPI_Element_GetPropertyValues(elemGuidsArray, ...)` |
| `PropertyCommands.cpp::SetPropertyValuesOfElementsCommand` | `for (element)` + `GetPropertyValuesByGuid` + `ACAPI_Element_SetProperty` по одному = 1000+ вызовов. Пакетный аналог: `ACAPI_Element_SetPropertyValues(...)` |
| `PropertyCommands.cpp::GetPropertyValuesOfAttributesCommand` | `ACAPI_Attribute_GetPropertyValuesByGuid` в цикле — то же для атрибутов |
| `ClassificationCommands.cpp::GetClassificationsOfElementsCommand` | Цикл по элементам. Проверить `ACAPI_Element_GetClassifications(elemGuidsArray, ...)` |
| `ClassificationCommands.cpp::SetClassificationsOfElementsCommand` | Цикл по элементам. Проверить `ACAPI_Element_SetClassifications(...)` |
| `ElementGDLParameterCommands.cpp` (обе команды) | GDL через memo — пакетного API может не быть. Проверить `BMKillHandle` для memo (утечка при долгой работе) |

**Как проверять сигнатуры, если DevKit недоступен:**

1. Внести правку, закоммитить в `fresh`.
2. Дождаться сборки в GitHub Actions.
3. Если падает — смотреть лог: «no matching function call» и полная
   сигнатура, которую компилятор ожидает.
4. Скорректировать — 1-3 итерации обычно хватает.

**Когда этим заниматься:** после того, как bulk-транспорт заработает
и мы замерим выигрыш. Тогда станет видно, где реальный bottleneck:
в парсинге JSON / в пересылке / в ACAPI. Если в ACAPI — оптимизировать
эти методы. Если в транспорте — не трогать ACAPI.

---

## 4b. Перезапуск AI Bridge

`agent_tools/*.py` (в том числе `github_provider.py`, `dialog.py`,
`bridge.py`, `protocol.py`, `response_handler.py`, `request_handler.py`)
**не подхватываются на лету**. Если правишь эти файлы —
**закрыть окно Bridge и открыть заново**.

Симптом «правка применилась, но поведение старое» = не перезапущен
Bridge.

Особенно важно при:
- смене `WORK_BRANCH` (см. раздел 0.2),
- правке UI-лэйаута (новые поля не появятся без перезапуска),
- изменениях в протоколе маркеров.

---

## 5. Известные грабли

### AC26: `CreateObjects` падает с `-2130313112`

`ACAPI_Element_Create` не создаёт Object на Archicad 26 в некоторых
конфигурациях. Симптом: `{"error": {"code": -2130313112,
"message": "Failed to create new Object"}}`. Полилинии, плиты,
колонны через тот же аддон создаются нормально — проблема именно
с library parts.

**Обход:** `RotateElementsByAngle` с `withCopy: true` + `ModifyObjects`
для сдвига/поворота копии. Проверено на живом.

### `ModifyObjects` не ставит `dimensions.x` напрямую

При `useFixSize: true` (по умолчанию у библиотечных объектов)
параметр `dimensions.x` в `ModifyObjects` игнорируется молча. Надо
явно передавать `useFixSize: false` в том же вызове.

**Важно:** после этого длина управляется через **`MEP_StraightLength`**
(GDL-параметр), а не через `A`. `A` — производный, пересчитывается
сам после установки `MEP_StraightLength`.

### GDL-параметры пересчитываются асинхронно

`SetGDLParametersOfElements` возвращает `success: true` сразу, но
значение читается старым ещё несколько сотен миллисекунд. Если надо
проверить — подождать `time.sleep(0.3-0.5)` или перечитать дважды.

### `EntityType.BIM` не существует

В Python-коде (IFC Explorer) правильный тип для Object —
`EntityType.BIM_OBJECT`. Ошибка `AttributeError: type object
'EntityType' has no attribute 'BIM'` — признак короткого имени.

### `archicad_get_elements` не пишет `libPart` в параметры

Возвращает `element_type`, `bbox_*`, `ArchicadDetail/origin.*`,
`story_index`, но **не** `ArchicadDetail/libPart.name`. Чтобы
определить библиотечный элемент — отдельно звать
`GetDetailsOfElements` и смотреть `details.libPart.name`.

### Палитра загружается последней

`TapirPalette::Instance()` в конце `Initialize` обёрнута в `try/catch`.
Если упадёт, команды **до** неё работают; **после** — нет. Всегда
добавляй новые команды **до** этого блока (см. #516).

### `@fresh` в `AI_CHANGES` — не значит «пиши в fresh»

Bridge пишет **всегда в `WORK_BRANCH`** (см. раздел 0.2). Алиас
`@work` и `@fresh` — оба резолвятся в `WORK_BRANCH`. Чтение идёт
честно по указанному ref.

---

## 6. Стиль кода

- **C++17** (AC26-28), **C++20** (AC29+). Не используй более
  свежие фичи без необходимости.
- **Отступ — 4 пробела**, стиль GS/Graphisoft.
- **Пробел перед скобкой вызова:** `Foo (args)`, `if (x)`, `for (...)`.
- **`GS::ObjectState`** для входов/выходов. `std::unordered_map`
  внутри, если нужен быстрый доступ.
- **Namespace** — `TapirCommand`, единый. Отдельные не заводить.
- **Логирование** — `ACAPI_WriteReport(...)` для важных событий,
  `DBPrintf` для отладочных.
- **Undoable command**: если команда меняет модель — оборачивай в
  `ACAPI_CallUndoableCommand("CommandName", [&]() { ... })`.

---

## 7. Тесты и замеры

- **Юнит-тестов в аддоне нет** — архитектура Add-On не позволяет.
  Проверка — только на живом проекте.
- **Python-клиент** — `tapir_commands.py` в IFC Explorer. Правки
  клиента — отдельная задача.
- **Замер** — обязателен для bulk-команд. Сравнивать: JSON-путь (текущий)
  vs base64+msgpack+zstd (новый) на одном наборе. Результат — в раздел 9.

---

## 8. Границы

### Можно

- Добавлять новые команды.
- Переписывать внутренности существующих команд, если это не меняет
  их JSON-схему входа/выхода.
- Добавлять опциональные параметры с дефолтами (backward compatible).
- Оптимизировать ACAPI-вызовы внутри существующих команд.

### Нельзя

- Менять namespace команды (`TapirCommand` — `final`).
- Ломать JSON-схему существующих команд.
- Удалять команды без явного запроса пользователя.
- Пытаться сделать свой HTTP-сервер / endpoint — невозможно.
- Пушить в `main` или `work` — только `fresh` (или рабочая ветка
  текущей задачи).

### Под вопросом

- Возвращать ли изменения в upstream. Отложено: сначала пусть bulk
  докажет себя 1-2 месяца, потом решаем про PR.
- Мержить ли `fresh` и `work`. Пока нет — `work` служит каналом для
  экспериментов с upstream-совместимостью.

---

## 9. Замеры

| Дата | Что меряли | JSON | Bulk | Отношение |
|---|---|---|---|---|
| — | — | — | — | — |

---

## 10. История решений

**2026-10-05:** Решено делать base64+msgpack+zstd в форке, а не ждать
upstream. Причины: upstream не двигается; наш форк уже сильно впереди;
«честность» = правильно указать fork в README и не жаловаться в
upstream issues, а не отдавать всё туда немедленно.

**2026-10-05:** Решено вендорить msgpack-cxx и zstd в репо, а не
требовать vcpkg/brew. Причина: аддон должен собираться «из коробки».

**2026-10-05:** Написан `BulkPing` (шаг 1). Транспорт base64
Python → C++ работает. Дальше — zstd, потом msgpack.

**2026-10-05:** `WORK_BRANCH` переключён с `ai_bridge/work` на `fresh`.
Bridge теперь пишет в `fresh`, `work` остаётся бэкапом.

**2026-10-05:** В UI Bridge (вкладка GitHub) добавлена read-only
строка «Рабочая ветка: fresh». Значение = `WORK_BRANCH`.

---

## 11. Контакты и ссылки

- **Форк:** https://github.com/kramolala-ui/tapir-archicad-automation (ветка `fresh`)
- **Upstream:** https://github.com/ENZYME-APD/tapir-archicad-automation
- **Автор форка:** kramolala-ui
- **Локальная копия:** `C:\Python_projects\tapir-custom\`

---

_Если этот файл устарел — правь его. Он живёт здесь как точка
входа для новых сессий и чтобы не терять контекст между перерывами._
