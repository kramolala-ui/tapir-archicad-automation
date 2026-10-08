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
  ноты, скрипт-UI, **14 bulk-команд** (`BulkPing`, `BulkGetPropertyValues`,
  `BulkGetTexts`, `BulkSetTexts`, `BulkFindReplaceText`, `BulkGetElementMesh`,
  `BulkGetElementData`, `BulkGetGroupMembers`, `BulkCloneElement`,
  `BulkMoveElements`, `BulkRotateElements`, `BulkSetElementData`,
  `BulkDeleteElements`, `BulkCloneLabels`). См. §4f — реестр и CI-грабли.
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

## 2c. API DevKit — где лежит, что внутри

**Расположение (локально):** `C:\API.Development.Kit.WIN.<N>.<build>`,
например `C:\API.Development.Kit.WIN.26.3000` (AC26). Один DevKit на
версию Archicad. На GitHub Actions — свой per-version (качается из
`dl.graphisoft.com` в runner).

### Ключевые папки

    Support/Inc/                  заголовки API
        ACAPinc.h                 точка входа (все ACAPI_*)
        APIdefs_*.h               структуры (API_Element, API_AddParType,
                                   API_ElemInfo3D, API_Component3D, ...)
        MigrationHelper.hpp       версионные хелперы (GetElemTypeId,
                                   GetAttributeIndex, ElementTypeName,
                                   StringToElemTypeID)

    Support/Modules/              модули Graphisoft — hpp + .lib
        GSModelDevLib/            ModelerAPI: Model, Element, MeshBody,
                                   Polygon, ConvexPolygon, Vertex
        GSModeler/                exp.h: EXPGetModel (2 перегрузки),
                                   SightPtr, IAttributeReader
        Model3D/                  Modeler::SightPtr, Model3DMain.hpp
        GSRoot/                   GS::UniString, GS::Array, GS::Owner
        Geometry/                 базовые геом. типы
        ... (41 модуль, у каждого Win/<Name>Imp.LIB)

    Support/Lib/Win/ACAP_STAT.lib     статическая линковка API
    Examples/ModelAccess_Test/        рабочий пример обхода 3D-геометрии
                                       через ModelerAPI (ориентир для
                                       BulkGetElementMesh)

### Линковка модулей — уже автоматическая

В `archicad-addon/Tools/CMakeCommon.cmake` функция
`LinkGSLibrariesToProject(target acVersion devKitDir)` делает:

```cmake
file (GLOB ModuleFolders ${devKitDir}/Modules/*)
target_include_directories (${target} SYSTEM PUBLIC ${ModuleFolders})
file (GLOB LibFilesInFolder ${devKitDir}/Modules/*/*/*.lib)
target_link_libraries (${target} ${LibFilesInFolder})
```

**Следствие:** все модули из `Support/Modules/` уже подключены —
include-пути и `.lib`. Для использования ModelerAPI **менять
`CMakeLists.txt` не надо** — достаточно `#include "Model.hpp"` (и
подобных) в .cpp.

### Ключевые константы для mesh

  • `ACAPI_3D_GetCurrentWindowSight(void** sightPtr)` — в `ACAPinc.h`.
    Возвращает `SightPtr` активного окна. Если активно 3D-окно — путь
    к mesh открыт; если FloorPlan — вернёт nullptr или ошибку.
  • `EXPGetModel(SightPtr, Model*, IAttributeReader*)` — в
    `Modules/GSModeler/exp.h`. Строит `ModelerAPI::Model` из SightPtr.
    Есть вторая перегрузка `(ConstModel3DPtr, ...)` — требует
    построенной 3D-модели.
  • `ACAPI_Attribute_GetCurrentAttributeSetReader()` — даёт
    `IAttributeReader` для EXPGetModel.
  • Обход mesh: `Model::GetElement(i, &Element)` →
    `Element.GetTessellatedBody(iBody, &MeshBody)` →
    `MeshBody.GetPolygon(i, &Polygon)` →
    `Polygon.GetConvexPolygon(i, &ConvexPolygon)` →
    `ConvexPolygon.GetVertexCount()`, `.GetVertexIndex(k)` →
    `MeshBody.GetVertex(idx, &Vertex)` → `Vertex.x/y/z`.

## 2a. Сборка, установка и проверка через AI Bridge

### Сборка — ТОЛЬКО через GitHub Actions

**Локальный API Dev Kit есть** (см. §2c) — но **сборка .apx
идёт только через GitHub Actions**. Локальная компиляция аддона
из командной строки не настроена: DevKit развёрнут, но CMake-кэш
и .vcxproj под каждую из 5 версий AC отсутствуют. Что это значит
на практике:

  • **Разведку по заголовкам DevKit делать можно и нужно** —
    сигнатуры, структуры, enum'ы, .lib'ы. Всё лежит в
    `C:\API.Development.Kit.WIN.26.3000\Support\`.
  • **Компилировать/линковать локально — нет.** Сборка зелёного
    .apx — только через workflow, ~5 мин.
  • Из этого следует: если сомневаешься в типе/сигнатуре —
    посмотри заголовок в DevKit (это быстро), а не гадай.
    Но не строй план «соберём локально, проверим».

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
| 8 | `BulkGetElementData` V2 | **✓ работает** (2026-10-06, details/bbox/props/GDL/class/relations) |
| 9 | `BulkGetElementMesh` | **⚠ только AC26** (ModelerAPI; на 25/27+ возвращает stub-error) |
| 10 | `BulkGetGroupMembers` | **✓ работает** (нативные группы Ctrl+G, рекурсивно) |
| 11 | `BulkCloneElement` v2 | **✓ работает** (sources[], один undo на батч) |
| 12 | Bridge: настраиваемые лимиты | **✓ работает** (read/search/batch — не молча режут) |
| 13 | `BulkMoveElements` / `BulkRotateElements` | **✓ работает** (перенос/поворот, один undo) |
| 14 | `BulkSetElementData` | **✓ работает** (element+GDL+Archicad/*+class/*; см. 4f) |
| 15 | `BulkDeleteElements` | **✓ работает** (массовое удаление, один undo) |
| 16 | `BulkGetGroupMembers with_data=true` | **✓ работает** (полные entity одним round-trip) |
| 17 | CI-матрица 25/26/27/28/29 | **✓ зелёная** (2026-10-07, см. 4f) |
| 18 | Схлопнуть дубль `CollectElementData` | TODO (см. 4f, бэклог) |

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

## 4e. Python-клиент (IFC_analyzer)

**Где живёт.** Python-клиент — в проекте **IFC_analyzer**, не здесь.
Путь: `C:\Python_projects\IFC_analyzer\plugins\archicad_plugin\bulk_connection.py`.

**Схема.** Отдельный класс `BulkConnection`, независимый от
`TapirConnection`. Переиспользует только транспорт (`run_command`)
через конструктор:

```python
from plugins.archicad_plugin.bulk_connection import BulkConnection, BulkError

bc = BulkConnection(port=19724)                # создаст свой TapirConnection
bc = BulkConnection(tapir=existing_tapir_conn) # переиспользует чужой
```

**Методы:**

```python
bc.ping(b'hello')                          # {size, preview_hex, compression, zstd_version}
bc.get_property_values(elems, props, as_dict=True)
                                           # {elementId: {propertyId: value}}
bc.get_texts(guids)                        # [{elementId, type, text}]
bc.get_texts_dict(guids, text_only=False)  # {elementId: text}
bc.get_text(guid)                          # Optional[str]
bc.set_texts([{elementId, text}])          # {updated, total, errors}
bc.set_text(guid, text)                    # bool
bc.find_replace_text(find, replace, elements=None,
                     case_sensitive=True, dry_run=True)
                                           # {scanned_count, matched_count, replaced_count, matches}

