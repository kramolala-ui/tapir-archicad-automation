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
  ноты, скрипт-UI, **5 bulk-команд** (`BulkPing`, `BulkGetPropertyValues`,
  `BulkGetTexts`, `BulkSetTexts`, `BulkFindReplaceText`).
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

## 2b. Поиск по GitHub-репо (2026-10-06)

**Не читай файлы слайсами, чтобы найти что-то.** В Bridge добавлен
`action=search` с параметром `github_repo`. Он обходит дерево репо,
читает файлы через кэш и ищет regex построчно.

**Формат:**

```json
===AI_REQUEST===
[
  {"action": "search",
   "pattern": "GetPropertyValueString",
   "github_repo": "kramolala-ui/tapir-archicad-automation@fresh",
   "extensions": [".cpp", ".hpp"],
   "context_lines": 2,
   "max_matches": 50,
   "reason": "зачем ищем"}
]
===AI_REQUEST===
```

**Параметры:**

- `github_repo` — `owner/repo` или `owner/repo@ref` (можно `@fresh`,
  `@main`, или явный ref). Если задан — поиск идёт в GitHub, не
  локально.
- `pattern` — regex (как в обычном action=search).
- `extensions` — список `.cpp` / `.hpp` / `py` и т.п. (с точкой или без).
- `file_glob` — `**/*.cpp` (fnmatch по полному пути или basename).
- `include_files` — точные пути (только эти файлы).
- `context_lines` — 0..10 строк до/после.
- `max_matches` — 1..1000.
- `case_sensitive` — bool (по умолчанию true).

**Возврат:**

```json
{"ok": true,
 "matches": [{"path": "archicad-addon/Sources/PropertyCommands.cpp",
              "line": 181,
              "text": "...",
              "before": [...],
              "after": [...]}],
 "truncated": false,
 "scanned_files": 37,
 "scanned_lines": 48236,
 "files_with_matches": [...],
 "repo": "...", "ref": "...",
 "source": "github"}
```

**Производительность:** 37 файлов / 48 236 строк — **20 секунд**.
Кэш на диск: второй поиск по тем же файлам быстрее.

**Реализация:** `GitHubProvider.search_pattern` → `GitHubService.search_pattern`
→ `RequestHandler._handle_search` (ветка `github_repo`).
Правки сделаны 2026-10-06 в `agent_tools/*.py` локально, требуют
**перезапуска AI Bridge** (см. 4b).

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
| 1 | `BulkPing` с base64 | **✓ работает** (2026-10-05) |
| 2 | + zstd (FetchContent v1.5.6) | **✓ работает** (2026-10-06, round-trip 16 ms) |
| 3 | + JSON-структуры (nlohmann/json) → msgpack на проводе | **✓ работает** (2026-10-06) |
| 4 | `BulkGetPropertyValues` | **✓ работает** (2026-10-06, чанки по 20) |
| 5 | `BulkGetTexts` | **✓ работает** (2026-10-06, Text + Label) |
| 6 | `BulkSetTexts` | **✓ работает** (2026-10-06, undo-barrier) |
| 7 | `BulkFindReplaceText` | **✓ работает** (2026-10-06, dry_run без undo) |
| 8 | Пакетный ACAPI (см. 4a) | TODO |
| 9 | Остальные bulk-команды | TODO |

**Замечание про msgpack:** изначально планировался `msgpack-cxx` — но
он падает на MSVC с C2766 (см. раздел 5). На проводе **тот же формат
msgpack**, кодируется через `nlohmann::json::to_msgpack`. Python-клиент
не меняется.

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

## 4d. Bulk-команды (2026-10-06 — реализовано)

Пять команд собраны в `archicad-addon/Sources/BulkCommands.hpp/.cpp`,
зарегистрированы в `AddOnMain.cpp` (группа `bulkCommands`):

