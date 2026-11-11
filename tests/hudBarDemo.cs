// #autoload
// #name = T2LaunchHudDemo
// #version = 1.0
// #author = t2launch contributors
// #description = HUD bar, HUDBot samples, compass, RadarHUD and sky rotation demo
//
// Install scripts/HudDemo/HudDemo.vl2 in base/ to load this script, the HUDBot
// horizontal/vertical samples, and the beta horizontal compass assets. The
// hook draws the T2 radar and rotates the skybox. DOOMHud remains optional in
// its separate scripts/DOOMHud/DOOMHud.vl2 package.
//
// Each bar picks its own direction with its fields; bars that don't set
// them fill left to right as always:
//   verticalFill = 1    fill bottom to top instead of left to right
//   flipVertical = 1    vertical bar: fill top to bottom
//                       horizontal bar: inverted (shows 1 - value)
//   flipHorizontal = 1  horizontal bar: fill right to left
//                       vertical bar: inverted (shows 1 - value)

if (!isObject(HudBarDemoTextProfile)) new GuiControlProfile(HudBarDemoTextProfile)
{
   fontType = "Arial Bold";
   fontSize = 13;
   fontColor = "0 255 0";
   justify = "center";
};
if (!isObject(HudBarDemoLeftTextProfile)) new GuiControlProfile(HudBarDemoLeftTextProfile)
{
   fontType = "Arial Bold";
   fontSize = 13;
   fontColor = "0 255 0";
   justify = "left";
};

// Native hook demo controls. The radar filters opposing players through the
// client's already-visible ShapeBase set; it does not enumerate hidden foes.
$T2Launch::HudDemo::Radar = 1;
$pref::skyRotationSpeed = 0.2; // radians per second; set to 0 to stop.
$T2Launch::HudDemo::CompassMode = "both"; // circular, horizontal, or both

// The fill is drawn in subRegion (x y width height), clipped to the extent.
// Something shrinks a new bar to its minExtent, so minExtent is the full
// size too. autoResize stays off so the bar isn't sized to a bitmap it
// doesn't have.
function hudBarDemoNewBar(%class, %name, %position, %size, %vertical, %flipV, %flipH)
{
   %region = "0 0" SPC %size;
   switch$ (%class)
   {
      case "HudDamage":
         %bar = new HudDamage(%name) { position = %position; extent = %size; minExtent = %size;
            subRegion = %region; autoResize = "0"; autoCenter = "0"; verticalFill = %vertical;
            flipVertical = %flipV; flipHorizontal = %flipH; };
      case "HudEnergy":
         %bar = new HudEnergy(%name) { position = %position; extent = %size; minExtent = %size;
            subRegion = %region; autoResize = "0"; autoCenter = "0"; verticalFill = %vertical;
            flipVertical = %flipV; flipHorizontal = %flipH; };
      case "HudHeat":
         %bar = new HudHeat(%name) { position = %position; extent = %size; minExtent = %size;
            subRegion = %region; autoResize = "0"; autoCenter = "0"; verticalFill = %vertical;
            flipVertical = %flipV; flipHorizontal = %flipH; };
      default:
         %bar = new HudCapacitor(%name) { position = %position; extent = %size; minExtent = %size;
            subRegion = %region; autoResize = "0"; autoCenter = "0"; verticalFill = %vertical;
            flipVertical = %flipV; flipHorizontal = %flipH; };
   }
   %bar.visible = "1";
   %bar.opacity = "0.8";
   %bar.displayMounted = "0";
   %bar.frameColor = "0 1 0 1";
   %bar.pulseRate = "500";
   %bar.pulseThreshold = "0.3";
   return %bar;
}

function hudBarDemoText(%group, %name, %profile, %position, %extent, %text)
{
   %group.add(new GuiTextCtrl(%name) {
      profile = %profile;
      position = %position;
      extent = %extent;
      visible = "1";
      text = %text;
   });
}

