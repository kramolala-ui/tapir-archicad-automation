#pragma once

#include "CommandBase.hpp"

#include <memory>

// 2026-10-13: Version-guards для команд 2-й/3-й волн.
//
// TAPIR_HAS_EXTENDED_BULK = 0: AC29. Там переехали/удалены
//   ACAPI_ElemDifferenceGenerator_*, API_IFCProperty, часть Property API.
//   Для AC29 остаются только «классические» bulk-команды (Ping, Get/Set
//   текстов, mesh, clone, move, rotate, delete, create, set_element_data,
//   get_element_data).
//
// TAPIR_HAS_PROJECT_DIFF = 0: AC25, AC26, AC29.
//   AC25: у API_ElemDifferenceGenerator_GenerateDifference 4-й аргумент
//   — указатель (не ссылка); API_ElemDifference не содержит
//   isEnvironmentChanged. Проще вырезать команду, чем городить
//   версионный шим.
//   AC26 (2026-10-13 вечер): команда на живом проекте (662 объекта,
//   3362 свойства) уходит в HTTP-таймаут, Archicad перестаёт отвечать
//   на все последующие запросы. Причина — подвисание в
//   ACAPI_ElemDifferenceGenerator_GetState или GenerateDifference.
//   Диагностика отложена. Рабочие версии для ProjectDiff: AC27, AC28.
//   AC29: API переехало.
#if defined(ServerMainVers_2900)
#  define TAPIR_HAS_EXTENDED_BULK 0
#else
#  define TAPIR_HAS_EXTENDED_BULK 1
#endif

#if defined(ServerMainVers_2500) || defined(ServerMainVers_2600) || defined(ServerMainVers_2900)
#  define TAPIR_HAS_PROJECT_DIFF 0
#else
#  define TAPIR_HAS_PROJECT_DIFF 1
#endif

// 2026-10-13: Валидация формул свойств через Goodies.
// APIAny_CheckPropertyExpressionStringID ('CPES') есть в AC26 DevKit.
// Для AC25 / AC27+ / AC28 / AC29 — не проверено, включаем только AC26.
// При появлении в других версиях — расширить условие.
#if defined(ServerMainVers_2600) && !defined(ServerMainVers_2700)
#  define TAPIR_HAS_PROPERTY_EXPR_CHECK 1
#else
#  define TAPIR_HAS_PROPERTY_EXPR_CHECK 0
#endif

class CreateElementsCommandBase;

// Bulk commands: бинарный транспорт (msgpack + zstd) поверх JSON-обёртки.
//
// Обоснование — AI_PRINCIPLES.md, разделы 4, 4d, 5, 9:
//   • Graphisoft JSON-сериализатор даёт O(N²) при N×K > ~5000 значений
//     (50 элементов × 100 свойств = 6.9 s, 100×100 = timeout);
//   • Серия вызовов GetPropertyValuesOfElements вешает Archicad
//     после второго вызова в одной сессии.
// Один Execute в C++ обходит обе проблемы: JSON-стену (msgpack+zstd
// на выходе) и нестабильность (один undo-барьер на всю операцию).

// =====================================================================
//  Классические команды — доступны на всех версиях Archicad.
// =====================================================================

