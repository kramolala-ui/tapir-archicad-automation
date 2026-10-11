# Правила трансформации элементов Archicad

Внутренняя заметка для тех, кто правит этот addon. Описывает
три бага, которые были допущены при реализации BulkGetElementMesh
и BulkGetElementPlanView, и правила, которые их предотвращают.

Связанные файлы:
  • BulkCommands.cpp — функция ApplyTranmat (правило 1);
  • BulkCommands.cpp — функция ExtractElementMesh, блок
    Object/Lamp (правила 2 и 3).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Правило 1. ApplyTranmat читает API_Tranmat как ROW-MAJOR
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

API_Tranmat в DevKit упакован как row-major (3 строки × 4 столбца):

    tmx[0..2]   — строка 0 матрицы поворота
    tmx[3]      — translation.x
    tmx[4..6]   — строка 1
    tmx[7]      — translation.y
    tmx[8..10]  — строка 2
    tmx[11]     — translation.z

Применение к точке (x, y, z):

    nx = tmx[0]*x + tmx[1]*y + tmx[2]*z  + tmx[3]
    ny = tmx[4]*x + tmx[5]*y + tmx[6]*z  + tmx[7]
    nz = tmx[8]*x + tmx[9]*y + tmx[10]*z + tmx[11]

ОШИБКА, которая была допущена ранее: column-major чтение
(tmx[0], tmx[4], tmx[8] как «столбец 0»). Это транспонировало
матрицу поворота — R(-a) вместо R(a). На углах 0 и π ошибка
невидима (R(0)=I, R(π)=-I симметричны), а на π/2, 3π/2, 5π/4
и т. п. объект зеркалился относительно точки вставки.

Проверка при правках: для объекта с углом π/2 мировой bbox
должен совпадать с Archicad bbox (поле bbox_center_x/y из
BulkGetElementData). Если он зеркален относительно object_pos —
в ApplyTranmat снова column-major.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Правило 2. НЕ перезаписывать bodyTran для Object / Lamp
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

body.tranmat от ACAPI_ModelAccess_Get3DInfo (поле API_Component3D
.body.tranmat) — УЖЕ содержит все трансформации GDL-элемента:

    • placement (object.angle);
    • GDL rotation (GDL/Rotation);
    • MEP connection rotations (GDL/RotationAroundZAxis /
      RotationAroundDAxis / RotationAroundWAxis);
    • reflected state.

Ранее в ExtractElementMesh была override-ветка, которая строила
матрицу только из object.angle. Она ТЕРЯЛА GDL-углы. У MEP-
отводов GDL/Rotation часто не совпадает с object.angle, и
объект разворачивался не туда.

Правило: для Object и Lamp использовать bodyTran как есть.
Никакого override.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Правило 3. Reflection — свойство GDL, НЕ мировая операция
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Отражение (Mirror) в Archicad — это GDL-состояние объекта.
При reflected=true GDL-движок выполняет скрипт в отражённой
локальной системе. Ось отражения ФИКСИРОВАНА (нельзя выбрать).
Визуальный эффект зависит от формы: симметричный отвод (90°) —
визуально no-op; несимметричный (180°) — разворот на 180°.

Archicad кодирует отражение парой (reflected, object.angle).
Угол object.angle — компенсирующий, чтобы объект остался
на месте.

Правило: НЕ применять Mirror_X на клиенте. Ни в C++, ни в
Python-нодах. Отражение уже учтено в body.tranmat.

Что пробовали и откатили:
  • мировое Mirror_X в Python (bulk_load_3d._mesh_from_row);
  • пропуск override для reflected (body.tranmat as-is).

Оба варианта давали двойное отражение для части объектов —
в итоге стало хуже, чем было. Правильный путь — вообще не
трогать reflected на клиенте.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Сравнение с 2D-планом (BulkGetElementPlanView)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

BulkGetElementPlanView возвращает примитивы УЖЕ в мировых XY —
transformation применять не нужно. Это эталон поведения:
Archicad сам отдаёт world-координаты через ShapePrims.

Для 3D та же логика: world-матрица уже есть в body.tranmat,
мы её только применяем через ApplyTranmat. Никаких override.

Исторические баги и полный разбор — в основном проекте, файл
docs/GEOMETRY_UNIFICATION.md, разделы 3.4 / 3.5 / 3.6.
