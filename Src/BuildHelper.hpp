#pragma once
#include "APIEnvir.h"
#include "ACAPinc.h"

namespace BuildHelper {
	bool SetCurveForSlab();
	bool CreateSlabAlongCurve(double width);
	bool SetCurveForShell();
	bool SetMeshForShell();
	bool CreateShellAlongCurve(double width);
}