bc.tapir                                   # доступ к нижнему TapirConnection
```

**Зависимости:** `msgpack>=1.0.0`, `zstandard>=0.20.0` (добавлены в
`requirements.txt` IFC_analyzer).

**Ограничения (см. раздел 5):**
- Запись Label и Text с удлинением текста — баг AC26 API.
- Чтение Label возвращает мусор (текст формируется из GDL).
- Часть Text (единично) может вернуть мусорный символ.

---

## 4f. Реестр bulk-команд + CI-грабли (2026-10-07)

Философия bulk-канала: один `Execute` = один undo-барьер = один
HTTP-round-trip. Это ответ на два ограничения ACAPI/Tapir (см. 4a,
раздел 5):

1. **JSON-стена Graphisoft:** большие JSON (>5000 значений) парсер не
   переваривает (O(N²)).
2. **Нестабильность серий:** два+ подряд вызова `GetPropertyValues` в
   одном скрипте вешают Archicad насмерть.

Bulk — не «ускорение ради ускорения». Если операция и так укладывается
в 50 мс на одном вызове — bulk ей не нужен. Bulk нужен там, где
JSON-стена или серия вызовов.

### Правила контракта

- Вход и выход — msgpack+zstd в `payload_b64` (см. §4).
- Один `Execute` = один `ACAPI_CallUndoableCommand` (для write).
- `dry_run` — опция для write (посчитать `applied_count` без применения).
- Per-source отчёт (`per_source[]`) — по каждому входному элементу
  отдельно: успех / ошибка / код ошибки. Ошибка одного элемента не
  рвёт батч.
- `compression` — `"none"` или `"zstd"`; передаётся в оба конца.

### Реестр 14 команд

| # | Команда | R/W | Версия AC | Назначение | Проверено вживую |
|---|---|---|---|---|---|
| 1 | `BulkPing` | — | 25–29 | Транспортный тест (base64+zstd round-trip) | ✅ 2026-10-07 |
| 2 | `BulkGetPropertyValues` | R | 25–29 | N×K свойств, чанки по 20 | ✅ 2026-10-06 |
| 3 | `BulkGetTexts` | R | 25–29 | Тексты Text/Label | ✅ 2026-10-07 |
| 4 | `BulkSetTexts` | W | 25–29 | Запись текста | ⚠ патч на `ApplyTextContent` отправлен, ждёт сборки |
| 5 | `BulkFindReplaceText` | W | 25–29 | Find/replace, `dry_run` | ✅ 2026-10-07 (dry_run) |
| 6 | `BulkGetElementMesh` | R | **25–26** | Меш: component-API + ear-clipping (невыпуклые полигоны), Zone через polygonOutline | ✅ 2026-10-07 (без 3D-окна, ear-clipping, v0 fixed) |
| 7 | `BulkGetElementData` | R | 25–29 | details/bbox/props/GDL/class/relations/**2D-geometry** | ✅ 2026-10-07 |
| 8 | `BulkGetGroupMembers` | R | 25–29 | Нативная группа (Ctrl+G) + `with_data=true` | ✅ 2026-10-07 (37 entity одним вызовом) |
| 9 | `BulkCloneElement` v2 | W | 25–29 | Копирование доноров из `sources[]` | ✅ 2026-10-07 |
| 10 | `BulkMoveElements` | W | 25–29 | Перенос по вектору (dx,dy,dz) | ✅ 2026-10-07 (Δx = 5.0 ровно) |
| 11 | `BulkRotateElements` | W | 25–29 | Поворот вокруг центра (по умолчанию — центр AABB) | ✅ 2026-10-07 (bbox 4.19/1.91→1.91/4.19) |
| 12 | `BulkSetElementData` | W | 25–29 | Универсальная запись: element+GDL+Archicad+class | ✅ 2026-10-07 (4/4 категории) |
| 13 | `BulkDeleteElements` | W | 25–29 | Массовое удаление одним `ACAPI_Element_Delete` | ✅ 2026-10-07 |
| 14 | `BulkCloneLabels` | W | 25–29 | Создание text-Label по донору + `instances[]` | ✅ 2026-10-07 (см. отдельный подраздел) |

### Общие хелперы (`BulkCommands.cpp`)

- `EncodeEnvelope` / `DecodeEnvelope` — msgpack+zstd ↔ base64.
- `CollectElementData(opts)` + `struct ElementDataOptions` — единый
  сборщик данных по списку гуидов. Сейчас используется в
  `BulkGetGroupMembers with_data=true`; в `BulkGetElementData::Execute`
  пока **дубль** (см. бэклог).
- `ApplyGdlBatch` / `ApplyClassBatch` / `ApplyPropertyBatch` — батч-запись
  в Archicad (GDL, классификации, свойства).
- `PropertyConversionUtils.hpp` — единая реализация
  `API_PropertyConversionUtilsInterface` (символы единиц, метрические
  типы). Вынесена из `PropertyCommands.cpp` — раньше был inline-класс
  в одном файле, теперь общий для двух.

### BulkSetElementData — 4 категории записи (проверено 2026-10-07)

Одна команда, один undo, один round-trip — пишет четыре типа полей
в элемент. Все проверены живьём на Object (`D610F7A9-...` в
«Шаблоне гидравлики IFC»), с полным циклом write → read → revert:

| Категория | Ключ в payload | API-канал | Пример |
|---|---|---|---|
| Element fields | `story_index`, `layer_index`, `object_pos_x/y`, `object_level`, `object_angle` | `ACAPI_Element_Change` + маски | `object_pos_x` 476.74 → 477.0 → 476.74 ✅ |
| GDL | `GDL/<name>` | `ACAPI_LibraryPart_OpenParameters` → `GetActParameters` → правка → `Element_Change(APIMemoMask_AddPars)` | `GDL/A` 0.8 → 1.0 → 0.8 ✅ |
| Archicad-свойство | `Archicad/<property-guid>` | `ACAPI_Property_SetPropertyValueFromString` + `ACAPI_Element_SetProperty` | `ElementID` `Ст. 1, 1А` → `TEST_BULK_27016` → `Ст. 1, 1А` ✅ |
| Классификация | `class/<system-guid>` = `<item-guid>` | `ACAPI_Element_RemoveClassificationItem` + `AddClassificationItem` | `Радиатор отопления` → `Схемы отопления` → `Радиатор отопления` ✅ |

**Read-only поля** (`bbox_*`) — попадают в `ignored_readonly[]`,
не пишутся. **Неизвестные ключи** — в `ignored_unknown[]`.
**Невалидный guid** в `<...-guid>` — в `errors[]` с ключом и
сообщением.

**Пример payload** (msgpack, всё в одном элементе):

```
{
  "entities": [{
    "guid": "D610F7A9-3C48-4DFB-A4FB-0E5E35414FB6",
    "parameters": {
      "story_index":        0,
      "layer_index":        813,
      "object_pos_x":       477.0,
      "GDL/A":              1.0,
      "Archicad/8BACB089-BBFE-41C2-B5A4-D1CEBC2F4FB3": "TEST_BULK_27016",
      "class/8D827552-0242-4637-9649-0A6319191A3D": "BBB4A714-59CD-4C72-92DB-352574B2F373"
    }
  }],
  "dry_run": false
}
```

**Известная косметика (см. бэклог):**

- **`applied[]` пустой при write.** Реальная запись проходит
  (проверено read-after-write), но `srcOut["applied"]` не заполняется
  ключами GDL/Archicad/class. В отчёте видно только element-поля.
- **`applied_count` = число батчей, не полей.** Всегда 1 на элемент,
  даже если записано 6 полей из 4 категорий.
- **`ApplyClassBatch` глотает ошибку `RemoveClassificationItem`.**
  Если старый item не снялся, `Add` нового может дать неверный
  результат.

**Регистр GUID'ов.** API Archicad возвращает guid в **UPPERCASE**
(`16CFDFE0-...`), а принимает **любой** — работает и в lowercase.
В клиенте сравнивать через `.lower()` — как `BulkConnection` уже
делает. Прямое `==` строк даст ложный промах.

**Нюанс GDL для Object/Lamp.** Параметры `A` и `B` требуют
**дополнительных масок** `xRatio` / `yRatio` (см.
`SetGDLParametersOfElementsCommand`). Обычные GDL-параметры пишутся
через `APIMemoMask_AddPars` без специальных масок. Для смены `A`
или `B` — использовать индивидуальную команду `SetGDLParameters`
до отдельной доработки bulk-канала.

### BulkGetElementData — расширения 2026-10-07

Три независимых расширения поверх V2 (details/bbox/props/GDL/class/relations):

**1. Zone-ветка в блоке `connected`.** `ACAPI_Grouping_GetConnectedElements`
на AC26 для Zone **не работает** — и Object, и Zone дают 0 рёбер (проверено
живьём на выделенной зоне). Замена — `ACAPI_Element_GetRelations` +
`API_RoomRelation` (та же логика, что в `GetRelationsOfElementsCommand`,
`ElementCommands.cpp:3065`). `kind` вычисляется по паре (owner_type,
target_type):

| Owner | Target | kind |
|---|---|---|
| Zone | Object | `zone_content` |
| Zone | Zone | `zone_neighbour` |
| Zone | Wall/Beam/CW/… | `zone_boundary` |
| не-Zone | любой | `connected_to` |

Обратный ход (window → wall) работает через `elem.label.parent` /
`elem.window.owner` (прямое поле элемента), не через Grouping.

**2. `with_2d_geometry` (default false).** В `entity["geometry"]` попадает
контур/точки/радиус/углы/текст для 8 типов 2D-элементов. Имена ключей
**совпадают** с `GetDetailsOfElements` — единый контракт для per-element
и bulk. Реализация — `Collect2DGeometryToJson(element, out)`; контуры
PolyLine/Hatch читаются через существующий `GetPolygonsFromMemoCoords`
из `CommandBase.hpp` (не дублируется).

| Тип | Что в `geometry` |
|---|---|
| PolyLine | `coordinates` + `arcs` + `room_separator` + `line_pen_index` |
| Line | `beg_coordinate` + `end_coordinate` + room_separator + pen |
| Arc | `origin` + `radius` + `angle` + `ratio` + `beg_angle` + `end_angle` + `reflected` |
| Circle | То же (различается по `header.type.typeID == API_CircleID`) |
| Hatch | `coordinates` + `holes[]` + `contour_pen_index` + `fill_pen_index` + `fill_background_pen_index` + `fill_id` + `building_material_id` + `show_area` |
| Label | `label_class` + `owner_element_id` + `beg/mid/end_coordinate` + `text` + `paragraph_count` |
| Text | `position` + `angle` + `height` + `pen` + `text` + `paragraph_count` |
| Hotspot | `position` |

**3. Text для Label/Text в geometry.** Плоская конкатенация всех параграфов
memo через тот же helper `ReadTextFromMemo`, что использует
`BulkGetTextsCommand`. `paragraph_count` — число параграфов.

**Диагностика формата.** Для Label автотекст подставляется **до** возврата
— в `text` приходит уже развёрнутое значение плюс литеральные `#имя`
маркеры (у которых нет значения). Проверено: `СТН-034\r#Тип, марка,
обозначение документа, опросного листа  аааа` — то же, что видно в UI.