function hudBarDemoBitmap(%group, %name, %position, %extent, %bitmap)
{
   %ctrl = new HudBitmapCtrl(%name) {
      profile = "GuiDefaultProfile";
      position = %position;
      extent = %extent;
      minExtent = %extent;
      visible = "1";
      bitmap = %bitmap;
      autoCenter = "0";
      autoResize = "0";
   };
   %group.add(%ctrl);
   return %ctrl;
}

function hudBarDemoBitmapFill(%group, %name, %position, %extent, %bitmap)
{
   %ctrl = new HudBitmapCtrl(%name) {
      profile = "GuiDefaultProfile";
      position = %position;
      extent = %extent;
      minExtent = "1 1";
      visible = "1";
      bitmap = %bitmap;
      autoCenter = "0";
      autoResize = "0";
   };
   %group.add(%ctrl);
   return %ctrl;
}

function hudBarDemoUpdateSkinFill(%control, %value, %horizontal, %origin, %fullLength, %crossSize)
{
   if (%value < 0) %value = 0;
   if (%value > 1) %value = 1;
   %length = mFloor(%fullLength * %value);
   %control.visible = (%length > 0);
   if (%horizontal)
   {
      %control.position = %origin SPC (getWord(%control.position, 1));
      %control.extent = %length SPC %crossSize;
   }
   else
   {
      %control.position = getWord(%control.position, 0) SPC (%origin - %length);
      %control.extent = %crossSize SPC %length;
   }
}

function hudBarDemoSetCompassMode(%mode)
{
   if (%mode !$= "circular" && %mode !$= "horizontal" && %mode !$= "both")
   {
      echo("Compass mode must be circular, horizontal, or both.");
      return;
   }
   $T2Launch::HudDemo::CompassMode = %mode;
   echo("Compass mode: " @ %mode);
   hudBarDemoApplyCompassMode();
}

function hudBarDemoApplyCompassMode()
{
   %mode = $T2Launch::HudDemo::CompassMode;
   %circle = (%mode $= "circular" || %mode $= "both" ||
      (%mode $= "horizontal" && !$T2Launch::HudDemo::CompassHeadingValid));
   %strip = (%mode $= "horizontal" || %mode $= "both");
   // These are the circular compass and its separate background in T2's
   // stock PlayGui.gui. Restore both in circular/both modes.
   if (isObject(compassHud)) compassHud.setVisible(%circle);
   if (isObject(hudCompassBack)) hudCompassBack.setVisible(%circle);
   if (isObject(HudDemoHorizontalCompass))
      HudDemoHorizontalCompass.setVisible(%strip);
   if (isObject(HudBarDemoCompassModeText))
   {
   %status = "Compass mode: " @ %mode;
   if (%mode $= "horizontal" && !$T2Launch::HudDemo::CompassHeadingValid)
      %status = %status @ " (observer fallback)";
   HudBarDemoCompassModeText.setValue(%status);
   }
}

function hudBarDemoAddHorizontalCompass()
{
   if (isObject(HudDemoHorizontalCompass))
      HudDemoHorizontalCompass.delete();
   %width = getWord(PlayGui.extent, 0);
   %height = getWord(PlayGui.extent, 1);
   %group = new GuiControl(HudDemoHorizontalCompass) {
      profile = "GuiDefaultProfile";
      horizSizing = "width";
      vertSizing = "height";
      position = "0 0";
      extent = %width SPC %height;
      visible = "1";
   };
   hudBarDemoBitmap(%group, HudDemoCompassNorth, "0 0", "15 15",
      "gui/hud_north");
   hudBarDemoBitmap(%group, HudDemoCompassEast, "0 0", "15 15",
      "gui/hud_east");
   hudBarDemoBitmap(%group, HudDemoCompassSouth, "0 0", "15 15",
      "gui/hud_south");
   hudBarDemoBitmap(%group, HudDemoCompassWest, "0 0", "15 15",
      "gui/hud_west");
   hudBarDemoBitmap(%group, HudDemoCompassStrip, "0 0", "630 14",
      "hudBarDemo_assets/compass_strip.png");
   HudDemoCompassNorth.opacity = "0.9";
   HudDemoCompassEast.opacity = "0.9";
   HudDemoCompassSouth.opacity = "0.9";
   HudDemoCompassWest.opacity = "0.9";
   HudDemoCompassNorth.visible = "0";
   HudDemoCompassEast.visible = "0";
   HudDemoCompassSouth.visible = "0";
   HudDemoCompassWest.visible = "0";
   HudDemoCompassStrip.visible = "0";
   PlayGui.add(%group);
   hook_hud_demo_compass_markers(HudDemoCompassNorth.getId(), HudDemoCompassEast.getId(),
      HudDemoCompassSouth.getId(), HudDemoCompassWest.getId(), HudDemoCompassStrip.getId());
}