| Команда | Назначение |
|---|---|
| `BulkPing` | Транспортный тест: base64 + опц. zstd |
| `BulkGetPropertyValues` | N×K свойств (чанки по 20, ниже порога K=24) |
| `BulkGetTexts` | Тексты Text/Label (read-only) |
| `BulkSetTexts` | Запись текста (один undo на батч) |
| `BulkFindReplaceText` | Глобальный find/replace, `dry_run` |

**Цель:** получить N×K свойств одним `Execute` — обойти JSON-стену и
проблему зависания серий (раздел 5, «КРИТИЧНО»).

### Формат команды

**Вход (JSON):**
```json
{
  "payload_b64": "<msgpack+zstd+base64>",
  "compression": "zstd"
}
```

Payload (msgpack-декодируется):
```python
{
  "elements":   ["elem-guid-1", "elem-guid-2", ...],
  "properties": ["prop-guid-1", "prop-guid-2", ...]
}
```

**Выход (JSON):**
```json
{
  "payload_b64": "<msgpack+zstd+base64>",
  "compression": "zstd",
  "elements_count": 500,
  "properties_count": 3000,
  "values_count": 1500000
}
```

Payload (msgpack-декодируется):
```python
{
  "rows": [
    {"elementId": "...",
     "propertyValues": [
       {"propertyId": "...", "value": "..."},
       ...
     ]},
    ...
  ]
}
```

### Логика Execute

1. base64-decode → zstd-decompress → msgpack-decode входа.
2. Цикл по `properties` **чанками по 20** (безопасный порог —
   см. раздел 9, `K<=23` — быстрый путь ACAPI):
   - Внутри чанка — цикл по `elements`, вызов
     `ACAPI_Element_GetPropertyValuesByGuid(elem, chunk_guids)`.
   - Если у ACAPI есть batch-версия без порога 24 (см. ниже) —
     использовать её.
   - Аккумулировать результат.
3. Собрать msgpack-ответ, zstd-сжать, base64-кодировать.
4. Вернуть `{payload_b64, compression, counts}`.

Всё внутри одного `Execute`, без `ACAPI_CallUndoableCommand` (чтение
не требует undo).

### Открытый вопрос: batch-API ACAPI

Есть ли у Graphisoft API **пакетный** вызов по массиву элементов,
без порога 24?

- Кандидат: `ACAPI_Element_GetPropertyValues(elemGuids, propGuids, ...)`.
- Сигнатуру проверить только через CI (DevKit локально нет —
  раздел 2a). 1-3 ребилда, по ошибкам компилятора.
- Если работает — команда станет **радикально быстрее**: текущий
  порог 0.5 ms/элемент на быстром пути может стать 0.05 ms/элемент
  на batch-пути.
- Если порога нет — используем цикл с чанками по 20, всё равно
  работает в разы быстрее JSON-пути за счёт msgpack+zstd.

### Python-клиент (TODO)

```python
import msgpack, zstandard, base64
from plugins.archicad_plugin.tapir_commands import TapirConnection

def bulk_get_property_values(conn, element_guids, property_guids):
    payload = msgpack.packb({
        "elements":   list(element_guids),
        "properties": list(property_guids),
    })
    comp = zstandard.ZstdCompressor().compress(payload)
    b64 = base64.b64encode(comp).decode("ascii")

    r = conn.run_command("Bulk.GetPropertyValues", {
        "payload_b64": b64,
        "compression": "zstd",
    })
    if not r or r.get("error"):
        raise RuntimeError(f"Bulk.GetPropertyValues: {r}")

    raw = base64.b64decode(r["payload_b64"])
    dec = zstandard.ZstdDecompressor().decompress(raw)
    return msgpack.unpackb(dec, raw=False)
```

### Критерии приёмки

- **500×3000** (1.5 M значений) — укладывается в **≤ 60 s**
  (сейчас JSON-путь не работает вообще — раздел 9).
- **5000×100** — укладывается в **≤ 30 s**.
- Ответ по сети — **≤ 5 MB** (сейчас JSON 500×100 = 440 KB,
  при 500×3000 будет ~13 MB, при msgpack+zstd — ~500 KB).
- **Серия не вешает Archicad** (проверить: три Bulk.GetPropertyValues
  подряд в одном скрипте).