### BulkCloneLabels — создание text-Label по донору (2026-10-07)

По образцу `BulkCloneElement`: donor + instances, один undo на батч.
Запись текста — через существующий `TextLabelDetails::ApplyTextContent`
(тот же путь, что CreateLabels/ModifyLabels).

Payload (msgpack в `payload_b64`):

```
{
  "sources": [ { "source_guid": "<donor-label-guid>",
                 "instances": [ ... ] }, ... ]
}
// ИЛИ одиночный донор
{ "source_guid": "...", "instances": [ ... ] }

// Instance:
{
  "beg_x": 1.0, "beg_y": 2.0,       // лидер-линия: начало (обязательно)
  "mid_x": 1.5, "mid_y": 2.5,       // изгиб (опционально)
  "end_x": 2.0, "end_y": 3.0,       // конец (опционально)
  "text":  "новый текст",            // опционально — перезапись memo
  "owner_guid": "host-element-guid", // опционально — к чему привязать
  "story_index": 0, "layer_index": 56 // опционально
}
```

**Ограничения v1:**
- Только `API_LabelID` с `labelClass=Text`. Symbol-Label → ошибка
  (`symbol labels not supported by BulkCloneLabels (use CreateLabels)`).
- Донор обязателен. Искусственный донор — отдельный путь, через
  существующую `CreateLabels` с явными полями.
- `delete_source` **не поддержан** в v1 (нужна точная сигнатура
  `ACAPI_Element_Delete` в контексте clone). Размножение без удаления
  источника — достаточный сценарий.
- В каждом `per_source` — свой набор `created_guids` и `errors`.

**Зачем нужна.** Массовая установка однотипных аннотаций на 500+ стен/зон
(маркировка, примечания) — одна bulk-команда вместо N одиночных
`CreateLabels`, один undo, один round-trip.

### CI-грабли (проверено на матрице 25/26/27/28/29)

**Матрица CI — 5 версий + `/WX`** (warnings-as-errors). Любой warning
на любой версии = красный билд. Файл `.github/workflows/archicad_addon.yml`.

**`TAPIR_AC26_ONLY` — почему «только 26».**
`ServerMainVers_2600` определён во **всех** DevKit'ах начиная с 26 — это
код версии, а не «эта версия = 26». Чтобы выразить «ровно 26», нужна
явная проверка:

```cpp
#if defined(ServerMainVers_2600) && !defined(ServerMainVers_2700)
#define TAPIR_AC26_ONLY 1
#else
#define TAPIR_AC26_ONLY 0
#endif
```

Причина: `ACAPI_3D_GetCurrentWindowSight` есть только в AC26. На 27+
`#include "Model.hpp"` ломает `GDL/PropertyListImp.hpp` (каскад
C2039/C3083). Mesh реализован через stub:

```cpp
#if !TAPIR_AC26_ONLY
    (void) elemHead;   // <-- обязательно, иначе C4100 + /WX
    errOut = "mesh not supported on this Archicad version (only AC26 for now)";
    return false;
#else
    // ... реальный обход ModelerAPI
#endif
```

**C4100 на stub-ветках.** MSVC в C4100 указывает на **сигнатуру**
параметра, не на место `(void)`. Пример из CI:

```
BulkCommands.cpp(1701,47): warning C4100: 'elemHead': unreferenced
```

Строка 1701 — это `bool ExtractElementMesh (const API_Elem_Head& elemHead,`.
Но `(void) elemHead;` лежит на строке 1717, и MSVC о нём не сообщает.
Правило: **любой параметр, использование которого условно через
`#if !TAPIR_AC26_ONLY` / `#ifdef ServerMainVers_X`, глушить `(void)` в
той ветке, где он не используется.** Иначе CI падает на 25/27/28/29,
и без чтения точной сигнатуры неочевидно, в чём дело.

**`APIERR_BADPARS = -2130313112`.** Общий код «неверные параметры».
Появляется в двух разных случаях, и путать их нельзя:

- **Неверный msgpack-payload:** в C++ `nlohmann::json::from_msgpack`
  бросает `type_error.302` (`type must be string, but is object`),
  `Execute` ловит и отдаёт `APIERR_BADPARS` с текстом ошибки. Пример —
  передали массив объектов вместо массива строк в `element_guids`.
- **Отказ `ACAPI_Element_Change`:** маски/memo не сошлись, ACAPI
  отвергает. Пример — `BulkSetTexts` через `SetTextContentAndParagraphs`
  (устаревший путь) на AC26; фикс — переход на
  `TextLabelDetails::ApplyTextContent`.

Правило диагностики: если в ответе есть `msg=`, это **первый** случай
— смотри формат payload. Если ошибка приходит как `change failed
(code=-2130313112)` — это **второй** случай, копай memo/mask.

**`ACAPI_LibPart_Get` — версии.**

- Есть в AC25 и AC26. Алиаса в `MigrationHelper.hpp` для 27+ **нет**
  (в файле есть `ACAPI_LibraryPart_GetParams` / `GetNum` /
  `GetParamValues` — но не `Get`).
- Значит `object_lib_part_name` в `BulkGetElementData` под
  `#if TAPIR_AC26_ONLY` — читается **только на 26**. На 25 тоже не
  читается, хотя функция доступна. TODO: расширить guard до
  `#if !defined(ServerMainVers_2700)`.

**MEP-типы в `GetConnectedElements` отвергаются Tapir.** Типы
`MEP`, `MEPFitting`, `MEPTerminator`, `MEPBranch`, `Pipe`, `Duct`,
`Cable` дают `-2130313112 Invalid connectedElementType`. Это контракт
AddOn, не наш баг. Для MEP — обход через `spatial_query` /
классификации / GDL-параметр `Connect_*`.

### Бэклог

- **Схлопнуть дубль `CollectElementData`.** Сейчас в
  `BulkGetElementData::Execute` ~220 строк копии логики helper'а.
  Отдельным патчем после обкатки `with_data` — `BulkGetElementData::Execute`
  становится тонкой обёрткой (`парсинг payload → CollectElementData →
  envelope`).
- **`BulkSetTexts` — патч отправлен 2026-10-07, не собран.** Замена
  `SetTextContentAndParagraphs` на `TextLabelDetails::ApplyTextContent`
  (тот же путь, что `ModifyTexts` / `ModifyLabels` /
  `SetDetailsOfElements`). Ждёт CI + живой тест. Корень проблемы — в
  старом пути memo не заполняется `(*memo.paragraphs)[0].just`, и на
  AC26 `ACAPI_Element_Change` отбивает его как `APIERR_BADPARS`.
- **`BulkGetGroupMembers`: `source_guids=[]` для элементов без группы.**
  Сейчас поля отсутствуют. Косметика.
- **`BulkSetDetails`** — обёртка над upstream `SetDetailsOfElements`
  (типовые поля Wall/Slab/Zone, не покрытые `BulkSetElementData`).
  Пока не начата.
- **Python-обёртка `BulkConnection`** в IFC_analyzer — **расширена**
  2026-10-07 под Move/Rotate/SetElementData/Delete/GetGroupMembers.
  Осталось: публичный `call()` для эскейп-хэтча и TypedDicts.
**Закрыто 2026-10-07:**

- ✅ `BulkSetElementData: applied[]` — теперь включает GDL/Archicad/class
  ключи из `notAppliedOut`. `ignored_locked[]` — для полей, которые ACAPI
  проглотил (locked параметр/свойство). `applied_count` = суммарное
  число применённых ключей по всем источникам (не батчей).
- ✅ `ApplyClassBatch` пробрасывает ошибку `RemoveClassificationItem`
  наружу. Раньше глоталась — маскировалась успехом `Add`.
- ✅ `BulkSetTexts` (текст Text/Label) — патч на `ApplyTextContent`
  собран, `BulkCloneLabels` использует тот же путь.

**Открыто:**

- **Python-обёртка `BulkConnection` в IFC_analyzer:** добавить
  `get_element_data(..., with_2d_geometry=True)`, `clone_labels(source_guid,
  instances)`, `call(op, payload)` для эскейп-хэтча и TypedDicts.
- **Нода-фасад `archicad_get_2d_geometry`** в IFC_analyzer — одна нода с
  параметром `types=['PolyLine','Hatch','Arc',...]`, обёртка над bulk.
  Заменит шаги GetDetailsOfElements + ручной разбор.
- **Нода `archicad_get_element_links`** — объединит `get_connected_elements`
  и `get_zone_elements`: плоский DataFrame `{source, target, kind, via}`
  с `kind ∈ {connected_to, zone_content, zone_boundary, zone_neighbour}`.
  Внутри — bulk-get-element-data с `connected_types` + опц. reverse-lookup
  для opening→host.
- **`BulkCloneLabels` v2:** поддержать `delete_source` (после проверки
  сигнатуры `ACAPI_Element_Delete`).

**История §4c.** Раздел с номером 4c в файле отсутствует (сразу 4d —
нумерационный пропуск). Не трогаем — так исторически.

---

## 5. Известные грабли