function hudBarDemoUpdateCompass()
{
   $T2Launch::HudDemo::CompassHeadingValid = false;
   if (!isObject(HudDemoCompassNorth) || !isObject(ServerConnection))
   {
      hudBarDemoApplyCompassMode();
      return;
   }
   HudDemoHorizontalCompass.extent = PlayGui.extent;
   %heading = hook_hud_demo_heading();
   if (%heading $= "")
   {
      hudBarDemoApplyCompassMode();
      return;
   }

    // The native hook projects the marker bearings through the active camera
    // view/projection matrices, so pitch follows the true horizon and yaw rotates
    // west/east in the same direction as the world.
   $T2Launch::HudDemo::CompassHeadingValid = true;
   hudBarDemoApplyCompassMode();
}

// Use the supplied HUDbot bar bitmap as the live fill as well as the frame.
// Scaling the alpha-shaped PNG keeps its fill inside the sample's silhouette;
// the tiny native controls only provide the hook-maintained live values.
function hudBarDemoAddSkin()
{
   if (isObject(HudBarDemoSkin))
      HudBarDemoSkin.delete();
   %group = new GuiControl(HudBarDemoSkin) {
      profile = "GuiDefaultProfile";
      horizSizing = "right";
      vertSizing = "bottom";
      position = "0 0";
      extent = "128 70";
      visible = "1";
   };
   PlayGui.add(%group);

   // Horizontal Tani/HUDbot sample.
    hudBarDemoBitmap(%group, HudBarDemoSkinHealthTrack, "0 0", "128 16",
       "hudBarDemo_assets/hudbot_horizontal/health_bar.png");
    HudBarDemoSkinHealthTrack.opacity = "0.22";
    %health = new HudDamage(HudBarDemoSkinHealth) {
       profile = "GuiDefaultProfile";
       position = "0 0"; extent = "128 16"; minExtent = "128 16";
       subRegion = "1 1 126 14";
       autoResize = "0";
       autoCenter = "0";
       visible = "1"; opacity = "0";
       displayMounted = "0";
    };
    %group.add(%health);
    hudBarDemoBitmapFill(%group, HudBarDemoSkinHealthFill, "1 1", "126 14",
       "hudBarDemo_assets/hudbot_horizontal/health_bar.png");
   hudBarDemoBitmap(%group, HudBarDemoSkinHealthFrame, "0 0", "128 16",
      "hudBarDemo_assets/hudbot_horizontal/health_frame.png");

    hudBarDemoBitmap(%group, HudBarDemoSkinEnergyTrack, "0 40", "128 8",
       "hudBarDemo_assets/hudbot_horizontal/energy_bar.png");
    HudBarDemoSkinEnergyTrack.opacity = "0.22";
    %energy = new HudEnergy(HudBarDemoSkinEnergy) {
       profile = "GuiDefaultProfile";
       position = "0 40"; extent = "128 8"; minExtent = "128 8";
       subRegion = "1 1 126 6";
       autoResize = "0";
       autoCenter = "0";
       visible = "1"; opacity = "0";
       displayMounted = "0";
    };
    %group.add(%energy);
    hudBarDemoBitmapFill(%group, HudBarDemoSkinEnergyFill, "1 41", "126 6",
       "hudBarDemo_assets/hudbot_horizontal/energy_bar.png");
   hudBarDemoBitmap(%group, HudBarDemoSkinEnergyFrame, "0 40", "128 8",
      "hudBarDemo_assets/hudbot_horizontal/energy_frame.png");

   hudBarDemoText(%group, HudBarDemoSkinHealthLabel, "HudBarDemoLeftTextProfile",
      "0 18", "82 18", "Health");
   hudBarDemoText(%group, HudBarDemoSkinEnergyLabel, "HudBarDemoLeftTextProfile",
      "0 52", "82 18", "Energy");
   hudBarDemoText(%group, HudBarDemoSkinHealthValue, "HudBarDemoTextProfile",
      "82 18", "46 18", "");
   hudBarDemoText(%group, HudBarDemoSkinEnergyValue, "HudBarDemoTextProfile",
      "82 52", "46 18", "");
}

