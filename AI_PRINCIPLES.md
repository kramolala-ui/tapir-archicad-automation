# AI Principles — работа с форком Tapir-archicad-automation

**Назначение.** Стартовый контекст для LLM, которая работает над
нашим форком Tapir-аддона. Прочитай целиком до того, как предлагать
правки.

**Аудитория.** LLM через AI Bridge (`===AI_REQUEST===`,
`===AI_CHANGES===`), работающая в ветке `fresh`.

---

## 0. Как подключить этот файл к промпту AI Bridge

### 0.1. Пользователь: вкладка «Промпт» → тип «GitHub»

В поле «Дополнительные инструкции» (или в конце github-промпта)
добавь строку:

```
Перед первым ответом в новой сессии прочитай файл:
  github://kramolala-ui/tapir-archicad-automation@fresh/AI_PRINCIPLES.md
и держи его в контексте до конца сессии.
```

Сохрани промпт. Со следующего запуска Bridge подтянет файл сам.

### 0.2. Пользователь: вкладка «GitHub»

- **Репозиторий:** `kramolala-ui/tapir-archicad-automation`
- **Токен:** PAT со scope `repo` (или `Contents: Read+Write`).
- **Рабочая ветка:** должна показывать `fresh`. Это значит, что
  `WORK_BRANCH` в `agent_tools/github_provider.py` равен `"fresh"`.
  Если показывает `ai_bridge/work` — правь константу + перезапускай
  Bridge (см. 4b).

### 0.3. LLM: первый ход в новой сессии

Всегда — `===AI_REQUEST===` с одним файлом:

```json
[{"action": "request",
  "files": ["github://kramolala-ui/tapir-archicad-automation@fresh/AI_PRINCIPLES.md"],
  "reason": "стартовый контекст"}]
```

Дальше — работа по разделам, ссылайся на них («см. 4c про статус
bulk-транспорта»).

---

## 1. Что это за проект

