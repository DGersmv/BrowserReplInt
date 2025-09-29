#ifndef GS_PROPERTYUTILS_HPP
#define GS_PROPERTYUTILS_HPP


#include "Definitions.hpp"


struct API_Property;

namespace GS {
	class UniString;
}


namespace PropertyUtils {

GSErrCode	PropertyToString (const API_Property& property, GS::UniString& propertyValue);

}


#endif