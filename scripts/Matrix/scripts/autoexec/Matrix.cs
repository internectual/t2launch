// Matrix: starts the sky views of the t2launch hook when the Matrix map loads.
// Needs the hook command set; on a stock client every call is skipped.

function matrixSkyDefaults()
{
	// Used when there is no saved "matrix" layout: a few animated views over the dark green fallback sky.
	t2launch_skyViewClear();
	t2launch_skyViewAdd("fr", "builtin:plasma", 0.1, 0.1, 0.8, 0.8, 0.9);
	t2launch_skyViewAdd("rt", "builtin:aurora", 0, 0, 1, 1, 1);
	t2launch_skyViewAdd("lf", "builtin:stars", 0, 0, 1, 1, 1);
	t2launch_skyViewAdd("up", "builtin:stars", 0, 0, 1, 1, 1);
}

function matrixSkyUpdate(%mission)
{
	if (!$T2Launch::SkyViews)
		return;
	if (%mission $= "Matrix")
	{
		t2launch_skyFullBox(1);
		if (t2launch_skyLayoutLoad("matrix") < 1)
			matrixSkyDefaults();
	}
	else
	{
		t2launch_skyViewClear();
		t2launch_skyFullBox(0);
	}
}

package MatrixSky
{
	function clientCmdMissionStartPhase3(%seq, %missionName)
	{
		parent::clientCmdMissionStartPhase3(%seq, %missionName);
		matrixSkyUpdate(%missionName);
	}
};
if (!isActivePackage(MatrixSky))
	activatePackage(MatrixSky);
