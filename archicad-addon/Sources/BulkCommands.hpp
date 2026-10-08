#pragma once

#include "CommandBase.hpp"

#include <memory>

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
// Each BulkCreate*Command wraps the matching Create*Command through
// CreateElementsCommandBase::CreateMany (one undo for the whole batch,
// per-item errors). The delegate is a std::shared_ptr so that the bulk
// wrapper never has to know the concrete Create*Command layout at
// declaration time - a forward-declare is enough here.
//
// Input payload (msgpack inside payload_b64):
//     { "<arrayFieldName>": [ {item}, {item}, ... ] }
//
// "<arrayFieldName>" is whatever the delegate was constructed with
// ("polylinesData" for CreatePolylines, "wallsData" for CreateWalls, ...)
// and is read at Execute time from delegate->GetArrayFieldName(), so the
// JSON and bulk channels accept the exact same shape.
//
// Output payload (msgpack inside payload_b64):
//     { "per_source":   [ {index, ok, guid|error} ],
//       "created_guids": ["guid", ...],
//       "created_count": N,
//       "errors_count":  M }
//
// Top-level response carries payload_b64, compression, created_count,
// errors_count - same envelope shape as the other Bulk* commands, so
// the Python client can read counts without unpacking msgpack.
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

class BulkCreatePolylinesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreatePolylinesCommand () : BulkCreateElementsCommandBase ("BulkCreatePolylines", std::make_shared<CreatePolylinesCommand> ()) {}
};

class BulkCreateLineElementsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateLineElementsCommand () : BulkCreateElementsCommandBase ("BulkCreateLineElements", std::make_shared<CreateLineElementsCommand> ()) {}
};

class BulkCreateArcsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateArcsCommand () : BulkCreateElementsCommandBase ("BulkCreateArcs", std::make_shared<CreateArcsCommand> ()) {}
};

class BulkCreateCirclesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateCirclesCommand () : BulkCreateElementsCommandBase ("BulkCreateCircles", std::make_shared<CreateCirclesCommand> ()) {}
};

class BulkCreateSplinesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateSplinesCommand () : BulkCreateElementsCommandBase ("BulkCreateSplines", std::make_shared<CreateSplinesCommand> ()) {}
};

class BulkCreateHotspotsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateHotspotsCommand () : BulkCreateElementsCommandBase ("BulkCreateHotspots", std::make_shared<CreateHotspotsCommand> ()) {}
};

class BulkCreateHatchesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateHatchesCommand () : BulkCreateElementsCommandBase ("BulkCreateHatches", std::make_shared<CreateHatchesCommand> ()) {}
};

class BulkCreateTextsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateTextsCommand () : BulkCreateElementsCommandBase ("BulkCreateTexts", std::make_shared<CreateTextsCommand> ()) {}
};

class BulkCreateLabelsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateLabelsCommand () : BulkCreateElementsCommandBase ("BulkCreateLabels", std::make_shared<CreateLabelsCommand> ()) {}
};

class BulkCreateColumnsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateColumnsCommand () : BulkCreateElementsCommandBase ("BulkCreateColumns", std::make_shared<CreateColumnsCommand> ()) {}
};

class BulkCreateSlabsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateSlabsCommand () : BulkCreateElementsCommandBase ("BulkCreateSlabs", std::make_shared<CreateSlabsCommand> ()) {}
};

class BulkCreateZonesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateZonesCommand () : BulkCreateElementsCommandBase ("BulkCreateZones", std::make_shared<CreateZonesCommand> ()) {}
};

class BulkCreateObjectsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateObjectsCommand () : BulkCreateElementsCommandBase ("BulkCreateObjects", std::make_shared<CreateObjectsCommand> ()) {}
};

class BulkCreateLampsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateLampsCommand () : BulkCreateElementsCommandBase ("BulkCreateLamps", std::make_shared<CreateLampsCommand> ()) {}
};

class BulkCreateMeshesCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateMeshesCommand () : BulkCreateElementsCommandBase ("BulkCreateMeshes", std::make_shared<CreateMeshesCommand> ()) {}
};

class BulkCreateWallsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateWallsCommand () : BulkCreateElementsCommandBase ("BulkCreateWalls", std::make_shared<CreateWallsCommand> ()) {}
};

class BulkCreateBeamsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateBeamsCommand () : BulkCreateElementsCommandBase ("BulkCreateBeams", std::make_shared<CreateBeamsCommand> ()) {}
};

class BulkCreateStairsCommand : public BulkCreateElementsCommandBase
{
public:
    BulkCreateStairsCommand () : BulkCreateElementsCommandBase ("BulkCreateStairs", std::make_shared<CreateStairsCommand> ()) {}
};
