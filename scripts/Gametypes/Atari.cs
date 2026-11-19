// Atari: server-side Atari 2600 for the MatrixAtari mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixAtari.
//
// The emulator itself (6507, TIA, RIOT) is native code in the hook, running on a server thread; this script is
// the glue. The picture is a wall of force-field blocks: the hook downsamples each frame to a coarse grid of
// colours and reports which cells changed (t2launch_atariFrame); this script parks and unparks pooled blocks.
// Everyone sees the same wall. The first two players to press fire take joystick ports 1 and 2; anyone else
// watches and can fly around freely.
//
// Controls for a player (stock keybinds): forward/back/strafe = joystick up/down/left/right, fire = the
// joystick button (held while held), jump = Select, jet = Reset, alt-fire = your difficulty switch, throw = the
// ROM chooser. In the chooser, strafe left/right picks a ROM and fire starts it.
// Needs server.atari_rom_dir (default base/roms) to hold 2K to 32K .bin files that you own.

$Atr::Mission = "MatrixAtari";
$Atr::ScreenH = 40;                  // height of the picture in metres; the width is 4/3 of it
$Atr::CenterZ = 320;                 // world height of the picture's centre
$Atr::Depth = 1;                     // thickness of a block
$Atr::CamDist = 52;
$Atr::CamZShift = -1;
$Atr::DisplayMs = 50;                // display refresh interval
$Atr::MaxChanges = 150;              // cells redrawn per refresh
// The camera is the input device and the stock fly speed is 40 m/s, so a key press used to slide the view. The
// engine reads $Camera::movementSpeed, so while this ruleset is on the camera crawls instead: a key moves it by
// centimetres, which is still readable as input but leaves the view effectively fixed.
$Atr::CamSpeed = 1;                  // metres/second while this ruleset is on (stock: 40)
$Atr::Eps = 0.02;                    // the camera is off its anchor by more than this
$Atr::InputMin = 0.01;               // camera movement per tick that counts as a key sample
$Atr::KeyGraceMs = 110;              // a key counts as held this long after its last sample
$Atr::RecentreMs = 250;
$Atr::IgnoreMs = 100;
$Atr::Drift = 0.4;                   // re-anchor when it has crept this far
$Atr::TickMs = 32;
$Atr::Ports = 2;
$Atr::ParkX = 3000;                  // parked blocks live far outside every client's scope
$Atr::ParkY = 3000;
$Atr::RomIdx = 0;

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

// the hook registers the Atari functions on the dedicated server only; elsewhere this prints one error and fails
function atrAvailable()
{
   return t2launch_atariGrid() !$= "";
}

function atrDatablocks()
{
   if (isObject(AtrCol1) || !atrAvailable())
      return;
   for (%i = 1; %i <= 32; %i++)
   {
      %rgb = t2launch_atariColor(%i);
      %c = getWord(%rgb, 0) / 255 SPC getWord(%rgb, 1) / 255 SPC getWord(%rgb, 2) / 255;
      eval("datablock ForceFieldBareData(AtrCol" @ %i @ ") : defaultForceFieldBare {"
         @ " color = \"" @ %c @ "\"; baseTranslucency = 0.95; needsNoPower = true; };");
   }
   eval("datablock ForceFieldBareData(AtrBezel) : defaultForceFieldBare {"
      @ " color = \"0.85 0.85 0.9\"; baseTranslucency = 0.3; needsNoPower = true; };");
}

// ---------------------------------------------------------------------------------------------------------
// the wall

function atrPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