function hudBarDemoAddVerticalSkin()
{
   if (isObject(HudBarDemoSkinVertical))
      HudBarDemoSkinVertical.delete();
   %group = new GuiControl(HudBarDemoSkinVertical) {
      profile = "GuiDefaultProfile";
      horizSizing = "right";
      vertSizing = "bottom";
      position = "0 0";
      extent = "46 160";
      visible = "1";
   };
   PlayGui.add(%group);

    hudBarDemoBitmap(%group, HudBarDemoVerticalHealthTrack, "0 0", "16 128",
       "hudBarDemo_assets/hudbot_vertical/health_bar.png");
    HudBarDemoVerticalHealthTrack.opacity = "0.22";
    %health = new HudDamage(HudBarDemoVerticalHealth) {
       profile = "GuiDefaultProfile"; position = "0 0"; extent = "16 128";
       minExtent = "16 128"; subRegion = "1 1 14 126"; verticalFill = "1";
       autoResize = "0"; autoCenter = "0"; visible = "1"; opacity = "0";
       displayMounted = "0";
    };
    %group.add(%health);
    hudBarDemoBitmapFill(%group, HudBarDemoVerticalHealthFill, "1 1", "14 126",
       "hudBarDemo_assets/hudbot_vertical/health_bar.png");
   hudBarDemoBitmap(%group, HudBarDemoVerticalHealthFrame, "0 0", "16 128",
      "hudBarDemo_assets/hudbot_vertical/health_frame.png");

    hudBarDemoBitmap(%group, HudBarDemoVerticalEnergyTrack, "24 0", "8 128",
       "hudBarDemo_assets/hudbot_vertical/energy_bar.png");
    HudBarDemoVerticalEnergyTrack.opacity = "0.22";
    %energy = new HudEnergy(HudBarDemoVerticalEnergy) {
       profile = "GuiDefaultProfile"; position = "24 0"; extent = "8 128";
       minExtent = "8 128"; subRegion = "0 1 8 126"; verticalFill = "1";
       autoResize = "0"; autoCenter = "0"; visible = "1"; opacity = "0";
       displayMounted = "0";
    };
    %group.add(%energy);
    hudBarDemoBitmapFill(%group, HudBarDemoVerticalEnergyFill, "24 1", "8 126",
       "hudBarDemo_assets/hudbot_vertical/energy_bar.png");
   hudBarDemoBitmap(%group, HudBarDemoVerticalEnergyFrame, "24 0", "8 128",
      "hudBarDemo_assets/hudbot_vertical/energy_frame.png");

   hudBarDemoText(%group, HudBarDemoVerticalHealthValue, "HudBarDemoTextProfile",
      "0 130", "18 18", "");
   hudBarDemoText(%group, HudBarDemoVerticalEnergyValue, "HudBarDemoTextProfile",
      "21 130", "18 18", "");
}