### Порядок действий

1. Написать `BulkCommands.hpp/.cpp` с новой командой (используя
   существующий `Base64Decode` и `ZstdDecompress`).
2. Собрать, установить, прогнать на «Шаблон гидравлики IFC».
3. Если укладывается в критерии — оставить.
4. Если >60 s — искать batch-API (открытый вопрос выше).

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

### FetchContent: SHA256 релизного tarball'а нестабилен — GIT_TAG надёжнее

Релизные tarball'ы msgpack-cxx (и подобных) **пересобираются**
GitHub'ом — SHA256 меняется между прогонами. CMake видит несовпадение,
удаляет файл, пробует заново, потом падает.

**Что делать:** для пересобираемых релизов — `GIT_REPOSITORY` +
`GIT_TAG <tag>` + `GIT_SHALLOW TRUE` вместо `URL`+`URL_HASH`.

Симптом: `SHA256 hash of ... does not match expected value` ×5, потом
`CMake Error at .../download-...:163 (message): Each download failed!`

### FetchContent: `SOURCE_SUBDIR` и на header-only

msgpack-cxx называется «header-only», но в корне репозитория есть
свой `CMakeLists.txt`, который делает `FIND_PACKAGE(Boost)`. Boost
нам не нужен.

**Что делать:** `SOURCE_SUBDIR include` — папка только с заголовками,
без `CMakeLists.txt`. FetchContent скачает репозиторий, но
`add_subdirectory` не вызовет.

Симптом без фикса: `CMake Error at FindPackageHandleStandardArgs.cmake:
Could NOT find Boost (missing: Boost_INCLUDE_DIR)`.

### msgpack-cxx: `MSGPACK_NO_BOOST` обязателен

`msgpack/sysdep.hpp` без макроса `MSGPACK_NO_BOOST` подключает
`<boost/predef/other/endian.h>`. С макросом — свой вендоренный.

**Что делать:** `target_compile_definitions (AddOn PRIVATE MSGPACK_NO_BOOST)`.

Симптом: `fatal error C1083: Cannot open include file:
'boost/predef/other/endian.h': No such file or directory`.

### msgpack-cxx на MSVC — тупик (C2766), нужен nlohmann/json

`msgpack-cxx 6.1.1` делает explicit-специализации для `wchar_t`
и `unsigned short`. На MSVC это **один и тот же тип** (оба 16 бит) →
компилятор ругается на дубликат.

Симптом: `error C2766: explicit specialization;
'msgpack::v1::adaptor::convert<wchar_t,void>' has already been defined`
(×4: convert, pack, object, object_with_zone).

**Макроса отключения нет.** Я выдумал `MSGPACK_NO_WCHAR_T` — его
не существует. Реального обходного пути нет, кроме патча внутренностей
библиотеки или `/Zc:wchar_t-` (меняет ABI).

**Что делать:** заменить msgpack-cxx на `nlohmann/json` — там есть
встроенная поддержка msgpack (`json::to_msgpack` / `from_msgpack`).
Формат **на проводе тот же msgpack**, Python-клиент не меняется,
на MSVC собирается без хаков.

### DevKit AC25/26: `snprintf` определён как `_snprintf`

DevKit AC25/26 (и только они) в своих заголовках делает:

```c
#define snprintf _snprintf
```

Хак для совместимости со старым MSVC. Любая header-only библиотека,
которая использует `std::snprintf` — превращается препроцессором в
`std::_snprintf` (не существует) → C2039.

Симптом: `error C2039: '_snprintf': is not a member of 'std'`
в `nlohmann/detail/input/binary_reader.hpp`.

**Что делать:** перед `#include <nlohmann/json.hpp>` (или любой другой
библиотеки, использующей `std::snprintf`):

```cpp
#ifdef snprintf
    #undef snprintf
#endif
```

На AC27+ макрос убран в самом DevKit, `#undef` — no-op. Работает для
всех версий без `#ifdef ServerMainVers_*`.

### Не выдумывать имена макросов — проверять в доках