function atrInitWall()
{
   %g = t2launch_atariGrid();
   $Atr::Cols = getWord(%g, 0);
   $Atr::Rows = getWord(%g, 1);
   $Atr::CellW = ($Atr::ScreenH * 4 / 3) / $Atr::Cols;
   $Atr::CellH = $Atr::ScreenH / $Atr::Rows;
   $Atr::Z0 = $Atr::CenterZ - $Atr::ScreenH / 2;
   if (isObject($Atr::Group))
      $Atr::Group.delete();
   $Atr::Group = new SimGroup();
   MissionCleanup.add($Atr::Group);
   for (%c = 1; %c <= 32; %c++)
      $Atr::PoolN[%c] = 0;
   %n = $Atr::Cols * $Atr::Rows;
   for (%i = 0; %i < %n; %i++)
   {
      $Atr::Col[%i] = 0;
      $Atr::Obj[%i] = "";
   }
   $Atr::Created = 0;
   $Atr::PurgedAt = 0;
   // a thin bezel around the picture
   %w = $Atr::ScreenH * 4 / 3;
   %h = $Atr::ScreenH;
   %t = 0.6;
   %x0 = -%w / 2;
   %z0 = $Atr::Z0;
   $Atr::Group.add(atrField(AtrBezel, %x0 - %t, %z0 - %t, %w + %t * 2, %t, 2));
   $Atr::Group.add(atrField(AtrBezel, %x0 - %t, %z0 + %h, %w + %t * 2, %t, 2));
   $Atr::Group.add(atrField(AtrBezel, %x0 - %t, %z0, %t, %h, 2));
   $Atr::Group.add(atrField(AtrBezel, %x0 + %w, %z0, %t, %h, 2));
   echo("Atari wall ready: " @ $Atr::Cols @ " x " @ $Atr::Rows);
}

function atrField(%db, %x, %z, %sx, %sz, %depth)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-%depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %depth SPC %sz;
   };
}

function atrBlock(%color)
{
   if ($Atr::PoolN[%color] > 0)
   {
      $Atr::PoolN[%color]--;
      return $Atr::Pool[%color, $Atr::PoolN[%color]];
   }
   %f = new ForceFieldBare() {
      dataBlock = "AtrCol" @ %color;
      position = $Atr::ParkX SPC $Atr::ParkY SPC 300;
      rotation = "1 0 0 0";
      scale = $Atr::CellW SPC $Atr::Depth SPC $Atr::CellH;
   };
   $Atr::Group.add(%f);
   $Atr::Created++;
   return %f;
}

function atrSetCell(%i, %color)
{
   %old = $Atr::Col[%i] + 0;
   if (%old == %color)
      return;
   if (%old > 0)
   {
      %o = $Atr::Obj[%i];
      %o.setTransform($Atr::ParkX SPC $Atr::ParkY SPC 300);
      $Atr::Pool[%old, $Atr::PoolN[%old]] = %o;
      $Atr::PoolN[%old]++;
   }
   $Atr::Obj[%i] = "";
   $Atr::Col[%i] = %color;
   if (%color > 0)
   {
      %b = atrBlock(%color);
      %cx = %i % $Atr::Cols;
      %cy = mFloor(%i / $Atr::Cols);
      %b.setTransform((%cx - $Atr::Cols / 2) * $Atr::CellW SPC (-$Atr::Depth / 2) SPC ($Atr::Z0 + ($Atr::Rows - 1 - %cy) * $Atr::CellH));
      $Atr::Obj[%i] = %b;
   }
}

// apply what the emulator changed since the last refresh: six characters per change, "iiiicc"
function atrRefresh()
{
   %s = t2launch_atariFrame($Atr::MaxChanges);
   %n = strlen(%s) / 6;
   for (%k = 0; %k < %n; %k++)
      atrSetCell(getSubStr(%s, %k * 6, 4) + 0, getSubStr(%s, %k * 6 + 4, 2) + 0);
   if ($Atr::Created != $Atr::PurgedAt)
   {
      $Atr::PurgedAt = $Atr::Created;
      atrPurgeZones();
   }
}

// ---------------------------------------------------------------------------------------------------------
// seats

function atrAnchor()
{
   return "0" SPC (-$Atr::CamDist) SPC ($Atr::CenterZ + $Atr::CamZShift) SPC "1 0 0 0";
}

function atrFreePort()
{
   for (%p = 0; %p < $Atr::Ports; %p++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).atrPort $= %p)
            %used = true;
      if (!%used)
         return %p;
   }
   return "";
}

