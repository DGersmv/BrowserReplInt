#pragma once
#include "UniString.hpp"
#include "BrowserRepl.hpp"   // здесь у тебя уже подключены JS::Object и всё остальное

namespace LandscapeHelper {
    GSErrCode DistributeSelected(double step, int count, GS::UniString& result);

    void RegisterLandscapeJS(JS::Object* jsACAPI);
}