Форк [ENZYME-APD/tapir-archicad-automation](https://github.com/ENZYME-APD/tapir-archicad-automation)
от `kramolala-ui`.

- **Upstream** — 105 команд, JSON API к Archicad.
- **Наш форк** — 260+ команд: MEP, IFC, слои, аннотации, solid
  operations, rotate elements, копирование/поворот, зоны, ключевые
  ноты, скрипт-UI, **bulk-команды (BulkPing в разработке)**.
- **Зачем форк:** upstream не двигается; нужны свои команды и
  оптимизации, на которые upstream не пойдёт.

**Рабочая ветка:** `fresh` (не `work`, не `main`, не `cpp_bridge/work`).

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
  → тестирует живой проект через AI Bridge (см. 2a)
```

### Правила правок

1. Всегда проверяй, что путь в `@fresh`.
2. До правки — `stat` или `read`.
3. Новый `.hpp/.cpp` в `Sources/` подхватывается CMake-глобом
   автоматически. Править `CMakeLists.txt` нужно только для
   **внешних зависимостей** (см. 4c).
4. Регистрация команды — в `AddOnMain.cpp::Initialize`, **до** блока
   «Loading the palette singleton…» (баг #516).
5. Один `AI_CHANGES` = 3-5 правок. `write` большого файла —
   отдельным блоком.
6. **Не делай `replace` для файла, только что созданного `write`** —
   raw-кэш не успевает, `fetch_file` даёт 404, батч откатится.
   Правь через повторный `write`.

---

## 2a. Сборка, установка и проверка через AI Bridge

### Сборка — ТОЛЬКО через GitHub Actions

**На машине разработчика НЕТ локального API Dev Kit.** Не ищи
`ACAPinc.h` / `ACAPI*.hpp` на диске, не предлагай «проверим сигнатуру
локально», не строй планы с локальной компиляцией — это невозможно.

Цикл сборки:

1. LLM пушит правку в `fresh`.
2. GitHub Actions собирает автоматически (триггер — `workflow_dispatch`
   вручную или push tag). Обычно ~5 мин на Windows-матрицу 25-29.
3. Artifacts внизу страницы workflow: `Tapir Add-On AC26 Win`,
   `... AC26 Mac`, `Tapir Installer Win/Mac`.

### ⚠ Всегда `Run workflow`, а не `Re-run jobs`

`Re-run jobs` воспроизводит **тот же коммит**, что в исходном run —
все правки, сделанные после него, не подхватятся. Симптом: снова
падает на ошибке, которую мы уже «исправили».

Правильно:
1. Actions → «Archicad Add-On Build and Release».
2. Справа «Run workflow», Branch=`fresh`.
3. Новый run возьмёт свежий HEAD.

### Установка в Archicad 26 (Win)

1. Скачать `Tapir Add-On AC26 Win` (zip).
2. Распаковать, там `TapirAddOn_AC26_Win.apx`.
3. Положить в папку Add-On'ов:
   `C:\Users\<user>\Documents\GRAPHISOFT\Add-Ons\Archicad 26\`
   (точный путь — Options → Add-On Manager → «Открыть папку»).
4. **Полностью закрыть Archicad** и открыть заново — без этого
   аддон не перезагрузится.
5. Options → Add-On Manager → убедиться, что `TapirAddOn` есть и
   активен.

### Проверка через AI Bridge (Python-скрипт)

Когда Archicad запущен с новым аддоном, проверяем команды через
`===AI_PROGRAM=== action=script`:

```python
from plugins.archicad_plugin.tapir_commands import TapirConnection
conn = TapirConnection(port=19723)
r = conn.run_command("BulkPing", {"payload_b64": "SGVsbG8="})
print(r)
# Ожидаем: {'size': 5, 'preview_hex': '48656c6c6f',
#            'compression': 'none', 'zstd_version': 10506}
```

Для zstd:

```python
import base64, zstandard
from plugins.archicad_plugin.tapir_commands import TapirConnection
conn = TapirConnection(port=19723)
data = b"Hello, bulk transport with zstd! " * 100
compressed = zstandard.ZstdCompressor().compress(data)
r = conn.run_command("BulkPing", {
    "payload_b64": base64.b64encode(compressed).decode("ascii"),
    "compression": "zstd",
})
print(r)
# Ожидаем: size=3200, compression='zstd', zstd_version=10506
```

Если `zstd_version` = `10506` — это 1.5.6, наша версия, всё правильно.
Если 0 или другое число — линковка подхватила чужую zstd (см. 5).

### Порт Tapir

По умолчанию `19723`. Если Archicad запущен не на этом порту,
порт покажет `probe_tapir` (см. `agent_tools/workspace/tapir_probe.py`)
или сам Tapir palette в UI Archicad.

---

## 3. Архитектура аддона

### CommandBase

Базовый класс — `Sources/CommandBase.hpp`.

Наследники реализуют:
- `GetName()` — имя команды.
- `GetInputParametersSchema()` — JSON-схема входа.
- **`GetRawResponseSchema()`** — JSON-схема ответа (НЕ
  `GetResponseSchema` — тот `final` в базовом!).
- `Execute(params, processControl)` — работа.

`GetNamespace()` — `final`, всегда `TapirCommand`. Свой HTTP-сервер
или endpoint через API-команды **невозможен**.

### Поток данных

```
Python → JSON → Graphisoft runtime → GS::ObjectState
       → CommandBase::Execute(params) → ответ ObjectState
       → Graphisoft runtime → JSON → Python
```

**Пункт «Graphisoft runtime парсит JSON» — самый медленный.**
Единственный способ обойти — упаковать данные в одну строку
`payload_b64` и парсить самим внутри `Execute` (см. 4).

### Группы команд

CommandGroup — организационная сущность, создаётся в `Initialize`.
Порядок: Application, Project, Element, Element Creation, Attribute,
Property, Classification, IFC, MEP, SolidElementOperation, ScriptUI,
Developer, **Bulk**.

---

## 4. Bulk-транспорт

### Проблема

3000 свойств × 20000 объектов упираются в:
1. Graphisoft JSON-парсер (большие JSON не переваривает).
2. Лимит батча ~500 элементов (40 HTTP-раундов).
3. Одиночные ACAPI-вызовы в цикле (см. 4a).

### Решение: base64(msgpack(zstd(data)))

```
Python:
  data → msgpack → zstd → base64 → "payload_b64": "..."

C++:
  payload_b64 → base64 → zstd-decompress → msgpack-parse
              → обработка порциями ВНУТРИ одного вызова
```

Выигрыш: 1 запрос вместо 40; zstd 5-10× по объёму; base64 съедает
33%, но это меньше выигрыша.

### Статус (2026-10-06)

| # | Шаг | Статус |
|---|---|---|
| 1 | `BulkPing` с base64 | **✓ работает** |
| 2 | + zstd (FetchContent v1.5.6) | **✓ работает** |
| 3 | + msgpack-cxx (header-only) | TODO |
| 4 | `Bulk.GetPropertyValues` | TODO |
| 5 | Пакетный ACAPI (см. 4a) | TODO |
| 6 | Остальные bulk-команды | TODO |

### Подключение zstd в CMakeLists.txt

zstd тянется через `FetchContent`:

```cmake
FetchContent_Declare (
    zstd
    URL          .../zstd-1.5.6.tar.gz
    URL_HASH     SHA256=8c29e06...
    SOURCE_SUBDIR build/cmake   # ВАЖНО: корневой CMakeLists — SDK, не рабочий
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable (zstd)

# Graphisoft CMakeCommon использует plain target_link_libraries
# (без PRIVATE/PUBLIC). Значит и мы должны без ключевых слов.
target_link_libraries (AddOn ${ZSTD_LINK_TARGET})
target_include_directories (AddOn SYSTEM PRIVATE
    ${zstd_SOURCE_DIR}/lib
    ...)
```

Правильные target-имена для 1.5.6: `libzstd_static` (используем),
fallback `zstd_static` / `libzstd` / `zstd`.

---

## 4a. Известные узкие места ACAPI

Не оптимизировать **до** того, как bulk-транспорт заработает — иначе
непонятно, что дало выигрыш.

| Файл / метод | Что не так |
|---|---|
| `PropertyCommands.cpp::GetPropertyValuesOfElementsCommand` | `for (element)` + `GetPropertyValuesByGuid` — 500 вызовов. Пакетный: `ACAPI_Element_GetPropertyValues(elemGuidsArray, ...)` |
| `PropertyCommands.cpp::SetPropertyValuesOfElementsCommand` | То же + `SetProperty` по одному = 1000+ вызовов |
| `PropertyCommands.cpp::GetPropertyValuesOfAttributesCommand` | `ACAPI_Attribute_GetPropertyValuesByGuid` в цикле |
| `ClassificationCommands.cpp::Get/SetClassificationsOfElementsCommand` | Цикл по элементам |
| `ElementGDLParameterCommands.cpp` | GDL через memo — пакетного API может не быть. Проверить `BMKillHandle` (утечка) |

Проверка сигнатур — только через CI (см. 2a): 1-3 ребилда пока не сойдётся.

---

## 4b. Перезапуск AI Bridge

`agent_tools/*.py` (`github_provider.py`, `dialog.py`, `bridge.py`,
`protocol.py`, `response_handler.py`, `request_handler.py`)
**не подхватываются на лету**. Правишь — закрываешь окно Bridge,
открываешь заново.

Симптом «правка применилась, но поведение старое» = забыл перезапустить.

Особенно важно при: смене `WORK_BRANCH`, правке UI-лэйаута, изменениях
в протоколе маркеров.

---

## 5. Известные грабли

### Graphisoft `target_link_libraries` — plain signature

`CMakeCommon.cmake` использует `target_link_libraries` **без ключевых
слов**. CMake запрещает смешивать plain и keyworded (`PRIVATE`/
`PUBLIC`) для одного target'а. Если добавить нашу зависимость — тоже
без ключевых слов.

Симптом: `CMake Error at CMakeLists.txt:NN (target_link_libraries):
The plain signature for target_link_libraries has already been used`.

### `CommandBase::GetResponseSchema` — `final`

Наследники переопределяют **`GetRawResponseSchema`**, а не
`GetResponseSchema`.

Симптом: `error C3248: 'CommandBase::GetResponseSchema': function
declared as 'final' cannot be overridden`.

Почему так: Archicad использует response schema для валидации;
failing command возвращает `{"error": {...}}`, что схема не принимает,
поэтому `GetResponseSchema` в базовом возвращает пустоту.
Документированная схема — через `GetRawResponseSchema`.

### FetchContent zstd: `SOURCE_SUBDIR build/cmake`

В корне репозитория zstd лежит «SDK»-CMakeLists (не создаёт
библиотечных таргетов). Рабочий проект — в `build/cmake`. Без
`SOURCE_SUBDIR build/cmake` в `FetchContent_Declare` CMake подхватит
корневой файл и таргет `libzstd_static` не появится.

Симптом: `CMake Error at CMakeLists.txt:NN (message): zstd: не найден
CMake target после FetchContent.`

### Матрица workflow: `fail-fast: false` обязателен

По умолчанию GitHub Actions `fail-fast: true` — падение одной версии
(например AC30 с RC-DevKit) **отменяет все остальные**. Нам нужны
AC26 независимо от судьбы AC28/29/30.

В `.github/workflows/archicad_addon.yml` в обоих матрицах
(`build_win`, `build_mac`) должен быть `fail-fast: false`.

### AC30: RC-DevKit недоступен

В матрице AC30 качается с `dl.graphisoft.com/release-candidate/30/`,
который сейчас падает. Пока не появится DevKit 30 на GitHub releases —
строка закомментирована в обеих матрицах. Вернуть, когда опубликуют.

### `Re-run jobs` не подхватывает свежие правки

См. 2a. Всегда `Run workflow` заново.

### AC26: `CreateObjects` падает с `-2130313112`

`ACAPI_Element_Create` не создаёт Object на AC26 в некоторых
конфигурациях. Обход: `RotateElementsByAngle` с `withCopy: true` +
`ModifyObjects`.

### `ModifyObjects` не ставит `dimensions.x` напрямую

При `useFixSize: true` (по умолчанию) `dimensions.x` игнорируется.
Явно `useFixSize: false` в том же вызове. Длина — через
**`MEP_StraightLength`** (GDL-параметр). `A` — производный.

### GDL-параметры пересчитываются асинхронно

`SetGDLParametersOfElements` возвращает `success: true` сразу,
но значение читается старым ещё 100-500 мс. При проверке —
`time.sleep(0.3-0.5)` или перечитать дважды.

### `EntityType.BIM` не существует

Правильный тип для Object — `EntityType.BIM_OBJECT`.

### `archicad_get_elements` не пишет `libPart` в параметры

Возвращает `element_type`, `bbox_*`, `ArchicadDetail/origin.*`,
`story_index`, но **не** `libPart.name`. Определять библиотечный
элемент — через `GetDetailsOfElements` → `details.libPart.name`.

### Палитра загружается последней

`TapirPalette::Instance()` в конце `Initialize` обёрнута в `try/catch`.
Если упадёт — команды **до** неё работают, **после** — нет. Всегда
добавляй команды **до** этого блока (#516).

### `@fresh` в пути ≠ запись в fresh

Bridge пишет всегда в `WORK_BRANCH`. Алиасы `@work` и `@fresh`
резолвятся в `WORK_BRANCH`. Чтение честно по указанному ref.

---

## 6. Стиль кода

- C++17 (AC26-28), C++20 (AC29+).
- Отступ — 4 пробела, стиль GS/Graphisoft.
- Пробел перед скобкой: `Foo (args)`, `if (x)`, `for (...)`.
- `GS::ObjectState` для входов/выходов.
- Namespace — `TapirCommand`, единый.
- Undoable command: `ACAPI_CallUndoableCommand("Name", [&]() { ... })`.
- Комментарии — по-русски в новых файлах.

---

## 7. Тесты

Юнит-тестов в аддоне **нет** — архитектура Add-On не позволяет.
Проверка — только на живом Archicad через AI Bridge (см. 2a).
Замеры — в разделе 9.

---

## 8. Границы

### Можно
- Новые команды.
- Переписывать внутренности существующих (схема входа/выхода неизменна).
- Опциональные параметры с дефолтами.
- Оптимизировать ACAPI-вызовы.

### Нельзя
- Менять namespace (`final`).
- Ломать JSON-схему существующих команд.
- Удалять команды.
- Свой HTTP-сервер / endpoint.
- Пушить в `main` или `work` — только `fresh`.

### Под вопросом
- PR в upstream — отложено (пусть bulk докажет себя 1-2 месяца).
- Мерж `fresh` и `work` — пока нет.

---

## 9. Замеры

### 2026-10-06 — zstd-транспорт (BulkPing, Archicad 26, проект «Шаблон гидравлики IFC»)

| Тест | Данные | zstd на Python | Через Tapir | Выигрыш |
|---|---|---|---|---|
| Текст × 100 | 3.3 KB | 52 B (ratio 63×) | round-trip OK, size совпал | — |
| Random 1 MB | 1 MB | 1 MB (ratio ~1.0) | 54 ms round-trip | — |
| Повторы 2 MB | 2 MB | 82 B (ratio 24390×) | **16 ms / запрос** | — |

**Ключевые цифры:**

- Round-trip на сжатом payload — **~16 ms**. Даже при базовом сценарии «500 элементов × 3000 свойств» это в разы быстрее, чем 40 HTTP-раундов по 500 элементов.
- **Совместимость zstd:** Python `libzstd 1.5.7` → C++ `libzstd 1.5.6` — распаковка корректна. Forward-compat подтверждён на уровне сжатия 22 (макс.).
- **Обработка ошибок:** битый zstd-фрейм → структурная ошибка «zstd: not a valid zstd frame» (не падение).
- Найдены и исправлены: (a) `compression` пустой → `'none'`; (b) unknown compression возвращал exception вместо ошибки (баг `GS::UniString::Printf` с `%T`).

### Итог

| Дата | Что | JSON | Bulk | Отношение |
|---|---|---|---|---|
| 2026-10-06 | zstd-транспорт (2 MB повторов) | — | 16 ms/запрос | baseline |

---

## 10. История решений

**2026-10-05:** base64+msgpack+zstd в форке, не ждём upstream.

**2026-10-05:** Вендорим msgpack-cxx и zstd в репо (FetchContent),
без vcpkg/brew.

**2026-10-05:** `WORK_BRANCH` переключён с `ai_bridge/work` на `fresh`.

**2026-10-05:** В UI Bridge (вкладка GitHub) добавлена read-only строка
«Рабочая ветка: fresh».

**2026-10-06:** `BulkPing` (шаг 1) собран и работает: base64 + zstd.
`zstd_version=10506` (1.5.6) в ответе. Следующий шаг — msgpack.

**2026-10-06:** В workflow `archicad_addon.yml` добавлен `fail-fast: false`
в обе матрицы; AC30 временно закомментирован (RC-DevKit недоступен).

**2026-10-06:** `CommandBase::GetResponseSchema` выяснен как `final` —
наследники переопределяют `GetRawResponseSchema`.

**2026-10-06:** Graphisoft `CMakeCommon.cmake` использует plain
`target_link_libraries` — все добавления без `PRIVATE`/`PUBLIC`.

**2026-10-06:** zstd подключается через `FetchContent` со
`SOURCE_SUBDIR build/cmake`.

---

## 11. Контакты и ссылки

- **Форк:** https://github.com/kramolala-ui/tapir-archicad-automation (ветка `fresh`)
- **Upstream:** https://github.com/ENZYME-APD/tapir-archicad-automation
- **Автор форка:** kramolala-ui
- **Локальная копия:** `C:\Python_projects\tapir-custom\`

---

_Если этот файл устарел — правь его. Он живёт здесь как точка
входа для новых сессий._