function atrPlayerCount()
{
   %n = 0;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
      if (ClientGroup.getObject(%i).atrPort !$= "")
         %n++;
   return %n;
}

function atrSendInput(%cl, %mask)
{
   if (%mask != %cl.atrSent)
   {
      %cl.atrSent = %mask;
      t2launch_atariInput(%cl.atrPort, %mask);
   }
}

function atrSendSwitches()
{
   %mask = $Atr::Toggles;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      if (%c.atrPort $= "")
         continue;
      if (%c.atrSelect) %mask = %mask | 2;
      if (%c.atrReset) %mask = %mask | 1;
   }
   if (%mask != $Atr::SentSwitches)
   {
      $Atr::SentSwitches = %mask;
      t2launch_atariSwitch(%mask);
   }
}

// ---------------------------------------------------------------------------------------------------------
// ROM chooser

function atrChooserOpen()
{
   $Atr::Roms = t2launch_atariList();
   $Atr::RomCount = getFieldCount($Atr::Roms);
   if ($Atr::RomIdx >= $Atr::RomCount)
      $Atr::RomIdx = 0;
   $Atr::Chooser = true;
   atrChooserShow();
}

function atrChooserShow()
{
   if ($Atr::RomCount < 1)
   {
      %text = "No ROMs found. Put 2K-32K .bin files in the server's ROM folder.";
   }
   else
      %text = "ROM " @ ($Atr::RomIdx + 1) @ " of " @ $Atr::RomCount @ ":  " @ getField($Atr::Roms, $Atr::RomIdx)
         @ "     strafe = choose, fire = start";
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      if (%c.atrPort !$= "")
         bottomPrint(%c, %text, 0, 1);
   }
}

function atrChooserMove(%d)
{
   if ($Atr::RomCount < 1)
      return;
   $Atr::RomIdx = ($Atr::RomIdx + %d + $Atr::RomCount) % $Atr::RomCount;
   atrChooserShow();
}

function atrChooserStart()
{
   if ($Atr::RomCount < 1)
   {
      atrChooserOpen();
      return;
   }
   %name = getField($Atr::Roms, $Atr::RomIdx);
   %r = t2launch_atariLoad(%name);
   if (%r $= "ok")
   {
      $Atr::Chooser = false;
      $Atr::Toggles = 0;
      messageAll('MsgAtari', '\c1Now playing %1.', %name);
      atrHudAll("PLAYING " @ %name @ "     fire = button, jump = Select, jet = Reset, alt-fire = difficulty, throw = ROM list");
   }
   else
      atrHudAll(%r);
}

function atrHudAll(%text)
{
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      if (%c.atrPort !$= "")
         bottomPrint(%c, %text, 6, 1);
   }
}

// ---------------------------------------------------------------------------------------------------------
// input: the d-pad is the camera's movement each tick (as in Tetris.cs and Snake.cs)