### Маркер «не задано» ≠ 0 в C++ (2026-10-07)

В `ExtractElementMesh` массив `localToGlobal` инициализировался `0`
как маркер «вершина не задана». Но индекс 0 **валиден**: для первой
вершины первого тела `outVertices.size() = 0` до `push_back`, и
`globalIdx = 0`. Проверка `if (gv1 == 0 || gv2 == 0) continue;`
отсеивала все рёбра, инцидентные v0 — терялась целая грань бокса
(8 vertexCount / 9 triangleCount / 3 boundary edges вместо 12 / 0).

**Правило.** В массивах, где 0 может быть валидным индексом,
использовать `UINT32_MAX` (или `-1` для signed, `nullptr` для
указателей) как маркер «не задано». Проверять `== kInvalidIdx`,
не `== 0`. Ноль — это данные, не маркер.

Симптом похожей ошибки в будущем: элемент присутствует в буфере, но
не участвует ни в одном треугольнике / ребре / связи. Первое, что
проверять — инициализация массивов-мэппингов нулём.

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

### Каналы AI_CHANGES и AI_PROGRAM — разные проекты

**AI_CHANGES** работает **только с tapir-archicad-automation** —
относительные пути интерпретируются как пути этого репо
(`github://kramolala-ui/tapir-archicad-automation@fresh/...`).

**Файлы IFC_analyzer** (`C:\Python_projects\IFC_analyzer\`) в AI_CHANGES
**не попадают** — там нет пути к ним. Если послать `path: "requirements.txt"`
или `path: "plugins/archicad_plugin/foo.py"` — утилита попробует их
найти в tapir-репо и упадёт с `old fragment not found` либо сделает
`write` в несуществующую папку.

**Правильный канал для IFC_analyzer** — `===AI_PROGRAM===` со
скриптом, который пишет файлы напрямую через `Path.write_text`.
Пример:

```python
from pathlib import Path
ROOT = Path(r'C:\Python_projects\IFC_analyzer')
(ROOT / 'plugins' / 'archicad_plugin' / 'bulk_connection.py').write_text(src, encoding='utf-8')
```

**Свежий пример (2026-10-06):** попытка записать `bulk_connection.py` и
`requirements.txt` через AI_CHANGES — 2 ошибки, полный откат. Тот же
содержимый файл через AI_PROGRAM script — применился за 2 секунды.

**Правило:** tapir-репо → AI_CHANGES; IFC_analyzer → AI_PROGRAM script.
Не смешивать.

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

### 2026-10-06 — Приёмка bulk-канала через Python-клиент (9/9)

Полный цикл приёмки через `BulkConnection` (IFC_analyzer):

| Шаг | Что | Результат |
|---|---|---|
| 1 | import `BulkConnection` | ✅ |
| 2 | создание через port | ✅ 34 ms, Tapir 1.7.1, «Шаблон гидравлики IFC» |
| 3 | ping b'acceptance' | ✅ 9 ms, size=10, hex matches |
| 4 | get_property_values 100×20 | ✅ **53 ms**, 2000 значений, 1568 заполнено |
| 5 | get_texts_dict все Text проекта | ✅ **54 ms**, 487/487 непустых |
| 6 | find_replace_text dry_run (кириллица) | ✅ 241 ms, scanned=1006 |
| 7 | set_text no-op | ✅ 98 ms, значение не изменилось |
| 8 | **СЕРИЯ 10 вызовов подряд** | ✅ **все ок**, avg 70 ms, max 303 ms |
| 9 | финальный ping | ✅ Archicad отвечает |

**Главное:** серия из 10 вызовов с чередованием команд (`ping`,
`get_property_values`, `get_texts_dict`, `find_replace_text`) прошла
без зависаний. Это **прямая замена** проблеме JSON-канала, где Archicad
вешался после 2-3 вызовов `GetPropertyValuesOfElements` подряд.

**Ключевые метрики на приёмке:**
- get_property_values 100×20 = 53 ms (JSON-путь на 5000 значений — 6.9 s).
- get_texts_dict 487 элементов = 54 ms.
- Серия 10 вызовов: max 303 ms (find_replace_text сканирует весь проект).

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
| 50 Text, no-op (тот же текст) | 89 ms | 50/50 ✅ |
| 3 Text, длиннее (баг AC26) | 47 ms | 0/3 ❌ code -2130312713 |

**BulkFindReplaceText** (поиск/замена):

| Тест | Время | scanned | matched | replaced |
|---|---|---|---|---|
| dry_run=true, поиск '1' по всему проекту | **263 ms** | 1 006 | 42 | 0 |
| dry_run=false, поиск '1' в 2 Text (нет match) | 52 ms | 2 | 0 | 0 |
| **dry_run=false, замена '2'→'7' в 1 Text** | 123 ms | 1 | 1 | **1** ✅ |

Реальная замена **односимвольного текста** на другой односимвольный работает
(длины совпадают, не попадает в баг AC26 удлинения). Многосимвольная
замена — с той же оговоркой, что и BulkSetTexts.

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

**2026-10-06:** Сборка зелёная на **AC25-29** (5 bulk-команд + nlohmann).
Одновременно исправлено: msgpack-cxx → nlohmann/json (C2766 на MSVC,
см. §5); `#undef snprintf` в BulkCommands.cpp (DevKit AC25/26 делает
`#define snprintf _snprintf`, см. §5).

**2026-10-06:** В IFC_analyzer добавлен `plugins/archicad_plugin/bulk_connection.py` —
отдельный класс `BulkConnection` для bulk-канала (8 методов +
свойство `tapir`). Независим от `TapirConnection`, переиспользует
только транспорт `run_command`. Схема описана в §4e этого файла.
Зависимости `msgpack`, `zstandard` добавлены в `requirements.txt`
IFC_analyzer.

**2026-10-06:** Замеры 5 bulk-команд на «Шаблоне гидравлики IFC» —
см. раздел 9. Ключевая цифра: `BulkGetPropertyValues` 500×20 = 287 ms
(10 000 значений), ×48 быстрее JSON-пути. `BulkGetTexts` 1006 элементов
= 261 ms.

**2026-10-06:** Собрана `BulkGetElementData` V2 — комплексное
чтение Archicad-элемента одним Execute (details / bbox / properties /
GDL / classifications / relations) через msgpack+zstd. См. §10a.

**2026-10-06:** `BulkGetElementMesh` — реализован через ModelerAPI.
Цепочка: `ACAPI_3D_GetCurrentWindowSight` → `Modeler::SightPtr` →
`EXPGetModel` → обход `MeshBody` (`GetElement` / `GetTessellatedBody`
/ `GetPolygon` / `GetConvexPolygon` / `GetVertexIndex` / `GetVertex`).
Fan-триангуляция + дедупликация вершин через remap. **Требует
активного 3D-окна** — Python-обёртка `get_element_mesh`
(`ensure_3d_window=True`) переключает сама через
`TapirConnection.change_window('3DModel')`.

Модули `GSModelDevLib` / `GSModeler` линкуются автоматически через
`LinkGSLibrariesToProject` (`Tools/CMakeCommon.cmake`) —
`CMakeLists.txt` менять не надо. Резервный путь (если 3D-окна нет) —
IFC-транспорт через `export_filtered_ifc` + `ifcopenshell.geom`.
См. §10a.

**2026-10-06:** Bulk-команды: три API-фикса закрыли сборку на
AC25-29. `GetElemTypeId(element.header)` вместо несуществующего
`header.typeID`; `GetAttributeIndex(element.header.layer)` (в AC27+
это класс, а не Int32); лямбда `ACAPI_CallUndoableCommand` должна
возвращать `NoError`. См. §10a «Ключевые API-факты».

**2026-10-06:** В `BulkGetElementData` добавлены: (а) `selected=true` —
читать текущее выделение Archicad одним вызовом (N=1 — клик, N>1 —
рамка); (б) `params[group_guid]` — нативная группировка Ctrl+G из
`element.header.groupGuid`; (в) для Object — `object_lib_part_name`,
`object_pos_x/y`, `object_level`, `object_angle`, `object_x/y_ratio`
(всё для переноса Object между проектами).

**2026-10-06:** Собрана `BulkGetGroupMembers` — читает нативные группы
Archicad через `ACAPI_ElementGroup_GetGroup` (element → parent) и
`ACAPI_ElementGroup_GetAllGroupedElems` (group → members, рекурсивно).
Один `element_guid` → вся группа (клик по прибору → обвязка). См. §10b.

**2026-10-06:** `BulkCloneElement` (в разработке) — bulk-клон одного
донора в N экземпляров. Читает `Get + GetMemo` источника, для каждой
позиции копирует element, правит `pos/level/angle/story/layer`,
опционально override GDL по имени, вызывает `ACAPI_Element_Create` под
одним undo. v1 — только `Object`. См. §10b.

**2026-10-06:** Bridge: убраны hardcoded обрезки в `read`/`search`/`batch`
(было 400/300 символов, 20 файлов, 2 МБ, 1000 строк, 10 ops). Всё стало
параметрами запроса с высокими дефолтами (2000 символов / 200 файлов /
20 МБ / 20000 строк / 50 ops). У каждой строки — флаг
`line_truncated: true`; в batch — `truncated: true`. Молчаливой обрезки
больше нет — она ломала якоря replace и жгла токены на повторные чтения.
См. §10b.

