*******************************************************************************
*                        TRIBES 2 CLASSIC MODIFICATION                        *
*                                Version: 1.1                                 *
*******************************************************************************



-------------------------------------------------------------------------------
                               Table of Contents:
-------------------------------------------------------------------------------

0. Version History

1. About Classic

2. Installation
     2.1 Player Configuration
     2.2 Player options
     2.3 Server Configuration

3. Changes to base Tribes2
     3.1 Game play changes
     3.2 DnD game type
     3.3 Maps

4. How-To
     4.1 Using Admin HUD
     4.2 Using CTF Practice Mode

5. Technical script references

6. Credits
     6.1 Producers / Scripters
     6.2 Mappers
     6.3 Code contributors
     6.4 Organizations
     6.5 Others

7. Legal
     7.1 Copyright information



-------------------------------------------------------------------------------
(0)                            VERSION HISTORY
-------------------------------------------------------------------------------
 
 Version 1.1 (10/31/02)

    - Added key-binds for "Max Throw Grenade" and "Max Throw Mine", which will
      throw a grenade or mine at maximum throwing range.
    - Added "Send Bottomprint Message" and "Send Centerprint Message" options
      to the Admin Hud. See section 4.1.
    - The maximum range of tank's mortar weapon is now properly adjusted for
      Classic's gravity.
    - Players have to wait 1 second to pick up a flag after they drop/throw it.
      This fixes the problem where players would instantly pick up the flag
      when trying to throw it some angles.
    - Players no longer pass through flags when moving fast.
    - Players take slightly less damage when colliding with objects.
    - Slight increase to satchel damage, explosion radius, and kickback.
    - The shield pack energy for the heavy armor absorbs more damage.
    - Chaingun is slightly more accurate at long range.
    - Mines do slightly less damage to static shapes (Inventories, stations,
      sensors, generators, beacons, satchel). This prevents them from be
      destroyed by a single mine-disk.
    - Generators are a bit more resistant to damage.
    - The vertical thrusting power of Shrike was increased slightly.
    - Beacons can only be markers now, not targeting devices.
    - The sound that's triggered when a team loses control of a FlipFlop was
      changed. The old one was the same as the flag taken sound.
    - Optimized the Acid Rain map for better fps. Terrain was added above each
      main base to bury invisible vehicle blockers that protrude through the
      roof of the base.
    - Optimized the Lakefront map for better fps.
    - Added Mobile Point Base on the Hillside map.
    - Removed the '-' character from Sub-zero's filename. This was causing a
      problem when Sub-zero was added to the map rotation section of the 
      ServerPrefs.cs file.
    - Fixed a bug that wasn't allowing players to make contact with a flag when
      the player was on the flagstand when the flag was returned.
    - Pratice HUD will only come up when the server is in CTF Practice mode.
    - Fixed a bug in Practice Mode that wasn't putting players in observer mode
      when the Reset option was used.
    - Practice Mode now destroys all existing deployables when an admin uses
      the "Load Deployables" feature. This prevents multiple stacks of
      deployables from cluttering, and lagging, the map.
    - Fixed a bug in Practice Mode that wasn't preventing the rapid-fire weapon
      cheat.
    - Fixed a bug in CTF Practice Mode that wasn't returning flags to the stand
      when the "Reset Map" option was used.
    - Removed the annoying chat box message spam from the Auto-password 
      feature.
    - Fixed the 'Ban Player' lobby option.
    - Fixed an old base bug that was trying to load a mission name instead of
      file.
    - Fixed some bugs in the Defend and Destroy gametype.
    - Fixed a bug that wasn't deleting vehicle flag-camping threads at the end
      of each map.
    - CRC checking is now off by default, instead of on.
    - Updated the three batch files used to start Tribes2 with Classic mod. The
      "DOS" window now disappears after it pops up and doesn't display debug
      messages.
    - Removed all of the Tribes2 trial demo code. This lowers the memory
      requirement and speeds up processing a bit.
    - Fixed a spelling error in station.cs that wasn't causing any problems but
      resulted in inaccurate text searches on the file.



-------------------------------------------------------------------------------
(1)                                ABOUT
-------------------------------------------------------------------------------

 The Classic modification brings speed and intensity back to the Tribes
 game. The majority of the changes revolve around creating a faster and more
 exciting game play experience. Included with this modification is a game type
 called "Defend and Destroy" (DnD), which many players will remember from 
 Tribes1. See section 3.2 for more information on DnD.
 
 Also included are 24 new maps, covering the game types Bounty, Capture & Hold,
 Capture the Flag, Defend & Destroy, Deathmatch, and Team Hunters. See section
 3.3 for map information.
 
 Finally, there are new game play options for clients (section 2.1), a remote 
 admin HUD (section 4.1), a full featured CTF practice mode (section 4.2), and
 new server admin features (section 2.3).



