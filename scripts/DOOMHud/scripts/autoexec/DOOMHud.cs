// #autoload
// #name = DOOMHuds
// #version = 1.0
// #date = 2026-11-07
// #author = Lou Cypher
// #warrior = LouCypher
// #email = asta_llama_lincoln@hotmail.com
// #web = http://deadzone.cjb.net
// #description = Standalone DOOM-style HUD for Tribes 2
// #category = GUI
// #status = beta
// #readme = scripts/autoexec/DOOMHud.txt
// #credit = Canadian-
// #credit = PenguinOfDoom
// #credit = Silverspirit
// #credit = UberGuy

if (!isObject(DOOMHuds))
   new ScriptObject(DOOMHuds) { class = DOOMHuds; };

function DOOMHuds::ValueToFields(%this, %base, %value, %amount)
{
   %amount = mFloor(%amount);
   if (%amount < 0) %amount = 0;
   if (%amount > 999) %amount = 999;

   %hundred = mFloor(%amount / 100);
   %remainder = %amount - (%hundred * 100);
   %ten = mFloor(%remainder / 10);
   %one = %remainder - (%ten * 10);

   eval(%value @ "H.setVisible(" @ (%amount > 99) @ ");");
   eval(%value @ "H.setBitmap(\"" @ %base @ %hundred @ "\");");
   eval(%value @ "T.setVisible(" @ (%amount > 9) @ ");");
   eval(%value @ "T.setBitmap(\"" @ %base @ %ten @ "\");");
   eval(%value @ "O.setVisible(1);");
   eval(%value @ "O.setBitmap(\"" @ %base @ %one @ "\");");
}

function DOOMHuds::Setup(%this)
{
   if (%this.Setup || !isObject(PlayGui)) return;
   exec("gui/DOOMGui.gui");
   if (!isObject(DOOMHud)) return;
   PlayGui.add(DOOMHud);
   %this.Layout();

   // The hook stores live HudBarBaseCtrl values. Tiny transparent controls
   // provide those values without l33thax, xHealth/xEnergy globals, or DSO mods.
   if (!isObject(DOOMHealthValue))
      DOOMHud.add(new HudDamage(DOOMHealthValue) {
         position = "0 0"; extent = "1 1"; minExtent = "1 1";
         subRegion = "0 0 1 1"; visible = "1"; opacity = "0";
         displayMounted = "0"; autoResize = "0"; autoCenter = "0";
      });
   if (!isObject(DOOMEnergyValue))
      DOOMHud.add(new HudEnergy(DOOMEnergyValue) {
         position = "1 0"; extent = "1 1"; minExtent = "1 1";
         subRegion = "0 0 1 1"; visible = "1"; opacity = "0";
         displayMounted = "0"; autoResize = "0"; autoCenter = "0";
      });

   %this.clearSlots();
   %this.Setup = true;
}

function DOOMHuds::Layout(%this)
{
   if (!isObject(PlayGui) || !isObject(DOOMHud)) return;
   // T2's vertSizing="top" pins the bottom edge (the name describes the
   // opposite edge). Seed the center/bottom offsets from the current logical
   // canvas once; GuiControl::parentResized maintains these anchors afterward.
   %width = getWord(PlayGui.extent, 0);
   %height = getWord(PlayGui.extent, 1);
   %left = mFloor((%width - 640) / 2);
   %top = %height - 64;
   if (%left < 0) %left = 0;
   if (%top < 0) %top = 0;
   DOOMHud.horizSizing = "center";
   DOOMHud.vertSizing = "top";
   DOOMHud.position = %left SPC %top;
   DOOMHud.extent = "640 64";
}

function DOOMHuds::clearSlots(%this)
{
   $DOOMnumweapons = 0;
   $DOOMactiveSlot = -1;
   for (%i = 1; %i < 7; %i++) {
      $DOOMslot[%i] = "";
      $DOOMammo[%i] = 0;
      eval("slot" @ %i @ ".setVisible(false);");
      eval("slot" @ %i @ ".setBitmap(\"DOOM/STGNUM" @ %i @ "\");");
   }
}

function DOOMHuds::addweapon(%this, %slot, %ammoAmount)
{
   if (%slot < 0 || %slot > 5 || $DOOMnumweapons >= 6) return;
   if (($WeaponNames[%slot] $= "TargetingLaser") ||
       ($WeaponNames[%slot] $= "TR2GoldTargetingLaser") ||
       ($WeaponNames[%slot] $= "TR2SilverTargetingLaser")) return;
   for (%i = 1; %i <= $DOOMnumweapons; %i++)
      if ($DOOMslot[%i] == %slot) return;
   $DOOMnumweapons++;
   if (%ammoAmount $= "") %ammoAmount = 0;
   $DOOMammo[$DOOMnumweapons] = %ammoAmount;
   $DOOMslot[$DOOMnumweapons] = %slot;
}