---

## 10a. BulkGetElementData / BulkGetElementMesh / геометрия (2026-10-06)

### BulkGetElementData V2 — done

Один Execute возвращает `{entities: [...], relations: [...]}` — все
данные Archicad-элемента одним пакетом (msgpack + zstd):

    entity.params:
      story_index, layer_index,
      bbox_size_x/y/z,
      GDL/<name>                (GDL-параметры)
      Archicad/<name>           (пользовательские свойства)
      class/<system>/<code>     (классификации)

    relations: [{from, to, type, ...}]

Исходник — `archicad-addon/Sources/BulkCommands.hpp/.cpp`.

### BulkGetElementMesh — component-API, работает на AC25/AC26 (2026-10-07)

**Что работает (актуальный путь).** Низкоуровневый 3D-component API:

    1. ACAPI_ModelAccess_Get3DInfo(elemHead, &info3D)
       — диапазон тел [fbody..lbody]. НЕ требует открытого 3D-окна.
    2. Для каждого тела iBody:
         ACAPI_ModelAccess_GetComponent(API_BodyID, iBody)
           → nVert, nPgon, nPedg, nEdge, tranmat
         API_VertID 1..nVert    → (x, y, z) локальные
         API_PgonID 1..nPgon    → fpedg, lpedg, status
         API_PedgID fpedg..lpedg→ pedg (знак = направление ребра)
         API_EdgeID abs(pedg)   → vert1, vert2
    3. Триангуляция — ear-clipping по контуру полигона.

Все индексы компонент у ACAPI 1-based (body, vert, pgon, pedg, edge).
В outVertices/outTriangles — 0-based (стандарт GL / VTK / ifcopenshell).

**Дедупликация вершин обязательна.** Одна вершина в нескольких
полигонах. Ключ remap: `(bodyIdx << 32) | localVertexIndex`.

**applyTransform=true**: к координатам вершин применяется `body.tranmat`
(ACAPI-вершины идут в локальных координатах, а не в world — см. #563
в ElementCommands.cpp:4148: bbox уже world, а вертексы — нет).

### Триангуляция — ear-clipping, не fan (2026-10-07)

**Проблема с fan.** Fan-триангуляция `(v0, vi, vi+1)` на невыпуклых
полигонах даёт треугольники с противоположной ориентацией — VTK
красит грани вразнобой (диагональные артефакты и «обращённые грани»
на плоских панелях радиаторов, стенах, плитах).

**Решение — ear-clipping + нормаль через Newell:**

    PolygonNormalNewell(verts, loop, n, outN)
        — устойчива к слегка негоометричным вершинам.
    PlaneAxesFromNormal(n, u, v)
        — базис в плоскости полигона.
    SignedArea2D(xs, ys, n)
        — shoelace, знак обхода контура.
    EarClip2D(xs, ys, outLocalTris)
        — O(n²) на полигон; для стен/плит 6-8 вершин — мгновенно.
    TriangulatePolygon(verts, contourGlobal, outTriangles)
        — оркестратор: Newell → проекция → при CW reverse →
          ear-clipping → запись в outTriangles.

**Fallback.** Если ear-clipping встал (вырожденный / самопересекающийся
контур) — возврат к fan на остатке. Полигон не теряется.

**Forward declaration.** `TriangulatePolygon` объявлена перед
`ExtractElementMesh`, определения helper'ов — ниже в том же
`#if !defined(ServerMainVers_2700)`-блоке.

**Include'ы.** В шапку добавлены `<cmath>` (std::sqrt в Len3) и
`<cstddef>` (ptrdiff_t в TriangulatePolygon). Раньше в файле не было
ни одного, ни другого — не полагаемся на транзитивные include.

### Баг потери первой вершины первого тела — маркер 0 (2026-10-07)

**Симптом.** Object и Zone возвращали `8 vertexCount / 9 triangleCount
/ 3 boundary edges` вместо `8 / 12 / 0`. Одна и та же грань бокса
(рёбра 1-3, 1-4, 3-4) отсутствовала у обоих типов.

**Причина.** `localToGlobal` инициализировался `0` как маркер «вершина
не задана». Но индекс 0 валиден: для первой вершины первого тела
`outVertices.size() = 0` до push_back, `globalIdx = 0`. Проверка
`if (gv1 == 0 || gv2 == 0) continue;` отсеивала все рёбра, инцидентные
v0.

**Фикс.** Маркер заменён на `UINT32_MAX` (`kInvalidIdx`). Проверка —
`gv == kInvalidIdx`. Меш никогда не содержит 4 миллиарда вершин,
конфликта с реальными индексами нет.

**Результат.** 12 triangles / 0 boundary edges для Object и Zone.
Watertight. Крупные полигоны (стены, плиты, тела фитингов, цилиндры)
без артефактов.

### Zone — через polygonOutline + extrude, НЕ через ACAPI mesh (2026-10-07)

**Почему.** Даже после фикса v0 ACAPI mesh для Zone теряет данные —
у Zone нет настоящего solid-body, `Get3DInfo` возвращает неполный
набор полигонов. Правильный источник — официальный `GetDetailsOfElements`:

    d = GetDetailsOfElements(guid)['details']
    d['polygonOutline']  — 2D-контур зоны, N точек (последняя = первая)
    d['holes']           — список дырок (пока не поддерживаем)
    d['zCoordinate']     — низ зоны (обычно 0)

Высота зоны (z_max) берётся из mesh-канала (bbox по Z). Контур
триангулируется `vtkContourTriangulator` (через `pv.PolyData.triangulate()`),
экструдируется на z_min..z_max, боковые стенки — quad-ы.

**Ориентация контура.** `polygonOutline` из Archicad может приходить
CW — нормали верх/низ смотрели бы внутрь. Shoelace → reverse при CW:
верх CCW (+Z наружу), низ CW (−Z наружу).

**Клиентский контракт.** См. `tools/_archicad_realtime_helpers.py`:
`fetch_zone_outlines(tapir, guids)` + `zone_mesh_from_outline(outline, z_min, z_max)`.

### Positional matching для GetDetailsOfElements / Get3DBoundingBoxes

**GetDetailsOfElements НЕ возвращает GUID в ответе.** Только
человекочитаемый `id` (например `'ЗОН-003'`), `floorIndex`,
`layerIndex`, `drawIndex`. `detailsOfElements` приходит в том же
порядке, что `elements` в запросе → матчим ПОЗИЦИОННО.

**Get3DBoundingBoxes** — та же история: `elementId` в ответе — номер,
не GUID; элементы идут в порядке запроса.

Это видно и в существующих нодах: `archicad_get_element_details`,
`archicad_get_connected_elements` (см. `core/operations/commands/
archicad_commands.py`) — все опираются на позиционный матчинг.

### Свойства mesh-канала (проверено 2026-10-07)

  • **Не зависит от активного 3D-окна.** Component API читает из
    ACAPI-модели в памяти, не через UI. SightPtr (ModelerAPI) — ушли
    из-за крашей на AC26.
  • **Не зависит от видимости слоёв.** `GetElementsByType` возвращает
    все элементы независимо от layer visibility и layer combination.
  • **Требует построенного 3D-кеша.** Один раз переключиться в 3D-вид,
    дать Archicad просчитать модель. Дальше — хоть на плане.
  • **`Get3DBoundingBoxes` возвращает bbox для 2D-символов.** Не
    признак наличия 3D-тела. У Object'ов с 2D-символом (GDL без 3D-
    скрипта) bbox есть, но `BulkGetElementMesh` отвечает `error='empty
    mesh (no triangles collected)'`. Реальная проверка — только чтение
    mesh. На тестовом проекте: 653 Object, 220 с mesh, 433 без 3D-тела.
  • **Батчинг обязателен.** `BulkGetElementMesh` на 2000+ элементов
    ломается (`no response from Tapir`). По 100 — стабильно.
  • **Реконнект при обрыве.** После 200+ элементов через одно
    соединение AC SDK может молча уронить следующую серию вызовов.
    Catch на `no response from Tapir` → пересоздать `TapirConnection`
    → retry один раз.

### Python-обёртка (IFC_analyzer)

Единый контракт для realtime-тестов — `tools/_archicad_realtime_helpers.py`.
Используется в `tools/test_realtime_archicad_geometry.py` (выделенные
элементы) и `tools/test_realtime_full_scene.py` (вся 3D-сцена).

  • `extract_guid(item)` — guid из элемента Tapir-ответа (4 формы).
  • `to_bytes(v)` — bytes из row (bulk может вернуть bytes / str).
  • `mesh_from_row(row)` — BulkGetElementMesh row → pv.PolyData:
    clean + compute_normals (consistent + auto_orient).
  • `mesh_hash(row)` — md5(vertices + triangles) для детекта изменений.
  • `z_range_from_row(row)` — (z_min, z_max) для extrude Zone.
  • `fetch_zone_outlines(tapir, guids)` — {guid: [(x,y), ...]} через
    GetDetailsOfElements + позиционный матчинг.
  • `signed_area_2d(pts)` — shoelace (знак обхода контура).
  • `triangulate_ring(pts_xy, z)` — через vtkContourTriangulator.
  • `zone_mesh_from_outline(outline, z_min, z_max)` — extrude призмы.

Единый префикс логов — `[rt]`.

### GUID-нормализация в C++ — TODO при следующей сборке