function hudBarDemoAddStatus()
{
   if (isObject(HudBarDemoStatus))
      HudBarDemoStatus.delete();
   %group = new GuiControl(HudBarDemoStatus) {
      profile = "GuiDefaultProfile";
      horizSizing = "left";
      vertSizing = "top";
      position = "10 10";
      extent = "190 72";
      visible = "1";
   };
   PlayGui.add(%group);
   hudBarDemoText(%group, HudBarDemoRadarStatus, "HudBarDemoLeftTextProfile", "0 0", "190 18",
      "Radar: visible contacts");
   hudBarDemoText(%group, HudBarDemoSkyStatus, "HudBarDemoLeftTextProfile", "0 18", "190 18",
      "Sky: " @ $pref::skyRotationSpeed @ " rad/s");
   hudBarDemoText(%group, HudBarDemoCompassModeText, "HudBarDemoLeftTextProfile", "0 36", "190 18", "");
   hudBarDemoText(%group, HudBarDemoSource, "HudBarDemoTextProfile", "0 54", "190 18", "");
}

function hudBarDemoAdd()
{
    %group = new GuiControl(HudBarDemo) {
      profile = "GuiDefaultProfile";
      horizSizing = "left";
      vertSizing = "top";
      position = "0 0";
      extent = "440 350";
      visible = "1";
   };
   PlayGui.add(%group);

    // Seven compact vertical examples with labels, then three horizontal
    // examples below. The group leaves room at its right for the two skin sets.
   %energy = "0.353 0.373 0.933 0.8";
   %class[0] = "HudDamage";    %name[0] = "Health";    %fill[0] = "0 1 0 0.8";   %flip[0] = "0 0";
   %class[1] = "HudEnergy";    %name[1] = "Energy";    %fill[1] = %energy;       %flip[1] = "0 0";
   %class[2] = "HudHeat";      %name[2] = "Heat";      %fill[2] = "1 0.3 0 0.8"; %flip[2] = "0 0";
   %class[3] = "HudCapacitor"; %name[3] = "Capacitor"; %fill[3] = "1 1 0 0.8";   %flip[3] = "0 0";
   %class[4] = "HudEnergy";    %name[4] = "Top down";  %fill[4] = %energy;       %flip[4] = "1 0";
   %class[5] = "HudEnergy";    %name[5] = "Inverted";  %fill[5] = %energy;       %flip[5] = "0 1";
   %class[6] = "HudEnergy";    %name[6] = "Inv. top";  %fill[6] = %energy;       %flip[6] = "1 1";
   %name[7] = "Energy, left to right";                  %flip[7] = "0 0";
   %name[8] = "Energy, right to left (flipHorizontal)"; %flip[8] = "0 1";
   %name[9] = "Energy, inverted (flipVertical)";        %flip[9] = "1 0";
   for (%i = 0; %i < 7; %i++)
   {
       %x = %i * 62;
       %bar = hudBarDemoNewBar(%class[%i], "HudBarDemoBar" @ %i, (%x + 17) SPC 0, "28 140", 1,
          getWord(%flip[%i], 0), getWord(%flip[%i], 1));
       %bar.fillColor = %fill[%i];
       %group.add(%bar);
       hudBarDemoText(%group, "HudBarDemoName" @ %i, "HudBarDemoTextProfile", %x SPC 142, "62 18", %name[%i]);
       hudBarDemoText(%group, "HudBarDemoValue" @ %i, "HudBarDemoTextProfile", %x SPC 158, "62 18", "");
   }
   for (%i = 7; %i < 10; %i++)
   {
       %y = 190 + (%i - 7) * 46;
       hudBarDemoText(%group, "HudBarDemoName" @ %i, "HudBarDemoLeftTextProfile", "0" SPC %y, "190 18",
          %name[%i]);
       %bar = hudBarDemoNewBar("HudEnergy", "HudBarDemoBar" @ %i, "0" SPC (%y + 18), "150 16", 0,
          getWord(%flip[%i], 0), getWord(%flip[%i], 1));
       %bar.fillColor = %energy;
       %group.add(%bar);
       hudBarDemoText(%group, "HudBarDemoValue" @ %i, "155" SPC (%y + 16), "35 18", "");
   }
}