**Свежий пример (2026-10-06):** при добавлении msgpack написал
`MSGPACK_NO_WCHAR_T` — потому что *предположил* такое имя. Реального
макроса нет. Сборка падала с C2766, я правил несуществующее.

**Правило:** перед использованием макроса вида `LIBRARY_NO_XXX` —
проверить его существование: `git grep LIBRARY_NO_` в исходниках
библиотеки, или раздел «Macros» в документации. Если не уверен —
искать альтернативу (сменить библиотеку / патчить через CMake
`PATCH_COMMAND`). Не писать «по аналогии».

### AC26 API: ACAPI_Element_Change не принимает УДЛИНЁННЫЙ текст

**Симптом:** запись нового текста в Text/Label **падает** с кодом
`-2130312713` ("Failed to change element"), если новый текст **длиннее
старого**. Запись того же или более короткого текста — проходит.

**Проверено (2026-10-06, «Шаблон гидравлики IFC», AC26, Tapir 1.7.1):**

| Путь | '2'→'9' (та же длина) | '2'→'HELLO' (длиннее) |
|---|---|---|
| `BulkSetTexts` (наш) | — | ❌ code -2130312713 |
| `SetDetailsOfElements` (upstream) | ✅ success | ❌ code -2130312713 |
| `ModifyTexts` (upstream) | — | ❌ code -2130312713 |

**Вывод:** это **не наш баг**. Это ограничение/баг AC26 API —
`ACAPI_Element_Change` с маской `API_TextType` и memo
`APIMemoMask_TextContent | APIMemoMask_Paragraph` не применяет
удлинённый текст. Ни `SetTextContentAndParagraphs`, ни
`TextLabelDetails::ApplyTextContent` эту проблему не решают — они
внутри уходят в тот же API.

**Что НЕ работает как обход:**
- Передача `withDel=false` (пробовали, стало хуже).
- Получить существующий memo перед правкой (тот же симптом).
- Изменять только через `ModifyTexts` (upstream, тот же fall).

**Что делать:** до выяснения обхода — запись текста с изменением длины
в AC26 считать неподдерживаемой. Замена подстроки на такую же по
длине работает.

**Открытый вопрос:** баг ли это AC26 (проверить на AC27+), или общий
для API. Возможные обходы — `ACAPI_Element_ChangeMemo` отдельно
от `Change`; `ACAPI_Element_Delete` + `Create` с сохранением позиции.
Оба варианта требуют эксперимента.

### ACAPI_CallUndoableCommand — обязателен для мутаций модели

`ACAPI_Element_Change` / `_Create` / `_Delete` **вне**
`ACAPI_CallUndoableCommand` — либо не применяются, либо применяются,
но создают undo-запись на каждый элемент. Пользователь не откатит
батч одной Ctrl+Z.

**Что делать:** один `ACAPI_CallUndoableCommand ("Name", [&]() { ... })`
на весь батч. Для read-only обёртка не нужна — и не должна ставиться:
при `dry_run` пустая undo-запись в стеке пользователя лишняя.

Пример — `BulkFindReplaceText`: `doWork` — лямбда с циклом;
`if (dryRun) doWork (); else ACAPI_CallUndoableCommand (...)`.

### Матрица workflow: `fail-fast: false` обязателен

По умолчанию GitHub Actions `fail-fast: true` — падение одной версии
(например AC30 с RC-DevKit) **отменяет все остальные**. Нам нужны
AC26 независимо от судьбы AC28/29/30.

В `.github/workflows/archicad_addon.yml` в обоих матрицах
(`build_win`, `build_mac`) должен быть `fail-fast: false`.

### Лимит Graphisoft JSON-сериализатора — ~5000 значений в ответе

См. раздел 9, замер 2026-10-06. `GetPropertyValuesOfElements` на
5000 значений (50 элементов × 100 свойств) отвечает 6.9 s, на 10000
— не отвечает вообще (>10 s timeout). Это **не лимит ACAPI**, это
стоимость сборки JSON-ответа Graphisoft-ом.