-------------------------------------------------------------------------------
(2)                             Installation
-------------------------------------------------------------------------------

 [2.1] Player Configuration

   a) Run the "Classic_LAN.bat" file in the "...\Tribes2\GameData" directory
      by double clicking on the file. This batch file first deletes all .dso
      files from this directory on down, then it runs Tribes2 Classic.

   b) Once Tribes2 loads, launch a CTF mission then exit Tribes 2. These steps
      are needed to create Classic configuration files.

   c) See section 2.2 for player configuration options.
   
   d) You can join online Tribes2 Classic servers by running Tribes2 from the
      default supplied shortcut.
   
   e) You can start a local game of Tribes2 Classic by double clicking the
      "Classic_LAN.bat" file in the "...\Tribes2\GameData" directory.
      
   f) You can start a online game of Tribes2 Classic by double clicking the
      "Classic_online.bat" file in the "...\Tribes2\GameData" directory.

   g) See section 2.3 to start a dedicated server. The above section is only
      for players.
     
     
 [2.2] Player options

   You can configure specific player options for Classic Mod by going to your
   "...\Tribes2\GameData\classic\prefs\" directory and opening up the
   "ClientPrefs.cs" files with a text editor. The Classic options can be 
   found by searching for "Classic", no quotes. The options are as follows:
   
      $pref::Classic::ignoreTeamDestroyMessages
         Set this to 0 to see chat HUD messages whenever a teammate destroys
         an enemy object. Set it 1 to hide these messages.
         
      $pref::Classic::playerHitSound
         This option controls whether or not you hear a sound whenever you 
         damage an enemy players. Set this option to 1 if you want to hear a 
         sound every time you hit a enemy player. Set it to 0 to turn this
         feature off.
         
      $pref::Classic::playerHitWav
         This option is ignored if you have $pref::Classic::playerHitSound set
         to 0. Otherwise, this option specifies a custom wav file to play when
         you hit enemy players. You don't need to set this option if you want
         use the default hit wav file.

         To specify a custom hit wav file, place it into the 
         "...\Tribes2\base\audio" directory. Create this audio directory if
         it isn't there. The value in the "ClientPrefs.cs" file has to be in 
         double quotes, has to begin with the two characters: ~w, and must
         contain a valid path to the wav file.

         Example 1:
         To use your own wav file you would create a "audio" directory under
         the "base" directory if it's not already there. This would be the
         path: "...\Tribes2\GameData\base\audio\fx\weapons\".
         Lets say your wav file has a filename of "guns.wav". Place this file
         in the audio directory and change the $pref::Classic::playerHitWav
         entry to:
         $pref::Classic::playerHitWav = "~wguns.wav";
         
         Example 2:
         The default value of:
         $pref::Classic::playerHitWav = "~wfx/weapons/cg_hard4.wav";
         looks for the cg_hard4.wav file in the directory:
         "...\Tribes2\GameData\base\audio\fx\weapons\ 
         This directory is part of the Tribes2 directory structure found in the
         audio.vl2 file. There are hundreds of sound effects in the audio.vl2
         you can use. You can browse vl2 files with any program that can read
         the zip format.
               
         The .wav file is played when damage is done with any of these weapons:
         Blaster, Plasma Gun, Chaingun, Disc, Grenades (launcher and hand), 
         Laser Rifle, Mortar, Missile, ShockLance, Impact (object to object,
         AKA ramming), controlled turrets, Shrike Blaster, Bomber Plasma, Bomber
         Bombs, Tank Chaingun, Tank Mortar, and Satchel.
      
      $pref::Classic::vehicleHitSound
         This options works exactly like the "$pref::Classic::playerHitSound"
         option, but triggers when you damage enemy vehicles.
      
      $pref::Classic::vehicleHitWav
         This is configured the same way as the "$pref::Classic::playerHitWav"
         option above.
         
      $pref::Classic::wasteRepairKit
         Set this to 1 if you want to be able to use your repair kit when your
         life is at maximum. Set it to 0 if you don't want to expend your 
         repair kit when you press your "use repair kit" key when your life is
         at maximum. Some players prefer to use their repair kit before killing
         themselves, thus preventing it from being picked up by enemy players.
         
         
 [2.3] Server Configuration

   a) Edit the "Classic_dedicated_server.bat" file in the 
      "...\Tribes2\GameData" directory and change the default port of 28000 if
      desired.
      
   b) Double click the "Classic_dedicated_server.bat" file in the 
      "...\Tribes2\GameData" directory. This will launch the program 
      "Infinite Spawn". Let the server load completely and then click the 
      "Quit" button in the Infinite Spawn window. We need to bring the server
      up and down so Classic Mod can write some server configuration files.

   c) Configure Classic server options by going to your 
      "...\Tribes2\GameData\classic\prefs\" directory and opening up the
      "ServerPrefs.cs" files with a text editor. The Classic options can be 
      found by searching for "Classic", no quotes ($Host::AdminPassword is the 
      sole exception). The options are as follows:

         $Host::AdminPassword
            Password to gain Admin status on server. The default value of
            "changethis" is ignored.

         $Host::ClassicAutoPWEnabled
            Automatic locking of server after $Host::LockServerPlayerCount 
            number of players on the server is reached.
            
         $Host::ClassicAutoPWPassword:
            $Host::Password is changed to this when automatic password is 
            triggered.
         
         $Host::ClassicAutoPWPlayerCount
            When server reaches this number of players, and 
            $Host::ClassicAutoPWEnabled is set to 1, the server join password is
            set to $Host::ClassicAutoPWPassword.

         $Host::ClassicAutoRestartServer
            Sets automatic server restart on and off. 1 is enabled, 0 is 
            disabled. If on, server will reload every $Host::ClassicRestartTime
            hours.

         $Host::ClassicEchoChat
            Echoes global chat to remote telnet console. 1 is enabled, 0 is 
            disabled.

         $Host::ClassicFairTeams
            Disallow players from making teams uneven. 1 is enabled, 0 is 
            disabled.

         $Host::ClassicLogEchoEnabled
            Echoes special messages to console. 1 is enabled, 0 is disabled.

         $Host::ClassicMaxTelepads
            Maximum numbers of Practice CTF Teleport Pads each player can 
            deploy.

         $Host::ClassicRandomizeTeams
            Randomly picks a team to place players on. 1 is enabled, 0 is 
            disabled.

         $Host::ClassicRandomMissions
            Randomly load missions of the same type instead of in reverse 
            alphabetical order. 1 is enabled, 0 is disabled.
            
         $Host::ClassicRestartTime
            Time in hours until server will automatically restart, but only
            if $Host::ClassicAutoRestartServer is set to 1.
            
         $Host::ClassicSuperAdminPassword
            Password to gain Super Admin status on the server. The default
            value of "changeme" is ignored.
            
         $Host::ClassicTelnet:
            Allow remote telnet access? 1 is enabled, 0 is disabled.

         $Host::ClassicTelnetListenPass
            Read-only telnet access password, for admins.
           
         $Host::ClassicTelnetPassword
            Full access telnet password, for super admins.
            
         $Host::ClassicTelnetPort
            Server port for remote telnet applications can connect to.

   d) You can now double click the "Classic_dedicated_server.bat" file in the
      "...\Tribes2\GameData" directory. This will launch the program 
      "Infinite Spawn", and start your configured dedicated server.