// returns the joystick direction bits currently held, and the camera is looked after
function atrReadPad(%cl, %now)
{
   %cam = %cl.camera;
   if (%cl.getControlObject() != %cam)
   {
      %cl.setControlObject(%cam);
      %cam.setTransform(atrAnchor());
      %cl.atrLastCam = "";
      return 0;
   }
   %t = %cam.getTransform();
   %px = getWord(%t, 0);
   %py = getWord(%t, 1);
   %pz = getWord(%t, 2);
   if (%cl.atrLastCam $= "")
      %cl.atrLastCam = %px SPC %py SPC %pz;
   %dx = %px - getWord(%cl.atrLastCam, 0);
   %fwd = %py - getWord(%cl.atrLastCam, 1);      // level camera: forward moves it along +Y
   %cl.atrLastCam = %px SPC %py SPC %pz;
   if (%now < %cl.atrIgnoreUntil)
   {
      %dx = 0;
      %fwd = 0;
   }
   %any = false;
   if (%dx < -$Atr::InputMin) { %cl.atrKeyL = %now; %any = true; }
   if (%dx > $Atr::InputMin) { %cl.atrKeyR = %now; %any = true; }
   if (%fwd > $Atr::InputMin) { %cl.atrKeyU = %now; %any = true; }
   if (%fwd < -$Atr::InputMin) { %cl.atrKeyD = %now; %any = true; }
   if (%any)
      %cl.atrSampleAt = %now;
   %a = atrAnchor();
   if (%now - %cl.atrSampleAt > $Atr::RecentreMs
      && (mAbs(%px - getWord(%a, 0)) > $Atr::Eps || mAbs(%py - getWord(%a, 1)) > $Atr::Eps || mAbs(%pz - getWord(%a, 2)) > $Atr::Eps))
   {
      %cam.setTransform(%a);
      %cam.setVelocity("0 0 0");
      %cl.atrLastCam = getWords(%a, 0, 2);
      %cl.atrIgnoreUntil = %now + $Atr::IgnoreMs;
   }
   else if (mAbs(%px - getWord(%a, 0)) > $Atr::Drift || mAbs(%py - getWord(%a, 1)) > $Atr::Drift || mAbs(%pz - getWord(%a, 2)) > $Atr::Drift)
   {
      %cam.setTransform(%a);
      %cl.atrLastCam = getWords(%a, 0, 2);
   }
   else if (mAbs(getWord(%t, 6)) > 0.004)
      %cam.setTransform(%px SPC %py SPC %pz SPC "1 0 0 0");
   %mask = 0;
   if (%now - %cl.atrKeyU < $Atr::KeyGraceMs) %mask = %mask | 1;
   if (%now - %cl.atrKeyD < $Atr::KeyGraceMs) %mask = %mask | 2;
   if (%now - %cl.atrKeyL < $Atr::KeyGraceMs) %mask = %mask | 4;
   if (%now - %cl.atrKeyR < $Atr::KeyGraceMs) %mask = %mask | 8;
   return %mask;
}

function atrTick()
{
   cancel($Atr::TickThread);
   $Atr::TickThread = schedule($Atr::TickMs, 0, atrTick);
   if (!isObject(MissionCleanup))
      return;
   $Atr::TickN++;
   %now = getSimTime();
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.atrJoined || !isObject(%cl.camera))
         continue;
      if (isObject(%cl.player))
      {
         %cl.player.setVelocity("0 0 0");
         if ($Atr::TickN % 30 == 0)
            %cl.player.setTransform("0 -150 300 1 0 0 0");
      }
      if (%cl.atrPort $= "")
      {
         // spectators keep the stock free-fly camera; make sure they have it
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %pad = atrReadPad(%cl, %now);
      if ($Atr::Chooser)
      {
         // strafe picks, fire starts; the joystick is idle meanwhile
         atrSendInput(%cl, 0);
         if (%cl.atrPort == 0)
         {
            // plain assignments: a nested ternary that yields -1 misbehaves in arithmetic in this engine
            %dir = 0;
            if (%pad & 8)
               %dir = 1;
            else if (%pad & 4)
               %dir = -1;
            if (%dir != 0 && %cl.atrChooseDir != %dir)
               atrChooserMove(%dir);
            %cl.atrChooseDir = %dir;
            if (%cl.atrFire && !%cl.atrFireSeen)
               atrChooserStart();
            %cl.atrFireSeen = %cl.atrFire;
         }
         continue;
      }
      %cl.atrFireSeen = %cl.atrFire;
      atrSendInput(%cl, %pad | (%cl.atrFire ? 16 : 0));
   }
   atrSendSwitches();
}

function atrDisplayTick()
{
   cancel($Atr::DisplayThread);
   $Atr::DisplayThread = schedule($Atr::DisplayMs, 0, atrDisplayTick);
   if (!isObject(MissionCleanup) || !isObject($Atr::Group))
      return;
   atrRefresh();
}