function hudBarDemoAnchor()
{
   if (!isObject(PlayGui) || !isObject(HudBarDemo)) return;
   %width = getWord(PlayGui.extent, 0);
   %height = getWord(PlayGui.extent, 1);
   // Reserve the stock inventory and weapon-HUD column on the right. The
   // remaining demo cluster is pinned to the bottom-right of that safe area.
   %x = %width - 440 - 176;
   %y = %height - 350 - 10;
   if (%x < 8) %x = 8;
   if (%y < 8) %y = 8;
   HudBarDemo.position = %x SPC %y;
   if (isObject(HudBarDemoSkin))
      HudBarDemoSkin.position = (%x + 205) SPC (%y + 190);
   if (isObject(HudBarDemoSkinVertical))
      HudBarDemoSkinVertical.position = (%x + 378) SPC (%y + 185);
}

function hudBarDemoTick()
{
   cancel($HudBarDemoSchedule);
   $HudBarDemoSchedule = schedule(100, 0, hudBarDemoTick);
   if (!isObject(PlayGui) || !PlayGui.isAwake())
      return;
   if (!isObject(HudBarDemo))
      hudBarDemoAdd();
   if (!isObject(HudBarDemoSkin))
      hudBarDemoAddSkin();
   if (!isObject(HudBarDemoStatus))
      hudBarDemoAddStatus();
   if (!isObject(HudBarDemoSkinVertical))
      hudBarDemoAddVerticalSkin();
    if (!isObject(HudDemoHorizontalCompass))
       hudBarDemoAddHorizontalCompass();
    hudBarDemoAnchor();
   hudBarDemoApplyCompassMode();
   hudBarDemoUpdateCompass();

   // Read the hook-maintained field directly. This avoids calling a scripted
   // class method before the deferred hook command registration is complete.
   // Inverted bars still report the real, uninverted value.
   %source = ".value field";
   for (%i = 0; %i < 10; %i++)
   {
      %bar = "HudBarDemoBar" @ %i;
      %value = %bar.value;
      if (%value $= "") %value = 0;
      %label = "HudBarDemoValue" @ %i;
      %label.setValue(mFloor(%value * 100 + 0.5) @ "%");
   }
   HudBarDemoSource.setValue("read with " @ %source);

   %health = HudBarDemoSkinHealth.value;
   %energy = HudBarDemoSkinEnergy.value;
   if (%health $= "") %health = 0;
   if (%energy $= "") %energy = 0;
    HudBarDemoSkinHealthValue.setValue(mFloor(%health * 100 + 0.5) @ "%");
    HudBarDemoSkinEnergyValue.setValue(mFloor(%energy * 100 + 0.5) @ "%");
    hudBarDemoUpdateSkinFill(HudBarDemoSkinHealthFill, %health, 1, 1, 126, 14);
    hudBarDemoUpdateSkinFill(HudBarDemoSkinEnergyFill, %energy, 1, 1, 126, 6);
   %verticalHealth = HudBarDemoVerticalHealth.value;
   %verticalEnergy = HudBarDemoVerticalEnergy.value;
   if (%verticalHealth $= "") %verticalHealth = 0;
   if (%verticalEnergy $= "") %verticalEnergy = 0;
    HudBarDemoVerticalHealthValue.setValue(mFloor(%verticalHealth * 100 + 0.5) @ "%");
    HudBarDemoVerticalEnergyValue.setValue(mFloor(%verticalEnergy * 100 + 0.5) @ "%");
    hudBarDemoUpdateSkinFill(HudBarDemoVerticalHealthFill, %verticalHealth, 0, 127, 126, 14);
    hudBarDemoUpdateSkinFill(HudBarDemoVerticalEnergyFill, %verticalEnergy, 0, 127, 126, 8);
}

echo("HUD bar demo loaded: vertical fill examples are left; horizontal samples are right.");
echo("HudBarDemo includes the HUDBot horizontal/vertical samples and beta compass strip.");
echo("RadarHUD uses teammates, LOS-visible opposing players, and the assigned waypoint.");
if (isObject(DOOMHuds))
   DOOMHuds.Setup();
else
   echo("Optional DOOMHud package not mounted; mount scripts/DOOMHud/DOOMHud.vl2 to show it.");
hudBarDemoTick();