-------------------------------------------------------------------------------
(3)                         Changes to base Tribes2
-------------------------------------------------------------------------------

 [3.1] Game play changes

    (ADMIN CHANGES)
      - SuperAdmins can now kick admins.
      - SuperAdmins can remove admin power from normal admins.
      - Admins can issue warnings to players, who will see a warning message 
        telling them they will be kicked from the server if their behavior 
        continues. Select "Warn player" from the menu that appears when you
        right click on a players name in the lobby list.
      - Added a Random Team assignment feature that can be toggled on and off
        with the Admin HUD.
      - Added 'Auto-Server-Restart' and 'Server Restart' features.
      - More flexible SAD (SuperAdmin) function.
      - Added a new mission list management system that gives admins better 
        control over mission rotation.
      - Added Auto-password feature that sets the join password on the server 
        when x number of players are on the server.
      - Added a random mission loading feature.
      - Added an Admin HUD to control admin features. See section 4.1.
      - Admins can send private messages to players. Select "Send Private 
        Message" from the menu that appears when you right click on a players
        name in the lobby list.
      - Added many new server preferences to the ServerPrefs.cs files. See 
        section 2.3.
      - Anyone trying to guess the SAD password by spamming values will be 
        banned.
      - Added a working "Add Bot" function to the lobby menu.
      - Allow super admins to admin players even when
        $Host::allowAdminPlayerVotes is false.
      - Added logging function to support file.
      - CRC checking is now off by default, instead of on.
   
    (PLAYER ARMOR CHANGES)
      - Completely new physics model.
      - Committing suicide no longer incurs a -10 penalty to a players score.
      - Delay from using suicide key changed from 10 seconds to 2.
      - Players no longer spawn invisible.
      - Reduced the time between being able to mount vehicles.
      - There's more clearance when ejecting from vehicles.
      - The screen doesn't shake as much when skiing.
      - Slight increase to heavy walk speed.
      - Default grenade launcher ammo for medium armor changed from 12 to 15.
       
    (HAND GRENADE CHANGES)
      - They inherit more velocity from a moving player.
      - Higher arc when tossed.
      - Players lose spawn invulnerability and uncloak when using grenades.
      - Added a key-bind for "Max Throw Grenade",  which will throw a grenade
        or mine at maximum throwing range.
      - Added a bind to throw grenade ammo.
      - Lowered the maximum time a player can be blinded by whiteouts.
      
    (CAMERA)
      - Cameras detect enemies using Sensor Jammer Packs.
      - Camera field of view was tweaked to remove the fisheye look.
      - Camera eye can now be controlled in the Command Screen.
      - Cameras can no longer be viewed from 3rd person in the Command Screen.
  
    (HUD CHANGES)
      - Vehicle Station HUD doesn't close after making a selection.
      - Vehicle Station HUD pops up as soon as player steps on vehicle pad.
      - Vehicle Station quick-keys are always mapped to the same keys, 
        regardless of which vehicles are available in the map.
      - Fixed a Objective HUD bug.
      - Added Admin HUDs and Practice Mode. See sections 4.1 and 4.2.

    (GENERAL CHANGES)
      - Added a Defend and Destroy game type. See section 3.2.
      - Added a waiting period when changing teams to prevent an exploit that
        could crash servers. Admins can always team change whomever they want.
      - Fixed a bug that was sending multiple repair messages to players when
        a teammate repaired an object.
      - Only the "Storm" and "Inferno" team names are used.
      - Every 20 seconds players can switch their spawn spot during the 
        tournament mode wait period by pressing the suicide key.
      - A global chat message is displayed to all players whenever a remote
        telnet connection has been established to the server.
      - Fixed bugs that weren't adding players to admin and super admin list.
      - Added a Mod HUD, so server side mods can have some client-side support.
      - Allow observer global chat to be seen by all, not just admins and other
        observers.
      - More information is displayed when observing a player.
      - Observer camera is faster.
      - A sound plays when you hit enemy players and vehicles. See section 2.2.
      - Added a client side option to allow players to use their repair kit
        even if their health is at maximum. See section 2.2.
      - Added chat messages that inform players when their teammates destroy 
        enemy objects. See section 2.2.
      - Some admin features now display the name of the admin issuing the 
        command.
      - Removed a client's direct requests to start model animations. This
        prevents players from purposely switching the animation frame for their
        player model.
      - Replaced the teammate repair spam message system with a new one that 
        sends teammates repair and destroy messages. This is for both 
        information purposes and for stat tracking.
      - Added a function for client-side scripters that returns a string that
        contains the mod name the server is running. See the setCameraSpeed()
        function in clientOverloads.cs for an example of how this works.
      - Beacons can only be markers now, not targeting devices.
      - Change the sound that's triggered when a team loses control of a 
        FlipFlop. The old one was the same as when your teams flag is taken.

    (FLAG CHANGES)
      - Flags now follow the movement vector of the dropping corpse instead of
        a random vector.
      - More tossing power when throwing the flag.
      - Added mass to the flag to prevent it from flying across the map.
      - Increased the pick up radius of the flag from 3m to 3.5m. This helps
        fix the 'passing through flag' problem.
      - Flags on stands no longer rise and hover over players standing on them.
      - Added fix to keep flag from sticking to OOB grid. Flag now bounces back
        in bounds.
      - Player loses spawn invulnerability when they pick up the flag.
      - Added code to give players a more accurate representation of where a
        dropped flagged is. This should keep players from passing through flags
        when they are out in the field. Thanks for this goes to KineticPoet and
        the Team Rabbit 2 team for a single flag solution.
      - Players have to wait 1 second to pick up a flag after they drop/throw it.
        This fixes the problem where players would instantly pick up the flag
        when trying to throw it some angles.
      - Made some changes that may help the problem of fast players passing
        through flags.

    (CLOAK PACK CHANGES)
      - Lowered energy consumption rate.
      - Players are now completely invisible when cloaked.

    (DEPLOYABLE PULSE SENSOR PACK CHANGES)
      - Increased detection radius from 150m to 175m.

    (SATCHEL PACK CHANGES)
      - Fixed a satchel bug where clients couldn't pick up packs when their
        satchel was destroyed from damage.
      - Satchel packs now stick when making contact with buildings and terrain.
      - The time a satchel explodes after pressing fire was changed from 8/10th
        of a second to 6/10ths.
      - Slight increase to satchel damage.
      - Explosion radius increased from 20 to 25.
      - Kickback increased.

    (REPAIR PACK CHANGES)
      - Slight increase to repair rate for non-player objects
      - Fixed lingering repair projectile bug (?).

    (SENSOR JAMMER PACK CHANGES)
      - When off, the user can only be detected (seen in enemy sensor net) if
        exposed by a camera. If not detected, the player will not project a red
        triangle over his head, and will not be targeted by sensor controlled
        turrets (all but sentry). When the SJP is on, it has the same affect as
        above, but for all friendly players over a 30m radius centered on the
        jamming player. When on, the SJP will also expose cloaked enemies.
      - Fixed a SJP bug that allowed players to retain jammed status after 
        dropping the pack.

    (SHIELD PACK)
      - Shield pack energy of heavy armor absorbs more damage.

    (TURRET CHANGES - ALL)
      - The name of a turret will be changed to the name of the player 
        controlling it.

    (GENERATOR CHANGES)
      - Generators are a bit more resistant to damage.
        
    (AA BARREL CHANGES)
      - Slight decrease to wake up and unfold time.

    (ELF BARREL CHANGES)
      - Slight decrease to wake up and unfold time.

    (MISSILE BARREL CHANGES)
      - Projectile sped up.
      - Missile barrels can no longer be controlled, which prevents long range
        missile fire on enemy targets that can't be distracted by flares.

    (MORTAR BARREL CHANGES)
      - Slight decrease to the projectile detonation time.
      - Projectile velocity and max range increased to match hand mortar.
      - Projectile doesn't float in the air as much.
      - Slight decrease to wake up and unfold time.
      - Increased targeting radius from 160m to 400m.

    (PLASMA BARREL CHANGES)
      - Increased damage.
      - Increased projectile velocity.
      - Decreased time projectile exists.
      - Reduced targeting radius from 150m to 120m.
      - Slight decrease to wake up and unfold time.

    (DEPLOYABLE TURRET CHANGES)
      - Slight decrease to wake up and unfold time.
      - The number of turrets that can be deployed within an area was increased
        by one.
      - The range deployable turrets can be placed near each other was slightly
        reduced.

    (DEPLOYABLE INVENTORY STATION CHANGES)
      - Players don't spin around anymore when using remote inventories.
      - The range deployable inventories can be placed near each other was
        slightly reduced.

    (VEHICLE CHANGES - ALL)
      - Changed the display of the Vehicle Station HUD.
      - Vehicle Station quick-keys are always mapped to the same keys, 
        regardless of which vehicles are available in the map.
      - The Vehicle purchase HUD will remain up until the player leaves
        the station, allowing them to make another selection if their first
        choice isn't available.
      - Added a check to see if a player is in a inventory station when 
        teleporting to a vehicle. This prevents boarding a vehicle with an
        illegal armor type, such as heavy armors piloting Grav Bikes.
      - Players in a multi-player vehicle will receive a message when someone
        enters that vehicle.
      - Lingering time for destroyed vehicles reduced from 2 to 1/2 a second.
      - Sensors from all vehicles are shown on Command Screen, not just the 
        shrike.
      - Fixed reticle problems.
      - Rewrote the Vehicles Station code to fix a energy bug that would show
        the station being powered when it wasn't.

    (BOMBER CHANGES)
      - Kickback from bomb explosion are stronger.
      - Bombers no longer do back-flips when damaged.
      - Turbo thruster is slightly stronger.
      - Maximum speed is slightly higher.
      - Vertical thruster is slightly stronger.
      - Added a bombing reticle to the pilot seat.

    (GRAV BIKE CHANGES)
      - Shield is weaker vs. weapon fire.
      - Maneuvering thruster is stronger.
      - Turbo thruster is stronger, resulting in higher speeds.
      - Added a chaingun to the nose.

    (HAVOK CHANGES)
      - Maneuvering thruster is stronger.
      - Turbo thruster is stronger, resulting in higher speeds.
      - Max speed increased.

    (MOBILE POINT BASE CHANGES)
      - Spawns a bit higher on vehicle pad to prevent it from being dry docked.
      - Turbo thruster is stronger, resulting in higher speeds.
      - Fixed a exploit that allowed concussion grenades to propel the MPB into
        the sky.
      - The minimum range the MPB can be deployed from buildings and turrets
        was significantly reduced.
      - Fixed bug that was displaying the MPB sensor in middle of map on the 
        command screen instead of origin of MPB.
      - Added a MPB teleporter. Once deployed, players can teleport to the MPB
        by stepping onto the new MPB teleport pad on the vehicle station the
        MPB was purchase at. 
      - MPB Turret is modular.

    (TANK CHANGES)
      - Stronger air cushion.
      - The speed the tank takes ground collision damage is higher.
      - Maneuvering thruster is stronger.
      - Turbo thruster is stronger, resulting in higher speeds.
      - Projectile velocity sped up a bit due to higher gravity. Maximum range
        is approximately 450 meters.

    (HAND CHAINGUN CHANGES)
      - Does a tiny bit more damage.
      - Bullet spread is slightly tighter.
      - Bullets travel a bit faster.

    (DISC CHANGES)
      - Projectile speed is slightly faster in and out of water.
      - Disc inherits more velocity from the firing player.
      - Discs skip off of water at angles of 20 degrees or less.
      - Disc spread removed.
      - The explosion kick is greater when damaging self, which allows for
        more powerful disc jumps.
      - Discs do a tiny bit more damage to heavy armors.

    (ELF CHANGES)
      - Increased range from 37 to 38.

    (GRENADE LAUNCHER)
      - Projectile inherits more velocity from the firing player.
      - Projectile is slightly faster.
      - Projectile doesn't float in the air as much.
      - Slight decrease to the projectile detonation time.

    (MINE)
      - More arc when thrown.
      - Inherits more velocity from throwing player.
      - Small decrease to kickback.       
      - Mines can be detonated as soon as they're thrown, AKA "mine discing".
      - Fixed a check to see if mines are banned.
      - Fixed a bug that would forget to decrement the mine count for each 
        team. This would eventually prevent a team from deploying any mines, 
        even if they didn't have any deployed.
      - Tiny increase to damage.
      - Slight increase to detonation range.
      - Raised mine limit to from 20 to 25.
      - Added key-bind for "Max Throw Mine", which will throw a mine at
        maximum throwing range.
      - Mines do slightly less damage to static shapes (Inventories, stations,
        sensors, generators, beacons, satchel). This prevents them from be
        destroyed by a single mine-disk.

    (MISSILE LAUNCHER CHANGES)
      - Missile duration is slightly longer.
      - Maximum projectile velocity increased.
      - Minimum heat to lock onto a target changed to match Missile Barrel.
      - Players can no longer rapid fire missiles.

    (HAND MORTAR CHANGES)
      - Slight decrease to the time the projectile detonates.
      - Explosion radius slightly reduced.
      - Projectile velocity sped up a bit due to higher gravity, but still has
        the same max range.
      - Projectile doesn't float in the air as much.
      - Players can no longer rapid fire mortars.

    (HAND PLASMA CHANGES)
      - Slightly faster plasma projectile.

    (SHOCKLANCE CHANGES)
      - Increased kickback.
      - Increased maximum hit range to 16m.
      - Minimum energy required to fire changed from 15 to 3.
      - Added chat messages, and 1 bonus point, for rear ShockLance hits.

 
 [3.2] DnD game type

   The Defend and Destroy game type from Tribes 1 returns to Tribes 2! Teams
   score 1 point per objective destroyed, which include Vehicle Stations, Large
   and Medium Pulse Sensors, Solar Panels, Generators and base turrets. Teams
   also receive points for each switch held. Included are 9 maps that are 
   usable with Defend and Destroy. See  section 3.3. Mappers should use the
   "DnD" string when making maps with this game type.

   Defend and Destroy isn't limited to just Classic mod.
   
 
 [3.3] Maps

   There are 24 new maps in Classic. The following is a matrix showing which
   game types the maps can be played on.

     MAP    			BOUNTY	CNH	CTF	DND	DM	TH
     ------------------       	------	---	---	---	--	--
     Acid Rain	   	   			X	X
     Blastside					X	X
     Broadside					X	X
     Confusco  	  	 	X		X	   	X
     Dangerous Crossing				X
     Desert of Death				X
     Gorgon	  	  	X		X	   	X
     Hillside	       				X	X
     IceRidge					X
     LakeFront				X	X	X
     Magmatic					X
     Raindance					X
     Ramparts	       		X		X	X  	X	X
     Rollercoaster				X
     Sandstorm	      			X	X	X
     Scarabrae					X	X
     Shock Ridge  			X	X
     Snowblind					X
     StarFallen	     				X	X
     Stonehenge					X
     Sub-zero	       				X
     Surreal        		X		X		X
     Titan	       	   			X
     WhiteDwarf					X