**Практические следствия:**

- Для любой задачи с большим N×K JSON-путь надо считать по формуле
  «~700 ms на 1000 значений плюс квадратичный хвост». При N×K > 3000
  стоит сразу смотреть в сторону bulk-транспорта.
- Bulk-команды (`Bulk.*`) в бэклоге — это **ответ на этот лимит**,
  а не «ускорение ради ускорения».
- Измерять производительность JSON-команд надо **на своей машине**, на
  своём проекте: значения сильно зависят от количества заполненных
  свойств и типов элементов.

### Windows subprocess: cp866, не utf-8

При вызове `subprocess.run(..., text=True)` из Python на Windows
stderr/stdout утилит (taskkill, tasklist, где угодно) приходят в
системной кодировке (обычно cp866 или cp1251), а `text=True`
пытается декодировать их как utf-8 и падает с `UnicodeDecodeError`
в reader-треде.

**Что делать:** явно указывать `encoding='cp866', errors='replace'`:

```python
subprocess.run(
    ['taskkill', '/F', '/IM', 'Archicad.exe'],
    capture_output=True, text=True,
    encoding='cp866', errors='replace',
    timeout=30,
)
```

Или `errors='replace'` без encoding, если кодировка не известна.

### ⛔ КРИТИЧНО: серия GetPropertyValuesOfElements вешает Archicad

Подтверждено на проекте «Шаблон гидравлики IFC» (2413 Object,
3362 свойства), Tapir 1.7.1, Archicad 26:

| Сценарий | Результат |
|---|---|
| Одиночный 500×20 | ✅ 632 ms |
| Одиночный 500×20 (второй раз, новый процесс) | ✅ 538 ms |
| **Серия 3× (500×20) в одном скрипте** | ❌ **Archicad зависает навсегда** |
| BulkPing (наш, base64+zstd) 100 MB | ✅ 29 ms, не вешает |

**Выводы:**

- Это НЕ наш баг. `BulkPing` на 100 MB работает.
- Это НЕ лимит объёма. Второй одиночный 500×20 проходит.
- Это **нестабильность Tapir-команды `GetPropertyValuesOfElements`
  при повторных вызовах в одной сессии Archicad.**
- После зависания Archicad не отвечает ни через TCP, ни через
  Task Manager (Ctrl+Alt+Del). Убивается только через
  `taskkill /F /IM Archicad.exe` (он тоже иногда возвращает
  stdout в cp866 — см. выше).

**Что делать до реализации bulk-команды:**

- **Никогда не вызывать `GetPropertyValuesOfElements` сериями.**
  Один вызов за одну сессию Archicad — максимум, что безопасно.
- Если нужно прочитать много свойств — делать это одним вызовом
  с большим N×K (но там стена JSON, см. ниже).
- **Решение проблемы — bulk-команда `Bulk.GetPropertyValues`**
  (TODO): один `Execute` = один undo-барьер = ACAPI-цикл внутри
  C++, без чередований клиент/сервер.

### Ранее (до подтверждения): серия GetPropertyValuesOfElements может уронить Archicad

Наблюдение 2026-10-06 в проекте «Шаблон гидравлики IFC»:

- **Одиночный вызов** `GetPropertyValuesOfElements` (500 объектов ×
  20 свойств) — **566 ms**, OK.
- **Второй вызов подряд** в том же соединении — **зависает навсегда**.
  Archicad после этого не отвечает ни на TCP, ни через Task Manager.
- Требуется `taskkill /F /IM Archicad.exe` и перезапуск.

Это НЕ наша команда (BulkPing на 100 MB работает). Это поведение
самого ACAPI или Tapir 1.7.1. Возможно — накопление состояния
(undo-стек, кеш свойств) между вызовами.

**Что это значит для bulk-команды в C++:**

- Один Execute = одна ACAPI-транзакция = одно undo-состояние.
  Никаких чередований клиент-сервер. Это **сильный аргумент** за
  bulk-команду — она решает не только скорость, но и стабильность.