class BulkPingCommand : public CommandBase
{
public:
    BulkPingCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetPropertyValuesCommand : public CommandBase
{
public:
    BulkGetPropertyValuesCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetTextsCommand : public CommandBase
{
public:
    BulkGetTextsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkSetTextsCommand : public CommandBase
{
public:
    BulkSetTextsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkFindReplaceTextCommand : public CommandBase
{
public:
    BulkFindReplaceTextCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetElementMeshCommand : public CommandBase
{
public:
    BulkGetElementMeshCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetElementPlanViewCommand : public CommandBase
{
public:
    BulkGetElementPlanViewCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetContextCommand : public CommandBase
{
public:
    BulkGetContextCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkSwitchContextCommand : public CommandBase
{
public:
    BulkSwitchContextCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetElementDataCommand : public CommandBase
{
public:
    BulkGetElementDataCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkCloneElementCommand : public CommandBase
{
public:
    BulkCloneElementCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetGroupMembersCommand : public CommandBase
{
public:
    BulkGetGroupMembersCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkMoveElementsCommand : public CommandBase
{
public:
    BulkMoveElementsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkRotateElementsCommand : public CommandBase
{
public:
    BulkRotateElementsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkSetElementDataCommand : public CommandBase
{
public:
    BulkSetElementDataCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkDeleteElementsCommand : public CommandBase
{
public:
    BulkDeleteElementsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkCloneLabelsCommand : public CommandBase
{
public:
    BulkCloneLabelsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// ---------------------------------------------------------------------
// Bulk-create family
//
// BulkCreateElementsCommandBase wraps any Create*Command that derives
// from CreateElementsCommandBase (ElementCreationCommands.hpp and
// ExtendedElementCommands.hpp) through its public CreateMany: one undo
// for the whole batch, per-item errors, exactly the same element
// construction path as the JSON channel. The delegate is a
// std::shared_ptr<CreateElementsCommandBase> so this header only needs
// a forward-declare of the base; the concrete Create*Command types are
// only named in BulkCommands.cpp where both headers are pulled in.
//
// Wire format (msgpack inside payload_b64), identical to the JSON
// channel but binary-transported:
//   input:  { "<arrayFieldName>": [ {item}, {item}, ... ] }
//   output: { "per_source":    [ {index, ok, guid|error} ],
//             "created_guids": ["guid", ...],
//             "created_count": N,
//             "errors_count":  M }
// "<arrayFieldName>" is read from delegate->GetArrayFieldName() at
// Execute time, so the same payload key the JSON command expects
// works here ("polylinesData" for CreatePolylines, "wallsData" for
// CreateWalls, ...).
//
// Top-level response carries payload_b64, compression, created_count,
// errors_count - matching the other Bulk* commands so clients can read
// counters without unpacking msgpack.
//
class BulkCreateElementsCommandBase : public CommandBase
{
public:
    BulkCreateElementsCommandBase (const GS::String& name,
                                   std::shared_ptr<CreateElementsCommandBase> delegate);

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

protected:
    GS::String                                 commandName;
    std::shared_ptr<CreateElementsCommandBase> delegate;
};

class BulkCreatePolylinesCommand    : public BulkCreateElementsCommandBase { public: BulkCreatePolylinesCommand (); };
class BulkCreateLineElementsCommand : public BulkCreateElementsCommandBase { public: BulkCreateLineElementsCommand (); };
class BulkCreateArcsCommand         : public BulkCreateElementsCommandBase { public: BulkCreateArcsCommand (); };
class BulkCreateCirclesCommand      : public BulkCreateElementsCommandBase { public: BulkCreateCirclesCommand (); };
class BulkCreateSplinesCommand      : public BulkCreateElementsCommandBase { public: BulkCreateSplinesCommand (); };
class BulkCreateHotspotsCommand     : public BulkCreateElementsCommandBase { public: BulkCreateHotspotsCommand (); };
class BulkCreateHatchesCommand      : public BulkCreateElementsCommandBase { public: BulkCreateHatchesCommand (); };
class BulkCreateTextsCommand        : public BulkCreateElementsCommandBase { public: BulkCreateTextsCommand (); };
class BulkCreateLabelsCommand       : public BulkCreateElementsCommandBase { public: BulkCreateLabelsCommand (); };
class BulkCreateColumnsCommand      : public BulkCreateElementsCommandBase { public: BulkCreateColumnsCommand (); };
class BulkCreateSlabsCommand        : public BulkCreateElementsCommandBase { public: BulkCreateSlabsCommand (); };
class BulkCreateZonesCommand        : public BulkCreateElementsCommandBase { public: BulkCreateZonesCommand (); };
class BulkCreateObjectsCommand      : public BulkCreateElementsCommandBase { public: BulkCreateObjectsCommand (); };
class BulkCreateLampsCommand        : public BulkCreateElementsCommandBase { public: BulkCreateLampsCommand (); };
class BulkCreateMeshesCommand       : public BulkCreateElementsCommandBase { public: BulkCreateMeshesCommand (); };
class BulkCreateWallsCommand        : public BulkCreateElementsCommandBase { public: BulkCreateWallsCommand (); };
class BulkCreateBeamsCommand        : public BulkCreateElementsCommandBase { public: BulkCreateBeamsCommand (); };
class BulkCreateStairsCommand       : public BulkCreateElementsCommandBase { public: BulkCreateStairsCommand (); };

// =====================================================================
//  Команды 2-й/3-й волн. ProjectDiff — отдельный guard (AC25 и AC29
//  вырезают). Остальные 8 — под TAPIR_HAS_EXTENDED_BULK (AC29 вырезает).
// =====================================================================

#if TAPIR_HAS_PROJECT_DIFF

class BulkGetProjectDiffCommand : public CommandBase
{
public:
    BulkGetProjectDiffCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

#endif  // TAPIR_HAS_PROJECT_DIFF

#if TAPIR_HAS_EXTENDED_BULK

class BulkGetProjectInfoCommand : public CommandBase
{
public:
    BulkGetProjectInfoCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetElementHeadersCommand : public CommandBase
{
public:
    BulkGetElementHeadersCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkCreatePropertyDefinitionsCommand : public CommandBase
{
public:
    BulkCreatePropertyDefinitionsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkDeletePropertyDefinitionsCommand : public CommandBase
{
public:
    BulkDeletePropertyDefinitionsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetPropertyDefinitionsCommand : public CommandBase
{
public:
    BulkGetPropertyDefinitionsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkChangePropertyDefinitionsCommand : public CommandBase
{
public:
    BulkChangePropertyDefinitionsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkGetPropertyGroupsCommand : public CommandBase
{
public:
    BulkGetPropertyGroupsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkChangePropertyGroupsCommand : public CommandBase
{
public:
    BulkChangePropertyGroupsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

#if TAPIR_HAS_PROPERTY_EXPR_CHECK

class BulkValidatePropertyDefinitionsCommand : public CommandBase
{
public:
    BulkValidatePropertyDefinitionsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

#endif  // TAPIR_HAS_PROPERTY_EXPR_CHECK

class BulkGetIFCPropertiesCommand : public CommandBase
{
public:
    BulkGetIFCPropertiesCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkSetIFCPropertiesCommand : public CommandBase
{
public:
    BulkSetIFCPropertiesCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class BulkRemoveIFCPropertiesCommand : public CommandBase
{
public:
    BulkRemoveIFCPropertiesCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

#endif  // TAPIR_HAS_EXTENDED_BULK