-------------------------------------------------------------------------------
(4)                                How-To
-------------------------------------------------------------------------------

 [4.1] Using Admin HUD

   The new admin HUD allows admins and super admins to issue commands from a
   HUD instead of using the console. To use the admin HUD, you must first bind
   a key to it in the SETTINGS -> CONTROLS area. You can issue the following
   admin commands by bringing up the admin HUD while you have admin or super 
   admin privileges:

    [Player options]

       - Enter Admin Password
           Players can admin themselves by typing the Admin password into the
           input field of the HUD.

       - Enter Super Admin Password
           Players can admin themselves by typing the Admin password into the
           input field of the HUD.

    [Super Admin options]

       - Set Join Password
           Changes server join password to the password entered in the input
           field of the HUD. Use "remove" (without the quotes) to remove the
           join password.

       - Set Admin Password
           Changes the server Admin password to the password entered in the
           input field of the HUD.

       - Set Super Admin Password
           Changes the server Super Admin password to the password entered in
           the input field of the HUD.

       - Set Random Teams
           Enables or disables random team selection for players on the server.
           0 = disabled, 1 = enabled.

       - Set Fair Teams
           Enables or disables team selection for players. When on, the server
           won't allow a player to join a team if that side would have 2 or 
           more players than the other team. 0 = disabled, 1 = enabled.

       - Set Max Players
           Changes the maximum number of players the server will allow. Use 
           numbers, such as 12, 24, 32, not spellings such as twelve, twenty 
           four, etc.

       - Set Auto-PW
           Enables automatic password setting of the server after a specified
           number of players is reached.

       - Set Auto-PW Password
           The join password set when Auto-PW is triggered.

       - Set Auto-PW Count
           The number of players on the server that will trigger the Auto-PW
           to set the join password.

       - Send Bottomprint Message
           Sends a pop-up message to the bottom of each players screen for 8
           seconds.

       - Send Centerprint Message
           Sends a pop-up message to the center of each players screen for 8
           seconds.

       - Remove Map From Rotation
           Removes map filename (less file extension) from the servers mission
           rotation.

       - Restore Map To Rotation
           Restores map filename (less file extension) from the servers mission
           rotation.

       - Remove GameType
           Removes mission type (ex. CnH) from the server.

       - Restore GameType
           Restores mission type (ex. CnH) to the server.

       - Restart server.
           Sends a command to restart the server.
      
 
 [4.2] Using CTF Practice Mode
 
   This mode contains a variety of CTF practice related features. Players and
   admins can control these features by using the Practice HUD. You should 
   first bind a key to 'Practice HUD' in the SETTINGS -> CONTROLS area.
   Practice mode features will only function, and the Practice HUD will only
   pop up, when the server is running in "Capture the Flag (Practice)" mode.
   Admins can put a server into practice mode changing the game type to
   "Capture the Flag (Practice)". 
  
   Only admins can use the upper two HUD views. Players set personal settings
   with the lower 3 HUD views. The controls are:

    - - - - - - - - - - - - - - - Admins only - - - - - - - - - - - - - - - - -
   {TOP LEFT PANE}
    999 Ammo		Each weapons has 999 ammo
    Spawn in Favorite	All players spawn in the favorite they have selected.
    No score limit	The map won't end when the cap limit is reached.
    Auto-return Flags	Flags will return to the stand 3 seconds after dropped.
    Spawn Only		Players aren't allowed to equip.
    Protect Assets	Inventories and generators can't be destroyed.
    Reset Map		Removes deployables and repairs all damaged equipment.
    
   {TOP RIGHT PANE}
    Minimum Turrets	Sets min # of turrets that can be deployed per side.
    Return Flag		Forces a flag to be returned.
    Save Deployables	Saves all deployed objects to a file on the server.
    Load Deployables	Loads a previously saved deploy setup.

    - - - - - - - - - - - - - - Client Settings - - - - - - - - - - - - - - - -
   {MIDDLE BAR}
    Disc		Follow your discs when fired.
    Grenade L.		Follow your grenade launcher projectiles when fired.
    Mortar		Follow your mortars when fired.
    Missile L.		Follow your missiles when fired.
    
   {BOTTOM LEFT PANE}
    Beacon Mode		Deploy normal beacons with beacon key.
    Telepad Mode	Deploy a teleport pad with beacon key.
    Select		Select a teleport pad for usage.
    Destroy		Destroy selected teleport pad.
    Teleport		Teleport self to selected teleport pad.
    
   {BOTTOM RIGHT PANE}
    Wildcat		Spawn a Grav bike at selected teleport pad.
    Beowulf		Spawn a Tank at selected teleport pad.
    Jericho		Spawn a MPB at selected teleport pad.
    Shrike		Spawn a Shrike at selected teleport pad.
    Thundersword	Spawn a Bomber at selected teleport pad.
    Havoc		Spawn a Transport at selected teleport pad.