function atrInitPlayer(%cl)
{
   %cl.atrJoined = true;
   if (%cl.atrPort $= "")
      %cl.atrPort = atrFreePort();
   %cam = %cl.camera;
   %cam.setFlyMode();
   %cam.setTransform(atrAnchor());
   %cl.setControlObject(%cam);
   commandToClient(%cl, 'setHudMode', 'Observer');
   %cl.schedule(100, "setControlObject", %cam);
   if (isObject(%cl.player))
   {
      %cl.player.setTransform("0 -150 300 1 0 0 0");
      %cl.player.setVelocity("0 0 0");
   }
   %cl.atrLastCam = "";
   %cl.atrSent = "";
   if (%cl.atrPort $= "")
   {
      bottomPrint(%cl, "Watching. Both joysticks are taken; you can fly around freely.", 8, 1);
      return;
   }
   if ($Atr::Chooser || atrStatusName() $= "")
      atrChooserOpen();
   else
      bottomPrint(%cl, "You are player " @ (%cl.atrPort + 1) @ ".  fire = button, jump = Select, jet = Reset, throw = ROM list", 8, 1);
}

function atrStatusName()
{
   %s = t2launch_atariStatus();
   return getSubStr(%s, 0, strstr(%s, "|") < 0 ? 0 : strstr(%s, "|"));
}

function atrFreeClient(%cl)
{
   if (%cl.atrPort !$= "")
      t2launch_atariInput(%cl.atrPort, 0);
   %cl.atrPort = "";
   %cl.atrJoined = false;
   %cl.atrFire = false;
   %cl.atrSelect = false;
   %cl.atrReset = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function atrWatch()
{
   cancel($Atr::WatchThread);
   $Atr::WatchThread = schedule(250, 0, atrWatch);
   %on = ($CurrentMission $= $Atr::Mission && isObject(MissionCleanup));
   if (%on && !$Atr::PkgOn)
   {
      if (!atrAvailable())
      {
         echo("Atari: the hook's Atari functions are not available (needs the dedicated server hook)");
         return;
      }
      activatePackage(T2LaunchAtari);
      $Atr::PkgOn = true;
      $Atr::SavedSpeed = $Camera::movementSpeed;
      $Camera::movementSpeed = $Atr::CamSpeed;
      $Atr::Chooser = false;
      $Atr::Toggles = 0;
      $Atr::SentSwitches = "";
      atrInitWall();
      atrTick();
      atrDisplayTick();
      echo("Atari ruleset on");
   }
   else if (!%on && $Atr::PkgOn)
   {
      deactivatePackage(T2LaunchAtari);
      $Atr::PkgOn = false;
      $Camera::movementSpeed = $Atr::SavedSpeed;
      cancel($Atr::TickThread);
      cancel($Atr::DisplayThread);
      t2launch_atariStop();
      echo("Atari ruleset off");
   }
}

package T2LaunchAtariBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      atrDatablocks();
   }
};

package T2LaunchAtari
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      atrInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   // press AND release events (the stock handler drops releases before the game sees them), so fire is a
   // real held button
   function Observer::onTrigger(%data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.atrJoined)
      {
         if (%client.atrPort $= "")
         {
            // a spectator takes a free joystick with fire
            if (%state && %trigger == 0)
            {
               %port = atrFreePort();
               if (%port !$= "")
               {
                  %client.atrPort = %port;
                  atrInitPlayer(%client);
               }
            }
            return;
         }
         if (%trigger == 0)
            %client.atrFire = %state;
         else if (%trigger == 2)
            %client.atrSelect = %state;
         else if (%trigger == 3)
            %client.atrReset = %state;
         else if (%trigger == 1 && %state)
         {
            // difficulty switch: flip this player's bit (no XOR operator is relied on)
            %bit = %client.atrPort == 0 ? 8 : 16;
            if ($Atr::Toggles & %bit)
               $Atr::Toggles = $Atr::Toggles & ~%bit;
            else
               $Atr::Toggles = $Atr::Toggles | %bit;
         }
         else if (%trigger == 4 && %state)
         {
            t2launch_atariStop();
            atrChooserOpen();
         }
         return;
      }
      Parent::onTrigger(%data, %obj, %trigger, %state);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      atrFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Atr::Installed)
{
   $Atr::Installed = true;
   activatePackage(T2LaunchAtariBoot);
   atrDatablocks();
   atrWatch();
   echo("Atari gametype script loaded");
}