**Проблема.** `BulkGetElementMesh` возвращает `elementId` в том
регистре, в каком он был передан на вход. `GetDetailsOfElements` и
`BulkGetElementData` — в UPPERCASE. На Python-стороне приходится
делать `.upper()` при сравнении ключей (уже сделано в
`_archicad_realtime_helpers.py`).

**Фикс (при следующей сборке).** В `BulkGetElementMesh::Execute`
(и, если есть, в аналогичных read-командах) — нормализовать
`row["elementId"]` через `APIGuidToString(guid)` вместо
`guidStr` из запроса. Это даст канонический UPPER автоматически.

**Не блокирует текущую работу** — `.upper()` на Python-стороне решает
проблему. Фикс — для чистоты контракта, убирает класс багов «два
источника — два регистра».

### Ключевые API-факты (для будущих команд)

Собраны из ошибок компиляции в этой сессии:

  • `element.header.typeID` НЕ существует. Правильно —
    `GetElemTypeId(element.header)` (helper из
    `MigrationHelper.hpp`). Исключение: у `API_AttributeHeader`
    есть `.typeID` — `attribute.header.typeID = API_BuildingMaterialID`.

  • `element.header.layer` — `API_AttributeIndex`. В AC25/26 —
    typedef Int32, в AC27+ — класс. `static_cast<int64_t>` падает
    в AC27+ (C2440). Правильно —
    `GetAttributeIndex(element.header.layer)`.

  • `API_AddParType.value.real` — ВСЕГДА `double`. Даже для
    целочисленных типов и типов-атрибутов (APIParT_PenCol,
    APIParT_LineTyp, APIParT_Mater, APIParT_FillPat,
    APIParT_BuildingMaterial, APIParT_Profile, APIParT_LightSw).
    `p.value.integer` НЕ существует.

  • `API_AddParType.name` — `const char[32]`, не `GS::UniString`.
    `std::string(p.name)` работает, `p.name.ToCStr()` — нет.

  • `ACAPI_CallUndoableCommand` в AC26+ принимает
    `std::function<GSErrCode(void)>`. Лямбда обязана возвращать
    `NoError` (иначе C2664).

  • `msgpack-cxx` на MSVC требует `MSGPACK_NO_WCHAR_T` (иначе
    wchar_t ≡ unsigned short → C2766) и `MSGPACK_NO_BOOST`
    (иначе sysdep.hpp тянет `<boost/predef/other/endian.h>`).

  • `SetTextContentAndParagraphs` — публичная утилита из
    `ElementCreationCommands.hpp`. Переиспользуется в новых
    командах вместо копирования логики.

  • `ACAPI_DisposeElemMemoHdls(&memo)` — после `GetMemo` и
    после `Change` (Change копирует данные, memo остаётся
    нашим). Паттерн совпадает с ElementCommands.cpp:2420.

  • `ACAPI_Element_GetElemList(API_TextID, &guids)` — двухаргументная
    форма (без APIFilterViewType) возвращает ВСЕ элементы проекта
    (не только текущего вида). Работает для API_TextID, API_LabelID
    и любого другого elem type.

### Bridge: ложный HTTP 500 на PUT → дубликаты кода (2026-10-06)

**Симптом:** после серии AI_CHANGES файл `BulkCommands.cpp` содержит
**три копии** одной функции `BulkCloneElementCommand::Execute` (и
столько же `ApplyGdlOverride`). Компилятор при этом молчит (файл
валиден синтаксически), но на линковке — `multiply defined symbol`.

**Корень:** GitHub Contents API на `PUT /repos/.../contents/<path>`
периодически отдаёт **HTTP 500** — но **сам PUT уже применён на
сервере**. Bridge обрабатывал 500 как «операция не прошла» → писал
`ОТКАЧЕНО` → пользователь (или LLM) посылал тот же блок заново →
Bridge дописывал содержимое второй раз → через три попытки — три
копии. Три копии BulkCloneElement в `BulkCommands.cpp` — прямое
следствие этого.

**Фикс (две части, обе в `agent_tools/response_handler.py`):**

1. **`_verify_github_write`** — после ошибки PUT делаем `GET` файла
   и сравниваем содержимое с ожидаемым. Если совпало (с точностью
   до `\r\n` / финального `\n`) — PUT **прошёл**, ошибка была
   только в ответе. Возвращаем `ok=True, verified_after_error=True`.

2. **Честный отчёт в `apply_changes`** — вместо единого
   `applied / failed` теперь раздельно:

       applied_local       — сколько локальных правок применилось
       applied_github      — сколько github://-правок применилось
       failed_local        — сколько локальных упало
       failed_github       — сколько github://-правок упало
       github_not_rolled_back : bool
                           — флаг «были github-правки, они на
                             сервере и НЕ откатываются»

   Текст `error` больше **не пишет** «все изменения возвращены»,
   если в пакете были github://-правки: `version_manager` работает
   только с локальной ФС и GitHub не трогает. Сообщение честное:
   «локальные откачены, github-правки уже на сервере и не
   откатываются».

**Следствие для LLM:** при ошибке пакета **не ретраить слепо весь
блок** — сначала посмотреть `applied_github`, `failed_github` и
`verified_after_error`. Если операция на самом деле прошла, повтор
создаст дубликаты.

### Bridge: таблица лимитов «было → стало» (2026-10-06)

Все hardcoded обрезки убраны в параметры запроса (`agent_tools/request_handler.py`):

    Параметр              Было        Стало (default)   Максимум
    -----------------------------------------------------------------
    max_files             20          200               10_000
    max_total_bytes       2 MB        20 MB             500 MB
    max_limit             2_000       20_000            200_000
    max_line_chars        400 / 300   2_000             100_000
    max_ops (batch)       10          50                1_000

**Как переопределять:** прямо в запросе — `{"max_line_chars": 50000}`
в корне `===AI_REQUEST===` (для read/search) или в `batch` (для ops).

**Сигналы обрезки:**
- В `read` / `search` — у каждой строки флаг `line_truncated: bool`.
- В `batch` — поле `truncated: bool` в ответе.
- Раньше молча резалось: длинные register_command с description
  приезжали обрезанными, и якоря `replace` не совпадали.

**Причина переделки:** десятки ошибок «old fragment not found» при
работе с AddOnMain.cpp были следствием молчаливой обрезки длинных
строк; LLM жёг токены на повторные чтения и дописывания.

### Python-обёртки в IFC_analyzer (в процессе)

`plugins/archicad_plugin/bulk_connection.py`:

    get_element_data(elements, selected, with_group_info,
                     with_group_members, ...)
    get_element_mesh(elements, ensure_3d_window=True)

**TODO** (следующий патч):

    get_selection()        # shortcut к selected=True
    get_group_members()    # обёртка над BulkGetGroupMembers
    bulk_clone_element()   # обёртка над BulkCloneElement

### Bridge: карта каналов (маркеров)

Все маркеры — обёрнуты в один ```json-фенс. Один ответ = один фенс.

    ===AI_REQUEST===        чтение кода (files / search / read / stat /
                            outline / list / batch / live). Не более
                            20 файлов на запрос (или свой max_files).
                            Массив операций внутри одного блока —
                            до 10 ops.

    ===AI_CHANGES===        правки кода (write / replace / delete).
                            Ровно один блок на ответ. Бэкап авто.
                            Для кода с отступами — base64-варианты
                            (content_b64 / old_b64 / new_b64).

    ===AI_TESTS===          прогон pytest. groups / target / marker /
                            extra / args.

    ===AI_PROGRAM===        runtime: command (одна команда через
                            dispatcher) / script (exec в контексте core)
                            / smoke (offscreen-сборка окон). Свежий
                            процесс = пустой ApplicationCore, вкладки
                            'main' нет — создавать первым делом.

    ===AI_FILES_REQUEST===  прицельное чтение ФАЙЛОВ-ДАННЫХ (docx,
                            xlsx, txt, csv, json). Не код.

    ===AI_FILES_CHANGES===  прицельная правка ФАЙЛОВ-ДАННЫХ.
                            source read-only, output — новый файл.

    ===AI_DONOR_*===        работа с донором (второй проект).
                            AI_DONOR_REQUEST / AI_DONOR_CHANGES /
                            AI_DONOR_PROGRAM / AI_TRANSFER.

    ===META_PROGRAM===      запуск метапрограммы из meta/programs/.

    ===GRAPH_OPS===         DSL граф-агента (роль B).

    ===META_GRAPH===        мета-граф из мета-нод (LLM-мета-редактор).

**Правила:**
- Один ответ = один ```json-фенс, внутри — маркер с двух сторон и
  валидный JSON. **Всё**, включая маркеры, внутри фенса.
- Не смешивать ===AI_REQUEST=== и ===AI_CHANGES=== в одном ответе.
- Не смешивать ===AI_CHANGES=== и ===AI_TESTS=== — сначала правка,
  дождаться apply, потом тесты отдельным ответом.
- Если пользователь потерял маркеры — парсер ищет JSON по фенсам и
  балансу фигурных скобок, но это страховка. Правильно — оборачивать
  целиком.

### Bridge: диагностика при подозрении на ложный откат

**Симптом:** Bridge написал «ОТКАЧЕНО — все изменения возвращены»,
но подозрение, что правка на самом деле применилась (из-за бага
HTTP 500 / ложных откатов прошлых версий).

