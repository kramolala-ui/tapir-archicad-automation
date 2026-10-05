#pragma once

#include "CommandBase.hpp"

// Bulk commands: бинарный транспорт (msgpack + zstd) поверх JSON-обёртки.
//
// Идея: сложные данные (тысячи свойств × десятки тысяч элементов)
// сериализуются в msgpack, сжимаются zstd, кодируются в base64 и
// кладутся в поле payload_b64 обычной JSON-команды. Graphisoft runtime
// видит просто строку, а разбор msgpack делает уже наш C++ код — без
// 40 HTTP-раундов и без парсинга гигантского JSON.
//
// BulkPing — тестовая команда: без msgpack и zstd, только base64.
// Проверяет, что транспорт от Python до C++ аддона работает.

class BulkPingCommand : public CommandBase
{
public:
    BulkPingCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
