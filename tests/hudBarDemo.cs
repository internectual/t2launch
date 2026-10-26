// HUD bar demo for the t2launch hook's ui.hud_get_value, ui.vertical_fill
// and ui.bar_flip options. Copy it to base/scripts/autoexec/ to load it at
// startup; while you play, bars on the right of the screen show each fill
// direction, with its getValue() reading underneath. Not part of the
// launcher: the hook itself needs no scripts.
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

function hudBarDemoAdd()
{
   %width = getWord(PlayGui.extent, 0);
   %height = getWord(PlayGui.extent, 1);
   %group = new GuiControl(HudBarDemo) {
      profile = "GuiDefaultProfile";
      horizSizing = "left";
      vertSizing = "center";
      position = (%width - 454) SPC mFloor(%height / 2 - 250);
      extent = "434 500";
      visible = "1";
   };
   PlayGui.add(%group);

   // Seven vertical bars (flips are "flipVertical flipHorizontal"), then
   // three horizontal energy bars.
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
      %bar = hudBarDemoNewBar(%class[%i], "HudBarDemoBar" @ %i, (%x + 11) SPC 0, "40 300", 1,
         getWord(%flip[%i], 0), getWord(%flip[%i], 1));
      %bar.fillColor = %fill[%i];
      %group.add(%bar);
      hudBarDemoText(%group, "HudBarDemoName" @ %i, "HudBarDemoTextProfile", %x SPC 304, "62 18", %name[%i]);
      hudBarDemoText(%group, "HudBarDemoValue" @ %i, "HudBarDemoTextProfile", %x SPC 320, "62 18", "");
   }
   for (%i = 7; %i < 10; %i++)
   {
      %y = 346 + (%i - 7) * 42;
      hudBarDemoText(%group, "HudBarDemoName" @ %i, "HudBarDemoLeftTextProfile", "11" SPC %y, "360 18",
         %name[%i]);
      %bar = hudBarDemoNewBar("HudEnergy", "HudBarDemoBar" @ %i, "11" SPC (%y + 18), "340 16", 0,
         getWord(%flip[%i], 0), getWord(%flip[%i], 1));
      %bar.fillColor = %energy;
      %group.add(%bar);
      hudBarDemoText(%group, "HudBarDemoValue" @ %i, "HudBarDemoTextProfile", "360" SPC (%y + 16), "62 18", "");
   }
   hudBarDemoText(%group, HudBarDemoSource, "HudBarDemoTextProfile", "0 476", "434 18", "");
}

function hudBarDemoTick()
{
   cancel($HudBarDemoSchedule);
   $HudBarDemoSchedule = schedule(100, 0, hudBarDemoTick);
   if (!isObject(PlayGui) || !PlayGui.isAwake())
      return;
   if (!isObject(HudBarDemo))
      hudBarDemoAdd();

   // getValue() is what scripted HUDs read; .value is the field the hook
   // writes. Show which one answered so a blank getValue() is obvious.
   // Inverted bars still report the real value.
   %source = "getValue()";
   for (%i = 0; %i < 10; %i++)
   {
      %bar = "HudBarDemoBar" @ %i;
      %value = %bar.getValue();
      if (%value $= "")
      {
         %value = %bar.value;
         %source = ".value field (getValue() was blank)";
      }
      %label = "HudBarDemoValue" @ %i;
      %label.setValue(mFloor(%value * 100 + 0.5) @ "%");
   }
   HudBarDemoSource.setValue("read with " @ %source);
}

echo("HUD bar demo loaded: bars appear on the right while you play.");
hudBarDemoTick();
