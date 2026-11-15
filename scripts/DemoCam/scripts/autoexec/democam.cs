// #autoload
// #name = DemoCam
// #version = 0.3
// #date = January 3, 2014 (original), updated for t2launch
// #category = Demos
// #author = Lou Cypher
// #warrior = LouCypher

// The three memPatch() calls of version 0.2 are gone. On Linux the equivalent changes are made
// natively by the t2launch hook: turn on the "Switch players in demos" option (review.demo_camera),
// which lets scripts see and control the demo's player objects.

$dcCO = 0;
$dcT = -1;
$dcL = false;
package dcp
{
	function StartSelectedDemo()
	{
		parent::StartSelectedDemo();
		GlobalActionMap.bindCmd(keyboard, "down", "", "changeDemoTarget(0);");
		GlobalActionMap.bindCmd(keyboard, "right", "", "changeDemoTarget(1);");
		GlobalActionMap.bindCmd(keyboard, "left", "", "changeDemoTarget(-1);");
	}

	function demoPlaybackComplete()
	{
		parent::demoPlaybackComplete();
		GlobalActionMap.unbind(keyboard, down);
		GlobalActionMap.unbind(keyboard, right);
		GlobalActionMap.unbind(keyboard, left);
	}

	function demoPlaybackUpdate(%curTime)
	{
		if (($dcL) && ($dcT != -1))
		{
			if (!isObject($dcCO))
			{
				%count = CommandTargetSet.getCount();
				for (%i=0;%i<%count; %i++)
				{
					if (CommandTargetSet.getObject(%i).getTarget() == $dcT) $dcCO = CommandTargetSet.getObject(%i);
				}
			}
			if (ServerConnection.getControlObject() != $dcCO) ServerConnection.setControlObject($dcCO);
		}
		parent::demoPlaybackUpdate(%curTime);
	}
};
if (!isActivePackage(dcp)) activatePackage(dcp);

function changeDemoTarget(%key)
{
	if (%key == 0)
	{
		$dcL = !$dcL;
		echo("lock: " @ $dcL);
		if(!$dcL) clientCmdBottomPrint( "lock: OFF", 3, 0 );
		return;
	}
	else
	{
		$dcL = true;
		%count = PlayerListGroup.getCount();
		for (%i=0;%i<%count; %i++)
		{
			if ($dcT == PlayerListGroup.getObject(%i).targetId) %obj = %i;
		}
		%obj += %key;
		if (%obj < 0) %obj = %count - 1;
		if (%obj == %count) %obj = 0;
		clientCmdBottomPrint( PlayerListGroup.getObject(%obj).name, 3, 0 );
		$dcT = PlayerListGroup.getObject(%obj).targetId;
		%count = CommandTargetSet.getCount();
		for (%i=0;%i<%count; %i++)
		{
			if (CommandTargetSet.getObject(%i).getTarget() == $dcT) $dcCO = CommandTargetSet.getObject(%i);
		}
		ServerConnection.setControlObject($dcCO);
	}
}