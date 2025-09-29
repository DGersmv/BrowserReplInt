#include "PropertyUtils.hpp"

#include "UniString.hpp"
#include "APIdefs_Properties.h"

#include "APIEnvir.h"
#include "ACAPinc.h"	


GSErrCode PropertyUtils::PropertyToString (const API_Property& property, GS::UniString& propertyValue)
{
	if (property.status == API_Property_NotAvailable || property.status == API_Property_NotEvaluated) {
		return APIERR_BADPROPERTY;
	}

	return ACAPI_Property_GetPropertyValueString (property, &propertyValue);
}