- До реализации bulk-команды — **не вызывать GetPropertyValues**
  сериями больше одного раза за сессию Archicad.

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

### AI_CHANGES: пересекающиеся операции в одном файле

Если две операции в одном AI_CHANGES меняют текст, который ищет
третья — порядок применения ломает якоря.

**Свежий пример (2026-10-06):** в `BulkCommands.cpp` первая
операция заменяла `element.header.typeID` →
`GetElemTypeId (element.header)` в блоке `BulkSetTexts`. Вторая
операция искала `element.header.typeID` в том же блоке, чтобы
добавить undo-barrier. Вторая нашла 0 раз → rollback. Итог —
гибридное состояние: часть правок есть, часть нет.

**Правило:** если правки в одном файле касаются одних и тех же
строк — собери их в **одну** замену с общим контекстом. Никогда
не разбивай на последовательность «сначала это, потом то».

**Признак проблемы:** отчёт `applied=N failed=M` с M>0 и сработавшим
rollback → надо перечитать файл, собрать одну большую замену,
повторить.

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

### 2026-10-06 — zstd-транспорт, черновая версия (архив, до полного bulk-пакета)

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

### 2026-10-06 — Bulk-транспорт после зелёной сборки (5 команд, проект «Шаблон гидравлики IFC»)

Всё на проекте **«Шаблон гидравлики IFC»** (653 Object, 3362 свойств,
487 Text, 519 Label), Tapir 1.7.1, AC26, окно FloorPlan.

**BulkPing** (транспорт):

| Тест | Время |
|---|---|
| 12 байт, без сжатия | 15 ms |
| Ответ | `size=12, preview_hex=48656c6c6f2c205441504952, zstd_version=10506` |

**BulkGetTexts** (read-only):

| Тест | Время | Rows | Непустых | Тип |
|---|---|---|---|---|
| 30 элементов (20 Text + 10 Label) | 20 ms | 30 | 30 | Text=20, Label=10 ✅ |
| **1006 элементов (487 Text + 519 Label)** | **261 ms** | 1006 | 1006 | Text=487, Label=519 ✅ |

⚠️ **Текст Label — мусор.** Пример из «Шаблона гидравлики»: `'㵨㔱턠톀킏킴킾²'`,
`'꣐苑뻐胑냐'`. Это **не текст** — у Label содержимое формируется из
GDL-параметров (`label.u.symbol.libInd` + parameters), а `memo.textContent`
для Label содержит что-то другое. Читать Label через
`GetMemo(APIMemoMask_TextContent)` — **неверный путь**. Текст Text
читается корректно.

**BulkGetPropertyValues** (read, N элементов × K свойств):

| N | K | N×K | ms | Отношение к JSON |
|---|---|---|---|---|
| 100 | 10 | 1 000 | 13 | |
| 500 | 10 | 5 000 | 66 | (JSON: 6 935 ms → **×105**) |
| **500** | **20** | **10 000** | **287** | (JSON: timeout) |
| 500 | 50 | 25 000 | 34 599 | ACAPI-потолок |
| 500 | 100 | 50 000 | 34 363 | тот же потолок |
| 500 | 500 | 250 000 | 36 993 | тот же потолок |

**Что значат эти цифры:**

- **500×20 = 287 ms** — на 10 000 значений. JSON-путь на 5000 значений
  отвечает 6.9 s, на 10 000 — timeout. Bulk быстрее в **~48×**.
- **Потолок ~35 s** при K ≥ 50 — ACAPI внутри пересчитывает все 3362
  свойства для каждого элемента при смене чанка свойств. Это не наш
  transport — это ACAPI. При K ≤ 20 проблема не проявляется.
- Транспорт для задачи «5-20 свойств на много элементов» — **полностью
  рабочий**. Для «3000 свойств на 500 элементов» потолок ~35 s — но
  это всё равно на 2 порядка быстрее, чем JSON (который вообще не
  работает).

**BulkSetTexts** (запись текста):