function DOOMHuds::removeweapon(%this, %slot)
{
   for (%i = 1; %i <= $DOOMnumweapons; %i++) {
      if ($DOOMslot[%i] != %slot) continue;
      for (%j = %i; %j < $DOOMnumweapons; %j++) {
         $DOOMslot[%j] = $DOOMslot[%j + 1];
         $DOOMammo[%j] = $DOOMammo[%j + 1];
      }
      $DOOMslot[$DOOMnumweapons] = "";
      $DOOMammo[$DOOMnumweapons] = 0;
      $DOOMnumweapons--;
      if ($DOOMactiveSlot == %slot) $DOOMactiveSlot = -1;
      return;
   }
}

function DOOMHuds::setweaponshudactive(%this, %slot)
{
   $DOOMactiveSlot = %slot;
   for (%i = 1; %i < 7; %i++)
      eval("slot" @ %i @ ".setBitmap(\"DOOM/STGNUM" @ %i @ "\");");
   if (%slot < 0) return;
   for (%i = 1; %i <= $DOOMnumweapons; %i++) {
      if ($DOOMslot[%i] == %slot) {
         eval("slot" @ %i @ ".setBitmap(\"DOOM/STYSNUM" @ %i @ "\");");
         return;
      }
   }
}

function DOOMHuds::Update(%this)
{
   cancel(%this.UpdateSchedule);
   %health = DOOMHealthValue.value;
   %energy = DOOMEnergyValue.value;
   if (%health $= "") %health = 0;
   if (%energy $= "") %energy = 0;
   %this.ValueToFields("DOOM/STTNUM", "health", mFloor(%health * 100 + 0.5));
   %this.ValueToFields("DOOM/STTNUM", "energy", mFloor(%energy * 100 + 0.5));

   // Optional compatibility hooks for scripts that track Doom-style keycards.
   key1.setVisible($DOOMHud::BlueKey);
   key2.setVisible($DOOMHud::YellowKey);
   key3.setVisible($DOOMHud::RedKey);
   if (%this.refresh) %this.UpdateSchedule = %this.schedule(100, Update);
}

package DOOMHuds
{
   function PlayGui::onWake(%this)
   {
      parent::onWake(%this);
      if (!DOOMHuds.Setup) DOOMHuds.Setup();
      DOOMHuds.refresh = true;
      DOOMHuds.Update();
   }

   function disconnect()
   {
      DOOMHuds.refresh = false;
      cancel(DOOMHuds.UpdateSchedule);
      parent::disconnect();
   }

   function DispatchLaunchMode()
   {
      parent::DispatchLaunchMode();
      if (!DOOMHuds.Setup) DOOMHuds.Setup();
   }

   function clientCmdSetAmmoHudCount(%amount)
   {
      parent::clientCmdSetAmmoHudCount(%amount);
      DOOMHuds.ValueToFields("DOOM/STTNUM", "ammo", %amount);
   }

   function SensorHud::update(%this)
   {
      if (isObject(DOOMSensor)) {
         DOOMSensor.setVisible(%this.ping || %this.jam);
         if (%this.jam) DOOMSensor.color = %this.jamColor;
         else DOOMSensor.color = %this.pingColor;
      }
      parent::update(%this);
   }

   function clientCmdSetWeaponsHudBitmap(%slot, %name, %bitmap)
   {
      parent::clientCmdSetWeaponsHudBitmap(%slot, %name, %bitmap);
   }

   function clientCmdSetWeaponsHudItem(%slot, %ammoAmount, %addItem)
   {
      parent::clientCmdSetWeaponsHudItem(%slot, %ammoAmount, %addItem);
      %present = false;
      for (%i = 1; %i <= $DOOMnumweapons; %i++)
         if ($DOOMslot[%i] == %slot) %present = true;
      if (%addItem && !%present) DOOMHuds.addweapon(%slot, %ammoAmount);
      else if (!%addItem && %present) DOOMHuds.removeweapon(%slot);
      for (%i = 1; %i <= $DOOMnumweapons; %i++)
         eval("slot" @ %i @ ".setVisible(true);");
      for (%i = $DOOMnumweapons + 1; %i < 7; %i++)
         eval("slot" @ %i @ ".setVisible(false);");
      DOOMHuds.setweaponshudactive($DOOMactiveSlot);
   }

   function clientCmdTogglePlayHuds(%val)
   {
      parent::clientCmdTogglePlayHuds(%val);
      if (isObject(DOOMHud)) DOOMHud.setVisible(%val);
   }

   function clientCmdSetWeaponsHudClearAll()
   {
      DOOMHuds.clearSlots();
      parent::clientCmdSetWeaponsHudClearAll();
   }

   function clientCmdSetWeaponsHudActive(%slot, %ret, %vis)
   {
      DOOMHuds.setweaponshudactive(%slot);
      parent::clientCmdSetWeaponsHudActive(%slot, %ret, %vis);
   }
};

if (!isActivePackage(DOOMHuds)) activatePackage(DOOMHuds);
if (isObject(PlayGui)) DOOMHuds.Setup();
