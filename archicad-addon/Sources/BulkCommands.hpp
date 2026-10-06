#pragma once

#include "CommandBase.hpp"

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