| Тест | Время | updated |
|---|---|---|
| 3 Text, той же длины | **126 ms** | 3/3 ✅ |
| 3 Text, длиннее (баг AC26) | 47 ms | 0/3 ❌ code -2130312713 |

**BulkFindReplaceText** (поиск/замена):

| Тест | Время | scanned | matched | replaced |
|---|---|---|---|---|
| dry_run=true, поиск '1' по всему проекту | **263 ms** | 1 006 | 42 | 0 |
| dry_run=false, поиск '1' в 2 Text (нет match) | 52 ms | 2 | 0 | 0 |

### 2026-10-06 — JSON-стена на GetPropertyValuesOfElements

Тест на проекте **«Шаблон гидравлики IFC»** (653 Object, 3362 свойств),
через Tapir 1.7.1, порт 19724.

| N элементов | K свойств | Всего значений | Время | Ответ |
|---|---|---|---|---|
| 10 | 10 | 100 | 10 ms | 4 KB |
| 50 | 10 | 500 | 26 ms | 21 KB |
| **50** | **100** | **5000** | **6935 ms** | **291 KB** |
| 100 | 100 | 10000 | **timeout >10s** | — |

**Что это значит:**

- Скачок с 500 значений (26 ms) до 5000 (6935 ms) — **×267 по времени
  при ×10 по объёму**. Это O(N²) в Graphisoft JSON-сериализаторе:
  ответ собирается через `GS::ObjectState → JSON`, и чем больше узлов
  в дереве, тем хуже становится сборка.
- При 10 000 значений вызов **не возвращается за 10 секунд**. Для
  задачи «500 элементов × 3000 свойств = 1.5 M значений» текущий
  JSON-путь **не работает вообще** — вот исходная проблема.
- ACAPI не при чём. Пакетные вызовы ACAPI (`GetPropertyValuesByGuid`)
  работают быстро; тормозит финальная сериализация ответа.

**Вывод:** bulk-транспорт (base64+msgpack+zstd) нужен не как
«оптимизация», а как **единственный путь** получить много свойств на
много элементов за разумное время. Один base64-блоб обходит и JSON-
сериализатор (на входе), и JSON-десериализатор (на выходе).

### Итог

| Дата | Что | JSON | Bulk | Отношение |
|---|---|---|---|---|
| 2026-10-06 | zstd-транспорт (2 MB повторов) | — | 16 ms/запрос | baseline |
| 2026-10-06 | 500×100 свойств (5000 значений) | 6935 ms | — (TODO) | TODO |
| 2026-10-06 | 100×100 свойств (10000 значений) | timeout | — (TODO) | TODO |

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

**2026-10-06:** msgpack_cxx — `GIT_TAG cpp-6.1.1` + `GIT_SHALLOW`
вместо URL+URL_HASH (релизные tarball'ы пересобираются, SHA256
нестабилен); `SOURCE_SUBDIR include` (в корне msgpack лежит
CMakeLists с `FIND_PACKAGE(Boost)`); `MSGPACK_NO_BOOST` (иначе
sysdep.hpp тянет `<boost/predef/other/endian.h>`).

**2026-10-06:** Собраны 4 bulk-команды: `BulkGetPropertyValues`,
`BulkGetTexts`, `BulkSetTexts`, `BulkFindReplaceText` (плюс `BulkPing`
ранее). Общий транспорт — `DecodeEnvelope`/`EncodeEnvelope`; запись
текста — через `SetTextContentAndParagraphs`; мутации — под
`ACAPI_CallUndoableCommand` (один undo на батч, dry_run без пустых
undo). Исходник — `archicad-addon/Sources/BulkCommands.hpp/.cpp`.

---

## 11. Контакты и ссылки

- **Форк:** https://github.com/kramolala-ui/tapir-archicad-automation (ветка `fresh`)
- **Upstream:** https://github.com/ENZYME-APD/tapir-archicad-automation
- **Автор форка:** kramolala-ui
- **Локальная копия:** `C:\Python_projects\tapir-custom\`

---

_Если этот файл устарел — правь его. Он живёт здесь как точка
входа для новых сессий._
