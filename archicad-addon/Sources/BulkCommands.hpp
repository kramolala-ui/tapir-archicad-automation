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


