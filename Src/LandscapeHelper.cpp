#include "LandscapeHelper.hpp"
#include "ACAPinc.h"

GSErrCode LandscapeHelper::DistributeSelected(double step, int count, GS::UniString& result)
{
    GS::UniString msg;
    msg.Printf("[LH] DistributeSelected step=%.2f count=%d", step, count);
    ACAPI_WriteReport(msg.ToCStr().Get(), false);

    result = "Распределение вызвано (заглушка)";
    return NoError;
}

void LandscapeHelper::RegisterLandscapeJS(JS::Object* jsACAPI)
{
    jsACAPI->AddItem(new JS::Function("DistributeNow", [](GS::Ref<JS::Base>) {
        ACAPI_WriteReport("[JS] DistributeNow called", false);

        GS::UniString result;
        LandscapeHelper::DistributeSelected(1.0, 0, result);

        return new JS::Value(result);
        }));
}
