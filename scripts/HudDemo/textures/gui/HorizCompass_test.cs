// #autoload
// #include = support/loadout.cs

if(!isObject(HorizCompass)) {
	new scriptobject (HorizCompass) {
		class = HorizCompass;
	    };
}

	new GuiControl(horizcompassGUI) {
		profile = "GuiDefaultProfile";
		horizSizing = "right";
		vertSizing = "bottom";
		position = "7 210";
		extent = "100 23";
		minExtent = "100 23";
		visible = "1";
		hideCursor = "0";
		bypassHideCursor = "0";
		helpTag = "0";

		new GuiControl(HC_axisCTRL) {
			profile = "GuiDefaultProfile";
			horizSizing = "center";
			vertSizing = "center";
			position = "0 4";
			extent = "100 14";
			minExtent = "100 14";
			visible = "1";

			new GuiBitmapCtrl(HC_axis) {
				profile = "GuiDefaultProfile";
				horizSizing = "width";
				vertSizing = "height";
				position = "0 0";
				extent = "630 14";
				minExtent = "630 14";
				visible = "1";
				bitmap = "gui/uicompass.jpg";
			};
		};
		new HudBitmapCtrl() {
			profile = "GuiDefaultProfile";
			horizSizing = "right";
			vertSizing = "bottom";
			position = "-4 0";
			extent = "108 23";
			minExtent = "108 23";
			visible = "1";
			hideCursor = "0";
			bypassHideCursor = "0";
			helpTag = "0";
			opacity = "1";
			bitmap = "gui/uibackgroundcompass";
			autoCenter = "1";
			autoResize = "0";
		};
	};

playgui.add(horizcompassGUI);
if (isObject(HM) && isObject(HudMover))
    hudmover::addhud(horizcompassGUI, "Horizon Comp");

package HorizCompass {
function GuiInspector::inspect(%this, %obj) {
		 parent::inspect(%this, %obj);

         if(!playgui.isAwake()) return;

         if(ServerConnection.getControlObject() == %obj) HorizCompass.Update();

         HorizCompass.turndir = getWord(InspectStaticrotation.getValue(), 2);
         HorizCompass.rotation = getWord(InspectStaticrotation.getValue(), 3);
}

function updatethis() {
         if(!playgui.isAwake()) return;
         
         if(!InspectFields.isAwake()) InspectFields.inspect(ServerConnection.getControlObject());
         HorizCompass.Update();
}

function HorizCompass::Update(%this) {
         if (%this.rotation > 270 || %this.turndir < 0.0)
             $Axispos1 = 360 - HorizCompass.rotation;
		     else
             $Axispos1 = %this.rotation;
             
             $Axispos1 = mfloor($Axispos1);

             if (HorizCompass.turndir < 0.0)
                HC_axis.setPosition((-1 * $Axispos1) + 45 - 2, 0);
                else if ($Axispos1 > 180 && $Axispos1 < 270)
                HC_axis.setPosition(- 1.0 * firstWord(HC_axis.extent) / 2 - 2- $Axispos1, 0);
                else
                HC_axis.setPosition((firstWord(HC_axis.extent) / 2 - firstWord(HC_axis.extent)) - $Axispos1 - 2, 0);

         if(isEventPending(%this.inspectupdate))
            cancel(%this.inspectupdate);

            %this.inspectupdate = schedule(25, 0, updatethis);
}
};
activatepackage(HorizCompass);
callback.add(PlayerSpawn, updatethis);