-------------------------------------------------------------------------------
(5)                       Technical script references
-------------------------------------------------------------------------------

 Scripters interested in finding where changes were made to the script code 
 should look at the file "classic_technical.txt". Each change listed 
 contains a list of the files edited for that change.



-------------------------------------------------------------------------------
(6)                                 Credits
-------------------------------------------------------------------------------

 [6.1] Producers / Scripters

   Paul "z0dd" Paella			z0dd@adelphia.net
   Ralph "ZOD" DeMicco			-

 
 [6.2] Mappers
   
   [Acid Rain]
     Rick "RavensBro" Kasten     	Mapper 1
     Paul "z0dd" Paella			Mapper 2
     Jason "CleverClothe" Archer	Flag hut prefab

   [Blastside]
     Alan "Nefilim" Schwertel	 	Main mapper; textures; prefabs, sky
     Gordon "ToKrZ" West		Base prefab work
     James "Gmd" Eftekari		Terrain textures
     Larry "Sparky" Watson		Terrain
     Paul "z0dd" Paella 		Editor/QA
    
   [Broadside]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Gordon "ToKrZ" West		Base prefab work
     James "Gmd" Eftekari		Terrain textures
     Larry "Sparky" Watson		Terrain
     Paul "z0dd" Paella		 	Editor/QA

   [Confusco]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Dangerous Crossing]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     James "Gmd" Eftekari		Terrain textures
     Larry "Sparky" Watson		Terrain
     Paul "z0dd" Paella 		Editor/QA
     
   [Desert of Death]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Gorgon]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA

   [Hillside]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Ice Ridge]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 	 	Editor/QA
     James "Gmd" Eftekari		Terrain textures
    
   [LakeFront]
     Ralph "ZOD" DeMicco 		Main mapper
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
     Christopher "Akira" Brethen 	Main base

   [Magmatic]
     Dale "Infamous Butcher" Bertheola	Main mapper; prefabs; textures
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
    
   [Raindance]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Ramparts]
     Paul "z0dd" Paella 		Mapper
     Chehmana "peachskin" Alexandre	Water texture
     Luke "raggy" Dyras			Map name

   [Rollercoaster]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Sandstorm]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA

   [Scarabrae]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Gordon "ToKrZ" West		Base object
     Paul "z0dd" Paella 		Editor/QA

   [ShockRidge]
     Nicolas "Rilke" Alioto 		Main mapper; textures; prefabs
     Paul "z0dd" Paella 		Secondary mapper; editor/QA

   [Snowblind]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Star Fallen]
     Christopher "v5planet" Wilhelm 	Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA

   [Surreal]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
     James "Gmd" Eftekari		Terrain textures

   [Stonehenge]
     Alan "Nefilim" Schwertel 		Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Editor/QA
     James "Gmd" Eftekari		Terrain textures

   [SubZero]
     Alan "Nefilim" Schwertel	 	Main mapper; textures; prefabs; sky
     Paul "z0dd" Paella 		Secondary mapper; editor/QA

   [Titan]
     Paul "z0dd" Paella 		Mapper
     Alan "Nefilim" Schwertel	 	Sky
     Christopher "Akira" Brethen 	Flag base object
     Jason "CleverClothe" Archer 	Wall objects around flag object
     James "Gmd" Eftekari		Terrain textures

   [White Dwarf]
     Nicolas "Rilke" Alioto 		Main mapper; textures; prefabs
     Paul "z0dd" Paella 		Secondary mapper; editor/QA
     Chehmana "peachskin" Alexandre	Sky

 
 [6.3] Code contributors

   - Todd "Blotter" Cappezza		Classic webpage
   - SquirrelOfDeath			Help with the passing through flag fix
   - Founder				Various code contributions
   - FSB-AO				Tourney team switching exploit fix
   - Andrew "Yogi" Weiland		Help with stats code
   - Marcos "littlewing" Kohler		Revelation of bizarre ... hacks!
   - Mike "KineticPoet" Johnston	False flag location fix

 
 [6.4] Organizations

   - Sierra				Publisher (www.sierra.com)
   - PNet (Edward "Goaks" Molnar)	Test servers (www.pandoranetworks.org)
   - WSBN				File hosting (www.wsbn.com)
   - Team Warfare			Ladder support (www.teamwarfare.com)
   - Tribal War				Endless criticism (www.tribalwar.com)
     
 
 [6.5] Others
    
   - Team Vanguard for a variety of help (www.team-vanguard.com)
   - Robert "Rasta" Villasana, Hamed "przn" Khoojinian (grammar patrol!)
   - Team Rapture for testing (www.teamrapture.com)
   - #t2scripters on irc2.dynamix.com 
   - #t2mappers on irc2.dynamix.com
   - Participants at TribesCon4 (www.tribescon.com)
   - Participants at UvaLan4 (www.cheaterswayside.com)
   - All the players that helped test this beast. You know who you are.
   - Everyone else we forgot to mention  :(



-------------------------------------------------------------------------------
(7)                                  Legal
-------------------------------------------------------------------------------

  [7.1] Copyright information

    ©1999, 2000, Sierra Entertainment, Inc. All rights reserved.
    Tribes is a registered trademark and Sierra and Sierra Studios are trademarks
    of Sierra Entertainment, Inc.