**Что делать — НЕ ретраить блок слепо.** Порядок:

1. **Проверить фактическое состояние через ===AI_REQUEST===** (action=search
   или action=read). Файл на GitHub — источник истины, не отчёт
   Bridge. Например: `search pattern="BulkCloneElementCommand::Execute"`
   → если совпадений 1 — правка на месте, ретрай не нужен.

2. **Посмотреть поля отчёта** (после фикса от 2026-10-06):
   `applied_local`, `applied_github`, `failed_local`, `failed_github`,
   `github_not_rolled_back`, `verified_after_error`.

   - `verified_after_error: true` — PUT прошёл, ошибка была только в
     ответе; считать операцию успешной.
   - `github_not_rolled_back: true` — в пакете были github://-правки,
     они на сервере и НЕ откатываются (`version_manager` работает
     только с локальной ФС). Ретрай = дубликаты.

3. **Если правка действительно не применилась** — ретраить можно. Но
   лучше бить мелкими кусками (2-3 правки на блок), чтобы избежать
   больших PUT'ов, на которых чаще бывает HTTP 500.

**Признак, что надо перезапустить AI Bridge:** правишь `agent_tools/*.py`
(`response_handler.py`, `request_handler.py`, `github_provider.py`,
`bridge.py`, `protocol.py`), а поведение остаётся старым. Правки
этих файлов **не подхватываются на лету** — закрыть окно Bridge,
открыть заново (см. §4b).

**Проверка, что патч Bridge вообще на месте:** посмотреть метаданные
файла через `stat`. `mtime` свежий + размер вырос (например,
`response_handler.py` был 558 строк → стал 633) — значит патч на
диске.

### Bridge: сводка исправленных багов (2026-10-06)

Собраны в сессии, все исправлены в `agent_tools/` локально. Требуют
перезапуска Bridge.

| # | Баг | Файл | Фикс |
|---|---|---|---|
| 1 | Молчаливая обрезка строк в `read` (400 символов) | `request_handler.py` | Параметр `max_line_chars` (default 2000), флаг `line_truncated` у каждой строки |
| 2 | Молчаливая обрезка в `search` (300 символов) | `request_handler.py` | То же, `max_line_chars` |
| 3 | Жёсткие лимиты `max_files=20` / `max_total_bytes=2MB` | `request_handler.py` | Параметры (default 200 / 20 MB) |
| 4 | Жёсткий `max_ops=10` в batch | `request_handler.py` | Параметр (default 50) |
| 5 | Жёсткий `max_limit=2000` строк за read | `request_handler.py` | Параметр (default 20000) |
| 6 | HTTP 500 на PUT считался провалом, ретрай дописывал копии | `response_handler.py` | `_verify_github_write` — GET после ошибки, сверка содержимого |
| 7 | Отчёт «все изменения откачены» при github-правках (version_manager их не откатывает) | `response_handler.py` | Раздельные `applied_local` / `applied_github` / `github_not_rolled_back` |

**Свежий пример ущерба (до фиксов):** три ложных откатa подряд
привели к **трём копиям** `BulkCloneElementCommand` в `BulkCommands.cpp`
(и трём копиям `ApplyGdlOverride`). Компилятор молчал (файл
синтаксически валиден), но линковка упала бы на `multiply defined
symbol`. Дедуп сделан через большой replace с якорем на хвосте
`BulkGetGroupMembers::Execute`.

**Урок:** до фиксов **никогда не верить отчёту Bridge об откате** —
проверять фактическое состояние файла. После фиксов — смотреть
`applied_*` / `failed_*` / `verified_after_error`.

### Тесты bulk-команд на AC26 (2026-10-06, вечер)

Живой прогон через `BulkConnection` (IFC_analyzer) на проекте
«Шаблон гидравлики IFC», Tapir 1.7.1, порт 19724, .apx от 20:21.

**Проверено вживую 2026-10-07 (AC26, «Шаблон гидравлики IFC»):**

| Команда | Сценарий | Результат |
|---|---|---|
| `BulkPing` | 'hello-bulk', 10 байт | ✅ `size=10`, `zstd_version=10506` |
| `BulkGetTexts` | 20 Text + 20 Label | ✅ 40 строк, 40 непустых |
| `BulkGetPropertyValues` | 4 элемента × 3 свойства | ✅ OK |
| `BulkFindReplaceText` (dry_run) | 1006 scanned по 5 подстрокам | ✅ 60/23/42/131/88 matched, replaced=0 |
| `BulkGetElementData` V2 | 3 Object, all sections | ✅ bbox + GDL + properties + classifications |
| `BulkGetGroupMembers` (без data) | 1 выделенный Object | ✅ 1 запись, 37 `member_guids`, единый `group_guid` |
| `BulkGetGroupMembers with_data=true` | та же группа, 37 членов | ✅ 37 entity в одном round-trip, `details+bbox+gdl`, 152–172 params (130–150 GDL), missing=0 |
| `BulkSetElementData` (dry_run) | 2 Object, `story_index` + `bbox_size_x` + `foo_bar` | ✅ `applied=[story_index]`, `ignored_readonly=[bbox_size_x]`, `ignored_unknown=[foo_bar]` |
| `BulkSetElementData` (write) | element-field: `object_pos_x` 476.74→477.0→476.74 | ✅ |
| `BulkSetElementData` (write) | GDL: `GDL/A` 0.8→1.0→0.8 | ✅ |
| `BulkSetElementData` (write) | Archicad: `ElementID` `Ст. 1, 1А`→`TEST_BULK_27016`→`Ст. 1, 1А` | ✅ |
| `BulkSetElementData` (write) | class: `Радиатор отопления`→`Схемы отопления`→`Радиатор отопления` | ✅ |
| `BulkCloneElement` v2 | 1 Object → 2 клона | ✅ 2 `created_guids`, `per_source` с `errors=[]` |
| `BulkMoveElements` | dx=+5.0 | ✅ `moved_count=1`, Δx = 5.0 ровно |
| `BulkRotateElements` | angle_rad=π/2 | ✅ `rotated_count=1`, bbox_size 4.19/1.91 → 1.91/4.19 |
| `BulkGetElementData` (Zone→Object) | выделенная зона, `connected_types=['Object']` | ✅ 26 рёбер, все `kind='zone_content'` |
| `BulkGetElementData` (Wall→Window) | стена, `connected_types=['Window']` | ✅ 2 окна |
| `BulkGetElementData` (Label-text) | выделенная выноска, `with_2d_geometry=true` | ✅ `geometry.text = 'СТН-034\r#Тип, марка…'`, `owner_element_id='A59207C8-…'` (Wall) |
| `BulkGetElementData` (2D Hatch) | выделенная штриховка, `with_2d_geometry=true` | ✅ 5 точек контура, `holes=[]`, `fill_id`, `contour_pen_index=0` |
| `BulkGetElementData` (2D Circle) | выделенная окружность, `with_2d_geometry=true` | ✅ `origin`, `radius=2.1`, `geometry.type='Circle'` (различается от Arc) |
| `BulkGetElementData` (2D PolyLine) | случайный PolyLine, `with_2d_geometry=true` | ✅ `coordinates`, `arcs`, `room_separator` |
| `BulkGetElementData` (TypeName fix) | окружность без патча | ⚠ возвращал `element_type='Unknown'`; после патча — `'Circle'` |
| `BulkDeleteElements` | те же 2 клона | ✅ `deleted_count=2`, после — оба `element not found` |
| `BulkSetTexts` | no-op `'2'→'2'` | ⚠ `-2130313112` — ждёт сборки патча |

**Открытые баги (write-канал):**

1. **`BulkSetTexts`.** Патч на `TextLabelDetails::ApplyTextContent` отправлен
   2026-10-07, но не собран/установлен. Корень: старая
   `SetTextContentAndParagraphs` не заполняет `(*memo.paragraphs)[0].just`
   — AC26 `ACAPI_Element_Change` отбивает memo как `APIERR_BADPARS`.
   См. §4f, бэклог.

**Закрытые баги:**

- `BulkGetGroupMembers` `groups_count=0` — исправлен: теперь считается
  как `out["groups"].size()` (после дедупа по `group_guid`), не как размер
  входного `group_guids[]`.
- `BulkGetGroupMembers` дубликаты записей — исправлено: дедуп по
  `group_guid`, все источники одной группы собираются в одну запись с
  массивами `source_guids[]` / `source_kinds[]`.

**Не проверено (нужны условия):**

- `BulkGetElementData` с `selected=true` — возвращает пустой ответ на
  AC26. Возможно, параметр поддержан в более новых сборках.
- `RotateElementsByAngle` (форк-команда) — ждёт свежий .apx.

**Устаревшее (архив):**

- `BulkGetElementMesh` — старая заметка про «крешит в FloorPlan»
  относится к версии через SightPtr + `EXPGetModel`. В 2026-10-07
  команда переписана на низкоуровневый component-API
  (`ACAPI_ModelAccess_Get3DInfo` + `GetComponent`), 3D-окно больше
  не требуется. Подробности — в `ExtractElementMesh` в
  `BulkCommands.cpp`. См. §4f, таблицу реестра.

**Локальный путь .apx (AC26, Win):**
`C:\Program Files\GRAPHISOFT\Archicad 26\Расширения Archicad\TapirAddOn_AC26_Win.apx`.

---

## 10b. Сессия 2026-10-08 — зеркалирование, иерархические типы, edges, дырки в mesh

### Mirror / object_reflected (Object / MEP)

**Проблема:** при Mirror Archicad поворачивает Object (обычно на 180° в
`element.object.angle`), но **само отражение** отдельным полем не
пробрасывалось. Tapir `GetDetailsOfElements` уже отдаёт `reflected`,
но `get_element_data` (C++ `CollectElementData` + инлайн `BulkGetElementData::Execute`)
его игнорировал. Клиент строит transform только по `angle` → отзеркаленный
объект рисуется «повёрнутым, но не отражённым».

**Фикс C++ (`archicad-addon/Sources/BulkCommands.cpp`, 2 места):**

- `CollectElementData` — после `object_x_ratio/y_ratio`:

      params["object_reflected"] = element.object.reflected;

- `BulkGetElementDataCommand::Execute` (инлайн-блок Object-специфичных
  полей) — то же самое.

После пересборки аддона в `parameters` появится `object_reflected: bool`.
Клиент может строить корректную матрицу:

    world = T(pos_x, pos_y, level) · Rz(angle) · S(xRatio·(−1 если reflected), yRatio, 1) · local_mesh

### Expand иерархических типов в ExtractElementMesh

**Проблема:** Column / Beam / CurtainWall / Stair / Railing не имеют
собственного 3D-тела в топ-хедере. `ACAPI_ModelAccess_Get3DInfo`
возвращает пустой range (`vc=0, tc=0`), вся геометрия — в **субэлементах**
(`memo.columnSegments`, `memo.cWallFrames`, `memo.stairTreads` и т.д.).

**Пример (live, AC26):**

    Column A01B5355-...: top    vc=0  tc=0  err='empty mesh'
                          segment vc=32 tc=36 err=None

`bbox` у субэлементов не работает (даёт −1e38), но нам он не нужен —
читаем просто по `.head`.

**Фикс C++ (`ExtractElementMesh`, перед `Get3DInfo`):**

1. Тип элемента — `GetElemTypeId(elemHead)` в наборе
   `{Column, Beam, CurtainWall, Stair, Railing}`.
2. `ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_All)`.
3. Рекурсивный обход memo-полей через шаблонную лямбду
   `appendSub(auto* subelemArray)`:

       Column:      memo.columnSegments
       Beam:        memo.beamSegments
       CurtainWall: memo.cWallSegments + cWallFrames + cWallPanels
                    + cWallJunctions + cWallAccessories
       Stair:       memo.stairRisers + stairTreads + stairStructures
       Railing:     memo.railingNodes + railingSegments + railingPosts
                    + railingRailEnds + railingRailConnections
                    + railingHandrailEnds + railingHandrailConnections
                    + railingToprailEnds + railingToprailConnections
                    + railingRails + railingToprails + railingHandrails
                    + railingPatterns + railingInnerPosts + railingPanels
                    + railingBalusterSets + railingBalusters

4. Каждый субэлемент — рекурсивный `ExtractElementMesh(subelem[i].head, ...)`,
   склейка через offset по `base = outVertices.size() / 3`.
5. Если хоть что-то собрано — `return true`, дальше (в `Get3DInfo` на
   топ-хедер) не идём.
6. Wall / Slab / Roof / Object / Morph / Zone / Mesh — не трогаются, у них
   есть своё solid-тело.

Регистр полей memo — точный, взят из рабочей
`GetSubelementsOfHierarchicalElementsCommand` (ElementCommands.cpp).

### Edges (standalone рёбра) — «усики» GDL-объектов

**Проблема:** GDL-объекты (символьные линии, оси, размерные линии)
содержат `body.nEdge` — standalone рёбра без полигонов. Раньше они не
читались, актёр рисовался без «усиков».

**Фикс C++ (`ExtractElementMesh`):**

- Чтение `body.nEdge` → массив `edges` (`uint32`, пары индексов вершин).
- В row добавляются поля `edgeCount` и `edges` (binary).
- Если `outEdges != nullptr` — заполняем; иначе пропускаем (обратная
  совместимость с вызовами без 6-го аргумента).

**Фикс Python (`tools/_archicad_realtime_helpers.py`):**

- `mesh_from_row`: чтение `row['edges']` → VTK-формат `[2, i, j, 2, ...]` →
  `mesh.lines = lines_arr`. Один PolyData несёт `faces` (triangles) + `lines`
  (standalone edges). Edge-only объекты — PolyData без faces, с lines.
- `mesh_hash`: включает edges (`md5(vertices + triangles + edges)`). Изменение
  только в standalone edges (подвинули ось/размерную линию) → актёр
  перерисуется.
- **Убран `auto_orient_normals=True`.** Эта операция для согласования
  нормалей стремится сделать mesh «закрытым» и **визуально заделывает
  реальные отверстия** в геометрии (стены с проёмами, объекты с вырезами,
  торцы воздуховодов). Оставлен простой `compute_normals(inplace=True)`.

**Диагностика (live, AC26):**

    Object 4F8F1842 (Отвод Воздуховода 20):
      vc=52, tc=72, edges=24 border
      border components (openings) = 6 (4 вершины каждое)
      CAPS (all 3 verts in same opening) = 0
      duplicate triangle coord-sets = 0

`CAPS = 0` подтвердил: **C++ не заделывает дырки**. Заделка была в
Python-рендере (`auto_orient_normals`), устранена в этом же патче.

### C5046 / C2572 — сигнатура ExtractElementMesh

**C5046** (`Symbol involving type with internal linkage not defined`, /WX →
C2220): forward-decl объявлял 5 аргументов (`errOut;`), а определение — 6
(`errOut, outEdges = nullptr`). MSVC трактовал как **разные перегрузки**;
5-арг версия без тела → C5046, `/WX` валит сборку.

**C2572** (`redefinition of default argument`): после первого фикса на
forward-decl появился `= nullptr`, но в **AC27+ определении** (стр. 2881)
тоже остался свой `= nullptr` → конфликт.

**Фикс (BulkCommands.cpp):**

- Forward-decl (`~1289`) — 6 аргументов, `outEdges = nullptr` —
  **единственный** default в TU.
- AC25/26 определение (`~2358`) — 6 аргументов, **без** default.
- AC27+ определение (`~2876`) — 6 аргументов, **без** default.
- Существующие вызовы с 5 арг получают 6-й по умолчанию из forward-decl.

**Компиляция:** зелёная на AC25/26/27+ (проверено в CI GitHub Actions
после обоих фиксов).

### Известные особенности AC26 (не чиним в этой сессии)

- **Arc с `begAngle==endAngle==0` при `radius>0`** — это полная окружность
  (Circle). Archicad не имеет отдельного типа `Circle`. `ElementTypeName`
  уже имеет `API_CircleID` в switch, но AC26 фактически отдаёт круги как
  `API_ArcID` с нулевыми углами. Отдельно чинить не надо — можно
  восстанавливать тип по углам.

- **Window / Door — `body.tranmat` даёт перепутанные оси.** Мировая
  ориентация окон неверна: локальные x/y/z ↔ мировые меняются местами.
  `bbox` окна верный (`x=толщина, y=ширина, z=высота`), а mesh после
  `apply_transform=True` поворачивает оси (локальная X → мировая Z и т.д.).
  Правильный путь — строить ориентацию из `element.window.pos/angle` +
  системы стены-хозяина. Отложено; для рендера пока используем `bbox`.

- **`Get3DInfo failed` на 2D-элементах** (Arc / Line / Circle / PolyLine /
  Hatch / Text / Label / Hotspot). Ожидаемо — у 2D нет тела. Клиент
  фильтрует по `element_type`. Early-return `vc=0, tc=0, err=None` — в
  backlog, не критично.

- **Библиотечный элемент «Оконный Проем Прямоугольный 26»** — это проём,
  не окно. Archicad IFC-экспортёр может выдавать `IfcOpeningElement` без
  парного `IfcWindow`. Это **данные проекта**, не код аддона. Проверять
  при отладке IFC-экспорта (см. опции IFC: Openings/Windows mapping).

### Backlog (в эту сессию не входит)

- `_decode_mesh_rows` в `plugins/archicad_plugin/bulk_connection.py` — не
  тронут. Если нужно, чтобы `bc.get_element_mesh()` возвращал `edges` —
  отдельная правка (тесты ходят через `bc._call('BulkGetElementMesh', ...)`
  напрямую и берут сырые rows — им edges уже доступны).
- Правильная мировая ориентация Window / Door (см. выше).
- Early-return в `ExtractElementMesh` для 2D-типов (vc=0/tc=0 без ошибки).
- Roundtrip 2D-аннотаций обратно в Archicad через `BulkCreateElements`
  (Text / Label / Dimension) — если понадобится.
- AC27+ mesh — graceful stub (не поддерживается, `ACAPI_ModelAccess_*`
  aliases не эмитируются MigrationHelper.hpp).

---

## 11. Контакты и ссылки

- **Форк:** https://github.com/kramolala-ui/tapir-archicad-automation (ветка `fresh`)
- **Upstream:** https://github.com/ENZYME-APD/tapir-archicad-automation
- **Автор форка:** kramolala-ui
- **Локальная копия:** `C:\Python_projects\tapir-custom\`

---

_Если этот файл устарел — правь его. Он живёт здесь как точка
входа для новых сессий._
